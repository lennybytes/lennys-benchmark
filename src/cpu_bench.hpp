// SPDX-License-Identifier: MIT
//
//  cpu_bench.hpp - multi-threaded CPU throughput benchmark.
//
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace bench {

struct CpuOptions {
    double secondsPerTest = 3.0;   // wall time each sub-test is executed
    int threads = 0;               // 0 => hardware concurrency
    uint64_t seed = 0xC0FFEE123456789ull;
    bool quick = false;            // skip the slowest tests
};

struct CpuTestResult {
    std::string name;      // "Integer (xoshiro256**)"
    std::string unit;      // "Mops/s", "GFLOP/s", "GB/s"
    double value = 0.0;    // measured throughput
    double score = 0.0;    // normalised sub-score
    double seconds = 0.0;  // measured duration
    int threads = 1;
    std::string note;
};

struct CpuBenchResult {
    std::vector<CpuTestResult> tests;
    double singleThreadScore = 0.0;  // geomean of single-threaded sub-scores
    double multiThreadScore  = 0.0;  // geomean of all sub-scores
    double totalScore        = 0.0;
    double scaling           = 1.0;  // multi/single integer throughput
    int threads = 1;
    double seconds = 0.0;
    uint64_t checksum = 0;
};

CpuBenchResult runCpuBenchmark(const CpuOptions& opts);

// Human readable rating for a score (e.g. "Excellent").
std::string scoreRating(double score);

} // namespace bench
