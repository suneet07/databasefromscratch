# LSM-Tree Key-Value Database from Scratch (C++)

A complete, zero-dependency **Log-Structured Merge-Tree (LSM-Tree)** storage engine written in modern C++14, inspired by **LevelDB**, **RocksDB**, and **Bitcask**.

## Architecture Overview

1. **Write-Ahead Log ([`src/wal.h`](src/wal.h))**
   - Sequential append-only binary log (`wal.log`) with IEEE 802.3 **CRC32 checksums** on every record.
   - Guarantees durability across crashes by replaying un-flushed records at startup and detecting truncated/corrupted tail writes.
2. **SkipList & MemTable ([`src/skiplist.h`](src/skiplist.h), [`src/memtable.h`](src/memtable.h))**
   - Probabilistic multi-level linked list (`kMaxHeight = 12`, branching factor $p = 1/4$) providing $O(\log N)$ insertions, updates, and lookups without tree rotations.
   - Deletes insert a `ValueType::kTombstone` marker so older SSTables on disk never need in-place mutation.
3. **Bloom Filter ([`src/bloom.h`](src/bloom.h))**
   - Uses Kirsch-Mitzenmacher double hashing (`FNV-1a` + `Murmur3` 64-bit avalanche finalizer) at 10 bits/key (~1% false positive rate, **0% false negatives**).
   - Embedded inside every SSTable so point lookups (`GET`) skip disk seeks when a key is not present.
4. **SSTable — Sorted String Table ([`src/sstable.h`](src/sstable.h))**
   - Immutable on-disk file (`.sst`) with 4 sections:
     1. **Data Section**: Sorted records `[Type(1B)][KeyLen(4B)][ValLen(4B)][Key][Value]`
     2. **Sparse Index Block**: `min_key`, `max_key`, and every 16th key's file offset (`kSparseIndexStride = 16`)
     3. **Bloom Filter Block**: Serialized bit array
     4. **Fixed 32-Byte Footer**: Offsets, sizes, entry count, and magic number `0x4C534D31` (`"LSM1"`)
5. **LSM Coordinator & Compaction ([`src/lsm.h`](src/lsm.h))**
   - Coordinates `Put`, `Get`, `Delete`, `Scan`, `Flush`, and multi-table `Compact` (which merges SSTables, deduplicates overwritten keys, and permanently garbage-collects Tombstones).

## Building & Running

```powershell
# Compile the interactive CLI / demo / benchmark binary
g++ -std=c++14 -O2 -Wall -Wextra src/main.cpp -o lsm_db.exe

# Compile and run the automated test suite
g++ -std=c++14 -O2 -Wall -Wextra tests/test_lsm.cpp -o test_lsm.exe
.\test_lsm.exe

# Run the guided walkthrough demo
.\lsm_db.exe --dir .\demo_data --demo

# Run the performance & Bloom filter benchmark (e.g. 5000 keys)
.\lsm_db.exe --dir .\bench_data --bench 5000

# Launch the interactive REPL
.\lsm_db.exe
```
