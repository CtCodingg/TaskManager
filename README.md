# TaskManager

A cross-platform system/task monitor written in C++17, CMake, and Qt Widgets
(+ Qt Charts). Targets:

- Ubuntu 22.04 and 24.04, x86_64
- Ubuntu 22.04 and 24.04, ARMv8 (aarch64) — including NVIDIA Jetson (JetPack/L4T)
- Windows 10/11, x64

No third-party dependencies beyond Qt and each OS's own system libraries
(PDH/IP Helper/PSAPI on Windows; glibc + /proc on Linux). NVML (for NVIDIA
GPU stats on desktop Linux) is loaded at runtime with `dlopen` if present —
it is never a hard link-time or build-time dependency, so the app still
builds and runs fine on machines without an NVIDIA driver.

## Features

- **Processes & threads**: sortable/filterable process table (PID, name,
  user, state, CPU%, RSS memory, thread count, priority, command line).
  Expand-on-demand thread listing per process. Kill / set priority.
- **CPU**: total load + per-core load, per-core frequency (Linux), CPU
  temperature where exposed by the kernel/driver, rolling history chart.
- **Memory**: used/available/cached, swap usage, rolling history chart.
- **GPU**: vendor-aware — NVIDIA via NVML (dlopen), Jetson/Tegra integrated
  GPU via sysfs (same source tegrastats uses), Intel/AMD best-effort via
  `/sys/class/drm/.../gpu_busy_percent`, Windows via the built-in "GPU
  Engine" / "GPU Adapter Memory" PDH counters (vendor-agnostic, no vendor
  SDK needed).
- **Disks**: per-volume usage (space) + per-device I/O throughput and
  utilization.
- **Network (deep info)**: per-interface RX/TX bandwidth, packet rate, link
  speed, link utilization %, and — the detailed part — **drop % and error %
  per interface**, computed each poll as
  `dropped / (successful + dropped) * 100`, plus cumulative counters.

## Project layout

```
TaskManager/
├── CMakeLists.txt            # top-level build, platform detection, Qt5/Qt6
├── CMakePresets.json         # convenience presets for VS Code / CLI
├── include/                  # public headers (platform-agnostic)
├── src/
│   ├── main.cpp
│   ├── MainWindow.cpp        # Qt UI: Processes / Performance / Network tabs
│   ├── ProcessModel.cpp      # QAbstractTableModel for the process table
│   ├── HistoryChartWidget.cpp# rolling QtCharts line-chart widget
│   ├── FormatUtils.cpp
│   └── platform/
│       ├── linux/            # /proc, statvfs, ioctl/ethtool, sysfs, NVML dlopen
│       └── win/               # Toolhelp32, PDH, IP Helper (GetIfTable2)
└── .vscode/                  # tasks.json, launch.json, settings.json
```

Each collector (`ProcessCollector`, `SystemStatsCollector`,
`NetworkStatsCollector`, `GpuStatsCollector`) has a single header in
`include/` and a platform-specific `.cpp` in `src/platform/{linux,win}/`.
CMake picks the right `.cpp` files based on `WIN32`/`UNIX`, so the rest of
the codebase (UI, models) never contains `#ifdef` platform branches.

## Prerequisites

### Ubuntu 22.04 / 24.04 (x86_64 and ARMv8/Jetson)

```bash
sudo apt update
sudo apt install -y build-essential cmake ninja-build gdb \
    qt6-base-dev qt6-charts-dev libgl1-mesa-dev

# Ubuntu 22.04 ships Qt6 in universe as of 22.04.x; if unavailable, use Qt5:
# sudo apt install -y qtbase5-dev libqt5charts5-dev
```

On Jetson (JetPack), the same `apt` packages work — the CMake script detects
`/etc/nv_tegra_release` automatically and enables the Tegra GPU sysfs
backend; no CUDA/NVML/JetPack SDK components are required to build.

### Windows 10/11

1. Install **Visual Studio 2022** (Desktop development with C++ workload) or
   the standalone **Build Tools for Visual Studio 2022**.
2. Install **CMake** (3.21+) and add it to `PATH`.
3. Install **Qt 6** (or Qt 5.15) via the Qt online installer, selecting the
   MSVC 2019/2022 64-bit component, plus **Qt Charts**.
4. Make sure Qt's `<QtInstall>\<version>\msvc2022_64\lib\cmake` directory is
   discoverable — either add it to `CMAKE_PREFIX_PATH` or set the
   environment variable:
   ```powershell
   setx CMAKE_PREFIX_PATH "C:\Qt\6.7.2\msvc2022_64"
   ```

## Building — command line

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
./build/TaskManager        # Linux
# build\Debug\TaskManager.exe   on Windows (MSVC generator)
```

If CMake can't find Qt automatically, pass it explicitly:

```bash
cmake -S . -B build -DCMAKE_PREFIX_PATH=/path/to/Qt/6.x/gcc_64
```

## Building — VS Code

This repo ships a full `.vscode/` setup:

- **`settings.json`** — points `CMake Tools` and the C/C++ extension at a
  Ninja-based build directory and `compile_commands.json` for IntelliSense.
- **`tasks.json`** — `CMake: Configure`, `CMake: Build (Debug/Release)`,
  `CMake: Clean`, `Run TaskManager`.
- **`launch.json`** — three debug targets:
  - *Debug TaskManager (Linux / gdb)* — for Ubuntu 22/24, x86_64 or ARMv8/Jetson.
  - *Debug TaskManager (Windows / MSVC)* — uses the `cppvsdbg` debugger.
  - *Debug TaskManager (Windows / MinGW gdb)* — if you build with MinGW instead.
- **`extensions.json`** — recommends the CMake Tools and C/C++ extensions.

Steps:

1. Install the recommended extensions (VS Code will prompt you).
2. Open the folder in VS Code.
3. `Ctrl+Shift+P` → **CMake: Select a Kit** → pick your compiler (GCC on
   Linux/Jetson, MSVC on Windows).
4. `F5` to build (via the `preLaunchTask`) and start debugging, or
   `Ctrl+Shift+B` to just build.

## Notes & extension points

- **Per-thread CPU%** is currently reported as 0 in the thread expansion
  view; wiring it up requires keeping a previous-sample map keyed by
  `(pid, tid)`, analogous to what `ProcessCollector::collect()` already does
  per-process. Left as a clearly marked extension point in
  `ProcessBackendLinux.cpp` / `ProcessBackendWin.cpp`.
- **Windows per-disk I/O throughput** (`SystemStatsCollector::collectDiskIo`)
  currently returns empty; extend with `PhysicalDisk(*)\Disk Read Bytes/sec`
  PDH counters following the same pattern as `GpuBackendWin.cpp`.
- **Windows GPU total VRAM** requires DXGI adapter enumeration
  (`IDXGIFactory1::EnumAdapters1` + `DXGI_ADAPTER_DESC`); the PDH-only path
  currently reports used memory but not total.
- The app polls processes every 1.5 s and system/network/GPU stats every 1 s;
  tune `kProcessPollMs` / `kStatsPollMs` in `MainWindow.cpp`.
