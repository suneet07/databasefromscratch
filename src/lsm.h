#ifndef LSM_ENGINE_H
#define LSM_ENGINE_H

#include "io.h"
#include "memtable.h"
#include "merge.h"
#include "sstable.h"
#include "types.h"
#include "wal.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <dirent.h>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <set>
#include <shared_mutex>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <direct.h>
#endif

namespace lsm {

struct LSMConfig {
    // Flush MemTable to a Level-0 SSTable when memory exceeds this size (default: 4 KB for easy interactive demo)
    size_t memtable_flush_threshold_bytes = 4 * 1024;
    // Automatically compact when the number of SSTables reaches this threshold
    size_t auto_compaction_sstable_threshold = 4;
    // fdatasync the WAL on every Put/Delete. true = an acknowledged write survives
    // power loss; false = survives a process crash only (much faster, like LevelDB's
    // default WriteOptions::sync = false).
    bool sync_wal = true;
};

struct LSMStats {
    size_t memtable_entries = 0;
    size_t memtable_bytes = 0;
    int skiplist_height = 0;
    size_t wal_bytes = 0;
    size_t total_sstables = 0;
    size_t wal_recovered_records = 0;
    size_t flushes_completed = 0;
    size_t compactions_completed = 0;
    size_t bloom_filter_saves = 0;
    size_t sstable_disk_reads = 0;
};

// Thread-safety: all public methods may be called concurrently.
//  - Put / Delete / Flush / Compact take an exclusive lock.
//  - Get / GetStats take a shared lock, so reads run in parallel with each other.
//  - Scan takes the shared lock only long enough to snapshot the MemTable range
//    and open iterators on the current SSTables; the merge itself runs unlocked.
//
// Crash-safety: the set of live SSTables is recorded in a MANIFEST file that is
// replaced atomically (write tmp -> fsync -> rename). Files not listed in it —
// half-finished flushes, inputs of an interrupted compaction — are deleted on
// open, so a crash can never resurrect keys a compaction already purged.
class LSMDatabase {
public:
    explicit LSMDatabase(const std::string& data_dir, LSMConfig config = LSMConfig())
        : data_dir_(data_dir),
          config_(config),
          next_table_id_(1),
          wal_recovered_records_(0),
          flushes_completed_(0),
          compactions_completed_(0),
          bloom_filter_saves_(0),
          sstable_disk_reads_(0) {

        EnsureDirectoryExists(data_dir_);
        LoadTables();

        std::string wal_path = data_dir_ + "/wal.log";
        std::vector<KeyValue> wal_records;
        wal_recovered_records_ = WriteAheadLog::Replay(wal_path, &wal_records);
        for (const auto& kv : wal_records) {
            memtable_.Apply(kv.key, kv.entry);
        }

        wal_.reset(new WriteAheadLog(wal_path));
    }

    ~LSMDatabase() {
        if (wal_) {
            wal_->Close();
        }
    }

    LSMDatabase(const LSMDatabase&) = delete;
    LSMDatabase& operator=(const LSMDatabase&) = delete;

    // Write Path:
    // 1. Append to Write-Ahead Log (sequential disk I/O + CRC32, optional fdatasync)
    // 2. Insert into MemTable SkipList (O(log N) in-memory)
    // 3. If MemTable exceeds threshold, flush to a new immutable SSTable
    bool Put(const std::string& key, const std::string& value) {
        std::unique_lock<std::shared_timed_mutex> lock(mu_);
        ValueEntry entry = ValueEntry::MakeValue(value);
        if (!wal_->Append(key, entry, config_.sync_wal)) {
            return false;
        }
        memtable_.Put(key, value);
        MaybeFlushAndCompactLocked();
        return true;
    }

    // Delete Path:
    // Appends a Tombstone marker. Never modifies existing SSTables in place!
    bool Delete(const std::string& key) {
        std::unique_lock<std::shared_timed_mutex> lock(mu_);
        ValueEntry tombstone = ValueEntry::MakeTombstone();
        if (!wal_->Append(key, tombstone, config_.sync_wal)) {
            return false;
        }
        memtable_.Delete(key);
        MaybeFlushAndCompactLocked();
        return true;
    }

