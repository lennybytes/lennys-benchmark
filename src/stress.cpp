// SPDX-License-Identifier: MIT
#include "stress.hpp"
#include "util.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#if defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#define BENCH_X86 1
#else
#define BENCH_X86 0
#endif

namespace bench {

namespace {

volatile std::sig_atomic_t g_signaled = 0;
std::atomic<bool> g_stop{false};

extern "C" void onSignal(int) {
    g_signaled = 1;
    g_stop.store(true, std::memory_order_relaxed);
}

enum class Isa { Base = 0, Avx2 = 1, Avx512 = 2 };

// FMA instructions retired per kernel iteration, used for the GFLOP/s estimate.
// Each kernel issues 4 FMAs per iteration; a FMA is two flops per lane.
double flopsPerIter(Isa isa) {
    switch (isa) {
        case Isa::Avx512: return 8.0 * 16.0 * 2.0;  // 256
        case Isa::Avx2:   return 8.0 * 8.0 * 2.0;   // 128
        default:          return 8.0 * 2.0;         //  16
    }
}

Isa detectIsa() {
#if BENCH_X86
    __builtin_cpu_init();
    if (__builtin_cpu_supports("avx512f") && __builtin_cpu_supports("fma"))
        return Isa::Avx512;
    if (__builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma"))
        return Isa::Avx2;
#endif
    return Isa::Base;
}

uint64_t hashFloat(uint64_t s, float f) {
    uint32_t u = 0;
    std::memcpy(&u, &f, sizeof(u));
    return s * 1099511628211ull ^ u;
}

uint64_t stressBase(uint64_t iters, uint64_t seed) {
    float a0 = static_cast<float>(seed & 0xffffu) + 1.0f;
    const float b = 0.9999999f, c = 5e-7f;
    float a1 = a0, a2 = a0, a3 = a0, a4 = a0, a5 = a0, a6 = a0, a7 = a0;
    uint32_t x = static_cast<uint32_t>(seed) | 1u;
    for (uint64_t i = 0; i < iters; ++i) {
        a0 = a0 * b + c;   // eight independent chains keep the FMA units busy
        a1 = a1 * b + c;
        a2 = a2 * b + c;
        a3 = a3 * b + c;
        a4 = a4 * b + c;
        a5 = a5 * b + c;
        a6 = a6 * b + c;
        a7 = a7 * b + c;
        x = x * 1664525u + 1013904223u;
        x ^= x >> 13;
    }
    uint64_t s = x;
    for (float v : {a0, a1, a2, a3, a4, a5, a6, a7}) s = hashFloat(s, v);
    return s;
}

#if BENCH_X86
__attribute__((target("avx2,fma")))
uint64_t stressAvx2(uint64_t iters, uint64_t seed) {
    const __m256 b = _mm256_set1_ps(0.9999999f);
    const __m256 c = _mm256_set1_ps(5e-7f);
    __m256 a0 = _mm256_set1_ps(1.0f + 1e-7f * static_cast<float>(seed & 0xffu));
    __m256 a1 = a0, a2 = a0, a3 = a0, a4 = a0, a5 = a0, a6 = a0, a7 = a0;
    for (uint64_t i = 0; i < iters; ++i) {
        a0 = _mm256_fmadd_ps(a0, b, c);
        a1 = _mm256_fmadd_ps(a1, b, c);
        a2 = _mm256_fmadd_ps(a2, b, c);
        a3 = _mm256_fmadd_ps(a3, b, c);
        a4 = _mm256_fmadd_ps(a4, b, c);
        a5 = _mm256_fmadd_ps(a5, b, c);
        a6 = _mm256_fmadd_ps(a6, b, c);
        a7 = _mm256_fmadd_ps(a7, b, c);
    }
    __m256 r = _mm256_add_ps(
        _mm256_add_ps(_mm256_add_ps(a0, a1), _mm256_add_ps(a2, a3)),
        _mm256_add_ps(_mm256_add_ps(a4, a5), _mm256_add_ps(a6, a7)));
    alignas(32) float out[8];
    _mm256_store_ps(out, r);
    uint64_t s = seed;
    for (float v : out) s = hashFloat(s, v);
    return s;
}

__attribute__((target("avx512f")))
uint64_t stressAvx512(uint64_t iters, uint64_t seed) {
    const __m512 b = _mm512_set1_ps(0.9999999f);
    const __m512 c = _mm512_set1_ps(5e-7f);
    __m512 a0 = _mm512_set1_ps(1.0f + 1e-7f * static_cast<float>(seed & 0xffu));
    __m512 a1 = a0, a2 = a0, a3 = a0, a4 = a0, a5 = a0, a6 = a0, a7 = a0;
    for (uint64_t i = 0; i < iters; ++i) {
        a0 = _mm512_fmadd_ps(a0, b, c);
        a1 = _mm512_fmadd_ps(a1, b, c);
        a2 = _mm512_fmadd_ps(a2, b, c);
        a3 = _mm512_fmadd_ps(a3, b, c);
        a4 = _mm512_fmadd_ps(a4, b, c);
        a5 = _mm512_fmadd_ps(a5, b, c);
        a6 = _mm512_fmadd_ps(a6, b, c);
        a7 = _mm512_fmadd_ps(a7, b, c);
    }
    __m512 r = _mm512_add_ps(
        _mm512_add_ps(_mm512_add_ps(a0, a1), _mm512_add_ps(a2, a3)),
        _mm512_add_ps(_mm512_add_ps(a4, a5), _mm512_add_ps(a6, a7)));
    alignas(64) float out[16];
    _mm512_store_ps(out, r);
    uint64_t s = seed;
    for (float v : out) s = hashFloat(s, v);
    return s;
}
#endif

inline uint64_t runKernel(Isa isa, uint64_t iters, uint64_t seed) {
    switch (isa) {
#if BENCH_X86
        case Isa::Avx512: return stressAvx512(iters, seed);
        case Isa::Avx2:   return stressAvx2(iters, seed);
#endif
        default:          return stressBase(iters, seed);
    }
}

const char* isaName(Isa isa) {
    switch (isa) {
        case Isa::Avx512: return "AVX-512";
        case Isa::Avx2:   return "AVX2+FMA";
        default:          return "scalar/SSE2";
    }
}

} // namespace

void installSignalHandler() {
    static bool done = false;
    if (done) return;
    done = true;
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
}

bool interrupted() { return g_signaled != 0; }

// ---------------------------------------------------------------------------

struct CpuStress::Impl {
    struct Worker {
        int tid = 0;
        Isa isa = Isa::Base;
        size_t memFloats = 0;
        std::vector<float> mem;
        std::atomic<uint64_t>* ops = nullptr;
        std::atomic<uint64_t>* bytes = nullptr;
        std::atomic<uint64_t>* sink = nullptr;

