#include "../engine/engine.h"
#include "../sstable/sstable.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

namespace {

using Clock = std::chrono::high_resolution_clock;

constexpr int kNumKeys = 5000;
constexpr int kFlushThreshold = 100;
constexpr int kCompactThreshold = 10000;
constexpr int kReadIterations = 2000;
constexpr int kValueSize = 64;

const char* kDataDir = "benchmarks/data";
const char* kOutputCsv = "benchmarks/output/phase7_baseline.csv";

double elapsedSec(const Clock::time_point& start, const Clock::time_point& end) {
    return std::chrono::duration<double>(end - start).count();
}

std::string makeValue(int size) {
    return std::string(static_cast<std::size_t>(size), 'x');
}

std::string makeKey(int i) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "key:%08d", i);
    return buf;
}

void cleanupDataDir() {
    std::error_code ec;
    std::filesystem::remove_all(kDataDir, ec);
    std::filesystem::create_directories(kDataDir, ec);
    std::filesystem::create_directories("benchmarks/output", ec);
}

void removeBenchFiles() {
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(kDataDir, ec)) {
        std::filesystem::remove_all(entry.path(), ec);
    }
}

struct CsvWriter {
    explicit CsvWriter(const char* path) : out_(path) {
        out_ << "benchmark,read_mode,num_sstables,num_keys,value_size_bytes,"
                "ops_per_sec,avg_latency_us,bloom_skips\n";
    }

    void write(const std::string& benchmark, const std::string& read_mode,
               std::size_t num_sstables, int num_keys, int value_size,
               double ops_per_sec, double avg_latency_us, std::size_t bloom_skips) {
        out_ << benchmark << ',' << read_mode << ',' << num_sstables << ','
             << num_keys << ',' << value_size << ','
             << std::fixed << std::setprecision(2) << ops_per_sec << ','
             << std::fixed << std::setprecision(3) << avg_latency_us << ','
             << bloom_skips << '\n';
    }

private:
    std::ofstream out_;
};

Engine makeEngine(const std::string& name) {
    return Engine(kDataDir + std::string("/") + name + ".wal", kFlushThreshold,
                  kCompactThreshold);
}

void populate(Engine& engine, int num_keys, int value_size) {
    const std::string value = makeValue(value_size);
    for (int i = 0; i < num_keys; ++i) {
        engine.put(makeKey(i), value);
    }
}

double benchWriteThroughput(CsvWriter& csv) {
    removeBenchFiles();
    Engine engine = makeEngine("write");

    const auto start = Clock::now();
    populate(engine, kNumKeys, kValueSize);
    const auto end = Clock::now();

    const double seconds = elapsedSec(start, end);
    const double ops = static_cast<double>(kNumKeys) / seconds;
    csv.write("write_throughput", "n/a", engine.sstables().size(), kNumKeys, kValueSize,
              ops, 0.0, 0);

    std::cout << "Write throughput: " << ops << " puts/sec ("
              << engine.sstables().size() << " SSTables)\n";
    return ops;
}

std::optional<std::string> engineGetLinear(const Engine& engine,
                                           const std::string& key) {
    if (engine.memtable().contains(key)) {
        return engine.memtable().get(key);
    }
    for (const auto& path : engine.sstables()) {
        SSTableReader reader(path);
        if (auto v = reader.getLinearScan(key); v.has_value()) {
            return v;
        }
    }
    return std::nullopt;
}

std::optional<std::string> engineGetMissLinear(const Engine& engine,
                                             const std::string& key) {
    if (engine.memtable().contains(key)) {
        return engine.memtable().get(key);
    }
    for (const auto& path : engine.sstables()) {
        SSTableReader reader(path);
        if (auto v = reader.getLinearScan(key); v.has_value()) {
            return v;
        }
    }
    return std::nullopt;
}

void benchReadHit(CsvWriter& csv, Engine& engine) {
    const std::string key = makeKey(kNumKeys / 2);

    const auto start_opt = Clock::now();
    for (int i = 0; i < kReadIterations; ++i) {
        auto v = engine.get(key);
        if (!v.has_value()) {
            std::cerr << "Expected hit for " << key << "\n";
            std::exit(1);
        }
    }
    const auto end_opt = Clock::now();

    const auto start_lin = Clock::now();
    for (int i = 0; i < kReadIterations; ++i) {
        auto v = engineGetLinear(engine, key);
        if (!v.has_value()) {
            std::cerr << "Expected linear hit for " << key << "\n";
            std::exit(1);
        }
    }
    const auto end_lin = Clock::now();

    const double sec_opt = elapsedSec(start_opt, end_opt);
    const double sec_lin = elapsedSec(start_lin, end_lin);
    const double ops_opt = kReadIterations / sec_opt;
    const double ops_lin = kReadIterations / sec_lin;
    const double lat_opt = (sec_opt * 1e6) / kReadIterations;
    const double lat_lin = (sec_lin * 1e6) / kReadIterations;

    csv.write("read_hit", "optimized", engine.sstables().size(), kReadIterations,
              kValueSize, ops_opt, lat_opt, 0);
    csv.write("read_hit", "linear_scan", engine.sstables().size(), kReadIterations,
              kValueSize, ops_lin, lat_lin, 0);

    std::cout << "Read hit  optimized: " << lat_opt << " us/op\n";
    std::cout << "Read hit  linear:    " << lat_lin << " us/op\n";
}

