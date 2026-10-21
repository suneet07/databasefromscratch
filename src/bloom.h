#ifndef LSM_BLOOM_H
#define LSM_BLOOM_H

#include <cstdint>
#include <string>
#include <vector>

namespace lsm {

// A space-efficient probabilistic bit array embedded in each SSTable.
// Guarantees ZERO false negatives: if MayContain(key) returns false, we can skip
// reading the SSTable from disk entirely!
class BloomFilter {
public:
    explicit BloomFilter(size_t expected_keys = 1024, size_t bits_per_key = 10) {
        // Optimal number of hash functions k = ln(2) * bits_per_key (~0.69 * bits_per_key)
        num_hashes_ = static_cast<uint8_t>(bits_per_key * 0.69);
        if (num_hashes_ < 1) num_hashes_ = 1;
        if (num_hashes_ > 16) num_hashes_ = 16;

        size_t num_bits = expected_keys * bits_per_key;
        if (num_bits < 64) num_bits = 64;
        size_t num_bytes = (num_bits + 7) / 8;
        bits_.assign(num_bytes, 0);
    }

    // Reconstruct a BloomFilter from serialized bytes stored in an SSTable
    static BloomFilter Deserialize(const std::string& raw) {
        BloomFilter bf(1, 10);
        if (raw.empty()) {
            return bf;
        }
        bf.num_hashes_ = static_cast<uint8_t>(raw[0]);
        bf.bits_.assign(raw.begin() + 1, raw.end());
        if (bf.bits_.empty()) {
            bf.bits_.assign(8, 0);
        }
        return bf;
    }

    // Serialize the filter to store inside an SSTable block
    std::string Serialize() const {
        std::string out;
        out.reserve(1 + bits_.size());
        out.push_back(static_cast<char>(num_hashes_));
        out.append(reinterpret_cast<const char*>(bits_.data()), bits_.size());
        return out;
    }

    void Add(const std::string& key) {
        uint64_t h = Hash64(key);
        uint32_t h1 = static_cast<uint32_t>(h);
        uint32_t h2 = static_cast<uint32_t>((h >> 32) | 1); // Ensure odd step

        size_t total_bits = bits_.size() * 8;
        for (uint8_t i = 0; i < num_hashes_; ++i) {
            uint32_t combined = h1 + i * h2;
            size_t bit_pos = combined % total_bits;
            bits_[bit_pos / 8] |= static_cast<uint8_t>(1u << (bit_pos % 8));
        }
    }

    bool MayContain(const std::string& key) const {
        if (bits_.empty()) return false;
        uint64_t h = Hash64(key);
        uint32_t h1 = static_cast<uint32_t>(h);
        uint32_t h2 = static_cast<uint32_t>((h >> 32) | 1);

        size_t total_bits = bits_.size() * 8;
        for (uint8_t i = 0; i < num_hashes_; ++i) {
            uint32_t combined = h1 + i * h2;
            size_t bit_pos = combined % total_bits;
            if ((bits_[bit_pos / 8] & static_cast<uint8_t>(1u << (bit_pos % 8))) == 0) {
                return false; // Definitely not in the set!
            }
        }
        return true; // Probably in the set
    }

    size_t size_in_bytes() const { return 1 + bits_.size(); }
    uint8_t num_hashes() const { return num_hashes_; }

private:
    // FNV-1a + Murmur3 finalizer 64-bit mixing hash
    static uint64_t Hash64(const std::string& str) {
        uint64_t hash = 14695981039346656037ULL;
        for (unsigned char c : str) {
            hash ^= c;
            hash *= 1099511628211ULL;
        }
        // MurmurHash3 64-bit finalizer avalanche
        hash ^= hash >> 33;
        hash *= 0xff51afd7ed558ccdULL;
        hash ^= hash >> 33;
        hash *= 0xc4ceb9fe1a85ec53ULL;
        hash ^= hash >> 33;
        return hash;
    }

    uint8_t num_hashes_;
    std::vector<uint8_t> bits_;
};

} // namespace lsm

#endif // LSM_BLOOM_H
