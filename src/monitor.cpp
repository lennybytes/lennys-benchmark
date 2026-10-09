// SPDX-License-Identifier: MIT
#include "monitor.hpp"
#include "util.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <thread>

#include <dirent.h>

namespace bench {

namespace {

std::string readFileStrip(const std::string& path) {
    std::ifstream f(path);
    std::string s;
    std::getline(f, s);
    // trim
    size_t a = s.find_first_not_of(" \t\r\n");
    size_t b = s.find_last_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    return s.substr(a, b - a + 1);
}

double readMilliCelsius(const std::string& path) {
    std::string s = readFileStrip(path);
    if (s.empty()) return -1.0;
    try { return std::stod(s) / 1000.0; } catch (...) { return -1.0; }
}

// Locate a hwmon directory by its chip name and return its first temperature
// input path (e.g. k10temp -> temp1_input for Tctl).
std::vector<std::string> findHwmonTempPaths(const std::string& chipName) {
    std::vector<std::string> paths;
    DIR* d = ::opendir("/sys/class/hwmon");
    if (!d) return paths;
    std::string base;
    dirent* e;
    while ((e = ::readdir(d)) != nullptr) {
        std::string name = e->d_name;
        if (name.rfind("hwmon", 0) != 0) continue;
        std::string dir = "/sys/class/hwmon/" + name;
        std::string chip = readFileStrip(dir + "/name");
        if (chip != chipName) continue;
        base = dir;
        break;
    }
    ::closedir(d);
    if (base.empty()) return paths;

    // collect temp*_input files, sorted
    DIR* d2 = ::opendir(base.c_str());
    if (!d2) return paths;
    std::vector<std::string> names;
    while ((e = ::readdir(d2)) != nullptr) {
        std::string n = e->d_name;
        if (n.size() > 6 && n.rfind("temp", 0) == 0 && n.substr(n.size() - 6) == "_input")
            names.push_back(n);
    }
    ::closedir(d2);
    std::sort(names.begin(), names.end());
    for (auto& n : names) paths.push_back(base + "/" + n);
    return paths;
}

std::string findThermalZone(const std::string& type) {
    for (int i = 0; i < 32; ++i) {
        std::string dir = "/sys/class/thermal/thermal_zone" + std::to_string(i);
        std::string t = readFileStrip(dir + "/type");
        if (t == type) return dir + "/temp";
    }
    return {};
}

double parseCsvDouble(const std::string& s) {
    std::string t;
    for (char c : s) {
        if (std::isdigit(static_cast<unsigned char>(c)) || c == '.' || c == '-' ||
            c == '+' || c == 'e' || c == 'E')
            t += c;
        else if (!t.empty())
            break; // stop at first non-numeric run (e.g. "58 C" -> "58")
    }
    if (t.empty()) return -1.0;
    try { return std::stod(t); } catch (...) { return -1.0; }
}

void splitCsv(const std::string& line, std::vector<std::string>& out) {
    out.clear();
    std::string cur;
    for (char c : line) {
        if (c == ',') { out.push_back(cur); cur.clear(); }
        else cur += c;
    }
    out.push_back(cur);
}

} // namespace

SensorReading readSensorsOnce(const std::vector<std::string>& cpuPaths,
                              const std::string& igpuPath,
                              const std::string& nvmePath) {
    SensorReading r;
    for (const auto& p : cpuPaths) {
        double t = readMilliCelsius(p);
        if (t > 0.0) { r.cpuTempC = t; break; }
    }
    if (r.cpuTempC < 0.0) {
        static const std::string fallback = findThermalZone("x86_pkg_temp");
        if (!fallback.empty()) r.cpuTempC = readMilliCelsius(fallback);
    }
    if (!igpuPath.empty()) r.igpuTempC = readMilliCelsius(igpuPath);
    if (!nvmePath.empty()) r.nvmeTempC = readMilliCelsius(nvmePath);

    // Dedicated GPU via nvidia-smi.
    FILE* pipe = ::popen(
        "nvidia-smi --query-gpu=temperature.gpu,temperature.memory,clocks.sm,"
        "clocks.mem,utilization.gpu,utilization.memory,power.draw,fan.speed "
        "--format=csv,noheader,nounits 2>/dev/null",
        "r");
    if (pipe) {
        std::array<char, 512> buf{};
        std::string out;
        size_t n;
        while ((n = std::fread(buf.data(), 1, buf.size(), pipe)) > 0) out.append(buf.data(), n);
        ::pclose(pipe);
        std::istringstream is(out);
        std::string line;
        if (std::getline(is, line) && !line.empty()) {
            std::vector<std::string> f;
            splitCsv(line, f);
            if (f.size() >= 8) {
                r.gpuTempC      = parseCsvDouble(f[0]);
                r.gpuMemTempC   = parseCsvDouble(f[1]);
                r.gpuSmClockMHz = parseCsvDouble(f[2]);
                r.gpuMemClockMHz = parseCsvDouble(f[3]);
                r.gpuUtilPct    = parseCsvDouble(f[4]);
                r.gpuMemUtilPct = parseCsvDouble(f[5]);
                r.gpuPowerW     = parseCsvDouble(f[6]);
                r.gpuFanPct     = parseCsvDouble(f[7]);
            }
        }
    }
    return r;
}

SensorStats Monitor::stats(const std::vector<double>& v) {
    SensorStats s;
    double sum = 0.0;
    double mn = 1e30, mx = -1e30;
    for (double x : v) {
        if (x < 0.0 || std::isnan(x)) continue;
        mn = std::min(mn, x);
        mx = std::max(mx, x);
        sum += x;
        ++s.count;
    }
    if (s.count == 0) return s;
    s.min = mn;
    s.max = mx;
    s.avg = sum / s.count;
    return s;
}

void Monitor::start(double intervalSeconds, bool live) {
    if (running_) return;
    interval_ = std::max(0.1, intervalSeconds);
    live_ = live;
    samples_.clear();

    // Cache sensor paths once.
    cpuTempPaths_ = findHwmonTempPaths("k10temp");
    if (cpuTempPaths_.empty()) cpuTempPaths_ = findHwmonTempPaths("coretemp");
    auto igpu = findHwmonTempPaths("amdgpu");
    if (!igpu.empty()) igpuTempPath_ = igpu.front();
    else {
        auto intel = findHwmonTempPaths("i915");
        if (!intel.empty()) igpuTempPath_ = intel.front();
    }
    auto nvme = findHwmonTempPaths("nvme");
    if (!nvme.empty()) nvmeTempPath_ = nvme.front();

    running_ = true;
    worker_ = std::thread([this]() { threadLoop(); });
}

void Monitor::stop() {
    running_ = false;
    if (worker_.joinable()) worker_.join();
}

void Monitor::threadLoop() {
    while (running_) {
        SensorReading r = readSensorsOnce(cpuTempPaths_, igpuTempPath_, nvmeTempPath_);
        samples_.push_back(r);
        if (live_) {
            std::printf("%s\n", formatReading(r).c_str());
            std::fflush(stdout);
        }
        std::this_thread::sleep_for(std::chrono::duration<double>(interval_));
    }
}

SensorStats Monitor::cpuTemp() const {
    std::vector<double> v;
    for (auto& s : samples_) v.push_back(s.cpuTempC);
    return stats(v);
}
SensorStats Monitor::gpuTemp() const {
    std::vector<double> v;
    for (auto& s : samples_) v.push_back(s.gpuTempC);
    return stats(v);
}
SensorStats Monitor::gpuClock() const {
    std::vector<double> v;
    for (auto& s : samples_) v.push_back(s.gpuSmClockMHz);
    return stats(v);
}
SensorStats Monitor::gpuUtil() const {
    std::vector<double> v;
    for (auto& s : samples_) v.push_back(s.gpuUtilPct);
    return stats(v);
}
SensorStats Monitor::gpuPower() const {
    std::vector<double> v;
    for (auto& s : samples_) v.push_back(s.gpuPowerW);
    return stats(v);
}
SensorStats Monitor::gpuMemClock() const {
    std::vector<double> v;
    for (auto& s : samples_) v.push_back(s.gpuMemClockMHz);
    return stats(v);
}

std::string formatReading(const SensorReading& r) {
    std::ostringstream o;
    o << "  [monitor] ";
    if (r.cpuTempC >= 0)      o << "CPU " << fixed(r.cpuTempC, 1) << "C  ";
    if (r.igpuTempC >= 0)     o << "iGPU " << fixed(r.igpuTempC, 0) << "C  ";
    if (r.gpuTempC >= 0)      o << "GPU " << fixed(r.gpuTempC, 0) << "C  ";
    if (r.nvmeTempC >= 0)     o << "SSD " << fixed(r.nvmeTempC, 0) << "C  ";
    if (r.gpuSmClockMHz >= 0) o << "| SM " << fixed(r.gpuSmClockMHz, 0) << " MHz  ";
    if (r.gpuMemClockMHz >= 0) o << "MEM " << fixed(r.gpuMemClockMHz, 0) << " MHz  ";
    if (r.gpuUtilPct >= 0)    o << "| util " << fixed(r.gpuUtilPct, 0) << "%  ";
    if (r.gpuPowerW >= 0)     o << "power " << fixed(r.gpuPowerW, 1) << " W";
    return o.str();
}

} // namespace bench
