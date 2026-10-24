#include "lsm.h"

#include <chrono>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

using namespace lsm;

namespace {

void PrintBanner(const LSMDatabase& db, const std::string& data_dir) {
    LSMStats stats = db.GetStats();
    std::cout << "====================================================================\n";
    std::cout << "  LSM-Tree Key-Value Storage Engine (Built from Scratch in C++)\n";
    std::cout << "  Components: WAL (CRC32) | SkipList MemTable | SSTables | Bloom\n";
    std::cout << "====================================================================\n";
    std::cout << "  Data Directory        : " << data_dir << "\n";
    std::cout << "  WAL Recovered Startup : " << stats.wal_recovered_records << " records\n";
    std::cout << "  Loaded SSTables       : " << stats.total_sstables << " files\n";
    std::cout << "  MemTable Flush Limit  : " << db.config().memtable_flush_threshold_bytes << " bytes\n";
    std::cout << "  WAL fsync per write   : " << (db.config().sync_wal ? "on" : "off (--no-sync)") << "\n";
    std::cout << "====================================================================\n";
    std::cout << "Type HELP for commands, or try: PUT user:1 Alice, GET user:1, STATS\n\n";
}

void PrintHelp() {
    std::cout << "\nAvailable Commands:\n"
              << "  PUT <key> <value>         Write or update a key-value pair (WAL -> MemTable)\n"
              << "  GET <key>                 Point lookup (MemTable -> Newest SSTable w/ Bloom Filter)\n"
              << "  DEL <key>                 Delete a key by writing a Tombstone marker\n"
              << "  SCAN [start] [end]        Range scan across MemTable + all SSTables in sorted order\n"
              << "  FLUSH                     Force-flush active MemTable into an immutable Level-0 SSTable\n"
              << "  COMPACT                   Merge all SSTables, deduplicate keys & purge Tombstones\n"
              << "  STATS                     Inspect internal MemTable, WAL, Bloom Filter & SSTable layout\n"
              << "  BENCH [count]             Run write/read/bloom-filter performance benchmark (default 5000)\n"
              << "  HELP                      Show this help menu\n"
              << "  EXIT / QUIT               Close the database cleanly\n\n";
}

void PrintStats(const LSMDatabase& db) {
    LSMStats s = db.GetStats();
    std::cout << "\n--- LSM-Tree Engine Internals ---\n";
    std::cout << "  [MemTable (SkipList)]\n";
    std::cout << "    Active Entries      : " << s.memtable_entries << "\n";
    std::cout << "    Memory Usage        : " << s.memtable_bytes << " / "
              << db.config().memtable_flush_threshold_bytes << " bytes\n";
    std::cout << "    SkipList Height     : " << s.skiplist_height << " / " << SkipList::kMaxHeight << "\n";
    std::cout << "  [Write-Ahead Log]\n";
    std::cout << "    Current WAL Size    : " << s.wal_bytes << " bytes\n";
    std::cout << "    Recovered at Boot   : " << s.wal_recovered_records << " records\n";
    std::cout << "  [Storage & Telemetry]\n";
    std::cout << "    Total SSTables      : " << s.total_sstables
              << " (Auto-compact at " << db.config().auto_compaction_sstable_threshold << ")\n";
    std::cout << "    Flushes Completed   : " << s.flushes_completed << "\n";
    std::cout << "    Compactions Run     : " << s.compactions_completed << "\n";
    std::cout << "    Bloom Filter Saves  : " << s.bloom_filter_saves << " disk reads avoided\n";
    std::cout << "    SSTable Disk Seeks  : " << s.sstable_disk_reads << " blocks read\n";

    const auto& tables = db.sstables();
    if (!tables.empty()) {
        std::cout << "\n  [On-Disk SSTables (Oldest -> Newest)]\n";
        for (const auto& t : tables) {
            std::cout << "    * ID #" << std::right << std::setw(4) << std::setfill('0') << t->table_id()
                      << " | Level " << t->level()
                      << " | Entries: " << std::setw(5) << std::setfill(' ') << t->num_entries()
                      << " | Size: " << std::setw(7) << t->file_size() << " B"
                      << " | SparseIdx: " << std::setw(3) << t->sparse_index_size()
                      << " | Bloom: " << std::setw(4) << t->bloom_filter_bytes() << " B"
                      << " | Keys: [" << t->min_key() << " .. " << t->max_key() << "]\n";
        }
    }
    std::cout << "---------------------------------\n";
}

void RunBenchmark(LSMDatabase& db, size_t count) {
    if (count == 0) count = 5000;
    std::cout << "\nRunning benchmark with " << count << " keys...\n";

    size_t old_threshold = db.config().memtable_flush_threshold_bytes;
    // Set 64 KB MemTable flush threshold during benchmark so we get several SSTables and compactions
    db.set_flush_threshold(64 * 1024);

    // 1. Sequential/Batched Writes
    auto t0 = std::chrono::high_resolution_clock::now();
    for (size_t i = 0; i < count; ++i) {
        std::ostringstream k, v;
        k << "bench_key_" << std::setw(7) << std::setfill('0') << i;
        v << "payload_value_" << i << "_lsm_storage_engine_test";
        db.Put(k.str(), v.str());
    }
    db.Flush();
    auto t1 = std::chrono::high_resolution_clock::now();

    // 2. Point Lookups (Existing Keys)
    size_t found = 0;
    auto t2 = std::chrono::high_resolution_clock::now();
    for (size_t i = 0; i < count; ++i) {
        std::ostringstream k;
        k << "bench_key_" << std::setw(7) << std::setfill('0') << i;
        std::string val;
        if (db.Get(k.str(), &val)) {
            found++;
        }
    }
    auto t3 = std::chrono::high_resolution_clock::now();

    // 3. Point Lookups (Non-existent keys inside key range to test Bloom Filter!)
    size_t stats_before_bloom = db.GetStats().bloom_filter_saves;
    size_t stats_before_disk = db.GetStats().sstable_disk_reads;
    auto t4 = std::chrono::high_resolution_clock::now();
    for (size_t i = 0; i < count; ++i) {
        std::ostringstream k;
        k << "bench_key_" << std::setw(7) << std::setfill('0') << i << "_missing";
        std::string val;
        db.Get(k.str(), &val);
    }
    auto t5 = std::chrono::high_resolution_clock::now();

    double write_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    double read_hit_ms = std::chrono::duration<double, std::milli>(t3 - t2).count();
    double read_miss_ms = std::chrono::duration<double, std::milli>(t5 - t4).count();

    size_t bloom_saves_delta = db.GetStats().bloom_filter_saves - stats_before_bloom;
    size_t disk_reads_delta = db.GetStats().sstable_disk_reads - stats_before_disk;

    std::cout << std::fixed << std::setprecision(2);
    std::cout << "  [Write Performance]     " << count << " PUTs in " << write_ms << " ms ("
              << (count * 1000.0 / (write_ms > 0 ? write_ms : 1)) << " ops/sec)\n";
    std::cout << "  [Read Hit Performance]  " << found << "/" << count << " GETs in " << read_hit_ms << " ms ("
              << (count * 1000.0 / (read_hit_ms > 0 ? read_hit_ms : 1)) << " ops/sec)\n";
    std::cout << "  [Read Miss w/ Bloom]    " << count << " missing GETs in " << read_miss_ms << " ms ("
              << (count * 1000.0 / (read_miss_ms > 0 ? read_miss_ms : 1)) << " ops/sec)\n";
    std::cout << "  [Bloom Filter Impact]   Avoided " << bloom_saves_delta
              << " SSTable disk reads (only " << disk_reads_delta << " false-positive disk checks)!\n\n";

    db.set_flush_threshold(old_threshold);
}

void RunGuidedDemo(LSMDatabase& db) {
    std::cout << ">>> Starting Guided LSM-Tree Walkthrough Demo <<<\n\n";

    std::cout << "1. Writing 4 keys to the database (logged to WAL + inserted into SkipList MemTable)...\n";
    db.Put("user:101", "Alice (Engineer)");
    db.Put("user:102", "Bob (Designer)");
    db.Put("user:103", "Charlie (Systems Architect)");
    db.Put("user:104", "Diana (Database Researcher)");

    std::cout << "2. Flushing active MemTable to an immutable Level-0 SSTable on disk...\n";
    db.Flush();

    std::cout << "3. Updating 'user:102' and deleting 'user:103' (writes a Tombstone in newer layer)...\n";
    db.Put("user:102", "Bob (Principal Designer - Updated!)");
    db.Delete("user:103");
    db.Put("user:105", "Evan (Distributed Systems)");
    db.Flush();

    PrintStats(db);

    std::cout << "\n4. Querying keys across layers (Newest SSTable shadows older SSTables):\n";
    for (const std::string k : {"user:101", "user:102", "user:103", "user:104", "user:105", "user:999"}) {
        std::string val;
        if (db.Get(k, &val)) {
            std::cout << "   GET " << std::setw(10) << std::left << k << " -> \"" << val << "\"\n";
        } else {
            std::cout << "   GET " << std::setw(10) << std::left << k << " -> (nil / deleted or not found)\n";
        }
    }

    std::cout << "\n5. Running Range SCAN [user:100 .. user:999]:\n";
    auto scan_res = db.Scan("user:100", "user:999");
    for (const auto& kv : scan_res) {
        std::cout << "   " << kv.first << " => " << kv.second << "\n";
    }

    std::cout << "\n6. Running COMPACT to merge SSTables, deduplicate 'user:102', and purge 'user:103' tombstone...\n";
    db.Compact();
    PrintStats(db);
}

} // namespace

