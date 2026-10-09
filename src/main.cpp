// SPDX-License-Identifier: MIT
//
//  main.cpp - command line interface, result reporting and JSON export for
//  "lennys-benchmark", a GPU + CPU benchmark.
//
#include "cpu_bench.hpp"
#include "gpu_bench.hpp"
#include "monitor.hpp"
#include "stress.hpp"
#include "system_info.hpp"
#include "util.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <unistd.h>

using namespace bench;

namespace {

constexpr const char* kName    = "lennys-benchmark";
constexpr const char* kVersion = "1.0.0";

struct Options {
    bool runCpu = true;
    bool runGpu = true;
    bool showInfo = false;
    bool listGpus = false;
    bool monitor = false;
    bool stress = false;       // sustained maximum-load mode
    bool stressCpu = false;    // stress the CPU
    bool stressGpu = false;    // stress the GPU
    double stressSeconds = 0.0;  // 0 => until interrupted
    bool json = false;
    std::string jsonPath;
    std::string gpuVendor = "auto";   // auto | nvidia | amd | system
    uint64_t seed = 0xC0FFEE123456789ull;

    CpuOptions cpu;
    GpuOptions gpu;
};

void printBanner() {
    const std::string bar(60, '=');
    std::printf("%s%s  %s%s\n", ansi::bold(), ansi::cyan(), bar.c_str(), ansi::reset());
    std::printf("  %slennys-benchmark%s  -  %sCPU + GPU benchmark%s  -  %sv%s%s\n",
                ansi::bold(), ansi::reset(), ansi::gray(), ansi::reset(),
                ansi::gray(), kVersion, ansi::reset());
    std::printf("%s%s  %s%s\n", ansi::bold(), ansi::cyan(), bar.c_str(), ansi::reset());
    std::printf("\n");
}

void printHelp() {
    std::printf("Usage: %s [options]\n\n", kName);
    std::printf("Benchmark selection:\n");
    std::printf("  --all                 run CPU and GPU benchmarks (default)\n");
    std::printf("  --cpu                 run only the CPU benchmark\n");
    std::printf("  --gpu                 run only the GPU benchmark\n");
    std::printf("  --info, -i            print detected hardware and exit\n");
    std::printf("  --list-gpus           list every GPU found on the system\n");
    std::printf("  --monitor             sample CPU/GPU temperatures & power live\n\n");
    std::printf("Timing:\n");
    std::printf("  --seconds N           set duration for both benchmarks\n");
    std::printf("  --cpu-seconds N       duration per CPU sub-test (default 3)\n");
    std::printf("  --gpu-seconds N       GPU benchmark duration (default 8)\n");
    std::printf("  --threads N           CPU worker threads (default: all cores)\n");
    std::printf("  --quick               faster, less thorough CPU test set\n\n");
    std::printf("GPU / scene:\n");
    std::printf("  --quality N           1..4 tessellation preset (default 3)\n");
    std::printf("  --instances N         number of instanced models (1..32)\n");
    std::printf("  --width N --height N  render/window resolution (default 1600x900)\n");
    std::printf("  --no-window           run without a visible window\n");
    std::printf("  --interactive         render until the window is closed (ESC)\n");
    std::printf("  --vsync               enable vertical sync\n");
    std::printf("  --wireframe           draw the scene as wireframe\n");
    std::printf("  --screenshot FILE     save a rendered frame to a PPM image\n");
    std::printf("  --supersample N       internal render scale 1..4 (default 1)\n");
    std::printf("  --gpu-vendor V        auto|nvidia|amd|system (default auto)\n\n");
    std::printf("Maximum-load stress test:\n");
    std::printf("  --stress              max out CPU and GPU until Ctrl+C\n");
    std::printf("  --stress-cpu          max out all CPU cores only\n");
    std::printf("  --stress-gpu          max out the GPU only\n");
    std::printf("  --stress-seconds N    stop the stress test after N seconds (0 = Ctrl+C)\n\n");
    std::printf("Output:\n");
    std::printf("  --json FILE           also write machine readable results\n");
    std::printf("  --seed N              RNG seed for the CPU tests\n");
    std::printf("  --color / --no-color  force / disable ANSI colours\n");
    std::printf("  --help, -h            show this help\n");
}

bool parseArgs(int argc, char** argv, Options& o, bool& wantHelp) {
    auto needValue = [&](int& i, const char* flag) -> const char* {
        if (i + 1 >= argc) {
            std::fprintf(stderr, "error: %s requires a value\n", flag);
            return nullptr;
        }
        return argv[++i];
    };

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--help" || a == "-h") { wantHelp = true; return true; }
        else if (a == "--all") { o.runCpu = o.runGpu = true; }
        else if (a == "--cpu") { o.runCpu = true; o.runGpu = false; }
        else if (a == "--gpu") { o.runCpu = false; o.runGpu = true; }
        else if (a == "--info" || a == "-i") { o.showInfo = true; }
        else if (a == "--list-gpus") { o.listGpus = true; }
        else if (a == "--monitor") { o.monitor = true; }
        else if (a == "--quick") { o.cpu.quick = true; }
        else if (a == "--interactive") { o.gpu.interactive = true; o.gpu.showWindow = true; }
        else if (a == "--no-window") { o.gpu.showWindow = false; }
        else if (a == "--window") { o.gpu.showWindow = true; }
        else if (a == "--vsync") { o.gpu.vsync = true; }
        else if (a == "--wireframe") { o.gpu.wireframe = true; }
        else if (a == "--screenshot") {
            const char* v = needValue(i, "--screenshot");
            if (!v) return false;
            o.gpu.screenshotPath = v;
        }
        else if (a == "--color") { ansi::enabled() = true; }
        else if (a == "--no-color") { ansi::enabled() = false; }
        else if (a == "--json") {
            const char* v = needValue(i, "--json");
            if (!v) return false;
            o.json = true;
            o.jsonPath = v;
        }
        else if (a == "--seconds") {
            const char* v = needValue(i, "--seconds");
            if (!v) return false;
            double d = std::atof(v);
            o.cpu.secondsPerTest = d;
            o.gpu.seconds = d;
        }
        else if (a == "--cpu-seconds") {
            const char* v = needValue(i, "--cpu-seconds");
            if (!v) return false;
            o.cpu.secondsPerTest = std::atof(v);
        }
        else if (a == "--gpu-seconds") {
            const char* v = needValue(i, "--gpu-seconds");
            if (!v) return false;
            o.gpu.seconds = std::atof(v);
        }
        else if (a == "--threads") {
            const char* v = needValue(i, "--threads");
            if (!v) return false;
            o.cpu.threads = std::atoi(v);
        }
        else if (a == "--quality") {
            const char* v = needValue(i, "--quality");
            if (!v) return false;
            o.gpu.quality = std::atoi(v);
        }
        else if (a == "--instances") {
            const char* v = needValue(i, "--instances");
            if (!v) return false;
            o.gpu.instances = std::atoi(v);
        }
        else if (a == "--width") {
            const char* v = needValue(i, "--width");
            if (!v) return false;
            o.gpu.width = std::atoi(v);
        }
        else if (a == "--height") {
            const char* v = needValue(i, "--height");
            if (!v) return false;
            o.gpu.height = std::atoi(v);
        }
        else if (a == "--seed") {
            const char* v = needValue(i, "--seed");
            if (!v) return false;
            o.seed = std::strtoull(v, nullptr, 0);
            o.cpu.seed = o.seed;
        }
        else if (a == "--gpu-vendor") {
            const char* v = needValue(i, "--gpu-vendor");
            if (!v) return false;
            o.gpuVendor = v;
        }
        else if (a == "--supersample") {
            const char* v = needValue(i, "--supersample");
            if (!v) return false;
            o.gpu.supersample = std::atoi(v);
        }
        else if (a == "--stress") { o.stress = true; o.stressCpu = o.stressGpu = true; }
        else if (a == "--stress-cpu") { o.stress = true; o.stressCpu = true; }
        else if (a == "--stress-gpu") { o.stress = true; o.stressGpu = true; }
        else if (a == "--stress-seconds") {
            const char* v = needValue(i, "--stress-seconds");
            if (!v) return false;
            o.stress = true;
            o.stressSeconds = std::atof(v);
        }
        else {
            std::fprintf(stderr, "error: unknown option '%s' (try --help)\n", a.c_str());
            return false;
        }
    }
    return true;
}

