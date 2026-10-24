#include "../src/lsm.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace lsm;

// Unlike assert(), CHECK stays active in Release builds (-DNDEBUG), so the
// suite can't silently pass with every check compiled out.
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::cerr << "\nCHECK FAILED: " #cond "\n  at " << __FILE__ << ":"   \
                      << __LINE__ << "\n";                                       \
            std::exit(1);                                                        \
        }                                                                        \
    } while (0)

namespace {

const int kNumTests = 10;
int g_test_no = 0;

void Begin(const char* name) {
    std::cout << "[TEST " << ++g_test_no << "/" << kNumTests << "] " << name << "... " << std::flush;
}

void MakeDir(const std::string& dir) {
#ifdef _WIN32
    _mkdir(dir.c_str());
#else
    mkdir(dir.c_str(), 0755);
#endif
}

std::vector<std::string> ListDir(const std::string& dir) {
    std::vector<std::string> names;
    DIR* d = opendir(dir.c_str());
    if (!d) return names;
    struct dirent* entry;
    while ((entry = readdir(d)) != nullptr) {
        std::string name = entry->d_name;
        if (name != "." && name != "..") names.push_back(name);
    }
    closedir(d);
    return names;
}

// Returns a fresh, empty directory for one test.
std::string FreshDir(const std::string& name) {
    std::string dir = "./test_lsm_tmp_" + name;
    MakeDir(dir);
    for (const auto& f : ListDir(dir)) std::remove((dir + "/" + f).c_str());
    return dir;
}

void CleanDir(const std::string& dir) {
    for (const auto& f : ListDir(dir)) std::remove((dir + "/" + f).c_str());
    std::remove(dir.c_str());
}

bool FileExists(const std::string& path) {
    std::ifstream f(path);
    return f.good();
}

void CopyFile(const std::string& from, const std::string& to) {
    std::ifstream in(from, std::ios::binary);
    std::ofstream out(to, std::ios::binary | std::ios::trunc);
    out << in.rdbuf();
}

std::string Key(int i) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "key_%05d", i);
    return buf;
}

void TestSkipList() {
    Begin("SkipList sorted insert, update, and search");
    SkipList sl;
    sl.Insert("charlie", ValueEntry::MakeValue("3"));
    sl.Insert("alice", ValueEntry::MakeValue("1"));
    sl.Insert("bob", ValueEntry::MakeValue("2"));
    sl.Insert("alice", ValueEntry::MakeValue("1_updated"));

    CHECK(sl.num_entries() == 3);

    ValueEntry entry;
    CHECK(sl.Search("alice", &entry) && entry.value == "1_updated");
    CHECK(sl.Search("bob", &entry) && entry.value == "2");
    CHECK(sl.Search("charlie", &entry) && entry.value == "3");
    CHECK(!sl.Search("david", &entry));

    std::vector<std::string> keys;
    for (auto it = sl.Begin(); it.valid(); it.next()) {
        keys.push_back(it.key());
    }
    CHECK(keys.size() == 3);
    CHECK(keys[0] == "alice" && keys[1] == "bob" && keys[2] == "charlie");
    std::cout << "PASSED\n";
}

void TestBloomFilter() {
    Begin("BloomFilter zero false-negatives & serialization");
    BloomFilter bf(1000, 10);
    for (int i = 0; i < 1000; ++i) {
        bf.Add("key_" + std::to_string(i));
    }

    std::string raw = bf.Serialize();
    BloomFilter loaded = BloomFilter::Deserialize(raw);

    for (int i = 0; i < 1000; ++i) {
        CHECK(loaded.MayContain("key_" + std::to_string(i)));
    }

    // Expected ~1% false positives, must be < 3%
    int false_positives = 0;
    for (int i = 1000; i < 11000; ++i) {
        if (loaded.MayContain("key_" + std::to_string(i))) {
            false_positives++;
        }
    }
    double fp_rate = false_positives / 10000.0;
    CHECK(fp_rate < 0.03);
    std::cout << "PASSED (FP rate = " << (fp_rate * 100.0) << "%)\n";
}

