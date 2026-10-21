#ifndef LSM_ENGINE_H
#define LSM_ENGINE_H

#include "memtable.h"
#include "sstable.h"
#include "types.h"
#include "wal.h"

#include <algorithm>
#include <cstdio>
#include <dirent.h>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <sys/stat.h>
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
        LoadExistingSSTables();

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
    // 1. Append to Write-Ahead Log (sequential disk I/O + CRC32)
    // 2. Insert into MemTable SkipList (O(log N) in-memory)
    // 3. If MemTable exceeds threshold, flush to a new immutable SSTable
    bool Put(const std::string& key, const std::string& value) {
        ValueEntry entry = ValueEntry::MakeValue(value);
        if (!wal_->Append(key, entry)) {
            return false;
        }
        memtable_.Put(key, value);
        MaybeFlushAndCompact();
        return true;
    }

    // Delete Path:
    // Appends a Tombstone marker. Never modifies existing SSTables in place!
    bool Delete(const std::string& key) {
        ValueEntry tombstone = ValueEntry::MakeTombstone();
        if (!wal_->Append(key, tombstone)) {
            return false;
        }
        memtable_.Delete(key);
        MaybeFlushAndCompact();
        return true;
    }

    // Read Path (Newest -> Oldest):
    // 1. Search active in-memory MemTable
    // 2. Search SSTables from newest (highest table_id) to oldest (lowest table_id)
    //    Each SSTable checks: [min_key, max_key] -> Bloom Filter -> Sparse Index -> Disk Block
    bool Get(const std::string& key, std::string* out_value) {
        ValueEntry entry;
        if (memtable_.Get(key, &entry)) {
            if (entry.is_tombstone()) {
                return false; // Key was deleted!
            }
            if (out_value) *out_value = entry.value;
            return true;
        }

        // Check SSTables from newest to oldest
        for (auto it = sstables_.rbegin(); it != sstables_.rend(); ++it) {
            if ((*it)->Get(key, &entry, &bloom_filter_saves_, &sstable_disk_reads_)) {
                if (entry.is_tombstone()) {
                    return false; // Tombstone in newer SSTable shadows older SSTables!
                }
                if (out_value) *out_value = entry.value;
                return true;
            }
        }
        return false;
    }

    // Range Query:
    // Merges entries across all SSTables (oldest to newest) and the active MemTable,
    // filtering out tombstones and returning sorted live [key, value] pairs in [start_key, end_key].
    std::vector< std::pair<std::string, std::string> > Scan(
        const std::string& start_key = "",
        const std::string& end_key = "") const {

        // Map keeps keys sorted; iterating oldest -> newest naturally overwrites older versions
        std::map<std::string, ValueEntry> merged;

        for (const auto& sst : sstables_) {
            if (!end_key.empty() && sst->min_key() > end_key) continue;
            if (!start_key.empty() && sst->max_key() < start_key) continue;

            std::vector<KeyValue> entries = sst->ReadAllEntries();
            for (const auto& kv : entries) {
                if (!start_key.empty() && kv.key < start_key) continue;
                if (!end_key.empty() && kv.key > end_key) break;
                merged[kv.key] = kv.entry;
            }
        }

        std::vector<KeyValue> mem_entries = memtable_.ExportSorted();
        for (const auto& kv : mem_entries) {
            if (!start_key.empty() && kv.key < start_key) continue;
            if (!end_key.empty() && kv.key > end_key) break;
            merged[kv.key] = kv.entry;
        }

        std::vector< std::pair<std::string, std::string> > results;
        for (const auto& pair : merged) {
            if (!pair.second.is_tombstone()) {
                results.push_back(std::make_pair(pair.first, pair.second.value));
            }
        }
        return results;
    }

    // Flush the in-memory MemTable to a new immutable Level-0 SSTable on disk
    // and truncate the Write-Ahead Log.
    bool Flush() {
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
        memtable_.Clear();
        wal_->Reset();
        flushes_completed_++;
        return true;
    }

    // Compaction:
    // Merges all existing SSTables into a single clean Level-1 SSTable:
    // - Deduplicates overwritten keys (retaining only the newest value)
    // - Permanently purges Tombstone (deleted) entries to reclaim disk space
    // - Removes old obsolete .sst files from disk
    bool Compact() {
        if (sstables_.size() < 2) {
            // Even with 1 SSTable, allow compaction if the user explicitly wants to purge tombstones
            if (sstables_.empty()) return false;
        }

        // Merge from oldest SSTable to newest SSTable so newer entries overwrite older ones
        std::map<std::string, ValueEntry> merged;
        for (const auto& sst : sstables_) {
            std::vector<KeyValue> entries = sst->ReadAllEntries();
            for (const auto& kv : entries) {
                merged[kv.key] = kv.entry;
            }
        }

        // Filter out tombstones (since we are compacting all SSTables down to the base level)
        std::vector<KeyValue> live_entries;
        live_entries.reserve(merged.size());
        for (const auto& pair : merged) {
            if (!pair.second.is_tombstone()) {
                live_entries.push_back(KeyValue{pair.first, pair.second});
            }
        }

        std::vector<std::shared_ptr<SSTable>> old_tables = sstables_;
        sstables_.clear();

        if (!live_entries.empty()) {
            uint64_t compacted_id = next_table_id_++;
            std::string compacted_path = FormatSSTablePath(compacted_id, 1);
            auto compacted_table = SSTable::Create(compacted_path, compacted_id, 1, live_entries);
            if (!compacted_table) {
                sstables_ = old_tables;
                return false;
            }
            sstables_.push_back(compacted_table);
        }

        // Delete old SSTable files from disk
        for (const auto& old_sst : old_tables) {
            std::remove(old_sst->filepath().c_str());
        }

        compactions_completed_++;
        return true;
    }

    LSMStats GetStats() const {
        LSMStats s;
        s.memtable_entries = memtable_.num_entries();
        s.memtable_bytes = memtable_.approximate_memory_usage();
        s.skiplist_height = memtable_.skiplist_height();
        s.wal_bytes = wal_ ? wal_->bytes_written() : 0;
        s.total_sstables = sstables_.size();
        s.wal_recovered_records = wal_recovered_records_;
        s.flushes_completed = flushes_completed_;
        s.compactions_completed = compactions_completed_;
        s.bloom_filter_saves = bloom_filter_saves_;
        s.sstable_disk_reads = sstable_disk_reads_;
        return s;
    }

    const std::vector<std::shared_ptr<SSTable>>& sstables() const { return sstables_; }
    const LSMConfig& config() const { return config_; }
    void set_flush_threshold(size_t bytes) { config_.memtable_flush_threshold_bytes = bytes; }

