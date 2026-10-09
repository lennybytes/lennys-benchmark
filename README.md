# lennys-benchmark

A self-contained, professional CPU + GPU benchmark for Linux, written in C++20.

- **Serious CPU suite** — multi-threaded integer, FP32, FP64 and memory-bandwidth
  tests with a scaling analysis.
- **Serious GPU suite** — renders a real 3D scene (a procedurally generated
  torus-knot model with instancing) in an X11 window using OpenGL 4.5 with
  physically based shading, six dynamic lights, HDR, bloom and ACES tonemapping,
  then measures geometry and fill throughput.
- **Hardware detection** — CPU model, cores/threads, caches, ISA features and RAM,
  plus **every** GPU on the system (discrete *and* integrated). On hybrid systems
  the discrete NVIDIA GPU is selected automatically through PRIME render offload.
- **Terminal friendly** — coloured tables, live FPS, JSON export, screenshot
  support and live temperature / clock / power monitoring.
- **Packaged** — native `.deb`, `.AppImage` and `.xbps` packages (see
  [`packaging/`](packaging/README.md)).

---

## Requirements

The program deliberately avoids CMake, GLFW and GLEW.

| Package | Purpose |
|---------|---------|
| `g++` (≥ 10, tested with 14) | compiler |
| `libepoxy-dev` | OpenGL/GLX function loading |
| `libgl-dev` (`libgl1-mesa-dev`) | OpenGL |
| `libx11-dev` | window / GLX |

Debian/Ubuntu:

```bash
sudo apt install build-essential libepoxy-dev libgl-dev libx11-dev
```

## Building

```bash
make -j"$(nproc)"        # produces bin/lennys-benchmark
```

Other targets: `make clean`, `make run`, `make install`, `make info`.

## Running

```bash
./bin/lennys-benchmark                  # CPU + GPU benchmark (opens a window)
./bin/lennys-benchmark --info           # print detected hardware and exit
./bin/lennys-benchmark --list-gpus      # list every GPU (discrete + integrated)
./bin/lennys-benchmark --gpu            # GPU only
./bin/lennys-benchmark --cpu            # CPU only
./bin/lennys-benchmark --interactive    # render the scene until ESC
```

A full run of roughly one minute with live thermals:

```bash
./bin/lennys-benchmark --cpu-seconds 6 --gpu-seconds 30 --monitor
```

### Options

```
--all                  run the CPU and GPU benchmarks (default)
--cpu                  run only the CPU benchmark
--gpu                  run only the GPU benchmark
--info, -i             print detected hardware and exit
--list-gpus            list every GPU found on the system
--monitor              sample CPU/GPU temperatures & power live

--seconds N            duration for both benchmarks
--cpu-seconds N        duration per CPU sub-test (default 3)
--gpu-seconds N        GPU benchmark duration (default 8)
--threads N            CPU worker threads (default: all cores)
--quick                faster, less thorough CPU test set

--quality N            1..4 tessellation preset (default 3)
--instances N          number of instanced models (1..32)
--width N --height N   render/window resolution (default 1600x900)
--no-window            run without a visible window
--interactive          render until the window is closed (ESC)
--vsync                enable vertical sync
--wireframe            draw the scene as wireframe
--screenshot FILE      save a rendered frame as a PPM image
--gpu-vendor V         auto | nvidia | amd | system (default auto)

--json FILE            also write machine readable results
--seed N               RNG seed for the CPU tests
--color / --no-color   force / disable ANSI colours
--help, -h             show the help
```

### Examples

```bash
# Quick CPU + GPU overview
./bin/lennys-benchmark --seconds 5

# Full load on the discrete GPU, result written as JSON
./bin/lennys-benchmark --gpu --quality 4 --gpu-seconds 20 --json results.json

# CPU only, all threads, thorough
./bin/lennys-benchmark --cpu --cpu-seconds 5 --threads 16

# Render a frame of the scene (PPM; convert with ffmpeg if you like)
./bin/lennys-benchmark --gpu --screenshot scene.ppm --gpu-seconds 2
ffmpeg -i scene.ppm scene.png
```

## Discrete GPU (optional)

`lennys-benchmark` detects GPUs through `nvidia-smi` and `lspci`. With the default
`--gpu-vendor auto`, if an NVIDIA GPU is present the program re-executes itself
once with

```
__NV_PRIME_RENDER_OFFLOAD=1
__GLX_VENDOR_LIBRARY_NAME=nvidia
```

and then renders on the discrete GPU. With `--gpu-vendor system` or
`--gpu-vendor amd` no offload is performed and the default GPU (for example the
integrated Radeon/Intel) is used.

## Monitoring

`--monitor` spawns a background thread that samples the CPU temperature
(`k10temp`/`coretemp`), integrated GPU, NVMe, and — through `nvidia-smi` — the
discrete GPU temperature, core/memory clocks, utilisation and power draw once
per second. A live line is printed while running and a min/avg/max summary is
added to the report.

## What is measured?

**CPU** (each sub-test is normalised so that the reference yields 1000 points):

| Test | Unit |
|------|------|
| Integer ALU (xoshiro256\*\*) | Mops/s |
| FP32 FMA (8 independent chains) | GFLOP/s |
| FP64 FMA (8 independent chains) | GFLOP/s |
| Memory bandwidth (STREAM triad) | GB/s |
| Integer ALU — all cores (scaling) | Mops/s |

**GPU**: FPS (avg, median, 1 % low, min/max), frame times, geometry and fill
throughput. The GPU score is the geometric mean of triangle and pixel
throughput, normalised against reference values.

## Interactive controls

| Key / mouse | Action |
|-------------|--------|
| Drag with the left mouse button | rotate the camera |
| Mouse wheel | zoom |
| `SPACE` | pause |
| `W` | toggle wireframe |
| `R` | reset the camera |
| `ESC` / close the window | quit |

## Packages

Ready-made packages can be built with

```bash
bash packaging/build-all.sh
```

which produces, under `packaging/build/`:

- `lennys-benchmark_1.0.0_amd64.deb`
- `lennys-benchmark-1.0.0-x86_64.AppImage`
- `lennys-benchmark-1.0.0_1.x86_64.xbps`

See [`packaging/README.md`](packaging/README.md) for how each format is built.
Tagged releases are built automatically by the GitHub Actions workflow in
[`.github/workflows/release.yml`](.github/workflows/release.yml) and attached to
the release.

## Project structure

```
lennys-benchmark/
├── Makefile
├── README.md
├── LICENSE
├── src/
│   ├── main.cpp          CLI, report, JSON
│   ├── util.hpp          timing, colours, formatting
│   ├── system_info.*     CPU/GPU detection
│   ├── cpu_bench.*       CPU benchmarks
│   ├── gpu_bench.*       window, GL context, 3D scene, shaders, post-processing
│   └── monitor.*         temperature / clock / power sampling
├── packaging/            .deb, .AppImage and .xbps builds
└── .github/workflows/    CI release pipeline
```

## License

MIT — see [LICENSE](LICENSE).
