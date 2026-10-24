#ifndef LSM_SSTABLE_H
#define LSM_SSTABLE_H

#include "bloom.h"
#include "io.h"
#include "types.h"
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace lsm {

// Immutable on-disk Sorted String Table (SSTable).
//
// File Binary Layout:
// +-------------------------------------------------------------------+
// | 1. DATA SECTION (offset 0)                                        |
// |    Sorted records: [Type(1B)][KeyLen(4B)][ValLen(4B)][Key][Value] |
// +-------------------------------------------------------------------+
// | 2. SPARSE INDEX BLOCK (offset = index_offset)                     |
// |    [MinKeyLen(4B)][MinKey][MaxKeyLen(4B)][MaxKey]                 |
// |    [NumSparseEntries(4B)]                                         |
// |    Repeated: [KeyLen(4B)][Key][FileOffset(8B)][BlockCount(4B)]    |
// +-------------------------------------------------------------------+
// | 3. BLOOM FILTER BLOCK (offset = bloom_offset)                     |
// |    Serialized bit array + hash count                              |
// +-------------------------------------------------------------------+
// | 4. FOOTER (32 bytes at EOF)                                       |
// |    [IndexOffset(8B)][IndexSize(4B)]                               |
// |    [BloomOffset(8B)][BloomSize(4B)]                               |
// |    [NumEntries(4B)][MagicNumber = 0x4C534D31 "LSM1" (4B)]         |
// +-------------------------------------------------------------------+
class SSTable {
public:
    static constexpr uint32_t kMagicNumber = 0x4C534D31u; // "LSM1"
    static constexpr size_t kFooterSize = 32;
    static constexpr size_t kSparseIndexStride = 16; // 1 in-memory index entry per 16 records

    struct SparseIndexEntry {
        std::string key;
        uint64_t file_offset;
        uint32_t record_count;
    };

    // Build and write a new immutable SSTable from a sorted list of KeyValue entries
    static std::shared_ptr<SSTable> Create(
        const std::string& filepath,
        uint64_t table_id,
        int level,
        const std::vector<KeyValue>& sorted_entries) {

        if (sorted_entries.empty()) {
            return nullptr;
        }

        // Crash safety: write everything to "<path>.tmp", fsync it, then atomically
        // rename it into place. A crash mid-write can only leave a stray .tmp file
        // (cleaned up on open), never a truncated table under the real name.
        const std::string tmp_path = filepath + ".tmp";
        std::ofstream out(tmp_path, std::ios::binary | std::ios::trunc);
        if (!out.is_open()) {
            return nullptr;
        }

        auto table = std::shared_ptr<SSTable>(new SSTable());
        table->filepath_ = filepath;
        table->table_id_ = table_id;
        table->level_ = level;
        table->num_entries_ = static_cast<uint32_t>(sorted_entries.size());
        table->min_key_ = sorted_entries.front().key;
        table->max_key_ = sorted_entries.back().key;
        table->bloom_ = BloomFilter(sorted_entries.size(), 10);

        // 1. Write Data Section and populate Sparse Index + Bloom Filter in memory
        for (size_t i = 0; i < sorted_entries.size(); ++i) {
            const auto& kv = sorted_entries[i];
            uint64_t record_offset = static_cast<uint64_t>(out.tellp());

            if (i % kSparseIndexStride == 0) {
                // NOTE: don't pass kSparseIndexStride to std::min — it takes its
                // arguments by reference, which ODR-uses the static constexpr member
                // and fails to link in C++14 debug builds (no out-of-line definition).
                const size_t remaining = sorted_entries.size() - i;
                uint32_t block_count = static_cast<uint32_t>(
                    remaining < kSparseIndexStride ? remaining : kSparseIndexStride);
                table->sparse_index_.push_back(
                    SparseIndexEntry{kv.key, record_offset, block_count});
            }

            table->bloom_.Add(kv.key);

            uint8_t type_byte = static_cast<uint8_t>(kv.entry.type);
            uint32_t key_len = static_cast<uint32_t>(kv.key.size());
            uint32_t val_len = static_cast<uint32_t>(kv.entry.value.size());

            out.write(reinterpret_cast<const char*>(&type_byte), sizeof(type_byte));
            out.write(reinterpret_cast<const char*>(&key_len), sizeof(key_len));
            out.write(reinterpret_cast<const char*>(&val_len), sizeof(val_len));
            if (key_len > 0) out.write(kv.key.data(), key_len);
            if (val_len > 0) out.write(kv.entry.value.data(), val_len);
        }

        // 2. Write Sparse Index Block (includes min_key and max_key metadata)
        uint64_t index_offset = static_cast<uint64_t>(out.tellp());
        WriteString(out, table->min_key_);
        WriteString(out, table->max_key_);

        uint32_t num_sparse = static_cast<uint32_t>(table->sparse_index_.size());
        out.write(reinterpret_cast<const char*>(&num_sparse), sizeof(num_sparse));
        for (const auto& idx : table->sparse_index_) {
            WriteString(out, idx.key);
            out.write(reinterpret_cast<const char*>(&idx.file_offset), sizeof(idx.file_offset));
            out.write(reinterpret_cast<const char*>(&idx.record_count), sizeof(idx.record_count));
        }
        uint64_t after_index = static_cast<uint64_t>(out.tellp());
        uint32_t index_size = static_cast<uint32_t>(after_index - index_offset);

        // 3. Write Bloom Filter Block
        uint64_t bloom_offset = after_index;
        std::string raw_bloom = table->bloom_.Serialize();
        uint32_t bloom_size = static_cast<uint32_t>(raw_bloom.size());
        out.write(raw_bloom.data(), bloom_size);

        // 4. Write 32-byte Footer
        uint32_t magic = kMagicNumber;
        out.write(reinterpret_cast<const char*>(&index_offset), sizeof(index_offset));
        out.write(reinterpret_cast<const char*>(&index_size), sizeof(index_size));
        out.write(reinterpret_cast<const char*>(&bloom_offset), sizeof(bloom_offset));
        out.write(reinterpret_cast<const char*>(&bloom_size), sizeof(bloom_size));
        out.write(reinterpret_cast<const char*>(&table->num_entries_), sizeof(table->num_entries_));
        out.write(reinterpret_cast<const char*>(&magic), sizeof(magic));

        out.flush();
        table->file_size_ = static_cast<uint64_t>(out.tellp());
        const bool write_ok = out.good();
        out.close();

        if (!write_ok || out.fail() || !io::SyncFile(tmp_path) ||
            !io::AtomicReplace(tmp_path, filepath)) {
            std::remove(tmp_path.c_str());
            return nullptr;
        }
        io::SyncDirectory(DirName(filepath));

        return table;
    }

    // Open an existing SSTable by reading ONLY its Footer, Bloom Filter, and Sparse Index!
    // Data records stay on disk until queried.
    static std::shared_ptr<SSTable> Open(const std::string& filepath, uint64_t table_id, int level) {
        std::ifstream in(filepath, std::ios::binary | std::ios::ate);
        if (!in.is_open()) return nullptr;

        std::streampos end_pos = in.tellg();
        if (end_pos < static_cast<std::streampos>(kFooterSize)) {
            return nullptr;
        }

        uint64_t file_size = static_cast<uint64_t>(end_pos);
        in.seekg(static_cast<std::streamoff>(file_size - kFooterSize), std::ios::beg);

        uint64_t index_offset = 0;
        uint32_t index_size = 0;
        uint64_t bloom_offset = 0;
        uint32_t bloom_size = 0;
        uint32_t num_entries = 0;
        uint32_t magic = 0;

        in.read(reinterpret_cast<char*>(&index_offset), sizeof(index_offset));
        in.read(reinterpret_cast<char*>(&index_size), sizeof(index_size));
        in.read(reinterpret_cast<char*>(&bloom_offset), sizeof(bloom_offset));
        in.read(reinterpret_cast<char*>(&bloom_size), sizeof(bloom_size));
        in.read(reinterpret_cast<char*>(&num_entries), sizeof(num_entries));
        in.read(reinterpret_cast<char*>(&magic), sizeof(magic));

        if (!in.good() || magic != kMagicNumber) {
            return nullptr;
        }

        auto table = std::shared_ptr<SSTable>(new SSTable());
        table->filepath_ = filepath;
        table->table_id_ = table_id;
        table->level_ = level;
        table->num_entries_ = num_entries;
        table->file_size_ = file_size;

        // Read Sparse Index Block
        in.seekg(static_cast<std::streamoff>(index_offset), std::ios::beg);
        if (!ReadString(in, &table->min_key_) || !ReadString(in, &table->max_key_)) {
            return nullptr;
        }

        uint32_t num_sparse = 0;
        if (!in.read(reinterpret_cast<char*>(&num_sparse), sizeof(num_sparse))) {
            return nullptr;
        }
        table->sparse_index_.reserve(num_sparse);
        for (uint32_t i = 0; i < num_sparse; ++i) {
            SparseIndexEntry entry;
            if (!ReadString(in, &entry.key)) return nullptr;
            if (!in.read(reinterpret_cast<char*>(&entry.file_offset), sizeof(entry.file_offset))) return nullptr;
            if (!in.read(reinterpret_cast<char*>(&entry.record_count), sizeof(entry.record_count))) return nullptr;
            table->sparse_index_.push_back(entry);
        }

        // Read Bloom Filter Block
        in.seekg(static_cast<std::streamoff>(bloom_offset), std::ios::beg);
        std::string raw_bloom(bloom_size, '\0');
        if (bloom_size > 0 && !in.read(&raw_bloom[0], bloom_size)) {
            return nullptr;
        }
        table->bloom_ = BloomFilter::Deserialize(raw_bloom);

        return table;
    }

    // Point lookup in this SSTable:
    // 1. Key-range check [min_key, max_key] (in-memory)
    // 2. Bloom filter check (in-memory)
    // 3. Binary search on Sparse Index (in-memory)
    // 4. Short sequential read of <= 16 records on disk
    bool Get(const std::string& key, ValueEntry* out_entry,
             size_t* bloom_hits = nullptr, size_t* disk_reads = nullptr) const {
        if (key < min_key_ || key > max_key_) {
            return false;
        }

        if (!bloom_.MayContain(key)) {
            if (bloom_hits) (*bloom_hits)++; // Saved a disk seek via Bloom Filter!
            return false;
        }

        if (sparse_index_.empty()) {
            return false;
        }

        // Binary search for the largest sparse_index_[i].key <= key
        size_t left = 0;
        size_t right = sparse_index_.size();
        while (left < right) {
            size_t mid = left + (right - left) / 2;
            if (sparse_index_[mid].key <= key) {
                left = mid + 1;
            } else {
                right = mid;
            }
        }

        if (left == 0) {
            return false;
        }
        const SparseIndexEntry& block = sparse_index_[left - 1];

        if (disk_reads) (*disk_reads)++;
        std::ifstream in(filepath_, std::ios::binary);
        if (!in.is_open()) return false;

        in.seekg(static_cast<std::streamoff>(block.file_offset), std::ios::beg);
        for (uint32_t i = 0; i < block.record_count; ++i) {
            KeyValue kv;
            if (!ReadRecord(in, &kv)) break;
            if (kv.key == key) {
                if (out_entry) *out_entry = kv.entry;
                return true;
            }
            if (kv.key > key) {
                break; // Passed target key in sorted data block
            }
        }
        return false;
    }

    // Forward-only streaming cursor over this table's records in key order.
    // The file is opened when the iterator is created, so on POSIX it stays
    // readable even if compaction unlinks the table while a scan is running.
    class Iterator {
    public:
        bool Valid() const { return valid_; }
        const KeyValue& Current() const { return current_; }
        const std::string& key() const { return current_.key; }
        const ValueEntry& entry() const { return current_.entry; }

        void Next() {
            if (remaining_ == 0 || !ReadRecord(in_, &current_)) {
                valid_ = false;
                return;
            }
            --remaining_;
            valid_ = true;
        }

        // Position at the first record with key >= target ("" = first record).
        // Uses the in-memory sparse index to jump to the right 16-record block,
        // so only that block is scanned instead of the whole file.
        void Seek(const std::string& target) {
            valid_ = false;
            if (!in_.is_open()) return;

            size_t block = 0;
            if (!target.empty() && target > table_->min_key_) {
                const auto& idx = table_->sparse_index_;
                size_t left = 0, right = idx.size();
                while (left < right) {
                    size_t mid = left + (right - left) / 2;
                    if (idx[mid].key <= target) left = mid + 1; else right = mid;
                }
                block = (left == 0) ? 0 : left - 1;
            }

            uint64_t offset = 0;
            if (!table_->sparse_index_.empty()) {
                offset = table_->sparse_index_[block].file_offset;
            }
            const uint64_t skipped = static_cast<uint64_t>(block) * kSparseIndexStride;
            remaining_ = (skipped >= table_->num_entries_) ? 0 : table_->num_entries_ - skipped;

            in_.clear();
            in_.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
            Next();
            while (valid_ && current_.key < target) Next();
        }

    private:
        friend class SSTable;
        explicit Iterator(std::shared_ptr<const SSTable> table)
            : table_(std::move(table)),
              in_(table_->filepath_, std::ios::binary),
              remaining_(0),
              valid_(false) {}

        std::shared_ptr<const SSTable> table_;
        std::ifstream in_;
        uint64_t remaining_;
        bool valid_;
        KeyValue current_;
    };

    // Create an unpositioned iterator; call Seek() before use.
    static std::unique_ptr<Iterator> NewIterator(const std::shared_ptr<const SSTable>& table) {
        return std::unique_ptr<Iterator>(new Iterator(table));
    }

    // Read all records in sorted order (loads the whole table into memory;
    // prefer NewIterator() for scans and compaction)
    std::vector<KeyValue> ReadAllEntries() const {
        std::vector<KeyValue> entries;
        entries.reserve(num_entries_);

        std::ifstream in(filepath_, std::ios::binary);
        if (!in.is_open()) return entries;

        for (uint32_t i = 0; i < num_entries_; ++i) {
            KeyValue kv;
            if (!ReadRecord(in, &kv)) break;
            entries.push_back(kv);
        }
        return entries;
    }

    const std::string& filepath() const { return filepath_; }
    uint64_t table_id() const { return table_id_; }
    int level() const { return level_; }
    void set_level(int l) { level_ = l; }
    uint32_t num_entries() const { return num_entries_; }
    uint64_t file_size() const { return file_size_; }
    const std::string& min_key() const { return min_key_; }
    const std::string& max_key() const { return max_key_; }
    size_t sparse_index_size() const { return sparse_index_.size(); }
    size_t bloom_filter_bytes() const { return bloom_.size_in_bytes(); }

private:
    SSTable() : table_id_(0), level_(0), num_entries_(0), file_size_(0) {}

    static std::string DirName(const std::string& path) {
        size_t pos = path.find_last_of("/\\");
        return pos == std::string::npos ? std::string(".") : path.substr(0, pos);
    }

    static void WriteString(std::ofstream& out, const std::string& s) {
        uint32_t len = static_cast<uint32_t>(s.size());
        out.write(reinterpret_cast<const char*>(&len), sizeof(len));
        if (len > 0) out.write(s.data(), len);
    }

    static bool ReadString(std::ifstream& in, std::string* out_s) {
        uint32_t len = 0;
        if (!in.read(reinterpret_cast<char*>(&len), sizeof(len))) return false;
        if (len > (64u << 20)) return false;
        out_s->assign(len, '\0');
        if (len > 0 && !in.read(&(*out_s)[0], len)) return false;
        return true;
    }

    static bool ReadRecord(std::ifstream& in, KeyValue* out_kv) {
        uint8_t type_byte = 0;
        uint32_t key_len = 0;
        uint32_t val_len = 0;
        if (!in.read(reinterpret_cast<char*>(&type_byte), sizeof(type_byte))) return false;
        if (!in.read(reinterpret_cast<char*>(&key_len), sizeof(key_len))) return false;
        if (!in.read(reinterpret_cast<char*>(&val_len), sizeof(val_len))) return false;
        if (key_len > (64u << 20) || val_len > (64u << 20)) return false;

        out_kv->key.assign(key_len, '\0');
        if (key_len > 0 && !in.read(&out_kv->key[0], key_len)) return false;

        out_kv->entry.value.assign(val_len, '\0');
        if (val_len > 0 && !in.read(&out_kv->entry.value[0], val_len)) return false;

        out_kv->entry.type = (type_byte == static_cast<uint8_t>(ValueType::kTombstone))
                                 ? ValueType::kTombstone
                                 : ValueType::kValue;
        return true;
    }

    std::string filepath_;
    uint64_t table_id_;
    int level_;
    uint32_t num_entries_;
    uint64_t file_size_;
    std::string min_key_;
    std::string max_key_;
    std::vector<SparseIndexEntry> sparse_index_;
    BloomFilter bloom_;
};

} // namespace lsm

#endif // LSM_SSTABLE_H
