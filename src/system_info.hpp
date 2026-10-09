// SPDX-License-Identifier: MIT
//
//  system_info.hpp - CPU, GPU and platform discovery.
//
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace bench {

struct CpuCache {
    std::string level;    // "L1d", "L2", "L3" ...
    uint64_t sizeBytes = 0;
};

struct CpuInfo {
    std::string vendor;          // GenuineIntel / AuthenticAMD
    std::string model;           // human readable brand string
    std::string architecture;    // x86_64 / aarch64
    int physicalCores = 0;
    int logicalCores  = 0;
    int sockets       = 0;
    double baseMHz    = 0.0;
    double maxMHz     = 0.0;
    uint64_t totalRamBytes = 0;
    std::vector<std::string> features;   // avx2, avx512f, ...
    std::vector<CpuCache>    caches;
};

// A GPU discovered by the OS (PCI enumeration / vendor tools), independent of
// whether an OpenGL context can currently be created on it.
struct GpuInfo {
    std::string name;            // "NVIDIA GeForce RTX 3080 Laptop GPU"
    std::string vendor;          // NVIDIA / AMD / Intel
    std::string pciId;
    std::string driver;
    uint64_t dedicatedVramBytes = 0;
    bool isDiscrete = false;
    bool hasVramInfo = false;
    bool isActive = false;       // currently selected for rendering
};

CpuInfo detectCpu();
std::vector<GpuInfo> detectGpus();
std::string hostname();
std::string osName();
std::string kernelVersion();

} // namespace bench