void TestWALAndCRC32() {
    Begin("WriteAheadLog replay, fsync & CRC32 corruption detection");
    std::string dir = FreshDir("wal");
    std::string wal_file = dir + "/wal.log";
    {
        WriteAheadLog wal(wal_file);
        CHECK(wal.Append("k1", ValueEntry::MakeValue("v1"), /*sync=*/true));
        CHECK(wal.Append("k2", ValueEntry::MakeTombstone()));
        CHECK(wal.Append("k3", ValueEntry::MakeValue("v3")));
        CHECK(wal.Sync());
        wal.Close();
    }

    // Append a bad-CRC record to simulate a torn write at power loss
    {
        std::ofstream corrupt(wal_file, std::ios::binary | std::ios::app);
        uint32_t bad_crc = 0x12345678;
        uint8_t type = 1;
        uint32_t klen = 4, vlen = 4;
        corrupt.write(reinterpret_cast<const char*>(&bad_crc), 4);
        corrupt.write(reinterpret_cast<const char*>(&type), 1);
        corrupt.write(reinterpret_cast<const char*>(&klen), 4);
        corrupt.write(reinterpret_cast<const char*>(&vlen), 4);
        corrupt.write("badkbadv", 8);
    }

    std::vector<KeyValue> records;
    size_t corrupted = 0;
    size_t recovered = WriteAheadLog::Replay(wal_file, &records, &corrupted);
    CHECK(recovered == 3);
    CHECK(corrupted == 1);
    CHECK(records[0].key == "k1" && records[0].entry.value == "v1");
    CHECK(records[1].key == "k2" && records[1].entry.is_tombstone());
    CHECK(records[2].key == "k3" && records[2].entry.value == "v3");

    CleanDir(dir);
    std::cout << "PASSED\n";
}

void TestSSTableBinaryFormat() {
    Begin("SSTable creation, atomic write, sparse index & reload");
    std::string dir = FreshDir("sst");

    std::vector<KeyValue> sorted;
    for (int i = 0; i < 100; ++i) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "item_%04d", i);
        sorted.push_back(KeyValue{buf, ValueEntry::MakeValue("val_" + std::to_string(i))});
    }

    std::string sst_path = dir + "/sst_L0_000001.sst";
    auto created = SSTable::Create(sst_path, 1, 0, sorted);
    CHECK(created != nullptr);
    CHECK(created->num_entries() == 100);
    // 100 entries with stride 16 -> ceil(100 / 16.0) = 7 sparse index entries
    CHECK(created->sparse_index_size() == 7);
    CHECK(!FileExists(sst_path + ".tmp")); // temp file renamed into place

    auto reopened = SSTable::Open(sst_path, 1, 0);
    CHECK(reopened != nullptr);
    CHECK(reopened->min_key() == "item_0000");
    CHECK(reopened->max_key() == "item_0099");

    for (int i = 0; i < 100; ++i) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "item_%04d", i);
        ValueEntry e;
        CHECK(reopened->Get(buf, &e));
        CHECK(e.value == "val_" + std::to_string(i));
    }

    size_t bloom_saves = 0;
    ValueEntry missing;
    CHECK(!reopened->Get("item_0050_nonexistent", &missing, &bloom_saves));

    CleanDir(dir);
    std::cout << "PASSED\n";
}