    // Read Path (Newest -> Oldest):
    // 1. Search active in-memory MemTable
    // 2. Search SSTables from newest (highest table_id) to oldest (lowest table_id)
    //    Each SSTable checks: [min_key, max_key] -> Bloom Filter -> Sparse Index -> Disk Block
    bool Get(const std::string& key, std::string* out_value) {
        std::shared_lock<std::shared_timed_mutex> lock(mu_);
        ValueEntry entry;
        if (memtable_.Get(key, &entry)) {
            if (entry.is_tombstone()) {
                return false; // Key was deleted!
            }
            if (out_value) *out_value = entry.value;
            return true;
        }

        size_t bloom_saves = 0;
        size_t disk_reads = 0;
        bool found = false;
        for (auto it = sstables_.rbegin(); it != sstables_.rend(); ++it) {
            if ((*it)->Get(key, &entry, &bloom_saves, &disk_reads)) {
                found = !entry.is_tombstone(); // Tombstone in newer SSTable shadows older ones
                if (found && out_value) *out_value = entry.value;
                break;
            }
        }
        bloom_filter_saves_ += bloom_saves;
        sstable_disk_reads_ += disk_reads;
        return found;
    }

    // Range Query over [start_key, end_key] ("" = unbounded on that side).
    // Streams a k-way merge of the MemTable and every overlapping SSTable, seeking
    // each table via its sparse index, so memory is O(#tables + results) instead of
    // O(total data). `limit` (0 = no limit) stops early once enough live keys are found.
    std::vector< std::pair<std::string, std::string> > Scan(
        const std::string& start_key = "",
        const std::string& end_key = "",
        size_t limit = 0) const {

        std::vector<std::unique_ptr<SSTable::Iterator>> table_iters;
        std::vector<KeyValue> mem_snapshot;
        {
            std::shared_lock<std::shared_timed_mutex> lock(mu_);
            mem_snapshot = memtable_.ExportRange(start_key, end_key);
            for (auto it = sstables_.rbegin(); it != sstables_.rend(); ++it) {
                const auto& sst = *it;
                if (!end_key.empty() && sst->min_key() > end_key) continue;
                if (!start_key.empty() && sst->max_key() < start_key) continue;
                table_iters.push_back(SSTable::NewIterator(sst)); // opens file now
            }
        }

        std::vector<std::unique_ptr<Cursor>> cursors; // newest first
        cursors.emplace_back(new VectorCursor(std::move(mem_snapshot)));
        for (auto& ti : table_iters) {
            ti->Seek(start_key);
            cursors.emplace_back(new SSTableCursor(std::move(ti)));
        }

        std::vector< std::pair<std::string, std::string> > results;
        for (MergingIterator merged(std::move(cursors)); merged.Valid(); merged.Next()) {
            if (!end_key.empty() && merged.key() > end_key) break;
            if (merged.entry().is_tombstone()) continue;
            results.emplace_back(merged.key(), merged.entry().value);
            if (limit != 0 && results.size() >= limit) break;
        }
        return results;
    }

    // Flush the in-memory MemTable to a new immutable Level-0 SSTable on disk
    // and truncate the Write-Ahead Log.
    bool Flush() {
        std::unique_lock<std::shared_timed_mutex> lock(mu_);
        return FlushLocked();
    }

    // Compaction:
    // Merges all existing SSTables into a single clean Level-1 SSTable:
    // - Deduplicates overwritten keys (retaining only the newest value)
    // - Permanently purges Tombstone (deleted) entries to reclaim disk space
    // - Removes old obsolete .sst files from disk
    bool Compact() {
        std::unique_lock<std::shared_timed_mutex> lock(mu_);
        return CompactLocked();
    }

    LSMStats GetStats() const {
        std::shared_lock<std::shared_timed_mutex> lock(mu_);
        LSMStats s;
        s.memtable_entries = memtable_.num_entries();
        s.memtable_bytes = memtable_.approximate_memory_usage();
        s.skiplist_height = memtable_.skiplist_height();
        s.wal_bytes = wal_ ? wal_->bytes_written() : 0;
        s.total_sstables = sstables_.size();
        s.wal_recovered_records = wal_recovered_records_;
        s.flushes_completed = flushes_completed_;
        s.compactions_completed = compactions_completed_;
        s.bloom_filter_saves = bloom_filter_saves_.load();
        s.sstable_disk_reads = sstable_disk_reads_.load();
        return s;
    }

    // Snapshot of the current SSTables (oldest first). Returned by value so the
    // caller holds no reference into state another thread may modify.
    std::vector<std::shared_ptr<SSTable>> sstables() const {
        std::shared_lock<std::shared_timed_mutex> lock(mu_);
        return sstables_;
    }

    LSMConfig config() const {
        std::shared_lock<std::shared_timed_mutex> lock(mu_);
        return config_;
    }

    void set_flush_threshold(size_t bytes) {
        std::unique_lock<std::shared_timed_mutex> lock(mu_);
        config_.memtable_flush_threshold_bytes = bytes;
    }

    void set_sync_wal(bool sync) {
        std::unique_lock<std::shared_timed_mutex> lock(mu_);
        config_.sync_wal = sync;
    }

private:
    static const char* kManifestName() { return "MANIFEST"; }
    static const char* kManifestHeader() { return "LSM-MANIFEST v1"; }

