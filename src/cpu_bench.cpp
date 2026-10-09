// SPDX-License-Identifier: MIT
#include "cpu_bench.hpp"
#include "util.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <thread>
#include <vector>

namespace bench {

namespace {

// Prevent the optimiser from discarding measured work.
template <typename T>
inline void keep(T& sink, T value) {
    sink = sink * static_cast<T>(31) + value;
}

// Globally visible side effect so the compiler cannot eliminate the loops.
std::atomic<uint64_t> g_cpuSink{0};

template <typename T>
inline uint64_t bitcast(const T& v) {
    uint64_t b = 0;
    std::memcpy(&b, &v, std::min(sizeof(T), sizeof(b)));
    return b;
}

template <typename T>
inline void publish(const T& v) {
    g_cpuSink.fetch_xor(bitcast(v), std::memory_order_relaxed);
}

// Run `body(tid, stop)` on `threads` threads for `seconds`; `body` polls `stop`
// and returns the number of completed operations. Returns the summed counter.
template <typename Body>
uint64_t runParallel(int threads, double seconds, Body&& body) {
    if (threads < 1) threads = 1;
    std::atomic<bool> stop{false};
    std::atomic<uint64_t> total{0};
    std::vector<std::thread> pool;
    pool.reserve(static_cast<size_t>(threads));

    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                        std::chrono::duration<double>(seconds));

    for (int t = 0; t < threads; ++t) {
        pool.emplace_back([&, t]() {
            uint64_t local = body(t, stop);
            total.fetch_add(local, std::memory_order_relaxed);
        });
    }
    std::this_thread::sleep_until(deadline);
    stop.store(true, std::memory_order_relaxed);
    for (auto& th : pool) th.join();
    return total.load(std::memory_order_relaxed);
}

// Reference throughputs used to normalise sub-scores; a machine around the
// reference value scores 1000 points on that test.
constexpr double kRefIntSingle  = 1100.0;   // Mops/s
constexpr double kRefFp32Single = 12.0;     // GFLOP/s
constexpr double kRefFp64Single = 12.0;     // GFLOP/s
constexpr double kRefMemSingle  = 12.0;     // GB/s
constexpr double kRefIntMulti   = 8500.0;   // Mops/s (all cores)

double normalise(double value, double ref) { return value / ref * 1000.0; }

double geomean(const std::vector<double>& v) {
    if (v.empty()) return 0.0;
    double sum = 0.0;
    for (double x : v) sum += std::log(std::max(x, 1e-9));
    return std::exp(sum / static_cast<double>(v.size()));
}

CpuTestResult makeIntResult(const CpuOptions& opts, int threads, double secs,
                            const std::string& name, double ref) {
    uint64_t total = runParallel(threads, secs, [&](int tid, std::atomic<bool>& stop) {
        Rng rng(opts.seed ^ (0x1000ull + static_cast<uint64_t>(tid)));
        uint64_t n = 0, acc = 0;
        while (!stop.load(std::memory_order_relaxed)) {
            for (int k = 0; k < 2048; ++k) acc += rng.next();
            n += 2048;
        }
        keep(acc, n);
        publish(acc);
        return n;
    });
    CpuTestResult r;
    r.name = name;
    r.unit = "Mops/s";
    r.value = static_cast<double>(total) / secs / 1e6;
    r.score = normalise(r.value, ref);
    r.seconds = secs;
    r.threads = threads;
    return r;
}

} // namespace

