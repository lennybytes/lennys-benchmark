// SPDX-License-Identifier: MIT
#include "system_info.hpp"
#include "util.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <set>
#include <sstream>
#include <thread>
#include <unistd.h>
#include <sys/utsname.h>

namespace bench {

namespace {

std::string execCapture(const std::string& cmd) {
    std::string out;
    FILE* pipe = ::popen((cmd + " 2>/dev/null").c_str(), "r");
    if (!pipe) return out;
    std::array<char, 4096> buf{};
    size_t n;
    while ((n = std::fread(buf.data(), 1, buf.size(), pipe)) > 0) {
        out.append(buf.data(), n);
        if (out.size() > (1u << 20)) break; // hard safety cap
    }
    ::pclose(pipe);
    return out;
}

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

std::vector<std::string> split(const std::string& s, char delim) {
    std::vector<std::string> out;
    std::string cur;
    std::istringstream is(s);
    while (std::getline(is, cur, delim)) out.push_back(cur);
    return out;
}

uint64_t parseCacheSize(const std::string& raw) {
    if (raw.empty()) return 0;
    char unit = raw.back();
    std::string num = raw;
    uint64_t mult = 1;
    if (unit == 'K' || unit == 'k') { num = raw.substr(0, raw.size() - 1); mult = 1024ull; }
    else if (unit == 'M' || unit == 'm') { num = raw.substr(0, raw.size() - 1); mult = 1024ull * 1024ull; }
    try { return static_cast<uint64_t>(std::stoull(num)) * mult; } catch (...) { return 0; }
}

bool containsIgnoreCase(const std::string& hay, const std::string& needle) {
    std::string a = hay, b = needle;
    std::transform(a.begin(), a.end(), a.begin(), [](unsigned char c){ return std::tolower(c); });
    std::transform(b.begin(), b.end(), b.begin(), [](unsigned char c){ return std::tolower(c); });
    return a.find(b) != std::string::npos;
}

} // namespace

CpuInfo detectCpu() {
    CpuInfo info;

    struct utsname uts {};
    if (::uname(&uts) == 0) info.architecture = uts.machine;

    std::ifstream f("/proc/cpuinfo");
    std::string line;
    std::set<std::pair<int, int>> physicalCoreIds;
    std::set<std::string> featureSet;
    std::set<int> sockets;
    int logical = 0;
    double mhzSum = 0.0;
    int mhzCount = 0;
    while (std::getline(f, line)) {
        auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string key = trim(line.substr(0, colon));
        std::string val = trim(line.substr(colon + 1));
        if (key == "vendor_id") {
            if (info.vendor.empty()) info.vendor = val;
        } else if (key == "model name" || key == "Processor") {
            if (info.model.empty()) info.model = val;
        } else if (key == "cpu MHz") {
            try { mhzSum += std::stod(val); ++mhzCount; } catch (...) {}
        } else if (key == "processor") {
            ++logical;
        } else if (key == "physical id") {
            try { sockets.insert(std::stoi(val)); } catch (...) {}
        } else if (key == "core id") {
            int coreId = 0;
            try { coreId = std::stoi(val); } catch (...) {}
            // Fall back to physical id 0 when the field is absent (common on ARM).
            physicalCoreIds.insert({0, coreId});
        } else if (key == "flags" || key == "Features") {
            for (auto& tok : split(val, ' ')) featureSet.insert(tok);
        }
    }
    info.logicalCores = logical > 0 ? logical : static_cast<int>(std::thread::hardware_concurrency());
    info.physicalCores = physicalCoreIds.empty() ? info.logicalCores
                                                 : static_cast<int>(physicalCoreIds.size());
    info.sockets = sockets.empty() ? 1 : static_cast<int>(sockets.size());
    if (mhzCount > 0) {
        info.baseMHz = mhzSum / mhzCount;
        info.maxMHz  = info.baseMHz;
    }

    {
        std::ifstream mf("/sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_max_freq");
        std::string v;
        if (mf >> v) {
            try { info.maxMHz = std::stod(v) / 1000.0; } catch (...) {}
        }
    }

    for (int idx = 0; idx < 10; ++idx) {
        std::string base = "/sys/devices/system/cpu/cpu0/cache/index" + std::to_string(idx) + "/";
        std::ifstream lv(base + "level"), ty(base + "type"), sz(base + "size");
        std::string l, t, s;
        if (!(lv >> l)) break;
        ty >> t;
        sz >> s;
        CpuCache c;
        c.level = "L" + l;
        if (t == "Data") c.level += "d";
        else if (t == "Instruction") c.level += "i";
        else if (t == "Unified") c.level += "u";
        c.sizeBytes = parseCacheSize(trim(s));
        info.caches.push_back(c);
    }

    for (auto& ft : featureSet) info.features.push_back(ft);

    {
        std::ifstream mi("/proc/meminfo");
        std::string k, unit;
        uint64_t v;
        while (mi >> k >> v >> unit) {
            if (k == "MemTotal:") { info.totalRamBytes = v * 1024ull; break; }
        }
    }

    if (info.vendor.empty()) info.vendor = "Unknown";
    if (info.model.empty())  info.model  = "Unknown CPU";
    if (info.sockets == 0)   info.sockets = 1;
    return info;
}

std::vector<GpuInfo> detectGpus() {
    std::vector<GpuInfo> gpus;

    // ---- NVIDIA via nvidia-smi (most reliable VRAM + driver data) ---------
    std::string nv = execCapture(
        "nvidia-smi --query-gpu=name,memory.total,driver_version --format=csv,noheader");
    if (!nv.empty()) {
        std::istringstream is(nv);
        std::string ln;
        while (std::getline(is, ln)) {
            if (trim(ln).empty()) continue;
            auto parts = split(ln, ',');
            if (parts.size() < 3) continue;
            GpuInfo g;
            g.vendor = "NVIDIA";
            g.name   = trim(parts[0]);
            uint64_t mib = 0;
            try { mib = static_cast<uint64_t>(std::stoull(trim(parts[1]))); } catch (...) {}
            g.dedicatedVramBytes = mib * 1024ull * 1024ull;
            g.driver = "NVIDIA " + trim(parts[2]);
            g.isDiscrete = true;
            g.hasVramInfo = mib > 0;
            gpus.push_back(g);
        }
    }

    // ---- Generic PCI enumeration via lspci --------------------------------
    std::string lp = execCapture("lspci -mm -nn");
    std::istringstream is(lp);
    std::string ln;
    while (std::getline(is, ln)) {
        bool isDisplay = ln.find("VGA compatible controller") != std::string::npos ||
                         ln.find("3D controller") != std::string::npos ||
                         ln.find("Display controller") != std::string::npos;
        if (!isDisplay) continue;

        std::vector<std::string> fields;
        {
            std::string cur;
            bool inQ = false;
            for (char c : ln) {
                if (c == '"') { inQ = !inQ; if (!inQ) { fields.push_back(cur); cur.clear(); } continue; }
                if (inQ) cur += c;
            }
        }
        std::string slot;
        {
            auto sp = ln.find(' ');
            slot = ln.substr(0, sp == std::string::npos ? ln.size() : sp);
        }

        GpuInfo g;
        g.pciId = slot;
        // fields: [class][vendor][device][...]
        if (fields.size() >= 3) g.name = trim(fields[2]);
        if (g.name.empty()) g.name = "Unknown GPU";

        if (containsIgnoreCase(g.name, "nvidia") ||
            containsIgnoreCase(g.name, "geforce") ||
            containsIgnoreCase(g.name, "quadro")) g.vendor = "NVIDIA";
        else if (containsIgnoreCase(g.name, "amd") ||
                 containsIgnoreCase(g.name, "ati") ||
                 containsIgnoreCase(g.name, "radeon")) g.vendor = "AMD";
        else if (containsIgnoreCase(g.name, "intel"))  g.vendor = "Intel";
        else                                            g.vendor = "Unknown";

        if (g.vendor == "NVIDIA") g.isDiscrete = true;

        // nvidia-smi is authoritative: skip any further NVIDIA entries.
        bool dup = false;
        for (auto& e : gpus) {
            if (g.vendor == "NVIDIA" && e.vendor == "NVIDIA") {
                dup = true;
                if (e.pciId.empty()) e.pciId = slot;
                break;
            }
            if (e.vendor == g.vendor && !e.name.empty() &&
                containsIgnoreCase(g.name, e.name.substr(0, std::min<size_t>(e.name.size(), 10)))) {
                dup = true;
                if (e.pciId.empty()) e.pciId = slot;
                break;
            }
        }
        if (!dup) gpus.push_back(g);
    }

    return gpus;
}

std::string hostname() {
    char buf[256] = {0};
    if (::gethostname(buf, sizeof(buf) - 1) == 0) return buf;
    return "unknown";
}

std::string osName() {
    std::ifstream f("/etc/os-release");
    std::string line;
    while (std::getline(f, line)) {
        if (line.rfind("PRETTY_NAME=", 0) == 0) {
            std::string v = line.substr(12);
            if (v.size() >= 2 && v.front() == '"' && v.back() == '"') v = v.substr(1, v.size() - 2);
            return v;
        }
    }
    return "Unknown OS";
}

std::string kernelVersion() {
    struct utsname uts {};
    if (::uname(&uts) == 0) return std::string(uts.sysname) + " " + uts.release;
    return "unknown";
}

} // namespace bench
