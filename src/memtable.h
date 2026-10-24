#ifndef LSM_MEMTABLE_H
#define LSM_MEMTABLE_H

#include "skiplist.h"
#include "types.h"
#include <string>
#include <vector>

namespace lsm {

// In-memory write buffer backed by a SkipList.
// All recent writes (puts and deletes) land here after being logged to the WAL.
// Because SkipList keeps keys sorted, flushing the MemTable to an SSTable is a fast
// linear O(N) sequential write.
class MemTable {
public:
    MemTable() = default;

    void Put(const std::string& key, const std::string& value) {
        skiplist_.Insert(key, ValueEntry::MakeValue(value));
    }

    void Delete(const std::string& key) {
        skiplist_.Insert(key, ValueEntry::MakeTombstone());
    }

    void Apply(const std::string& key, const ValueEntry& entry) {
        skiplist_.Insert(key, entry);
    }

    // Returns true if the key exists in the MemTable (check out_entry->is_tombstone() to see if deleted)
    bool Get(const std::string& key, ValueEntry* out_entry) const {
        return skiplist_.Search(key, out_entry);
    }

    // Export all entries in sorted key order (used when flushing to an SSTable on disk)
    std::vector<KeyValue> ExportSorted() const {
        std::vector<KeyValue> result;
        result.reserve(skiplist_.num_entries());
        for (SkipList::Iterator it = skiplist_.Begin(); it.valid(); it.next()) {
            result.push_back(KeyValue{it.key(), it.entry()});
        }
        return result;
    }

    // Export entries with start_key <= key <= end_key ("" = unbounded), via SkipList::Seek
    std::vector<KeyValue> ExportRange(const std::string& start_key, const std::string& end_key) const {
        std::vector<KeyValue> result;
        for (SkipList::Iterator it = skiplist_.Seek(start_key); it.valid(); it.next()) {
            if (!end_key.empty() && it.key() > end_key) break;
            result.push_back(KeyValue{it.key(), it.entry()});
        }
        return result;
    }

    void Clear() {
        skiplist_.Clear();
    }

    size_t num_entries() const { return skiplist_.num_entries(); }
    size_t approximate_memory_usage() const { return skiplist_.bytes_used(); }
    int skiplist_height() const { return skiplist_.current_height(); }

private:
    SkipList skiplist_;
};

} // namespace lsm

#endif // LSM_MEMTABLE_H