CpuBenchResult runCpuBenchmark(const CpuOptions& opts) {
    CpuBenchResult result;
    const int maxThreads = opts.threads > 0
                               ? opts.threads
                               : static_cast<int>(std::max(1u, std::thread::hardware_concurrency()));
    result.threads = maxThreads;
    const double secs = std::max(0.2, opts.secondsPerTest);

    double cpuStart = nowSeconds();

    // -- 1. Integer throughput, single thread -------------------------------
    result.tests.push_back(makeIntResult(opts, 1, secs, "Integer ALU (xoshiro256**)", kRefIntSingle));
    const double intSingle = result.tests.back().value;

    // -- 2. FP32 FMA throughput (8 independent chains) ---------------------
    {
        uint64_t total = runParallel(1, secs, [&](int tid, std::atomic<bool>& stop) {
            float l0 = 0.51f + 0.001f * tid, l1 = 0.62f, l2 = 0.73f, l3 = 0.84f;
            float l4 = 0.55f, l5 = 0.66f, l6 = 0.77f, l7 = 0.88f;
            const float m = 0.9999999f, e = 1e-6f;
            uint64_t n = 0;
            while (!stop.load(std::memory_order_relaxed)) {
                for (int k = 0; k < 256; ++k) {
                    l0 = l0 * m + e; l1 = l1 * m + e; l2 = l2 * m + e; l3 = l3 * m + e;
                    l4 = l4 * m + e; l5 = l5 * m + e; l6 = l6 * m + e; l7 = l7 * m + e;
                }
                n += 256ull * 8ull;
            }
            float acc = l0 + l1 + l2 + l3 + l4 + l5 + l6 + l7;
            keep(acc, static_cast<float>(n));
            publish(acc);
            return n * 2ull; // one FMA == 2 FLOPs
        });
        CpuTestResult r;
        r.name = "FP32 FMA (8-way ILP)";
        r.unit = "GFLOP/s";
        r.value = static_cast<double>(total) / secs / 1e9;
        r.score = normalise(r.value, kRefFp32Single);
        r.seconds = secs;
        result.tests.push_back(r);
    }

    // -- 3. FP64 FMA throughput (8 independent chains for ILP) --------------
    if (!opts.quick) {
        uint64_t total = runParallel(1, secs, [&](int tid, std::atomic<bool>& stop) {
            double d0 = 0.51 + 0.001 * tid, d1 = 0.62, d2 = 0.73, d3 = 0.84;
            double d4 = 0.55, d5 = 0.66, d6 = 0.77, d7 = 0.88;
            const double m = 0.9999999999, e = 1e-9;
            uint64_t n = 0;
            while (!stop.load(std::memory_order_relaxed)) {
                for (int k = 0; k < 256; ++k) {
                    d0 = d0 * m + e; d1 = d1 * m + e; d2 = d2 * m + e; d3 = d3 * m + e;
                    d4 = d4 * m + e; d5 = d5 * m + e; d6 = d6 * m + e; d7 = d7 * m + e;
                }
                n += 256ull * 8ull;
            }
            double acc = d0 + d1 + d2 + d3 + d4 + d5 + d6 + d7;
            keep(acc, static_cast<double>(n));
            publish(acc);
            return n * 2ull; // one FMA == 2 FLOPs
        });
        CpuTestResult r;
        r.name = "FP64 FMA (8-way ILP)";
        r.unit = "GFLOP/s";
        r.value = static_cast<double>(total) / secs / 1e9;
        r.score = normalise(r.value, kRefFp64Single);
        r.seconds = secs;
        result.tests.push_back(r);
    }

    // -- 4. Memory bandwidth (STREAM triad) --------------------------------
    {
        const size_t elements = 24u * 1024u * 1024u; // 192 MiB per array
        std::vector<double> A(elements), B(elements), C(elements);
        for (size_t i = 0; i < elements; ++i) { A[i] = 1.0 + static_cast<double>(i) * 1e-9; B[i] = 2.0; }

        uint64_t total = runParallel(1, secs, [&](int, std::atomic<bool>& stop) {
            uint64_t bytes = 0;
            const double scalar = 3.0;
            while (!stop.load(std::memory_order_relaxed)) {
                for (size_t i = 0; i < elements; ++i)
                    C[i] = A[i] + scalar * B[i];
                bytes += elements * sizeof(double) * 3ull;
            }
            publish(C[elements - 1]);
            return bytes;
        });
        double gbs = static_cast<double>(total) / secs / 1e9;
        CpuTestResult r;
        r.name = "Memory bandwidth (triad)";
        r.unit = "GB/s";
        r.value = gbs;
        r.score = normalise(gbs, kRefMemSingle);
        r.seconds = secs;
        r.note = bytesHuman(elements * sizeof(double) * 3) + " set";
        result.tests.push_back(r);
    }

    // -- 5. Integer throughput, all cores (scaling) -------------------------
    if (maxThreads > 1) {
        CpuTestResult r = makeIntResult(opts, maxThreads, secs, "Integer ALU - all cores", kRefIntMulti);
        result.scaling = intSingle > 0.0 ? r.value / intSingle : 0.0;
        r.note = "scaling x" + fixed(result.scaling, 2) + " @ " + std::to_string(maxThreads) + "T";
        result.tests.push_back(r);
    } else {
        result.scaling = 1.0;
    }

    // -- sub-scores ---------------------------------------------------------
    std::vector<double> allScores;
    std::vector<double> singleScores;
    for (auto& t : result.tests) {
        allScores.push_back(t.score);
        if (t.threads == 1) singleScores.push_back(t.score);
    }
    result.multiThreadScore  = geomean(allScores);
    result.singleThreadScore = geomean(singleScores);
    result.totalScore        = result.multiThreadScore;
    result.seconds           = nowSeconds() - cpuStart;
    return result;
}

std::string scoreRating(double score) {
    if (score >= 2000) return "Outstanding";
    if (score >= 1400) return "Excellent";
    if (score >= 1000) return "Very good";
    if (score >= 700)  return "Good";
    if (score >= 450)  return "Average";
    if (score >= 250)  return "Below average";
    return "Entry level";
}

} // namespace bench