// When the user asks for the dedicated GPU we re-execute the process with the
// PRIME render-offload environment so the correct GLX vendor library is used.
void ensureGpuSelection(const Options& o, char** argv) {
    if (!o.runGpu) return;
    if (o.gpuVendor == "system" || o.gpuVendor == "amd" || o.gpuVendor == "integrated") return;
    if (std::getenv("PK_BENCH_OFFLOADED")) return;

    // auto: only offload when an NVIDIA GPU is actually present.
    bool hasNvidia = false;
    for (auto& g : detectGpus()) {
        if (g.vendor == "NVIDIA") hasNvidia = true;
    }
    if (o.gpuVendor == "nvidia") hasNvidia = true;
    if (!hasNvidia) return;

    setenv("__NV_PRIME_RENDER_OFFLOAD", "1", 1);
    setenv("__GLX_VENDOR_LIBRARY_NAME", "nvidia", 1);
    setenv("PK_BENCH_OFFLOADED", "1", 1);

    char exe[4096] = {0};
    ssize_t n = ::readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n > 0) {
        execv(exe, argv);
        // If execv fails we simply continue in the current process.
    }
}

void printSystem(const CpuInfo& cpu, const std::vector<GpuInfo>& gpus) {
    std::printf("%s%sSystem%s\n", ansi::bold(), ansi::cyan(), ansi::reset());
    auto row = [](const std::string& k, const std::string& v) {
        std::printf("  %s%-11s%s %s\n", ansi::gray(), (k + ":").c_str(), ansi::reset(), v.c_str());
    };
    row("Host", hostname());
    row("OS", osName());
    row("Kernel", kernelVersion());
    row("CPU", cpu.model);
    row("Topology", std::to_string(cpu.physicalCores) + " cores / " +
                    std::to_string(cpu.logicalCores) + " threads" +
                    (cpu.sockets > 1 ? " (" + std::to_string(cpu.sockets) + " sockets)" : ""));
    if (cpu.maxMHz > 0)
        row("Clock", fixed(cpu.baseMHz, 0) + " MHz base / " + fixed(cpu.maxMHz, 0) + " MHz max");
    row("Memory", bytesHuman(cpu.totalRamBytes));
    if (!cpu.caches.empty()) {
        std::string cache;
        for (auto& c : cpu.caches) {
            if (!cache.empty()) cache += ", ";
            cache += c.level + " " + bytesHuman(c.sizeBytes);
        }
        row("Caches", cache);
    }
    std::string ext;
    static const char* interesting[] = {"avx2", "avx512f", "fma", "sse4_2", "sha_ni", "aes"};
    for (const char* f : interesting) {
        if (std::find(cpu.features.begin(), cpu.features.end(), f) != cpu.features.end()) {
            if (!ext.empty()) ext += " ";
            ext += f;
        }
    }
    if (!ext.empty()) row("ISA", ext);
    if (!gpus.empty()) {
        std::printf("  %s%-11s%s %s\n", ansi::gray(), "GPUs:", ansi::reset(), gpus[0].name.c_str());
        for (size_t i = 1; i < gpus.size(); ++i)
            std::printf("  %-12s %s\n", "", gpus[i].name.c_str());
    }
    std::printf("\n");
}

