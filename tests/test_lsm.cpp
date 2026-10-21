#include "../src/lsm.h"

#include <cassert>
#include <cstdio>
#include <iostream>
#include <string>
#include <vector>

using namespace lsm;

namespace {

void CleanTestDir(const std::string& dir) {
    DIR* d = opendir(dir.c_str());
    if (!d) return;
    struct dirent* entry;
    while ((entry = readdir(d)) != nullptr) {
        std::string name = entry->d_name;
        if (name != "." && name != "..") {
            std::string full = dir + "/" + name;
            std::remove(full.c_str());
        }
    }
    closedir(d);
}

void TestSkipList() {
    std::cout << "[TEST 1/6] SkipList sorted insert, update, and search... ";
    SkipList sl;
    sl.Insert("charlie", ValueEntry::MakeValue("3"));
    sl.Insert("alice", ValueEntry::MakeValue("1"));
    sl.Insert("bob", ValueEntry::MakeValue("2"));
    sl.Insert("alice", ValueEntry::MakeValue("1_updated"));

    assert(sl.num_entries() == 3);

    ValueEntry entry;
    assert(sl.Search("alice", &entry) && entry.value == "1_updated");
    assert(sl.Search("bob", &entry) && entry.value == "2");
    assert(sl.Search("charlie", &entry) && entry.value == "3");
    assert(!sl.Search("david", &entry));

    std::vector<std::string> keys;
    for (auto it = sl.Begin(); it.valid(); it.next()) {
        keys.push_back(it.key());
    }
    assert(keys.size() == 3);
    assert(keys[0] == "alice" && keys[1] == "bob" && keys[2] == "charlie");
    std::cout << "PASSED\n";
}

void TestBloomFilter() {
    std::cout << "[TEST 2/6] BloomFilter zero false-negatives & serialization... ";
    BloomFilter bf(1000, 10);
    for (int i = 0; i < 1000; ++i) {
        bf.Add("key_" + std::to_string(i));
    }

    std::string raw = bf.Serialize();
    BloomFilter loaded = BloomFilter::Deserialize(raw);

    // Must have ZERO false negatives
    for (int i = 0; i < 1000; ++i) {
        assert(loaded.MayContain("key_" + std::to_string(i)));
    }

    // Check false positive rate on 10,000 unseen keys (expected ~1%, must be < 3%)
    int false_positives = 0;
    for (int i = 1000; i < 11000; ++i) {
        if (loaded.MayContain("key_" + std::to_string(i))) {
            false_positives++;
        }
    }
    double fp_rate = false_positives / 10000.0;
    assert(fp_rate < 0.03);
    std::cout << "PASSED (FP rate = " << (fp_rate * 100.0) << "%)\n";
}

void TestWALAndCRC32() {
    std::cout << "[TEST 3/6] WriteAheadLog replay & CRC32 corruption detection... ";
    std::string dir = "./test_lsm_tmp";
#ifdef _WIN32
    _mkdir(dir.c_str());
#else
    mkdir(dir.c_str(), 0755);
#endif
    CleanTestDir(dir);

    std::string wal_file = dir + "/wal.log";
    {
        WriteAheadLog wal(wal_file);
        assert(wal.Append("k1", ValueEntry::MakeValue("v1")));
        assert(wal.Append("k2", ValueEntry::MakeTombstone()));
        assert(wal.Append("k3", ValueEntry::MakeValue("v3")));
        wal.Close();
    }

    // Append a corrupted partial/bad-CRC record at the end to simulate mid-write power loss
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
    assert(recovered == 3);
    assert(corrupted == 1);
    assert(records[0].key == "k1" && records[0].entry.value == "v1");
    assert(records[1].key == "k2" && records[1].entry.is_tombstone());
    assert(records[2].key == "k3" && records[2].entry.value == "v3");

    CleanTestDir(dir);
    std::cout << "PASSED\n";
}

void TestSSTableBinaryFormat() {
    std::cout << "[TEST 4/6] SSTable creation, sparse index binary search & reload... ";
    std::string dir = "./test_lsm_tmp";
    CleanTestDir(dir);

    std::vector<KeyValue> sorted;
    for (int i = 0; i < 100; ++i) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "item_%04d", i);
        sorted.push_back(KeyValue{buf, ValueEntry::MakeValue("val_" + std::to_string(i))});
    }

    std::string sst_path = dir + "/sst_L0_000001.sst";
    auto created = SSTable::Create(sst_path, 1, 0, sorted);
    assert(created != nullptr);
    assert(created->num_entries() == 100);
    // 100 entries with stride 16 -> ceil(100 / 16.0) = 7 sparse index entries
    assert(created->sparse_index_size() == 7);

    auto reopened = SSTable::Open(sst_path, 1, 0);
    assert(reopened != nullptr);
    assert(reopened->min_key() == "item_0000");
    assert(reopened->max_key() == "item_0099");

    for (int i = 0; i < 100; ++i) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "item_%04d", i);
        ValueEntry e;
        assert(reopened->Get(buf, &e));
        assert(e.value == "val_" + std::to_string(i));
    }

    size_t bloom_saves = 0;
    ValueEntry missing;
    assert(!reopened->Get("item_0050_nonexistent", &missing, &bloom_saves));

    CleanTestDir(dir);
    std::cout << "PASSED\n";
}