private:
    void MaybeFlushAndCompact() {
        if (memtable_.approximate_memory_usage() >= config_.memtable_flush_threshold_bytes) {
            Flush();
            if (sstables_.size() >= config_.auto_compaction_sstable_threshold) {
                Compact();
            }
        }
    }

    std::string FormatSSTablePath(uint64_t table_id, int level) const {
        std::ostringstream oss;
        oss << data_dir_ << "/sst_L" << level << "_"
            << std::setw(6) << std::setfill('0') << table_id << ".sst";
        return oss.str();
    }

    static void EnsureDirectoryExists(const std::string& dir) {
#ifdef _WIN32
        _mkdir(dir.c_str());
#else
        mkdir(dir.c_str(), 0755);
#endif
    }

    // Discover and load any existing .sst files in data_dir_ sorted by table_id
    void LoadExistingSSTables() {
        DIR* dir = opendir(data_dir_.c_str());
        if (!dir) return;

        struct DiscoveredFile {
            uint64_t table_id;
            int level;
            std::string full_path;
        };
        std::vector<DiscoveredFile> discovered;

        struct dirent* entry;
        while ((entry = readdir(dir)) != nullptr) {
            std::string name = entry->d_name;
            if (name.size() > 8 && name.substr(0, 5) == "sst_L" &&
                name.substr(name.size() - 4) == ".sst") {
                // Format: sst_L<level>_<6-digit-id>.sst
                int level = name[5] - '0';
                size_t underscore_pos = name.find('_', 5);
                if (underscore_pos != std::string::npos) {
                    std::string id_str = name.substr(underscore_pos + 1, name.size() - underscore_pos - 5);
                    uint64_t id = static_cast<uint64_t>(std::strtoull(id_str.c_str(), nullptr, 10));
                    discovered.push_back(DiscoveredFile{id, level, data_dir_ + "/" + name});
                }
            }
        }
        closedir(dir);

        std::sort(discovered.begin(), discovered.end(),
                  [](const DiscoveredFile& a, const DiscoveredFile& b) {
                      return a.table_id < b.table_id;
                  });

        for (const auto& f : discovered) {
            auto sst = SSTable::Open(f.full_path, f.table_id, f.level);
            if (sst) {
                sstables_.push_back(sst);
                if (f.table_id >= next_table_id_) {
                    next_table_id_ = f.table_id + 1;
                }
            }
        }
    }

    std::string data_dir_;
    LSMConfig config_;
    MemTable memtable_;
    std::unique_ptr<WriteAheadLog> wal_;
    std::vector<std::shared_ptr<SSTable>> sstables_;
    uint64_t next_table_id_;

    // Telemetry counters
    size_t wal_recovered_records_;
    size_t flushes_completed_;
    size_t compactions_completed_;
    size_t bloom_filter_saves_;
    size_t sstable_disk_reads_;
};

} // namespace lsm

#endif // LSM_ENGINE_H
