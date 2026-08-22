# TaskManager -- Dear ImGui edition (no Qt)

A from-scratch port of the Qt-based TaskManager to a fully Qt-free stack:
**GLFW + Dear ImGui + ImPlot + OpenGL 3**, fetched via CMake `FetchContent`
at configure time (no system Qt install, no `windeployqt`, no GPL/LGPL
entanglement from Qt Charts -- everything here is MIT/zlib licensed).

## Status: foundation + one fully-worked tab

This is **not** a complete port. Porting the entire Qt edition (5 tabs,
sortable/filterable tables, live charts, dialogs, dark theme) is a multi-week
project on its own, and I can't compile-test in this environment, so I've
deliberately scoped this first pass to:

**Done and should build:**
- `CMakeLists.txt` -- fetches GLFW, Dear ImGui, ImPlot; builds them as static
  libs; links everything (no external installs needed beyond a C++17
  compiler and OpenGL, which every desktop OS already has).
- `main.cpp` -- GLFW window, OpenGL 3.3 core context, ImGui/ImPlot init,
  render loop. Replaces `QApplication`/`MainWindow::show()`/`app.exec()`.
- `Theme.cpp` -- dark palette ported 1:1 from the Qt edition's
  `resources/style.qss` / `UiTheme.h` (same hex colors), applied via
  `ImGuiStyle` instead of QSS.
- `backend/Types.h`, `backend/FormatUtils.*` -- Qt-free (`std::string` /
  `std::vector` / `std::map` instead of `QString`/`QVector`/`QMap`).
- `backend/ProcessCollector` (Linux + Windows) -- full port of the Qt
  edition's `/proc` and Toolhelp32 backends, same logic, Qt-free.
- `ui/ProcessesTab.*` -- **the fully-worked reference tab**: sortable,
  filterable process table via `ImGui::BeginTable` +
  `ImGuiListClipper` (for scroll performance with large process counts),
  color-coded CPU%/state cells, End Task button. This replaces
  `ProcessModel` + `QSortFilterProxyModel` + `QTableView` entirely.
- `App.cpp` -- top-level tab bar, polls `ProcessCollector` on a timer
  (mirrors the Qt edition's `QTimer` cadence), draws stub placeholders for
  the four not-yet-ported tabs.

**Not done -- stubbed with a note in the UI:**
- Performance tab (CPU/Memory/GPU/Disk) -- needs `SystemStatsCollector` +
  `GpuStatsCollector` ported (same mechanical Qt->std:: swap as
  ProcessCollector), plus `ImPlot::PlotLine` in a rolling buffer to
  replace `HistoryChartWidget`.
- Network tab -- needs `NetworkStatsCollector` ported; UI is the same
  `ImGui::BeginTable` pattern as Processes.
- Connections tab -- needs `ProcessConnectionCollector` ported; same
  table pattern.
- Bandwidth tab -- needs `ProcessBandwidthCollector` ported. The Linux
  (Netlink/raw-capture) and Windows (EStats/ETW) backend logic barely
  touches Qt types to begin with, so this is mostly a search-and-replace
  of `QString`/`QMap` for `std::string`/`std::map`.
- Settings window, About window -- trivial as `ImGui::BeginPopupModal()`
  blocks; no real porting work, just not written yet.
- `--track-bandwidth` CLI flag, Windows UAC self-elevation -- logic
  carries over unchanged from the Qt edition's `main.cpp` (it's pure
  WinAPI, no Qt involved); just needs re-pasting into this `main.cpp`.

## The pattern to extend it

Every remaining tab follows the exact same shape as `ProcessesTab`:

1. A `Collector` class (port the matching one from the Qt edition,
   swapping `QString`/`QVector`/`QMap` for `std::string`/`std::vector`/
   `std::map` -- the platform API calls themselves don't change at all).
2. A `SomethingTab` class with `updateData(...)` (called on a poll timer
   from `App::draw()`) and `draw()` (called every frame, builds an
   `ImGui::BeginTable` or `ImPlot::PlotLine`).
3. Wire it into `App::draw()`'s tab bar, replacing the matching
   `drawStubTab(...)` call.

## Building

```bash
cmake -S . -B build -G Ninja      # Linux/Jetson
cmake --build build
./build/TaskManagerImGui

# Windows
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

First configure will take a minute or two -- it's cloning GLFW/Dear
ImGui/ImPlot from GitHub via `FetchContent`. Needs network access and git
on the build machine (only at configure time, not at runtime).

## Honest caveat

None of this has been compiled. GLFW/ImGui/ImPlot's exact API surface
(especially `ImGuiTableSortSpecs`, `ImGuiListClipper`, and the OpenGL3
loader header interaction with GLFW's own `gl.h`) is based on my
knowledge of these libraries, not a verified build. Expect at least one
round of compiler-error fixes on real hardware, same as every other part
of this project so far -- please paste the first errors you hit and
we'll work through them.
