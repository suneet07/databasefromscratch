#ifndef LSM_TYPES_H
#define LSM_TYPES_H

#include <cstdint>
#include <string>

namespace lsm {

// Record type: kValue represents an active value, kTombstone represents a deleted key.
// In an LSM-Tree, deletes cannot overwrite old SSTables in-place because SSTables are immutable.
// Instead, a delete writes a "tombstone" marker that shadows older values until compaction purges both.
enum class ValueType : uint8_t {
    kValue = 0x01,
    kTombstone = 0x02
};

struct ValueEntry {
    ValueType type;
    std::string value;

    static ValueEntry MakeValue(const std::string& v) {
        return ValueEntry{ValueType::kValue, v};
    }

    static ValueEntry MakeTombstone() {
        return ValueEntry{ValueType::kTombstone, ""};
    }

    bool is_tombstone() const {
        return type == ValueType::kTombstone;
    }
};

struct KeyValue {
    std::string key;
    ValueEntry entry;
};

// Standard IEEE 802.3 CRC32 checksum for data integrity in WAL and SSTables
inline uint32_t ComputeCRC32(const void* data, size_t length, uint32_t previous_crc = 0) {
    const uint8_t* bytes = static_cast<const uint8_t*>(data);
    uint32_t crc = ~previous_crc;
    for (size_t i = 0; i < length; ++i) {
        crc ^= bytes[i];
        for (int bit = 0; bit < 8; ++bit) {
            if (crc & 1) {
                crc = (crc >> 1) ^ 0xEDB88320u;
            } else {
                crc >>= 1;
            }
        }
    }
    return ~crc;
}

} // namespace lsm

#endif // LSM_TYPES_H