        void run() {
            mem.assign(memFloats, 1.0f + 1e-6f * static_cast<float>(tid));
            uint64_t seed = 0x9E3779B97F4A7C15ull * static_cast<uint64_t>(tid + 1);
            uint64_t localOps = 0, localBytes = 0, acc = 0;
            constexpr uint64_t kChunk = 1u << 20;   // ~0.4 ms of FMA per chunk
            while (!g_stop.load(std::memory_order_relaxed) && !g_signaled) {
                acc ^= runKernel(isa, kChunk, seed + localOps);
                localOps += kChunk;
                // Streaming triad over a cache-sized buffer: keeps the memory
                // subsystem busy without starving the cores of FMA work.
                if (memFloats) {
                    float m = 0.999999f;
                    for (size_t i = 0; i < memFloats; ++i)
                        mem[i] = mem[i] * m + 1.0f;
                    localBytes += static_cast<uint64_t>(memFloats) * sizeof(float) * 2;
                }
                if (localOps >= kChunk * 4) {
                    ops->fetch_add(localOps, std::memory_order_relaxed);
                    bytes->fetch_add(localBytes, std::memory_order_relaxed);
                    localOps = localBytes = 0;
                }
            }
            ops->fetch_add(localOps, std::memory_order_relaxed);
            bytes->fetch_add(localBytes, std::memory_order_relaxed);
            sink->fetch_xor(acc, std::memory_order_relaxed);
        }
    };

