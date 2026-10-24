# LSM-Tree Key-Value Database from Scratch (C++)

A complete, zero-dependency **Log-Structured Merge-Tree (LSM-Tree)** storage engine written in modern C++14, inspired by **LevelDB**, **RocksDB**, and **Bitcask**.

## Architecture Overview

1. **Write-Ahead Log ([`src/wal.h`](src/wal.h))**
   - Sequential append-only binary log (`wal.log`) with IEEE 802.3 **CRC32 checksums** on every record.
   - Replays un-flushed records at startup and detects truncated/corrupted tail writes.
   - `fdatasync` after every write by default (`LSMConfig::sync_wal`), so an acknowledged write survives power loss, not just a process crash. Pass `--no-sync` to the CLI to trade that for ~10x write throughput.
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
   - **Streaming range scans & compaction** ([`src/merge.h`](src/merge.h)): a min-heap k-way merge over the MemTable and per-SSTable iterators. Each iterator seeks via the sparse index, so a scan reads only the blocks in range and holds one record per source in memory; the newest version of each key wins. `Scan` also takes an optional `limit`.
6. **MANIFEST & crash-safe files ([`src/io.h`](src/io.h))**
   - SSTables are written to `*.tmp`, fsync'd, then atomically renamed into place.
   - The set of live tables is recorded in a `MANIFEST` file (same tmp → fsync → rename protocol). On open, any table not listed (a half-finished flush, or an input of an interrupted compaction) is deleted. Without this, a crash between "write compacted table" and "delete old tables" could **resurrect keys whose tombstones were already purged**, which a regression test covers.
7. **Concurrency**
   - All public methods are thread-safe. Writes take an exclusive lock; `Get` and `GetStats` share a reader lock; `Scan` holds the lock only to snapshot state, then merges unlocked (open file handles keep compacted-away tables readable on POSIX).

## Building & Testing

Requires a C++14 compiler and CMake ≥ 3.14. Linux and macOS are tested in CI; Windows works with MinGW (MSVC lacks `dirent.h`).

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure

# Sanitizer builds (GCC/Clang)
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DLSM_SANITIZE=address   # ASan + UBSan
cmake -S . -B build-tsan -DCMAKE_BUILD_TYPE=Debug -DLSM_SANITIZE=thread    # TSan
```

Without CMake: `g++ -std=c++14 -O2 -pthread tests/test_lsm.cpp -o test_lsm && ./test_lsm`

The suite covers the SkipList, Bloom filter, WAL corruption handling, the SSTable format and iterator seeks, tombstone shadowing, crash recovery, MANIFEST crash safety, a 6,000-op randomized test against a `std::map` reference (with flushes, compactions and reopens), and concurrent readers and writers. CI runs it on Linux (C++14/17, ASan+UBSan, TSan) and macOS.

## Running

```bash
./build/lsm_db --dir ./demo_data --demo                     # guided walkthrough
./build/lsm_db --dir ./bench_data --bench 5000              # performance & Bloom filter benchmark
./build/lsm_db --dir ./bench_data --no-sync --bench 5000    # same, without per-write fsync
./build/lsm_db                                              # interactive REPL
```
