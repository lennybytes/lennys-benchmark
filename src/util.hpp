// SPDX-License-Identifier: MIT
//
//  util.hpp - small helpers shared across the benchmark: ANSI styling,
//  high-resolution timing, formatting and JSON escaping.
//
#pragma once

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <sstream>
#include <iomanip>
#include <vector>

namespace bench {

// ---------------------------------------------------------------------------
//  Terminal styling
// ---------------------------------------------------------------------------
namespace ansi {
inline const char* reset()   { return "\033[0m";  }
inline const char* bold()    { return "\033[1m";  }
inline const char* dim()     { return "\033[2m";  }
inline const char* red()     { return "\033[31m"; }
inline const char* green()   { return "\033[32m"; }
inline const char* yellow()  { return "\033[33m"; }
inline const char* blue()    { return "\033[34m"; }
inline const char* magenta() { return "\033[35m"; }
inline const char* cyan()    { return "\033[36m"; }
inline const char* white()   { return "\033[97m"; }
inline const char* gray()    { return "\033[90m"; }

inline bool& enabled() {
    static bool on = true;
    return on;
}
} // namespace ansi

// Colourise a string when colour output is enabled.
inline std::string paint(const std::string& s, const char* colour) {
    if (!ansi::enabled()) return s;
    return std::string(colour) + s + ansi::reset();
}

// ---------------------------------------------------------------------------
//  Timing
// ---------------------------------------------------------------------------
class Timer {
public:
    using clock = std::chrono::steady_clock;

    Timer() : start_(clock::now()), mark_(start_) {}

    void reset() { start_ = mark_ = clock::now(); }

    double seconds() const {
        return std::chrono::duration<double>(clock::now() - start_).count();
    }
    double ms() const { return seconds() * 1000.0; }

    // Seconds elapsed since the previous mark (also advances the mark).
    double markSecs() {
        auto n = clock::now();
        double d = std::chrono::duration<double>(n - mark_).count();
        mark_ = n;
        return d;
    }

private:
    clock::time_point start_;
    clock::time_point mark_;
};

// Wall clock in seconds since the process started.
inline double nowSeconds() {
    using clock = std::chrono::steady_clock;
    static const clock::time_point origin = clock::now();
    return std::chrono::duration<double>(clock::now() - origin).count();
}

// ---------------------------------------------------------------------------
//  Formatting
// ---------------------------------------------------------------------------
inline std::string humanNumber(double v, int precision = 2) {
    static const char* suffixes[] = { "", "K", "M", "G", "T", "P" };
    int idx = 0;
    double a = std::fabs(v);
    while (a >= 1000.0 && idx < 5) { a /= 1000.0; v /= 1000.0; ++idx; }
    std::ostringstream os;
    os << std::fixed << std::setprecision(precision) << v << suffixes[idx];
    return os.str();
}

inline std::string fixed(double v, int precision) {
    std::ostringstream os;
    os << std::fixed << std::setprecision(precision) << v;
    return os.str();
}

inline std::string bytesHuman(uint64_t bytes) {
    static const char* suffixes[] = { "B", "KiB", "MiB", "GiB", "TiB" };
    double v = static_cast<double>(bytes);
    int idx = 0;
    while (v >= 1024.0 && idx < 4) { v /= 1024.0; ++idx; }
    std::ostringstream os;
    os << std::fixed << std::setprecision(idx == 0 ? 0 : 2) << v << ' ' << suffixes[idx];
    return os.str();
}

// Reproduce a string `s` repeated `n` times.
inline std::string repeat(const std::string& s, int n) {
    std::string out;
    out.reserve(s.size() * static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) out += s;
    return out;
}

// Escape a string for embedding into JSON.
inline std::string jsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
//  Deterministic fast RNG (xoshiro256**) - used by CPU workers & the GPU side
//  shares the same seed through the command line.
// ---------------------------------------------------------------------------
class Rng {
public:
    explicit Rng(uint64_t seed = 0x9E3779B97F4A7C15ull) { seedState(seed); }

    void seedState(uint64_t s) {
        for (int i = 0; i < 4; ++i) {
            s += 0x9E3779B97F4A7C15ull;
            uint64_t z = s;
            z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
            z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
            state_[i] = z ^ (z >> 31);
        }
    }

    uint64_t next() {
        const uint64_t result = rotl(state_[1] * 5, 7) * 9;
        const uint64_t t = state_[1] << 17;
        state_[2] ^= state_[0];
        state_[3] ^= state_[1];
        state_[1] ^= state_[2];
        state_[0] ^= state_[3];
        state_[2] ^= t;
        state_[3] = rotl(state_[3], 45);
        return result;
    }

    double nextDouble() {
        return (next() >> 11) * (1.0 / 9007199254740992.0);
    }

private:
    static uint64_t rotl(uint64_t x, int k) { return (x << k) | (x >> (64 - k)); }
    uint64_t state_[4]{};
};

// FNV-1a over a buffer - used for checksums so the optimiser cannot remove work.
inline uint64_t fnv1a(const void* data, size_t len, uint64_t h = 1469598103934665603ull) {
    const unsigned char* p = static_cast<const unsigned char*>(data);
    for (size_t i = 0; i < len; ++i) {
        h ^= p[i];
        h *= 1099511628211ull;
    }
    return h;
}

} // namespace bench