    bool FlushLocked() {
        if (memtable_.num_entries() == 0) {
            return false;
        }

        std::vector<KeyValue> sorted_entries = memtable_.ExportSorted();
        uint64_t table_id = next_table_id_++;
        std::string sst_path = FormatSSTablePath(table_id, 0);

        auto new_table = SSTable::Create(sst_path, table_id, 0, sorted_entries);
        if (!new_table) {
            return false;
        }

        sstables_.push_back(new_table);
        if (!WriteManifestLocked()) {
            // Not recorded -> not live. The WAL still holds the data.
            sstables_.pop_back();
            std::remove(sst_path.c_str());
            return false;
        }

        // Only now is the data durable elsewhere, so the WAL can be dropped.
        memtable_.Clear();
        wal_->Reset();
        flushes_completed_++;
        return true;
    }

    bool CompactLocked() {
        if (sstables_.empty()) return false;

        // Stream-merge every table (newest first so newer versions win) and drop
        // tombstones: we compact everything down to the base level, so there is
        // nothing older left for a tombstone to shadow.
        std::vector<std::unique_ptr<Cursor>> cursors;
        for (auto it = sstables_.rbegin(); it != sstables_.rend(); ++it) {
            auto ti = SSTable::NewIterator(*it);
            ti->Seek("");
            cursors.emplace_back(new SSTableCursor(std::move(ti)));
        }
        std::vector<KeyValue> live_entries;
        for (MergingIterator merged(std::move(cursors)); merged.Valid(); merged.Next()) {
            if (!merged.entry().is_tombstone()) {
                live_entries.push_back(KeyValue{merged.key(), merged.entry()});
            }
        }

        std::vector<std::shared_ptr<SSTable>> old_tables = sstables_;
        std::string compacted_path;
        sstables_.clear();

        if (!live_entries.empty()) {
            uint64_t compacted_id = next_table_id_++;
            compacted_path = FormatSSTablePath(compacted_id, 1);
            auto compacted_table = SSTable::Create(compacted_path, compacted_id, 1, live_entries);
            if (!compacted_table) {
                sstables_ = old_tables;
                return false;
            }
            sstables_.push_back(compacted_table);
        }

        // Commit point: once the manifest names only the new table, the old ones
        // are garbage even if we crash before deleting them.
        if (!WriteManifestLocked()) {
            sstables_ = old_tables;
            if (!compacted_path.empty()) std::remove(compacted_path.c_str());
            return false;
        }

        for (const auto& old_sst : old_tables) {
            std::remove(old_sst->filepath().c_str());
        }
        io::SyncDirectory(data_dir_);

        compactions_completed_++;
        return true;
    }

    void MaybeFlushAndCompactLocked() {
        if (memtable_.approximate_memory_usage() >= config_.memtable_flush_threshold_bytes) {
            FlushLocked();
            if (sstables_.size() >= config_.auto_compaction_sstable_threshold) {
                CompactLocked();
            }
        }
    }

    std::string FormatSSTablePath(uint64_t table_id, int level) const {
        return data_dir_ + "/" + FormatSSTableName(table_id, level);
    }

    static std::string FormatSSTableName(uint64_t table_id, int level) {
        std::ostringstream oss;
        oss << "sst_L" << level << "_" << std::setw(6) << std::setfill('0') << table_id << ".sst";
        return oss.str();
    }

    static bool ParseSSTableName(const std::string& name, uint64_t* id, int* level) {
        // Format: sst_L<level>_<id>.sst
        if (name.size() <= 10 || name.compare(0, 5, "sst_L") != 0 ||
            name.compare(name.size() - 4, 4, ".sst") != 0) {
            return false;
        }
        size_t underscore = name.find('_', 5);
        if (underscore == std::string::npos || underscore == 5) return false;
        std::string level_str = name.substr(5, underscore - 5);
        std::string id_str = name.substr(underscore + 1, name.size() - underscore - 5);
        if (id_str.empty() || level_str.find_first_not_of("0123456789") != std::string::npos ||
            id_str.find_first_not_of("0123456789") != std::string::npos) {
            return false;
        }
        *level = std::atoi(level_str.c_str());
        *id = static_cast<uint64_t>(std::strtoull(id_str.c_str(), nullptr, 10));
        return true;
    }

    static void EnsureDirectoryExists(const std::string& dir) {
#ifdef _WIN32
        _mkdir(dir.c_str());
#else
        mkdir(dir.c_str(), 0755);
#endif
    }

    std::vector<std::string> ListDirectory() const {
        std::vector<std::string> names;
        DIR* dir = opendir(data_dir_.c_str());
        if (!dir) return names;
        struct dirent* entry;
        while ((entry = readdir(dir)) != nullptr) {
            names.push_back(entry->d_name);
        }
        closedir(dir);
        return names;
    }

