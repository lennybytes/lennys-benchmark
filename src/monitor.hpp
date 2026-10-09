// SPDX-License-Identifier: MIT
//
//  monitor.hpp - background temperature / clock / power sampler.
//
#pragma once

#include <cstdint>
#include <string>
#include <thread>
#include <vector>

namespace bench {

struct SensorReading {
    double cpuTempC      = -1.0;  // k10temp Tctl / CPU package
    double igpuTempC     = -1.0;  // AMD/NVIDIA integrated edge
    double nvmeTempC     = -1.0;  // SSD composite
    double gpuTempC      = -1.0;  // dedicated GPU (nvidia-smi)
    double gpuMemTempC   = -1.0;
    double gpuSmClockMHz = -1.0;
    double gpuMemClockMHz = -1.0;
    double gpuUtilPct    = -1.0;
    double gpuMemUtilPct = -1.0;
    double gpuPowerW     = -1.0;
    double gpuFanPct     = -1.0;
};

// Aggregate for one metric across all samples.
struct SensorStats {
    double min = 0.0;
    double avg = 0.0;
    double max = 0.0;
    int count = 0;
    bool valid() const { return count > 0; }
};

// Samples sensors in a background thread until stop() is called.
class Monitor {
public:
    ~Monitor() { stop(); }

    // intervalSeconds: sampling period. live: print one line per sample.
    void start(double intervalSeconds, bool live);
    void stop();

    bool running() const { return running_; }
    const std::vector<SensorReading>& samples() const { return samples_; }

    // Per-metric statistics over the collected samples.
    static SensorStats stats(const std::vector<double>& v);

    SensorStats cpuTemp()  const;
    SensorStats gpuTemp()  const;
    SensorStats gpuClock() const;
    SensorStats gpuUtil()  const;
    SensorStats gpuPower() const;
    SensorStats gpuMemClock() const;

private:
    void threadLoop();

    std::thread worker_;
    bool running_ = false;
    bool live_ = false;
    double interval_ = 1.0;
    std::vector<SensorReading> samples_;
    std::vector<std::string> cpuTempPaths_;
    std::string igpuTempPath_;
    std::string nvmeTempPath_;
};

// Read every sensor once (blocking; may spawn nvidia-smi).
SensorReading readSensorsOnce(const std::vector<std::string>& cpuPaths,
                              const std::string& igpuPath,
                              const std::string& nvmePath);

// One-shot summary helper used by --monitor and the report.
std::string formatReading(const SensorReading& r);

} // namespace bench