    Isa isa = Isa::Base;
    int nthreads = 0;
    std::atomic<uint64_t> ops{0};
    std::atomic<uint64_t> bytes{0};
    std::atomic<uint64_t> sink{0};
    std::atomic<bool> running{false};
    std::vector<std::thread> threads;
    std::vector<std::unique_ptr<Worker>> workers;
    std::chrono::steady_clock::time_point t0;
};

CpuStress::~CpuStress() {
    if (impl_) {
        g_stop.store(true, std::memory_order_relaxed);
        for (auto& t : impl_->threads)
            if (t.joinable()) t.join();
        delete impl_;
    }
}

void CpuStress::start(int threads) {
    if (impl_ && impl_->running.load()) return;
    if (!impl_) impl_ = new Impl();

    int n = threads > 0 ? threads : static_cast<int>(std::thread::hardware_concurrency());
    if (n < 1) n = 1;
    impl_->nthreads = n;
    impl_->isa = detectIsa();
    impl_->ops.store(0);
    impl_->bytes.store(0);
    impl_->sink.store(0);
    impl_->workers.clear();
    impl_->threads.clear();

    // Keep the scratch buffer cache-sized (~16 MiB total) so the cores stay
    // compute-bound instead of stalling on DRAM.
    const size_t perThread = std::min<size_t>(
        1u << 20,
        std::max<size_t>(1u << 14,
                         (static_cast<size_t>(16) << 20) / static_cast<size_t>(n) / sizeof(float)));

    g_stop.store(false, std::memory_order_relaxed);
    impl_->t0 = std::chrono::steady_clock::now();

    for (int i = 0; i < n; ++i) {
        auto w = std::make_unique<Impl::Worker>();
        w->tid = i;
        w->isa = impl_->isa;
        w->memFloats = perThread;
        w->ops = &impl_->ops;
        w->bytes = &impl_->bytes;
        w->sink = &impl_->sink;
        Impl::Worker* wp = w.get();
        impl_->workers.push_back(std::move(w));
        impl_->threads.emplace_back([wp] { wp->run(); });
    }
    impl_->running.store(true);

    std::printf("  %sCPU stress online%s  %d threads  -  ISA %s  -  %.0f MiB scratch\n",
                ansi::green(), ansi::reset(), n, isaName(impl_->isa),
                static_cast<double>(perThread) * sizeof(float) * static_cast<double>(n) / (1024.0 * 1024.0));
    std::fflush(stdout);
}

void CpuStress::stop() {
    if (!impl_ || !impl_->running.load()) return;
    g_stop.store(true, std::memory_order_relaxed);
    for (auto& t : impl_->threads)
        if (t.joinable()) t.join();
    impl_->threads.clear();
    impl_->workers.clear();
    impl_->running.store(false);

    double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - impl_->t0).count();
    if (el <= 0.0) el = 1e-9;
    double gflops = static_cast<double>(impl_->ops.load()) * flopsPerIter(impl_->isa) / el / 1e9;
    double gbs = static_cast<double>(impl_->bytes.load()) / el / 1e9;
    std::printf("\n  %sCPU stress stopped%s  %.1f s  -  %.1f GFLOP/s  -  %.1f GB/s memory\n",
                ansi::green(), ansi::reset(), el, gflops, gbs);
    std::fflush(stdout);
}

bool CpuStress::running() const { return impl_ && impl_->running.load(); }

double CpuStress::gflops() const {
    if (!impl_) return 0.0;
    double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - impl_->t0).count();
    if (el <= 0.0) return 0.0;
    return static_cast<double>(impl_->ops.load()) * flopsPerIter(impl_->isa) / el / 1e9;
}

double CpuStress::gigabytesPerSecond() const {
    if (!impl_) return 0.0;
    double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - impl_->t0).count();
    if (el <= 0.0) return 0.0;
    return static_cast<double>(impl_->bytes.load()) / el / 1e9;
}

int CpuStress::threadCount() const { return impl_ ? impl_->nthreads : 0; }
const char* CpuStress::isa() const { return impl_ ? isaName(impl_->isa) : "n/a"; }

void CpuStress::wait(double seconds) {
    if (!impl_) return;
    auto start = std::chrono::steady_clock::now();
    double last = -1.0;
    while (true) {
        if (interrupted()) break;
        double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        if (seconds > 0.0 && el >= seconds) break;
        if (el - last >= 0.5) {
            last = el;
            if (seconds > 0.0)
                std::printf("\r  %sstressing%s  %5.1f / %.0f s   %s %.0f GFLOP/s%s   %.1f GB/s   ",
                            ansi::yellow(), ansi::reset(), el, seconds,
                            ansi::gray(), gflops(), ansi::reset(), gigabytesPerSecond());
            else
                std::printf("\r  %sstressing%s  %5.1f s (Ctrl+C to stop)   %s %.0f GFLOP/s%s   %.1f GB/s   ",
                            ansi::yellow(), ansi::reset(), el,
                            ansi::gray(), gflops(), ansi::reset(), gigabytesPerSecond());
            std::fflush(stdout);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    std::printf("\n");
}

} // namespace bench