void TestSSTableIteratorSeek() {
    Begin("SSTable streaming iterator Seek across sparse-index blocks");
    std::string dir = FreshDir("iter");

    // Even keys only, so we can seek to gaps
    std::vector<KeyValue> sorted;
    for (int i = 0; i < 200; i += 2) {
        sorted.push_back(KeyValue{Key(i), ValueEntry::MakeValue(std::to_string(i))});
    }
    auto table = SSTable::Create(dir + "/sst_L0_000001.sst", 1, 0, sorted);
    CHECK(table != nullptr);

    auto it = SSTable::NewIterator(table);
    it->Seek("");
    int count = 0;
    for (; it->Valid(); it->Next()) count++;
    CHECK(count == 100);

    for (int target = -1; target <= 201; ++target) {
        std::string t = target < 0 ? std::string("a") : Key(target);
        it->Seek(t);
        int expect = target < 0 ? 0 : (target % 2 == 0 ? target : target + 1);
        if (expect >= 200) {
            CHECK(!it->Valid());
        } else {
            CHECK(it->Valid() && it->key() == Key(expect));
            // And the remaining records stream in order to the end
            int n = 0;
            std::string prev;
            for (; it->Valid(); it->Next(), ++n) {
                CHECK(prev.empty() || prev < it->key());
                prev = it->key();
            }
            CHECK(n == (200 - expect) / 2);
        }
    }
    it->Seek("zzz");
    CHECK(!it->Valid());

    CleanDir(dir);
    std::cout << "PASSED\n";
}

void TestTombstoneShadowingAndCompaction() {
    Begin("Multi-layer Tombstone shadowing & SSTable Compaction");
    std::string dir = FreshDir("compact");

    LSMDatabase db(dir);
    db.Put("a", "old_a");
    db.Put("b", "old_b");
    db.Put("c", "old_c");
    db.Flush(); // SSTable #1

    db.Put("a", "new_a");
    db.Delete("b");
    db.Put("d", "new_d");
    db.Flush(); // SSTable #2

    CHECK(db.GetStats().total_sstables == 2);

    std::string val;
    CHECK(db.Get("a", &val) && val == "new_a");
    CHECK(!db.Get("b", &val)); // Shadowed by tombstone in SSTable #2
    CHECK(db.Get("c", &val) && val == "old_c");
    CHECK(db.Get("d", &val) && val == "new_d");

    CHECK(db.Compact());
    CHECK(db.GetStats().total_sstables == 1);
    // 'b' and its tombstone are purged, leaving a, c, d
    CHECK(db.sstables().front()->num_entries() == 3);

    CHECK(db.Get("a", &val) && val == "new_a");
    CHECK(!db.Get("b", &val));
    CHECK(db.Get("c", &val) && val == "old_c");
    CHECK(db.Get("d", &val) && val == "new_d");

    CleanDir(dir);
    std::cout << "PASSED\n";
}

void TestCrashRecoveryAndRangeScan() {
    Begin("Crash recovery (WAL + SSTables) & Range Scan");
    std::string dir = FreshDir("recovery");

    {
        LSMDatabase db(dir);
        db.Put("k1", "v1_flushed");
        db.Put("k2", "v2_flushed");
        db.Flush();

        // Only in WAL + MemTable when the DB closes
        db.Put("k2", "v2_unflushed_update");
        db.Put("k3", "v3_unflushed");
        db.Delete("k1");
    }

    {
        LSMDatabase recovered_db(dir);
        CHECK(recovered_db.GetStats().wal_recovered_records == 3);
        CHECK(recovered_db.GetStats().total_sstables == 1);

        std::string val;
        CHECK(!recovered_db.Get("k1", &val));
        CHECK(recovered_db.Get("k2", &val) && val == "v2_unflushed_update");
        CHECK(recovered_db.Get("k3", &val) && val == "v3_unflushed");

        auto scan = recovered_db.Scan("k1", "k9");
        CHECK(scan.size() == 2);
        CHECK(scan[0].first == "k2" && scan[0].second == "v2_unflushed_update");
        CHECK(scan[1].first == "k3" && scan[1].second == "v3_unflushed");

        auto limited = recovered_db.Scan("", "", 1);
        CHECK(limited.size() == 1 && limited[0].first == "k2");
    }

    CleanDir(dir);
    std::cout << "PASSED\n";
}