void TestTombstoneShadowingAndCompaction() {
    std::cout << "[TEST 5/6] Multi-layer Tombstone shadowing & SSTable Compaction... ";
    std::string dir = "./test_lsm_tmp";
    CleanTestDir(dir);

    LSMDatabase db(dir);
    db.Put("a", "old_a");
    db.Put("b", "old_b");
    db.Put("c", "old_c");
    db.Flush(); // SSTable #1

    db.Put("a", "new_a"); // Overwrite 'a'
    db.Delete("b");       // Delete 'b' with tombstone
    db.Put("d", "new_d");
    db.Flush(); // SSTable #2

    assert(db.GetStats().total_sstables == 2);

    std::string val;
    assert(db.Get("a", &val) && val == "new_a");
    assert(!db.Get("b", &val)); // Shadowed by tombstone in SSTable #2!
    assert(db.Get("c", &val) && val == "old_c");
    assert(db.Get("d", &val) && val == "new_d");

    // Compact SSTable #1 and SSTable #2 into a single Level-1 SSTable
    assert(db.Compact());
    assert(db.GetStats().total_sstables == 1);
    // After compaction, 'b' and its tombstone are completely purged, leaving only a, c, d (3 entries)
    assert(db.sstables().front()->num_entries() == 3);

    assert(db.Get("a", &val) && val == "new_a");
    assert(!db.Get("b", &val));
    assert(db.Get("c", &val) && val == "old_c");
    assert(db.Get("d", &val) && val == "new_d");

    CleanTestDir(dir);
    std::cout << "PASSED\n";
}

void TestCrashRecoveryAndRangeScan() {
    std::cout << "[TEST 6/6] Crash recovery (WAL + SSTables) & Range Scan... ";
    std::string dir = "./test_lsm_tmp";
    CleanTestDir(dir);

    {
        LSMDatabase db(dir);
        db.Put("k1", "v1_flushed");
        db.Put("k2", "v2_flushed");
        db.Flush(); // Written to SSTable

        // Written only to WAL + MemTable (not flushed before db closes)
        db.Put("k2", "v2_unflushed_update");
        db.Put("k3", "v3_unflushed");
        db.Delete("k1");
    }

    // Reopen database on same directory: should load SSTable AND replay WAL
    {
        LSMDatabase recovered_db(dir);
        assert(recovered_db.GetStats().wal_recovered_records == 3);
        assert(recovered_db.GetStats().total_sstables == 1);

        std::string val;
        assert(!recovered_db.Get("k1", &val)); // Deleted in WAL!
        assert(recovered_db.Get("k2", &val) && val == "v2_unflushed_update");
        assert(recovered_db.Get("k3", &val) && val == "v3_unflushed");

        auto scan = recovered_db.Scan("k1", "k9");
        assert(scan.size() == 2);
        assert(scan[0].first == "k2" && scan[0].second == "v2_unflushed_update");
        assert(scan[1].first == "k3" && scan[1].second == "v3_unflushed");
    }

    CleanTestDir(dir);
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
    TestTombstoneShadowingAndCompaction();
    TestCrashRecoveryAndRangeScan();
    std::cout << "=========================================================\n";
    std::cout << "  ALL 6 TEST SUITES PASSED SUCCESSFULLY!\n";
    std::cout << "=========================================================\n";
    return 0;
}
