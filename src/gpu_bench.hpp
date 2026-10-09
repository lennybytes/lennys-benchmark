// SPDX-License-Identifier: MIT
//
//  gpu_bench.hpp - OpenGL rendering benchmark with a real 3D scene.
//
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace bench {

struct GpuOptions {
    int width  = 1600;
    int height = 900;
    int quality = 3;          // 1..4 tessellation preset
    double seconds = 8.0;     // benchmark duration
    bool showWindow = true;   // create a visible X11 window
    bool vsync = false;
    bool interactive = false;  // run until the window is closed / ESC
    bool wireframe = false;
    int  instances = 0;       // 0 => derived from quality
    int  bloomIterations = 6;
    bool suppressProgress = false;  // no \r progress (used with --monitor)
    std::string screenshotPath;  // optional PPM dump after warm-up
};

// Everything we can learn about the active OpenGL device.
struct GpuDeviceInfo {
    std::string vendor;
    std::string renderer;
    std::string version;
    std::string glsl;
    int major = 0;
    int minor = 0;
    bool vramKnown = false;
    uint64_t vramTotalBytes = 0;
    uint64_t vramAvailableBytes = 0;
    int maxTextureSize = 0;
    int maxSamples = 0;
    int maxVertexAttribs = 0;
    int maxUniformBlockSize = 0;
    std::string contextFlavor;  // "Core"
};

struct GpuBenchResult {
    GpuDeviceInfo device;
    int width = 0;
    int height = 0;
    int quality = 0;
    int instances = 0;
    uint64_t trianglesPerFrame = 0;
    uint64_t verticesPerFrame = 0;
    uint64_t drawCallsPerFrame = 0;
    uint64_t frames = 0;
    double seconds = 0.0;
    double avgFps = 0.0;
    double minFps = 0.0;
    double maxFps = 0.0;
    double p1Fps = 0.0;       // 1 % low
    double medianFps = 0.0;
    double avgFrameMs = 0.0;
    double geometryRate = 0.0;  // triangles / second
    double pixelRate = 0.0;     // pixels / second
    double score = 0.0;
    std::vector<double> frameTimesMs;
};

// Query only the active GL device (opens and immediately closes a window).
bool queryGpuDevice(GpuDeviceInfo& out, std::string& error);

// Run the full GPU benchmark; returns false (and sets `error`) on failure.
bool runGpuBenchmark(const GpuOptions& opts, GpuBenchResult& out, std::string& error);

// Human readable pass/fail style rating.
std::string gpuScoreRating(double score);

} // namespace bench
