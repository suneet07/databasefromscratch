#ifndef LSM_SKIPLIST_H
#define LSM_SKIPLIST_H

#include "types.h"
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

namespace lsm {

// A probabilistic multi-level linked list providing O(log N) search and insertion
// without the complex rebalancing rotations of red-black or AVL trees.
// Used by LevelDB and RocksDB as the core backing structure for the MemTable.
class SkipList {
public:
    static constexpr int kMaxHeight = 12;

    struct Node {
        std::string key;
        ValueEntry entry;
        std::vector<Node*> next;

        Node(const std::string& k, const ValueEntry& e, int height)
            : key(k), entry(e), next(height, nullptr) {}
    };

    class Iterator {
    public:
        explicit Iterator(const Node* node) : node_(node) {}

        bool valid() const { return node_ != nullptr; }
        void next() {
            if (node_) node_ = node_->next[0];
        }
        const std::string& key() const { return node_->key; }
        const ValueEntry& entry() const { return node_->entry; }

    private:
        const Node* node_;
    };

    SkipList()
        : head_(new Node("", ValueEntry::MakeTombstone(), kMaxHeight)),
          current_height_(1),
          num_entries_(0),
          bytes_used_(0),
          rng_state_(0xDEADBEEFCAFEBABEULL) {}

    ~SkipList() {
        Clear();
        delete head_;
    }

    SkipList(const SkipList&) = delete;
    SkipList& operator=(const SkipList&) = delete;

    // Insert or update a key with a ValueEntry (either a live value or a tombstone).
    void Insert(const std::string& key, const ValueEntry& entry) {
        Node* update[kMaxHeight];
        Node* current = head_;

        for (int level = current_height_ - 1; level >= 0; --level) {
            while (current->next[level] != nullptr && current->next[level]->key < key) {
                current = current->next[level];
            }
            update[level] = current;
        }

        current = current->next[0];

        // If key already exists in the MemTable, overwrite its entry in-place
        if (current != nullptr && current->key == key) {
            bytes_used_ -= current->entry.value.size();
            bytes_used_ += entry.value.size();
            current->entry = entry;
            return;
        }

        int height = RandomHeight();
        if (height > current_height_) {
            for (int level = current_height_; level < height; ++level) {
                update[level] = head_;
            }
            current_height_ = height;
        }

        Node* new_node = new Node(key, entry, height);
        for (int level = 0; level < height; ++level) {
            new_node->next[level] = update[level]->next[level];
            update[level]->next[level] = new_node;
        }

        num_entries_++;
        // Track approximate memory footprint: key + value + node pointer overhead
        bytes_used_ += key.size() + entry.value.size() + sizeof(Node) + height * sizeof(Node*);
    }

    // Lookup a key in O(log N) time. Returns true if found (even if it's a tombstone!).
    bool Search(const std::string& key, ValueEntry* out_entry) const {
        const Node* current = head_;
        for (int level = current_height_ - 1; level >= 0; --level) {
            while (current->next[level] != nullptr && current->next[level]->key < key) {
                current = current->next[level];
            }
        }
        current = current->next[0];
        if (current != nullptr && current->key == key) {
            if (out_entry) {
                *out_entry = current->entry;
            }
            return true;
        }
        return false;
    }

    // Return an iterator positioned at the first key >= target
    Iterator Seek(const std::string& target) const {
        const Node* current = head_;
        for (int level = current_height_ - 1; level >= 0; --level) {
            while (current->next[level] != nullptr && current->next[level]->key < target) {
                current = current->next[level];
            }
        }
        return Iterator(current->next[0]);
    }

    Iterator Begin() const {
        return Iterator(head_->next[0]);
    }

    void Clear() {
        Node* current = head_->next[0];
        while (current != nullptr) {
            Node* next = current->next[0];
            delete current;
            current = next;
        }
        for (int i = 0; i < kMaxHeight; ++i) {
            head_->next[i] = nullptr;
        }
        current_height_ = 1;
        num_entries_ = 0;
        bytes_used_ = 0;
    }

    size_t num_entries() const { return num_entries_; }
    size_t bytes_used() const { return bytes_used_; }
    int current_height() const { return current_height_; }

private:
    // Fast xorshift64* PRNG for coin flips (p = 1/4 per level like LevelDB)
    uint64_t NextRandom() {
        rng_state_ ^= rng_state_ >> 12;
        rng_state_ ^= rng_state_ << 25;
        rng_state_ ^= rng_state_ >> 27;
        return rng_state_ * 0x2545F4914F6CDD1DULL;
    }

    int RandomHeight() {
        int height = 1;
        // Branching probability 1/4: keeps node pointer overhead low while maintaining O(log N)
        while (height < kMaxHeight && (NextRandom() & 3) == 0) {
            height++;
        }
        return height;
    }

    Node* head_;
    int current_height_;
    size_t num_entries_;
    size_t bytes_used_;
    uint64_t rng_state_;
};

} // namespace lsm

#endif // LSM_SKIPLIST_H