void printCpuReport(const CpuInfo& cpu, const CpuBenchResult& r) {
    std::string durStr = fixed(r.tests.empty() ? 0.0 : r.tests[0].seconds, 1) + "s";
    std::printf("%s%sCPU Benchmark%s  (%d threads, %s per test)\n",
                ansi::bold(), ansi::cyan(), ansi::reset(),
                r.threads, durStr.c_str());

    std::printf("  %s%-34s %14s %10s  %s%s\n", ansi::gray(),
                "Test", "Result", "Score", "Note", ansi::reset());
    std::printf("  %s\n", repeat("-", 78).c_str());
    int idx = 1;
    for (auto& t : r.tests) {
        std::string value = fixed(t.value, t.value >= 100 ? 0 : 2) + " " + t.unit;
        std::printf("  %2d %-32s %14s %10.0f  %s%s%s\n",
                    idx++, t.name.c_str(), value.c_str(), t.score,
                    ansi::gray(), t.note.c_str(), ansi::reset());
    }
    std::printf("  %s\n", repeat("-", 78).c_str());
    std::printf("  %sCPU Score : %.0f  (%s)%s    single-core %.0f    multi-core scaling x%.2f\n\n",
                ansi::bold(), r.totalScore, scoreRating(r.totalScore).c_str(), ansi::reset(),
                r.singleThreadScore, r.scaling);
}

