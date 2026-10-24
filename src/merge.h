#ifndef LSM_MERGE_H
#define LSM_MERGE_H

#include "sstable.h"
#include "types.h"

#include <memory>
#include <queue>
#include <string>
#include <utility>
#include <vector>

namespace lsm {

// Abstract sorted cursor over (key, ValueEntry) records.
class Cursor {
public:
    virtual ~Cursor() = default;
    virtual bool Valid() const = 0;
    virtual const std::string& key() const = 0;
    virtual const ValueEntry& entry() const = 0;
    virtual void Next() = 0;
};

// Cursor over an already-sorted in-memory vector (used for the MemTable snapshot).
class VectorCursor : public Cursor {
public:
    explicit VectorCursor(std::vector<KeyValue> entries)
        : entries_(std::move(entries)), pos_(0) {}
    bool Valid() const override { return pos_ < entries_.size(); }
    const std::string& key() const override { return entries_[pos_].key; }
    const ValueEntry& entry() const override { return entries_[pos_].entry; }
    void Next() override { ++pos_; }

private:
    std::vector<KeyValue> entries_;
    size_t pos_;
};

// Adapter from SSTable::Iterator to Cursor.
class SSTableCursor : public Cursor {
public:
    explicit SSTableCursor(std::unique_ptr<SSTable::Iterator> it) : it_(std::move(it)) {}
    bool Valid() const override { return it_->Valid(); }
    const std::string& key() const override { return it_->key(); }
    const ValueEntry& entry() const override { return it_->entry(); }
    void Next() override { it_->Next(); }

private:
    std::unique_ptr<SSTable::Iterator> it_;
};

// K-way merge of sorted cursors using a min-heap.
//
// Cursors must be supplied NEWEST FIRST. When several cursors hold the same key,
// only the newest version is surfaced (it may be a tombstone — callers decide
// whether to skip it) and the older duplicates are skipped.
//
// Cost per output record: O(log K) heap work, with only one record per source
// held in memory — versus the old approach of loading every overlapping table
// into a std::map.
class MergingIterator {
public:
    explicit MergingIterator(std::vector<std::unique_ptr<Cursor>> newest_first)
        : cursors_(std::move(newest_first)), valid_(false) {
        for (size_t i = 0; i < cursors_.size(); ++i) {
            if (cursors_[i]->Valid()) heap_.push(HeapItem{cursors_[i]->key(), i});
        }
        Advance();
    }

    bool Valid() const { return valid_; }
    const std::string& key() const { return current_.key; }
    const ValueEntry& entry() const { return current_.entry; }
    void Next() { Advance(); }

private:
    struct HeapItem {
        std::string key;
        size_t rank; // lower = newer
    };
    struct Greater {
        bool operator()(const HeapItem& a, const HeapItem& b) const {
            if (a.key != b.key) return a.key > b.key;
            return a.rank > b.rank;
        }
    };

    void Advance() {
        if (heap_.empty()) {
            valid_ = false;
            return;
        }
        // Top of heap = smallest key, newest source for that key.
        HeapItem top = heap_.top();
        heap_.pop();
        Cursor* winner = cursors_[top.rank].get();
        current_.key = top.key;
        current_.entry = winner->entry();
        valid_ = true;
        Refill(top.rank);

        // Drop older versions of the same key from other sources.
        while (!heap_.empty() && heap_.top().key == current_.key) {
            size_t rank = heap_.top().rank;
            heap_.pop();
            Refill(rank);
        }
    }

    void Refill(size_t rank) {
        Cursor* c = cursors_[rank].get();
        c->Next();
        if (c->Valid()) heap_.push(HeapItem{c->key(), rank});
    }

    std::vector<std::unique_ptr<Cursor>> cursors_;
    std::priority_queue<HeapItem, std::vector<HeapItem>, Greater> heap_;
    KeyValue current_;
    bool valid_;
};

} // namespace lsm

#endif // LSM_MERGE_H
