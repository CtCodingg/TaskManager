# CtTaskManager

A cross-platform system/task monitor built with **GLFW + Dear ImGui +
ImPlot + OpenGL 3**, fetched via CMake `FetchContent` at configure time.
All MIT/zlib licensed -- no GPL/LGPL dependencies. Targets Ubuntu 22/24
(x86_64 and ARMv8/Jetson) and Windows 10/11.

## Features

- **Processes** -- sortable/filterable table, color-coded CPU%/state, End Task.
- **Performance** -- CPU (total + per-core bars + temp), Memory, GPU,
  Disk volumes/IO, each with a rolling ImPlot chart.
- **Network** -- per-interface bandwidth, link speed/utilization, drop%/error%.
- **Connections** -- every TCP/UDP connection (IPv4+IPv6) with owning
  process, always on, no elevated privileges needed.
- **Bandwidth** (opt-in via `--track-bandwidth`) -- per-process download/
  upload, TCP + UDP. Linux: Netlink socket-diag + raw `AF_PACKET` capture.
  Windows: TCP Extended Statistics API + ETW Kernel-Network provider
  consumption. Windows requires Administrator (UAC prompt); Linux UDP
  needs root/`CAP_NET_RAW`, TCP works unprivileged.
- **Settings** -- rate unit (bits/bytes) + refresh rate, persisted to a
  small config file next to the executable.
- **About** -- version, author (CtCodingg), GitHub link, module list.
- `--help` / `-h` for usage info.

## Architecture

```
src/
  main.cpp              GLFW window, OpenGL3 context, ImGui/ImPlot init,
                         CLI flag parsing, Windows elevation, render loop
  App.h/.cpp             top-level state, tab bar, poll-timer dispatch
  Theme.h/.cpp            dark palette applied via ImGuiStyle
  backend/                platform collectors (std:: types throughout)
    Types.h               shared data structs
    FormatUtils.*          byte/bit/percent formatting, rate-unit toggle
    *Collector.h + platform/{linux,win}/*.cpp
      ProcessCollector, SystemStatsCollector, GpuStatsCollector,
      NetworkStatsCollector, ProcessConnectionCollector,
      ProcessBandwidthCollector
  ui/                     one file pair per tab, same shape everywhere:
                          updateData(...) on a poll timer, draw() every frame
    ProcessesTab, PerformanceTab, NetworkTab, ConnectionsTab, BandwidthTab
    HistoryChart.h         reusable rolling-buffer ImPlot chart
    SettingsWindow, AboutWindow   ImGui popup modals
```

Platform-specific implementations live in `src/backend/platform/{linux,win}/`,
selected via CMakeLists.txt `WIN32`/`UNIX` conditionals -- the collector
headers and all UI code stay free of `#ifdef` guards.

## Building

```bash
cmake -S . -B build -G Ninja      # Linux/Jetson
cmake --build build
./build/CtTaskManager

# Windows
cmake -S . -B build -G "Visual Studio 18 2026"
cmake --build build --config Release
```

First configure clones GLFW/Dear ImGui/ImPlot via `FetchContent` -- needs
network access and git, only at configure time.

On Windows, the MSVC C/C++ runtime is statically linked (`/MT`), so the
resulting `.exe` needs no `vcruntime140.dll`/`msvcp140.dll` on the target
machine. On Linux, `libstdc++`/`libgcc` are statically linked for the same
reason (portability across systems with a different GCC runtime); glibc
itself is deliberately left dynamic (`getpwuid()`, used for the process
table's "User" column, relies on NSS, which breaks under full static linking).

## Command-line flags

| Flag | Effect |
|---|---|
| *(none)* | Normal startup. No admin/elevated rights on either platform. |
| `-h`, `--help` | Prints usage and exits, no window opens. |
| `--track-bandwidth` | Adds the Bandwidth tab. Windows: triggers one UAC prompt (declining falls back to a normal launch without the tab). Linux: TCP works unprivileged; UDP needs `sudo setcap cap_net_raw+ep <binary>` or root. |

## Notes & extension points

- **Per-thread CPU%** is currently reported as 0 in `ProcessCollector`;
  wiring it up needs a previous-sample map keyed by `(pid, tid)`, analogous
  to what `collect()` already does per-process.
- **Windows per-disk I/O throughput** (`SystemStatsCollector::collectDiskIo`)
  currently returns empty; extend with `PhysicalDisk(*)\Disk Read Bytes/sec`
  PDH counters following the same pattern as the GPU backend.
- **Windows GPU total VRAM** requires DXGI adapter enumeration
  (`IDXGIFactory1::EnumAdapters1` + `DXGI_ADAPTER_DESC`); the PDH-only path
  currently reports used memory but not total.
- The app polls processes/stats roughly every second; connections every
  ~3s and bandwidth every ~2s by default (see `App::pollIfDue`), all
  scaled by the Settings refresh-rate value.

## Honest caveat

This is a large amount of systems-level code written without a compiler
to verify against. The highest-risk areas, in rough order:

1. **ImPlot/ImGui table API details** (`ImGuiTableSortSpecs`,
   `ImGuiListClipper`, `ImPlot::PlotLine` signatures).
2. **Windows ETW property names** (`"PID"`, `"size"`) and task-name
   matching for UDP bandwidth tracking -- never verified against a real
   Windows machine.
3. Everything else (the `/proc` parsing, Netlink, PDH, IP Helper code) is
   built on well-documented, stable OS interfaces, so it's lower risk by
   comparison.

Expect at least one round of compiler-error fixes on real hardware --
please paste the first errors you hit and we'll work through them.