int main(int argc, char* argv[]) {
    std::string data_dir = "./lsm_data";
    bool run_demo = false;
    bool run_bench = false;
    bool sync_wal = true;
    size_t bench_count = 5000;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--dir" && i + 1 < argc) {
            data_dir = argv[++i];
        } else if (arg == "--no-sync") {
            sync_wal = false;
        } else if (arg == "--demo") {
            run_demo = true;
        } else if (arg == "--bench") {
            run_bench = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                bench_count = static_cast<size_t>(std::strtoull(argv[++i], nullptr, 10));
            }
        }
    }

    LSMConfig config;
    config.sync_wal = sync_wal;
    LSMDatabase db(data_dir, config);

    if (run_demo) {
        PrintBanner(db, data_dir);
        RunGuidedDemo(db);
        return 0;
    }

    if (run_bench) {
        PrintBanner(db, data_dir);
        RunBenchmark(db, bench_count);
        PrintStats(db);
        return 0;
    }

    PrintBanner(db, data_dir);

    std::string line;
    while (true) {
        std::cout << "lsm-db> " << std::flush;
        if (!std::getline(std::cin, line)) {
            std::cout << "\n";
            break;
        }

        std::istringstream iss(line);
        std::string cmd;
        if (!(iss >> cmd)) {
            continue;
        }

        // Normalize command to uppercase
        for (char& c : cmd) {
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        }

        if (cmd == "EXIT" || cmd == "QUIT") {
            std::cout << "Closing LSM-Tree engine cleanly. Goodbye!\n";
            break;
        } else if (cmd == "HELP") {
            PrintHelp();
        } else if (cmd == "PUT" || cmd == "SET") {
            std::string key;
            if (!(iss >> key)) {
                std::cout << "(error) Usage: PUT <key> <value>\n";
                continue;
            }
            std::string value;
            std::getline(iss >> std::ws, value);
            if (value.empty()) {
                std::cout << "(error) Usage: PUT <key> <value>\n";
                continue;
            }
            if (db.Put(key, value)) {
                std::cout << "OK\n";
            } else {
                std::cout << "(error) Failed to write to WAL\n";
            }
        } else if (cmd == "GET") {
            std::string key;
            if (!(iss >> key)) {
                std::cout << "(error) Usage: GET <key>\n";
                continue;
            }
            std::string value;
            if (db.Get(key, &value)) {
                std::cout << "\"" << value << "\"\n";
            } else {
                std::cout << "(nil)\n";
            }
        } else if (cmd == "DEL" || cmd == "DELETE") {
            std::string key;
            if (!(iss >> key)) {
                std::cout << "(error) Usage: DEL <key>\n";
                continue;
            }
            if (db.Delete(key)) {
                std::cout << "OK (tombstone written)\n";
            } else {
                std::cout << "(error) Failed to write tombstone\n";
            }
        } else if (cmd == "SCAN") {
            std::string start_key, end_key;
            iss >> start_key >> end_key;
            auto results = db.Scan(start_key, end_key);
            if (results.empty()) {
                std::cout << "(empty scan result)\n";
            } else {
                for (size_t i = 0; i < results.size(); ++i) {
                    std::cout << (i + 1) << ") \"" << results[i].first
                              << "\" => \"" << results[i].second << "\"\n";
                }
            }
        } else if (cmd == "FLUSH") {
            if (db.Flush()) {
                std::cout << "OK (MemTable flushed to new Level-0 SSTable)\n";
            } else {
                std::cout << "(noop) MemTable is currently empty\n";
            }
        } else if (cmd == "COMPACT") {
            if (db.Compact()) {
                std::cout << "OK (SSTables merged, overwritten keys deduplicated, tombstones purged)\n";
            } else {
                std::cout << "(noop) No SSTables to compact\n";
            }
        } else if (cmd == "STATS" || cmd == "INFO") {
            PrintStats(db);
        } else if (cmd == "BENCH") {
            size_t count = 5000;
            iss >> count;
            RunBenchmark(db, count);
        } else if (cmd == "DEMO") {
            RunGuidedDemo(db);
        } else {
            std::cout << "(error) Unknown command '" << cmd << "'. Type HELP for available commands.\n";
        }
    }

    return 0;
}