void benchReadMiss(CsvWriter& csv, Engine& engine) {
    const std::string key = "missing-key-xyz-99999";

    const auto start_opt = Clock::now();
    for (int i = 0; i < kReadIterations; ++i) {
        engine.get(key);
    }
    const auto end_opt = Clock::now();

    const auto start_lin = Clock::now();
    for (int i = 0; i < kReadIterations; ++i) {
        engineGetMissLinear(engine, key);
    }
    const auto end_lin = Clock::now();

    const double sec_opt = elapsedSec(start_opt, end_opt);
    const double sec_lin = elapsedSec(start_lin, end_lin);
    const double ops_opt = kReadIterations / sec_opt;
    const double ops_lin = kReadIterations / sec_lin;
    const double lat_opt = (sec_opt * 1e6) / kReadIterations;
    const double lat_lin = (sec_lin * 1e6) / kReadIterations;

    engine.get(key);
    const std::size_t bloom_skips = engine.lastBloomSkips();

    csv.write("read_miss", "optimized", engine.sstables().size(), kReadIterations,
              kValueSize, ops_opt, lat_opt, bloom_skips);
    csv.write("read_miss", "linear_scan", engine.sstables().size(), kReadIterations,
              kValueSize, ops_lin, lat_lin, 0);

    std::cout << "Read miss optimized: " << lat_opt << " us/op (bloom skips: "
              << bloom_skips << ")\n";
    std::cout << "Read miss linear:    " << lat_lin << " us/op\n";
}

void benchSstableCountSweep(CsvWriter& csv) {
    const std::vector<int> targets = {1, 2, 4, 8};

    for (int target : targets) {
        removeBenchFiles();
        Engine engine = makeEngine("sweep_" + std::to_string(target));
        const int keys_to_insert = kFlushThreshold * target;
        populate(engine, keys_to_insert, kValueSize);

        const std::size_t sstables = engine.sstables().size();
        const std::string key = makeKey(0);

        const auto start_opt = Clock::now();
        for (int i = 0; i < kReadIterations; ++i) {
            engine.get(key);
        }
        const auto end_opt = Clock::now();

        const auto start_lin = Clock::now();
        for (int i = 0; i < kReadIterations; ++i) {
            engineGetLinear(engine, key);
        }
        const auto end_lin = Clock::now();

        const double sec_opt = elapsedSec(start_opt, end_opt);
        const double sec_lin = elapsedSec(start_lin, end_lin);
        const double lat_opt = (sec_opt * 1e6) / kReadIterations;
        const double lat_lin = (sec_lin * 1e6) / kReadIterations;

        csv.write("read_hit_vs_sstables", "optimized", sstables, kReadIterations,
                  kValueSize, kReadIterations / sec_opt, lat_opt, 0);
        csv.write("read_hit_vs_sstables", "linear_scan", sstables, kReadIterations,
                  kValueSize, kReadIterations / sec_lin, lat_lin, 0);

        std::cout << "SSTables=" << sstables << " optimized=" << lat_opt
                  << " us/op linear=" << lat_lin << " us/op\n";
    }
}

}  // namespace

int main() {
    cleanupDataDir();
    CsvWriter csv(kOutputCsv);

    std::cout << "=== Phase 7 baseline benchmarks ===\n";
    std::cout << "keys=" << kNumKeys << " flush_threshold=" << kFlushThreshold
              << " value_size=" << kValueSize << " bytes\n\n";

    benchWriteThroughput(csv);

    removeBenchFiles();
    Engine read_engine = makeEngine("read");
    populate(read_engine, kNumKeys, kValueSize);
    benchReadHit(csv, read_engine);
    benchReadMiss(csv, read_engine);

    std::cout << "\n--- SSTable count sweep ---\n";
    benchSstableCountSweep(csv);

    std::cout << "\nResults written to " << kOutputCsv << "\n";
    std::cout << "Run: python3 benchmarks/plot_benchmarks.py\n";
    return 0;
}