void TestManifestCrashSafety() {
    Begin("MANIFEST: interrupted compaction can't resurrect deleted keys");
    std::string dir = FreshDir("manifest");
    std::string stash = FreshDir("manifest_stash");

    {
        LSMDatabase db(dir);
        db.Put("a", "1");
        db.Put("b", "doomed");
        db.Flush();     // table with b
        // Snapshot the table holding the live 'b'...
        for (const auto& f : ListDir(dir)) {
            if (f.compare(0, 4, "sst_") == 0) CopyFile(dir + "/" + f, stash + "/" + f);
        }
        db.Delete("b");
        db.Flush();     // table with b's tombstone
        CHECK(db.Compact()); // b and its tombstone are purged
    }

    // ...then simulate a crash midway through deleting the compaction inputs:
    // the tombstone table is gone but the table with the old 'b' survived.
    // Plus a torn half-written table.
    for (const auto& f : ListDir(stash)) CopyFile(stash + "/" + f, dir + "/" + f);
    { std::ofstream junk(dir + "/sst_L0_000099.sst.tmp"); junk << "partial"; }

    {
        LSMDatabase db(dir);
        std::string val;
        CHECK(db.Get("a", &val) && val == "1");
        CHECK(!db.Get("b", &val)); // Without a manifest this came back from the dead
        CHECK(db.GetStats().total_sstables == 1);
        CHECK(db.Scan().size() == 1);
    }
    // Orphans and temp files were garbage-collected on open
    int sst_files = 0;
    for (const auto& f : ListDir(dir)) {
        CHECK(f.find(".tmp") == std::string::npos);
        if (f.compare(0, 4, "sst_") == 0) sst_files++;
    }
    CHECK(sst_files == 1);

    // Legacy directories without a MANIFEST still open (and get upgraded)
    std::remove((dir + "/MANIFEST").c_str());
    {
        LSMDatabase db(dir);
        std::string val;
        CHECK(db.Get("a", &val) && val == "1");
    }
    CHECK(FileExists(dir + "/MANIFEST"));

    CleanDir(dir);
    CleanDir(stash);
    std::cout << "PASSED\n";
}

void TestRandomizedAgainstReference() {
    Begin("Randomized ops vs std::map reference (flush/compact/reopen/scan)");
    std::string dir = FreshDir("random");
    std::map<std::string, std::string> ref;
    std::mt19937 rng(12345);

    LSMConfig config;
    config.memtable_flush_threshold_bytes = 2 * 1024; // lots of flushes
    config.auto_compaction_sstable_threshold = 5;
    config.sync_wal = false;

    auto verify_scan = [&](LSMDatabase& db, const std::string& lo, const std::string& hi, size_t limit) {
        auto got = db.Scan(lo, hi, limit);
        std::vector<std::pair<std::string, std::string>> want;
        for (auto it = lo.empty() ? ref.begin() : ref.lower_bound(lo); it != ref.end(); ++it) {
            if (!hi.empty() && it->first > hi) break;
            want.push_back(*it);
            if (limit && want.size() >= limit) break;
        }
        CHECK(got == want);
    };

    std::unique_ptr<LSMDatabase> db(new LSMDatabase(dir, config));
    for (int step = 0; step < 6000; ++step) {
        int op = static_cast<int>(rng() % 100);
        std::string k = Key(static_cast<int>(rng() % 800));
        if (op < 55) {
            std::string v = "v" + std::to_string(step);
            CHECK(db->Put(k, v));
            ref[k] = v;
        } else if (op < 75) {
            CHECK(db->Delete(k));
            ref.erase(k);
        } else if (op < 90) {
            std::string v;
            bool found = db->Get(k, &v);
            auto it = ref.find(k);
            CHECK(found == (it != ref.end()));
            if (found) CHECK(v == it->second);
        } else if (op < 97) {
            std::string lo = Key(static_cast<int>(rng() % 900));
            std::string hi = Key(static_cast<int>(rng() % 900));
            if (lo > hi) std::swap(lo, hi);
            if (rng() % 5 == 0) lo.clear();
            if (rng() % 5 == 0) hi.clear();
            verify_scan(*db, lo, hi, rng() % 3 == 0 ? (rng() % 20 + 1) : 0);
        } else if (op < 99) {
            db->Compact();
        } else {
            db.reset(); // close without an explicit Flush, then reopen (WAL replay)
            db.reset(new LSMDatabase(dir, config));
        }
    }
    verify_scan(*db, "", "", 0);
    CHECK(db->GetStats().compactions_completed > 0 || db->GetStats().flushes_completed > 0);

    db.reset();
    CleanDir(dir);
    std::cout << "PASSED\n";
}

