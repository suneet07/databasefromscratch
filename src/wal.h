#ifndef LSM_WAL_H
#define LSM_WAL_H

#include "types.h"
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

namespace lsm {

// Write-Ahead Log (WAL) ensures durability (the 'D' in ACID).
// Before any write enters the in-memory MemTable, it is appended sequentially to disk
// with a CRC32 checksum. If the process crashes, the WAL is replayed on startup.
//
// Binary Record Layout:
// +------------+-----------+------------+------------+---------+-----------+
// | CRC32 (4B) | Type (1B) | KeyLen(4B) | ValLen(4B) | Key ... | Value ... |
// +------------+-----------+------------+------------+---------+-----------+
class WriteAheadLog {
public:
    explicit WriteAheadLog(const std::string& path) : path_(path), bytes_written_(0) {
        OpenForAppend();
    }

    ~WriteAheadLog() {
        Close();
    }

    WriteAheadLog(const WriteAheadLog&) = delete;
    WriteAheadLog& operator=(const WriteAheadLog&) = delete;

    // Append a key-value or tombstone operation to the log and flush to OS buffer
    bool Append(const std::string& key, const ValueEntry& entry) {
        if (!out_.is_open()) {
            OpenForAppend();
            if (!out_.is_open()) return false;
        }

        uint8_t type_byte = static_cast<uint8_t>(entry.type);
        uint32_t key_len = static_cast<uint32_t>(key.size());
        uint32_t val_len = static_cast<uint32_t>(entry.value.size());

        // Compute CRC32 over header fields + key + value
        uint32_t crc = ComputeCRC32(&type_byte, sizeof(type_byte), 0);
        crc = ComputeCRC32(&key_len, sizeof(key_len), crc);
        crc = ComputeCRC32(&val_len, sizeof(val_len), crc);
        if (key_len > 0) {
            crc = ComputeCRC32(key.data(), key_len, crc);
        }
        if (val_len > 0) {
            crc = ComputeCRC32(entry.value.data(), val_len, crc);
        }

        out_.write(reinterpret_cast<const char*>(&crc), sizeof(crc));
        out_.write(reinterpret_cast<const char*>(&type_byte), sizeof(type_byte));
        out_.write(reinterpret_cast<const char*>(&key_len), sizeof(key_len));
        out_.write(reinterpret_cast<const char*>(&val_len), sizeof(val_len));
        if (key_len > 0) {
            out_.write(key.data(), key_len);
        }
        if (val_len > 0) {
            out_.write(entry.value.data(), val_len);
        }
        out_.flush();

        if (!out_.good()) {
            return false;
        }

        bytes_written_ += sizeof(crc) + sizeof(type_byte) + sizeof(key_len) + sizeof(val_len) + key_len + val_len;
        return true;
    }

    // Replay existing WAL file on startup, verifying CRC32 checksums on every record.
    // Returns number of valid records recovered (and populates out_corrupted if a bad tail is detected).
    static size_t Replay(const std::string& path, std::vector<KeyValue>* out_records, size_t* out_corrupted = nullptr) {
        if (out_corrupted) *out_corrupted = 0;
        std::ifstream in(path, std::ios::binary);
        if (!in.is_open()) {
            return 0;
        }

        size_t recovered = 0;
        while (in.peek() != EOF) {
            uint32_t stored_crc = 0;
            uint8_t type_byte = 0;
            uint32_t key_len = 0;
            uint32_t val_len = 0;

            if (!in.read(reinterpret_cast<char*>(&stored_crc), sizeof(stored_crc))) break;
            if (!in.read(reinterpret_cast<char*>(&type_byte), sizeof(type_byte))) {
                if (out_corrupted) (*out_corrupted)++;
                break;
            }
            if (!in.read(reinterpret_cast<char*>(&key_len), sizeof(key_len))) {
                if (out_corrupted) (*out_corrupted)++;
                break;
            }
            if (!in.read(reinterpret_cast<char*>(&val_len), sizeof(val_len))) {
                if (out_corrupted) (*out_corrupted)++;
                break;
            }

            // Sanity cap against corrupted huge lengths (e.g. > 64 MB)
            if (key_len > (64u << 20) || val_len > (64u << 20)) {
                if (out_corrupted) (*out_corrupted)++;
                break;
            }

            std::string key(key_len, '\0');
            if (key_len > 0 && !in.read(&key[0], key_len)) {
                if (out_corrupted) (*out_corrupted)++;
                break;
            }

            std::string value(val_len, '\0');
            if (val_len > 0 && !in.read(&value[0], val_len)) {
                if (out_corrupted) (*out_corrupted)++;
                break;
            }

            // Verify CRC32 checksum
            uint32_t computed_crc = ComputeCRC32(&type_byte, sizeof(type_byte), 0);
            computed_crc = ComputeCRC32(&key_len, sizeof(key_len), computed_crc);
            computed_crc = ComputeCRC32(&val_len, sizeof(val_len), computed_crc);
            if (key_len > 0) computed_crc = ComputeCRC32(key.data(), key_len, computed_crc);
            if (val_len > 0) computed_crc = ComputeCRC32(value.data(), val_len, computed_crc);

            if (computed_crc != stored_crc) {
                if (out_corrupted) (*out_corrupted)++;
                break;
            }

            ValueType vtype = (type_byte == static_cast<uint8_t>(ValueType::kTombstone))
                                  ? ValueType::kTombstone
                                  : ValueType::kValue;
            out_records->push_back(KeyValue{key, ValueEntry{vtype, value}});
            recovered++;
        }
        return recovered;
    }

    // Truncate and reset the WAL after the MemTable has been safely flushed to an SSTable
    void Reset() {
        Close();
        std::ofstream trunc_stream(path_, std::ios::binary | std::ios::trunc);
        trunc_stream.close();
        bytes_written_ = 0;
        OpenForAppend();
    }

    void Close() {
        if (out_.is_open()) {
            out_.flush();
            out_.close();
        }
    }

    size_t bytes_written() const { return bytes_written_; }
    const std::string& path() const { return path_; }

private:
    void OpenForAppend() {
        out_.open(path_, std::ios::binary | std::ios::app);
        if (out_.is_open()) {
            out_.seekp(0, std::ios::end);
            std::streampos pos = out_.tellp();
            if (pos > 0) {
                bytes_written_ = static_cast<size_t>(pos);
            }
        }
    }

    std::string path_;
    std::ofstream out_;
    size_t bytes_written_;
};

} // namespace lsm

#endif // LSM_WAL_H