void printGpuReport(const GpuBenchResult& r) {
    std::printf("%s%sGPU Benchmark%s  (%dx%d, quality %d, %d instances)\n",
                ansi::bold(), ansi::cyan(), ansi::reset(),
                r.width, r.height, r.quality, r.instances);

    auto row = [](const std::string& k, const std::string& v) {
        std::printf("  %s%-13s%s %s\n", ansi::gray(), (k + ":").c_str(), ansi::reset(), v.c_str());
    };
    row("Device", r.device.renderer);
    row("Vendor", r.device.vendor);
    row("OpenGL", r.device.version);
    row("GLSL", r.device.glsl);
    if (r.device.vramKnown)
        row("VRAM", bytesHuman(r.device.vramTotalBytes) + " (free " +
                    bytesHuman(r.device.vramAvailableBytes) + ")");
    row("Geometry", humanNumber(static_cast<double>(r.trianglesPerFrame)) + " triangles / frame, " +
                    humanNumber(static_cast<double>(r.verticesPerFrame)) + " vertices");
    row("Frames", std::to_string(r.frames) + " in " + fixed(r.seconds, 2) + " s");
    std::printf("  %s\n", repeat("-", 78).c_str());
    std::printf("  %-24s avg %8.2f FPS   median %8.2f   %s1%% low %8.2f%s   min %7.2f   max %7.2f\n",
                "Frames per second", r.avgFps, r.medianFps, ansi::yellow(), r.p1Fps, ansi::reset(),
                r.minFps, r.maxFps);
    std::printf("  %-24s %8.2f ms  (%s%.2f ms GPU-bound best case%s)\n",
                "Average frame time", r.avgFrameMs, ansi::gray(),
                r.frameTimesMs.empty() ? 0.0 : *std::min_element(r.frameTimesMs.begin(), r.frameTimesMs.end()),
                ansi::reset());
    std::printf("  %-24s %8s triangles/s\n", "Geometry throughput", humanNumber(r.geometryRate).c_str());
    std::printf("  %-24s %8s pixels/s\n", "Fill throughput", humanNumber(r.pixelRate).c_str());
    std::printf("  %s\n", repeat("-", 78).c_str());
    std::printf("  %sGPU Score : %.0f  (%s)%s\n\n", ansi::bold(), r.score,
                gpuScoreRating(r.score).c_str(), ansi::reset());
}

void printThermals(const Monitor& m) {
    SensorStats cpu = m.cpuTemp();
    SensorStats gpu = m.gpuTemp();
    SensorStats clk = m.gpuClock();
    SensorStats mclk = m.gpuMemClock();
    SensorStats util = m.gpuUtil();
    SensorStats pwr = m.gpuPower();
    if (!cpu.valid() && !gpu.valid()) return;

    std::printf("%s%sThermals & Monitoring%s  (%zu samples)\n",
                ansi::bold(), ansi::cyan(), ansi::reset(), m.samples().size());

    auto cell = [](double v, int prec, const std::string& unit) {
        return fixed(v, prec) + " " + unit;
    };
    auto row = [&](const std::string& name, const SensorStats& s, int prec, const std::string& unit) {
        if (!s.valid()) return;
        std::printf("  %-18s  min %-12s avg %-12s max %-12s\n",
                    name.c_str(),
                    cell(s.min, prec, unit).c_str(),
                    cell(s.avg, prec, unit).c_str(),
                    cell(s.max, prec, unit).c_str());
    };
    row("CPU temperature", cpu, 1, "C");
    row("GPU temperature", gpu, 0, "C");
    row("GPU core clock", clk, 0, "MHz");
    row("GPU memory clock", mclk, 0, "MHz");
    row("GPU utilization", util, 0, "%");
    row("GPU power draw", pwr, 1, "W");
    std::printf("\n");
}