void TestConcurrentReadersAndWriters() {
    Begin("Concurrent writers + readers + scans (flushes & compactions racing)");
    std::string dir = FreshDir("concurrent");

    LSMConfig config;
    config.memtable_flush_threshold_bytes = 1024;
    config.auto_compaction_sstable_threshold = 3;
    config.sync_wal = false;
    LSMDatabase db(dir, config);

    const int kWriters = 4;
    const int kKeysPerWriter = 300;
    const int kRounds = 3;
    std::atomic<bool> done(false);
    std::atomic<int> scan_errors(0);

    std::vector<std::thread> threads;
    for (int w = 0; w < kWriters; ++w) {
        threads.emplace_back([&db, w]() {
            for (int round = 0; round < kRounds; ++round) {
                for (int i = 0; i < kKeysPerWriter; ++i) {
                    std::string k = "w" + std::to_string(w) + "_" + Key(i);
                    if (round == kRounds - 1 && i % 7 == 0) {
                        db.Delete(k);
                    } else {
                        db.Put(k, std::to_string(round));
                    }
                }
            }
        });
    }
    for (int r = 0; r < 3; ++r) {
        threads.emplace_back([&db, &done, &scan_errors, r]() {
            std::mt19937 rng(static_cast<unsigned>(r));
            while (!done.load()) {
                std::string v;
                db.Get("w" + std::to_string(rng() % kWriters) + "_" + Key(static_cast<int>(rng() % kKeysPerWriter)), &v);
                auto res = db.Scan("w1", "w3", 200);
                for (size_t i = 1; i < res.size(); ++i) {
                    if (!(res[i - 1].first < res[i].first)) scan_errors++; // must be sorted, unique
                }
                db.GetStats();
            }
        });
    }
    for (int w = 0; w < kWriters; ++w) threads[w].join();
    done = true;
    for (size_t t = kWriters; t < threads.size(); ++t) threads[t].join();

    CHECK(scan_errors.load() == 0);
    const std::string last = std::to_string(kRounds - 1);
    for (int w = 0; w < kWriters; ++w) {
        for (int i = 0; i < kKeysPerWriter; ++i) {
            std::string v;
            bool found = db.Get("w" + std::to_string(w) + "_" + Key(i), &v);
            if (i % 7 == 0) {
                CHECK(!found);
            } else {
                CHECK(found && v == last);
            }
        }
    }
    CHECK(db.GetStats().flushes_completed > 0);

    CleanDir(dir);
    std::cout << "PASSED\n";
}

} // namespace

int main() {
    std::cout << "=========================================================\n";
    std::cout << "  Running LSM-Tree Storage Engine Unit & Integration Tests\n";
    std::cout << "=========================================================\n";
    TestSkipList();
    TestBloomFilter();
    TestWALAndCRC32();
    TestSSTableBinaryFormat();
    TestSSTableIteratorSeek();
    TestTombstoneShadowingAndCompaction();
    TestCrashRecoveryAndRangeScan();
    TestManifestCrashSafety();
    TestRandomizedAgainstReference();
    TestConcurrentReadersAndWriters();
    std::cout << "=========================================================\n";
    std::cout << "  ALL " << kNumTests << " TEST SUITES PASSED SUCCESSFULLY!\n";
    std::cout << "=========================================================\n";
    return 0;
}
