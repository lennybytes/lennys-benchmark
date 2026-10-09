// SPDX-License-Identifier: MIT
//
//  stress.hpp - sustained maximum-load CPU stress test and signal handling.
//
#pragma once

#include <cstdint>

namespace bench {

// Installs SIGINT/SIGTERM handlers so that long-running stress tests can shut
// down cleanly and still print a summary. Idempotent; call once from main.
void installSignalHandler();

// True once SIGINT/SIGTERM was received.
bool interrupted();

// Concurrent maximum-load CPU stress. Each worker keeps every logical core at
// 100 % with an FMA kernel (AVX-512 / AVX2+FMA / scalar, chosen at runtime) plus
// a streaming memory-triad to also load the memory controller and cache.
class CpuStress {
public:
    CpuStress() = default;
    ~CpuStress();
    CpuStress(const CpuStress&) = delete;
    CpuStress& operator=(const CpuStress&) = delete;

    // Launch the workers. `threads <= 0` uses every logical CPU.
    void start(int threads = 0);

    // Stop the workers, join them and print a summary line.
    void stop();

    // Block for `seconds` (or until interrupted when `seconds <= 0`), printing
    // live throughput once per second.
    void wait(double seconds);

    bool running() const;
    double gflops() const;
    double gigabytesPerSecond() const;
    int threadCount() const;
    const char* isa() const;

private:
    struct Impl;
    Impl* impl_ = nullptr;
};

} // namespace bench