std::string isoTimeNow() {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_r(&t, &tm);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
    return buf;
}

bool writeJson(const std::string& path, const CpuInfo& cpu, const std::vector<GpuInfo>& gpus,
               const CpuBenchResult* cr, const GpuBenchResult* gr, double total) {
    std::ofstream f(path);
    if (!f) return false;
    std::ostringstream o;
    o << "{\n";
    o << "  \"benchmark\": \"" << kName << "\",\n";
    o << "  \"version\": \"" << kVersion << "\",\n";
    o << "  \"timestamp\": \"" << isoTimeNow() << "\",\n";
    o << "  \"system\": {\n";
    o << "    \"host\": \"" << jsonEscape(hostname()) << "\",\n";
    o << "    \"os\": \"" << jsonEscape(osName()) << "\",\n";
    o << "    \"kernel\": \"" << jsonEscape(kernelVersion()) << "\",\n";
    o << "    \"cpu\": {\n";
    o << "      \"model\": \"" << jsonEscape(cpu.model) << "\",\n";
    o << "      \"vendor\": \"" << jsonEscape(cpu.vendor) << "\",\n";
    o << "      \"physical_cores\": " << cpu.physicalCores << ",\n";
    o << "      \"logical_cores\": " << cpu.logicalCores << ",\n";
    o << "      \"base_mhz\": " << fixed(cpu.baseMHz, 0) << ",\n";
    o << "      \"max_mhz\": " << fixed(cpu.maxMHz, 0) << ",\n";
    o << "      \"memory_bytes\": " << cpu.totalRamBytes << "\n";
    o << "    },\n";
    o << "    \"gpus\": [\n";
    for (size_t i = 0; i < gpus.size(); ++i) {
        o << "      {\"name\": \"" << jsonEscape(gpus[i].name) << "\", "
          << "\"vendor\": \"" << jsonEscape(gpus[i].vendor) << "\", "
          << "\"dedicated\": " << (gpus[i].isDiscrete ? "true" : "false") << ", "
          << "\"vram_bytes\": " << gpus[i].dedicatedVramBytes << "}";
        o << (i + 1 < gpus.size() ? ",\n" : "\n");
    }
    o << "    ]\n";
    o << "  },\n";

    if (cr) {
        o << "  \"cpu\": {\n";
        o << "    \"score\": " << fixed(cr->totalScore, 1) << ",\n";
        o << "    \"single_thread_score\": " << fixed(cr->singleThreadScore, 1) << ",\n";
        o << "    \"scaling\": " << fixed(cr->scaling, 3) << ",\n";
        o << "    \"threads\": " << cr->threads << ",\n";
        o << "    \"tests\": [\n";
        for (size_t i = 0; i < cr->tests.size(); ++i) {
            auto& t = cr->tests[i];
            o << "      {\"name\": \"" << jsonEscape(t.name) << "\", \"value\": " << fixed(t.value, 3)
              << ", \"unit\": \"" << jsonEscape(t.unit) << "\", \"score\": " << fixed(t.score, 1) << "}";
            o << (i + 1 < cr->tests.size() ? ",\n" : "\n");
        }
        o << "    ]\n";
        o << "  },\n";
    }
    if (gr) {
        o << "  \"gpu\": {\n";
        o << "    \"score\": " << fixed(gr->score, 1) << ",\n";
        o << "    \"width\": " << gr->width << ", \"height\": " << gr->height << ",\n";
        o << "    \"quality\": " << gr->quality << ", \"instances\": " << gr->instances << ",\n";
        o << "    \"renderer\": \"" << jsonEscape(gr->device.renderer) << "\",\n";
        o << "    \"vendor\": \"" << jsonEscape(gr->device.vendor) << "\",\n";
        o << "    \"opengl_version\": \"" << jsonEscape(gr->device.version) << "\",\n";
        o << "    \"vram_bytes\": " << gr->device.vramTotalBytes << ",\n";
        o << "    \"triangles_per_frame\": " << gr->trianglesPerFrame << ",\n";
        o << "    \"frames\": " << gr->frames << ",\n";
        o << "    \"seconds\": " << fixed(gr->seconds, 3) << ",\n";
        o << "    \"fps\": {\"avg\": " << fixed(gr->avgFps, 2) << ", \"median\": " << fixed(gr->medianFps, 2)
          << ", \"p1_low\": " << fixed(gr->p1Fps, 2) << ", \"min\": " << fixed(gr->minFps, 2)
          << ", \"max\": " << fixed(gr->maxFps, 2) << "},\n";
        o << "    \"geometry_rate\": " << fixed(gr->geometryRate, 0) << ",\n";
        o << "    \"pixel_rate\": " << fixed(gr->pixelRate, 0) << "\n";
        o << "  },\n";
    }

    o << "  \"total_score\": " << fixed(total, 1) << "\n";
    o << "}\n";
    f << o.str();
    return true;
}

} // namespace

