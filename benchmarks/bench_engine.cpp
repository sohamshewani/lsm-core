#include <chrono>
#include <vector>
#include <string>
#include <numeric>
#include <algorithm>
#include <iostream>
#include <thread>
#include <atomic>
#include <cstdint>

// -----------------------------------------------------------------------------
// Mock implementation of 'lsm-core' to make the microbenchmark self-contained
// and compilable. In a real-world integration, include the actual lsm-core headers.
// -----------------------------------------------------------------------------
namespace lsm {
    struct Options {
        size_t memtable_size_mb = 64;
    };

    class DB {
    public:
        static DB* Open(const Options&, const std::string&) { return new DB(); }
        ~DB() = default;

        bool Put(const std::string& key, const std::string& value) {
            // Simulate trivial LSM write path overhead
            std::atomic_signal_fence(std::memory_order_relaxed);
            return true;
        }

        bool Get(const std::string& key, std::string* value) {
            // Simulate trivial LSM read path overhead
            std::atomic_signal_fence(std::memory_order_relaxed);
            *value = "mock_value";
            return true;
        }
    };
}

// -----------------------------------------------------------------------------
// Benchmark Instrumentation Utilities
// -----------------------------------------------------------------------------
struct LatencyStats {
    double ops_sec;
    uint64_t p50_ns;
    uint64_t p99_ns;
    uint64_t min_ns;
    uint64_t max_ns;
};

template <typename Fn>
LatencyStats measure_workload(uint64_t iterations, Fn&& fn) {
    std::vector<uint64_t> latencies;
    latencies.reserve(iterations);

    // Warmup phase
    for (uint64_t i = 0; i < std::min(iterations / 10, (uint64_t)10000); ++i) {
        fn(i);
    }

    auto start_time = std::chrono::high_resolution_clock::now();

    for (uint64_t i = 0; i < iterations; ++i) {
        auto op_start = std::chrono::high_resolution_clock::now();
        fn(i);
        auto op_end = std::chrono::high_resolution_clock::now();
        
        uint64_t ns = std::chrono::duration_cast<std::chrono::nanoseconds>(op_end - op_start).count();
        latencies.push_back(ns);
    }

    auto end_time = std::chrono::high_resolution_clock::now();
    auto total_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end_time - start_time).count();

    std::sort(latencies.begin(), latencies.end());

    LatencyStats stats;
    stats.ops_sec = static_cast<double>(iterations) * 1e9 / static_cast<double>(total_ns);
    stats.p50_ns  = latencies[iterations * 50 / 100];
    stats.p99_ns  = latencies[iterations * 99 / 100];
    stats.min_ns  = latencies.front();
    stats.max_ns  = latencies.back();

    return stats;
}

int main() {
    const std::string db_path = "./lsm_bench_core_db";
    lsm::Options options;
    options.memtable_size_mb = 128;

    lsm::DB* db = lsm::DB::Open(options, db_path);
    if (!db) {
        std::cerr << "Failed to initialize lsm-core DB instance." << std::endl;
        return 1;
    }

    const uint64_t kNumOperations = 500000;

    // Execute Workloads
    auto write_stats = measure_workload(kNumOperations, [&](uint64_t i) {
        std::string key = "key_" + std::to_string(i);
        std::string val = "value_" + std::to_string(i);
        db->Put(key, val);
    });

    auto read_stats = measure_workload(kNumOperations, [&](uint64_t i) {
        std::string key = "key_" + std::to_string(i % kNumOperations);
        std::string val;
        db->Get(key, &val);
    });

    delete db;

    // Output formatted tabular metrics to stdout
    std::cout << "\n";
    std::cout << "=================================================================================\n";
    std::cout << "                       LSM-CORE MICROBENCHMARK RESULTS                           \n";
    std::cout << "=================================================================================\n";
    std::cout << "| Workload    | Operations  | Throughput (ops/sec) | P50 (ns) | P99 (ns) | Max (ns) |\n";
    std::cout << "|-------------|-------------|----------------------|----------|----------|----------|\n";
    
    // Print Write Row
    printf("| %-11s | %-11llu | %20.2f | %8llu | %8llu | %8llu |\n",
           "Write", 
           static_cast<unsigned long long>(kNumOperations), 
           write_stats.ops_sec, 
           static_cast<unsigned long long>(write_stats.p50_ns), 
           static_cast<unsigned long long>(write_stats.p99_ns),
           static_cast<unsigned long long>(write_stats.max_ns));

    // Print Read Row
    printf("| %-11s | %-11llu | %20.2f | %8llu | %8llu | %8llu |\n",
           "Read", 
           static_cast<unsigned long long>(kNumOperations), 
           read_stats.ops_sec, 
           static_cast<unsigned long long>(read_stats.p50_ns), 
           static_cast<unsigned long long>(read_stats.p99_ns),
           static_cast<unsigned long long>(read_stats.max_ns));

    std::cout << "=================================================================================\n";
    std::cout << "\n";

    return 0;
}