    // Atomically persist the current table list (tmp -> fsync -> rename -> fsync dir).
    bool WriteManifestLocked() {
        const std::string path = data_dir_ + "/" + kManifestName();
        const std::string tmp = path + ".tmp";
        {
            std::ofstream out(tmp, std::ios::trunc);
            if (!out.is_open()) return false;
            out << kManifestHeader() << "\n";
            out << "next " << next_table_id_ << "\n";
            for (const auto& sst : sstables_) {
                out << "table " << FormatSSTableName(sst->table_id(), sst->level()) << "\n";
            }
            out << "end\n";
            out.flush();
            if (!out.good()) return false;
        }
        if (!io::SyncFile(tmp) || !io::AtomicReplace(tmp, path)) {
            std::remove(tmp.c_str());
            return false;
        }
        io::SyncDirectory(data_dir_);
        return true;
    }

    // Returns true and fills `names`/`next_id` iff a complete manifest exists.
    bool ReadManifest(std::vector<std::string>* names, uint64_t* next_id) const {
        std::ifstream in(data_dir_ + "/" + kManifestName());
        if (!in.is_open()) return false;
        std::string line;
        if (!std::getline(in, line) || line != kManifestHeader()) return false;
        bool complete = false;
        while (std::getline(in, line)) {
            if (line == "end") { complete = true; break; }
            if (line.compare(0, 5, "next ") == 0) {
                *next_id = static_cast<uint64_t>(std::strtoull(line.c_str() + 5, nullptr, 10));
            } else if (line.compare(0, 6, "table ") == 0) {
                names->push_back(line.substr(6));
            }
        }
        return complete;
    }

    struct TableFile {
        uint64_t table_id;
        int level;
        std::string name;
    };

    void LoadTables() {
        std::vector<std::string> dir_entries = ListDirectory();

        // Leftovers from a crash mid-write are never valid.
        for (const auto& name : dir_entries) {
            if (name.size() > 4 && name.compare(name.size() - 4, 4, ".tmp") == 0) {
                std::remove((data_dir_ + "/" + name).c_str());
            }
        }

        std::vector<std::string> manifest_names;
        uint64_t manifest_next = 1;
        const bool have_manifest = ReadManifest(&manifest_names, &manifest_next);

        std::vector<TableFile> tables;
        if (have_manifest) {
            std::set<std::string> live(manifest_names.begin(), manifest_names.end());
            for (const auto& name : manifest_names) {
                TableFile f;
                if (ParseSSTableName(name, &f.table_id, &f.level)) {
                    f.name = name;
                    tables.push_back(f);
                }
            }
            // Anything not in the manifest is an orphan (interrupted flush or
            // compaction input that was already superseded). Delete it.
            for (const auto& name : dir_entries) {
                uint64_t id; int level;
                if (ParseSSTableName(name, &id, &level) && !live.count(name)) {
                    std::remove((data_dir_ + "/" + name).c_str());
                }
            }
            next_table_id_ = std::max(next_table_id_, manifest_next);
        } else {
            // Legacy directory (pre-manifest) or fresh DB: trust what's on disk.
            for (const auto& name : dir_entries) {
                TableFile f;
                if (ParseSSTableName(name, &f.table_id, &f.level)) {
                    f.name = name;
                    tables.push_back(f);
                }
            }
        }

        std::sort(tables.begin(), tables.end(),
                  [](const TableFile& a, const TableFile& b) { return a.table_id < b.table_id; });

        for (const auto& f : tables) {
            auto sst = SSTable::Open(data_dir_ + "/" + f.name, f.table_id, f.level);
            if (sst) {
                sstables_.push_back(sst);
            } else {
                std::cerr << "lsm: warning: could not open SSTable " << f.name << "\n";
            }
            if (f.table_id >= next_table_id_) {
                next_table_id_ = f.table_id + 1;
            }
        }

        if (!have_manifest) {
            WriteManifestLocked(); // upgrade legacy dir / initialise fresh one
        }
    }

    std::string data_dir_;
    LSMConfig config_;
    MemTable memtable_;
    std::unique_ptr<WriteAheadLog> wal_;
    std::vector<std::shared_ptr<SSTable>> sstables_;
    uint64_t next_table_id_;
    mutable std::shared_timed_mutex mu_;

    // Telemetry counters (read-path counters are bumped under a shared lock)
    size_t wal_recovered_records_;
    size_t flushes_completed_;
    size_t compactions_completed_;
    std::atomic<size_t> bloom_filter_saves_;
    std::atomic<size_t> sstable_disk_reads_;
};

} // namespace lsm

#endif // LSM_ENGINE_H