// Sustained maximum-load test: keep every CPU core and/or the GPU pinned at
// 100 % until the time limit is reached or the user presses Ctrl+C.
void runStressMode(const Options& o, Monitor& monitor) {
    const bool runCpu = o.stressCpu;
    const bool runGpu = o.stressGpu;

    std::printf("%s%sMaximum-load Stress Test%s\n", ansi::bold(), ansi::cyan(), ansi::reset());
    const std::string goal = (runCpu && runGpu) ? "CPU + GPU" : (runCpu ? "CPU" : "GPU");
    if (o.stressSeconds > 0.0)
        std::printf("  targets   : %s%s%s\n  duration  : %s seconds\n\n",
                    ansi::yellow(), goal.c_str(), ansi::reset(),
                    fixed(o.stressSeconds, 0).c_str());
    else
        std::printf("  targets   : %s%s%s\n  duration  : until Ctrl+C\n\n",
                    ansi::yellow(), goal.c_str(), ansi::reset());

    CpuStress cpu;
    if (runCpu) cpu.start(o.cpu.threads);

    if (runGpu) {
        GpuOptions g = o.gpu;
        g.stress = true;
        g.showWindow = true;
        g.seconds = o.stressSeconds;
        g.quality = 4;
        if (g.instances <= 0) g.instances = 32;
        if (g.supersample < 2) g.supersample = 2;   // double internal resolution
        GpuBenchResult r;
        std::string err;
        if (!runGpuBenchmark(g, r, err))
            std::fprintf(stderr, "  error: GPU stress failed: %s\n", err.c_str());
        else
            std::printf("  %sGPU stress%s  avg %.1f FPS  -  %llu frames in %.1f s\n",
                        ansi::green(), ansi::reset(), r.avgFps,
                        static_cast<unsigned long long>(r.frames), r.seconds);
    } else {
        cpu.wait(o.stressSeconds);
    }

    if (runCpu) cpu.stop();
    (void)monitor;
    std::printf("\n");
}

int main(int argc, char** argv) {
    ansi::enabled() = ::isatty(1) && std::getenv("NO_COLOR") == nullptr;
    std::ios::sync_with_stdio(false);

    Options o;
    bool wantHelp = false;
    if (!parseArgs(argc, argv, o, wantHelp)) return 2;
    if (wantHelp) { printBanner(); printHelp(); return 0; }

    installSignalHandler();  // clean shutdown for the stress modes

    if (o.showInfo || o.listGpus) {
        CpuInfo cpu = detectCpu();
        std::vector<GpuInfo> gpus = detectGpus();
        if (o.showInfo) {
            printBanner();
            std::printf("%s%sHardware Information%s\n\n", ansi::bold(), ansi::cyan(), ansi::reset());
            printSystem(cpu, gpus);
        }
        if (o.listGpus) {
            std::printf("%sGPUs detected on this system:%s\n", ansi::bold(), ansi::reset());
            if (gpus.empty()) std::printf("  none\n");
            for (size_t i = 0; i < gpus.size(); ++i) {
                auto& g = gpus[i];
                std::printf("  [%zu] %s\n", i, g.name.c_str());
                std::printf("      vendor      : %s\n", g.vendor.c_str());
                if (!g.pciId.empty())  std::printf("      pci         : %s\n", g.pciId.c_str());
                if (!g.driver.empty()) std::printf("      driver      : %s\n", g.driver.c_str());
                if (g.dedicatedVramBytes)
                    std::printf("      vram        : %s\n", bytesHuman(g.dedicatedVramBytes).c_str());
                std::printf("      dedicated   : %s\n", g.isDiscrete ? "yes" : "no (integrated)");
            }
        }
        return 0;
    }

    printBanner();

    CpuInfo cpu = detectCpu();
    std::vector<GpuInfo> gpus = detectGpus();
    printSystem(cpu, gpus);

    // Move to the dedicated GPU before any GL context is created.
    if (o.stress) o.runGpu = o.stressGpu;
    ensureGpuSelection(o, argv);

    Monitor monitor;
    if (o.monitor) {
        o.gpu.suppressProgress = true;
        std::printf("  %sTemperature / power monitoring enabled (1 Hz)%s\n\n",
                    ansi::yellow(), ansi::reset());
        monitor.start(1.0, true);
    }

    if (o.stress) {
        runStressMode(o, monitor);
        if (o.monitor) { monitor.stop(); printThermals(monitor); }
        return 0;
    }

    std::unique_ptr<CpuBenchResult> cpuResult;
    std::unique_ptr<GpuBenchResult> gpuResult;

    if (o.runCpu) {
        std::printf("  %sRunning CPU benchmark...%s\n\n", ansi::yellow(), ansi::reset());
        cpuResult = std::make_unique<CpuBenchResult>(runCpuBenchmark(o.cpu));
        printCpuReport(cpu, *cpuResult);
    }

    if (o.runGpu) {
        std::printf("  %sRunning GPU benchmark%s...\n\n", ansi::yellow(), ansi::reset());
        GpuBenchResult gr;
        std::string error;
        if (runGpuBenchmark(o.gpu, gr, error)) {
            gpuResult = std::make_unique<GpuBenchResult>(std::move(gr));
            printGpuReport(*gpuResult);
        } else {
            std::printf("  %sGPU benchmark failed: %s%s\n\n", ansi::red(), error.c_str(), ansi::reset());
            if (o.runCpu) gpuResult.reset();
        }
    }

    // ------- overall summary -------
    if (o.monitor) monitor.stop();

    std::vector<double> scores;
    if (cpuResult) scores.push_back(std::max(cpuResult->totalScore, 1.0));
    if (gpuResult) scores.push_back(std::max(gpuResult->score, 1.0));
    double combined = 0.0;
    if (!scores.empty()) {
        double logSum = 0.0;
        for (double s : scores) logSum += std::log(s);
        combined = std::exp(logSum / static_cast<double>(scores.size()));
    }

    std::printf("%s%sSummary%s\n", ansi::bold(), ansi::cyan(), ansi::reset());
    std::printf("  %s\n", repeat("=", 60).c_str());
    if (cpuResult)
        std::printf("  %-22s %8.0f   %s\n", "CPU score", cpuResult->totalScore,
                    scoreRating(cpuResult->totalScore).c_str());
    if (gpuResult)
        std::printf("  %-22s %8.0f   %s\n", "GPU score", gpuResult->score,
                    gpuScoreRating(gpuResult->score).c_str());
    if (cpuResult && gpuResult)
        std::printf("  %-22s %8.0f   %s\n", "Combined score", combined,
                    scoreRating(combined).c_str());
    std::printf("  %s\n\n", repeat("=", 60).c_str());

    if (o.monitor) printThermals(monitor);

    if (o.json) {
        if (writeJson(o.jsonPath, cpu, gpus, cpuResult.get(), gpuResult.get(), combined))
            std::printf("  Results written to %s\n", o.jsonPath.c_str());
        else
            std::fprintf(stderr, "error: could not write %s\n", o.jsonPath.c_str());
    }

    return 0;
}
