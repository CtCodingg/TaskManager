// htop-style terminal UI for --tui mode. Runs the same backend
// collectors as the graphical mode (src/backend/) on a background
// polling thread; this file is purely the view + keyboard-input layer.
//
// Tabs (number keys 1-5): Processes, Performance, Network, Connections,
// Bandwidth (only shown if --track-bandwidth was passed). A persistent
// header (title + CPU/memory meter bars) stays visible across every tab,
// matching htop's own always-on-top meters; only the body below changes.

#include "TuiApp.h"

#include "../backend/ProcessCollector.h"
#include "../backend/SystemStatsCollector.h"
#include "../backend/GpuStatsCollector.h"
#include "../backend/NetworkStatsCollector.h"
#include "../backend/ProcessConnectionCollector.h"
#include "../backend/ProcessBandwidthCollector.h"
#include "../backend/FormatUtils.h"

#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/color.hpp>
#include <ftxui/screen/terminal.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

using namespace ftxui;

namespace {

// --- formatting helpers, matching htop's compact column style ------------

std::string compactBytes(uint64_t bytes) {
    static const char* units[] = {"B", "K", "M", "G", "T", "P"};
    double v = static_cast<double>(bytes);
    int i = 0;
    while (v >= 1024.0 && i < 5) { v /= 1024.0; ++i; }
    char buf[16];
    if (i == 0) std::snprintf(buf, sizeof(buf), "%4.0f%s", v, units[i]);
    else std::snprintf(buf, sizeof(buf), "%5.1f%s", v, units[i]);
    return buf;
}

std::string toLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}

Color colorForPercent(double percent) {
    if (percent >= 85.0) return Color::Red;
    if (percent >= 60.0) return Color::Yellow;
    return Color::Green;
}

enum class SortBy { Cpu, Mem, Pid, Name };

// Pure "x's key < y's key" relation for a given sort column, reused with
// swapped arguments for descending order (see sortProcesses() below).
// Negating the ascending result instead breaks strict weak ordering
// whenever two elements tie on the sort key -- e.g. two processes both
// at 0.0% CPU, common right after startup -- and std::sort can throw
// "invalid comparator" as a result.
bool processKeyLess(SortBy sortBy, const ProcessInfo& x, const ProcessInfo& y) {
    switch (sortBy) {
        case SortBy::Cpu:  return x.cpuPercent < y.cpuPercent;
        case SortBy::Mem:  return x.memRssBytes < y.memRssBytes;
        case SortBy::Pid:  return x.pid < y.pid;
        case SortBy::Name: return x.name < y.name;
    }
    return x.pid < y.pid;
}

void sortProcesses(std::vector<ProcessInfo>& processes, SortBy sortBy, bool descending) {
    std::sort(processes.begin(), processes.end(), [&](const ProcessInfo& a, const ProcessInfo& b) {
        return descending ? processKeyLess(sortBy, b, a) : processKeyLess(sortBy, a, b);
    });
}

std::string pidToNameFrom(const std::vector<ProcessInfo>& processes, int64_t pid) {
    for (const auto& p : processes) if (p.pid == pid) return p.name;
    return "(pid " + std::to_string(pid) + ")";
}

// --- shared state between the poll thread and the render thread ----------

struct TuiState {
    std::mutex mutex;
    std::vector<ProcessInfo> processes;
    CpuStats cpu;
    MemoryStats mem;
    std::vector<DiskVolume> diskVolumes;
    std::vector<DiskIoStats> diskIo;
    std::vector<GpuInfo> gpus;
    NetworkStats network;
    std::vector<ProcessConnection> connections;
    std::vector<ProcessInterfaceBandwidth> bandwidth;
};

// A single per-core (or overall) meter bar: "[||||||    42.3%]" with a
// color-coded fill, matching htop's iconic header bars.
Element meterBar(const std::string& label, double percent, int width) {
    float ratio = static_cast<float>(std::min(100.0, std::max(0.0, percent)) / 100.0);
    char pctBuf[16];
    std::snprintf(pctBuf, sizeof(pctBuf), "%5.1f%%", percent);

    return hbox({
        text(label) | size(WIDTH, EQUAL, 7),
        text("["),
        dbox({
            gauge(ratio) | color(colorForPercent(percent)) | size(WIDTH, EQUAL, width),
            hbox({filler(), text(pctBuf), filler()}) | size(WIDTH, EQUAL, width),
        }),
        text("]"),
    });
}

// --- per-tab body builders -------------------------------------------

Element buildPerformanceView(const MemoryStats& mem, const std::vector<DiskVolume>& disks,
                              const std::vector<DiskIoStats>& diskIo, const std::vector<GpuInfo>& gpus) {
    Elements lines;

    lines.push_back(text("Memory") | bold);
    lines.push_back(text("  " + FormatUtils::bytes(mem.usedBytes) + " / " + FormatUtils::bytes(mem.totalBytes) +
                          "  (" + FormatUtils::percent(mem.usedPercent()) + ")"));
    lines.push_back(text("  Swap: " + FormatUtils::bytes(mem.swapUsedBytes) + " / " + FormatUtils::bytes(mem.swapTotalBytes)) | dim);
    lines.push_back(text(""));

    lines.push_back(text("GPU") | bold);
    if (gpus.empty()) {
        lines.push_back(text("  No supported GPU backend detected on this system.") | dim);
    } else {
        for (const auto& g : gpus) {
            std::string loadStr = g.loadPercent >= 0 ? FormatUtils::percent(g.loadPercent) : "n/a";
            std::string memStr = FormatUtils::bytes(g.memUsedBytes) +
                (g.memTotalBytes ? " / " + FormatUtils::bytes(g.memTotalBytes) : "");
            lines.push_back(text("  " + g.name + " (" + g.vendor + ")"));
            std::string detail = "    Load: " + loadStr + "   Mem: " + memStr;
            if (g.temperatureC >= 0) detail += "   Temp: " + std::to_string(static_cast<int>(g.temperatureC)) + "C";
            lines.push_back(text(detail) | dim);
        }
    }
    lines.push_back(text(""));

    lines.push_back(text("Disk Volumes") | bold);
    lines.push_back(text("  MOUNT                FS       USED     TOTAL  USE%") | dim);
    for (const auto& v : disks) {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "  %-20.20s %-8.8s %6s %9s %5.1f%%",
                      v.mountPoint.c_str(), v.fsType.c_str(),
                      compactBytes(v.usedBytes).c_str(), compactBytes(v.totalBytes).c_str(), v.usedPercent());
        lines.push_back(text(buf) | color(colorForPercent(v.usedPercent())));
    }
    lines.push_back(text(""));

    lines.push_back(text("Disk I/O") | bold);
    lines.push_back(text("  DEVICE        READ/s    WRITE/s   UTIL%") | dim);
    for (const auto& d : diskIo) {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "  %-12.12s %9s %10s %6.1f%%",
                      d.device.c_str(), FormatUtils::rate(d.readBytesPerSec).c_str(),
                      FormatUtils::rate(d.writeBytesPerSec).c_str(), d.utilizationPercent);
        lines.push_back(text(buf));
    }

    return vbox(lines) | flex;
}

Element buildNetworkView(const NetworkStats& net) {
    Elements lines;
    lines.push_back(text("Total: v " + FormatUtils::rate(net.totalRxBytesPerSec) +
                          "   ^ " + FormatUtils::rate(net.totalTxBytesPerSec)) | bold);
    lines.push_back(text(""));
    lines.push_back(text("INTERFACE       STATUS    DOWN        UP     LINK     RX-DROP%  TX-DROP%") | dim);

    for (const auto& ifs : net.interfaces) {
        char buf[160];
        std::snprintf(buf, sizeof(buf), "%-15.15s %-8s %9s %9s %8s %9.3f %9.3f",
                      ifs.name.c_str(), ifs.isUp ? "Up" : "Down",
                      FormatUtils::rate(ifs.rxBytesPerSec).c_str(), FormatUtils::rate(ifs.txBytesPerSec).c_str(),
                      ifs.linkSpeedMbps ? (std::to_string(ifs.linkSpeedMbps) + "Mbps").c_str() : "n/a",
                      ifs.rxDropPercent, ifs.txDropPercent);
        Color statusColor = ifs.isUp ? Color::Green : Color::GrayDark;
        lines.push_back(hbox({text(buf)}) | color(statusColor));
    }
    return vbox(lines) | flex;
}

Element buildConnectionsView(const std::vector<ProcessConnection>& connections,
                              const std::vector<ProcessInfo>& processes,
                              const std::string& filterText, int scrollOffset, int visibleRows) {
    Elements lines;
    lines.push_back(text("PROCESS              PID PROTO LOCAL                     REMOTE                    STATE") | bold);

    std::string needle = toLower(filterText);
    std::vector<const ProcessConnection*> filtered;
    for (const auto& c : connections) {
        std::string name = pidToNameFrom(processes, c.pid);
        if (!needle.empty()) {
            std::string haystack = toLower(name + " " + c.protocol + " " + c.localAddress + " " +
                                            c.remoteAddress + " " + c.state);
            if (haystack.find(needle) == std::string::npos) continue;
        }
        filtered.push_back(&c);
    }

    int end = std::min(static_cast<int>(filtered.size()), scrollOffset + visibleRows);
    for (int i = scrollOffset; i < end; ++i) {
        const ProcessConnection& c = *filtered[i];
        std::string name = pidToNameFrom(processes, c.pid);
        std::string local = c.localAddress + ":" + std::to_string(c.localPort);
        std::string remote = (c.remoteAddress.empty() || c.remotePort == 0)
            ? "-" : c.remoteAddress + ":" + std::to_string(c.remotePort);

        char buf[256];
        std::snprintf(buf, sizeof(buf), "%-18.18s %6lld %-5.5s %-25.25s %-25.25s ",
                      name.c_str(), static_cast<long long>(c.pid), c.protocol.c_str(),
                      local.c_str(), remote.c_str());

        Color stateColor = (c.state == "ESTABLISHED") ? Color::Cyan
                          : (c.state == "LISTEN") ? Color::Blue
                          : Color::GrayDark;
        lines.push_back(hbox({text(buf), text(c.state) | color(stateColor)}));
    }
    lines.push_back(text(""));
    lines.push_back(text(std::to_string(filtered.size()) + " connections") | dim);
    return vbox(lines) | flex;
}

Element buildBandwidthView(const std::vector<ProcessInterfaceBandwidth>& entries,
                            const std::vector<ProcessInfo>& processes,
                            const std::string& note, int scrollOffset, int visibleRows) {
    Elements lines;
    if (!note.empty()) {
        lines.push_back(text(note) | color(Color::Yellow));
        lines.push_back(text(""));
    }
    lines.push_back(text("PROCESS              PID INTERFACE       DOWN         UP  TOTAL (session)") | bold);

    std::vector<ProcessInterfaceBandwidth> sorted = entries;
    std::sort(sorted.begin(), sorted.end(), [&](const ProcessInterfaceBandwidth& a, const ProcessInterfaceBandwidth& b) {
        std::string nameA = pidToNameFrom(processes, a.pid);
        std::string nameB = pidToNameFrom(processes, b.pid);
        if (nameA != nameB) return nameA < nameB;
        if (a.pid != b.pid) return a.pid < b.pid;
        return a.interfaceName < b.interfaceName;
    });

    int end = std::min(static_cast<int>(sorted.size()), scrollOffset + visibleRows);
    for (int i = scrollOffset; i < end; ++i) {
        const auto& e = sorted[i];
        std::string name = pidToNameFrom(processes, e.pid);
        char buf[220];
        std::string total = "v" + FormatUtils::bytes(e.stats.rxBytesTotal) + " ^" + FormatUtils::bytes(e.stats.txBytesTotal);
        std::snprintf(buf, sizeof(buf), "%-18.18s %6lld %-15.15s %10s %10s  %s",
                      name.c_str(), static_cast<long long>(e.pid), e.interfaceName.c_str(),
                      FormatUtils::rate(e.stats.rxBytesPerSec).c_str(), FormatUtils::rate(e.stats.txBytesPerSec).c_str(),
                      total.c_str());
        lines.push_back(text(buf));
    }
    if (entries.empty() && note.empty()) {
        lines.push_back(text("Tracking active. No processes with measurable traffic yet.") | dim);
    }
    return vbox(lines) | flex;
}

} // namespace

int runTuiApp(bool enableBandwidthTracking) {
    ProcessCollector processCollector;
    SystemStatsCollector systemCollector;
    GpuStatsCollector gpuCollector;
    NetworkStatsCollector networkCollector;
    ProcessConnectionCollector connectionCollector;

    std::unique_ptr<ProcessBandwidthCollector> bandwidthCollector;
    std::string bandwidthNote;
    if (enableBandwidthTracking) {
        bandwidthCollector = std::make_unique<ProcessBandwidthCollector>();
        if (!bandwidthCollector->start()) {
            bandwidthNote = "Bandwidth tracking could not start: " + bandwidthCollector->lastError();
        } else {
            bandwidthNote = bandwidthCollector->lastError(); // may be non-empty even on success (partial availability)
        }
    }

    static const char* kTabNames[] = {"Processes", "Performance", "Network", "Connections", "Bandwidth"};
    int tabCount = enableBandwidthTracking ? 5 : 4;

    TuiState state;
    std::atomic<bool> running{true};

    auto screen = ScreenInteractive::Fullscreen();

    std::thread poller([&] {
        auto veryOld = std::chrono::steady_clock::now() - std::chrono::hours(1);
        auto lastProcess = veryOld, lastStats = veryOld, lastNetwork = veryOld;
        auto lastConnections = veryOld, lastBandwidth = veryOld;

        while (running.load()) {
            auto now = std::chrono::steady_clock::now();
            bool changed = false;

            if (now - lastProcess >= std::chrono::milliseconds(1500)) {
                auto processes = processCollector.collect();
                std::lock_guard<std::mutex> lock(state.mutex);
                state.processes = std::move(processes);
                lastProcess = now;
                changed = true;
            }
            if (now - lastStats >= std::chrono::milliseconds(1000)) {
                auto cpu = systemCollector.collectCpu();
                auto mem = systemCollector.collectMemory();
                auto diskVolumes = systemCollector.collectDiskVolumes();
                auto diskIo = systemCollector.collectDiskIo();
                auto gpus = gpuCollector.collect();
                std::lock_guard<std::mutex> lock(state.mutex);
                state.cpu = cpu;
                state.mem = mem;
                state.diskVolumes = std::move(diskVolumes);
                state.diskIo = std::move(diskIo);
                state.gpus = std::move(gpus);
                lastStats = now;
                changed = true;
            }
            if (now - lastNetwork >= std::chrono::milliseconds(1000)) {
                auto net = networkCollector.collect();
                std::lock_guard<std::mutex> lock(state.mutex);
                state.network = std::move(net);
                lastNetwork = now;
                changed = true;
            }
            if (now - lastConnections >= std::chrono::milliseconds(3000)) {
                auto conns = connectionCollector.collect();
                std::lock_guard<std::mutex> lock(state.mutex);
                state.connections = std::move(conns);
                lastConnections = now;
                changed = true;
            }
            if (bandwidthCollector && bandwidthCollector->isRunning() &&
                now - lastBandwidth >= std::chrono::milliseconds(2000)) {
                auto bw = bandwidthCollector->collect();
                std::lock_guard<std::mutex> lock(state.mutex);
                state.bandwidth = std::move(bw);
                lastBandwidth = now;
                changed = true;
            }

            if (changed) screen.PostEvent(Event::Custom);
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    });

    // --- interactive state -------------------------------------------
    int activeTab = 0;
    int selected = 0;            // Processes tab: selected row index
    int scrollOffsets[5] = {0, 0, 0, 0, 0}; // Connections/Bandwidth tabs: scroll position
    SortBy sortBy = SortBy::Cpu;
    bool sortDescending = true;
    bool filterMode = false;
    std::string filterText;
    bool confirmKillPending = false;
    int64_t confirmKillPid = -1;
    std::string confirmKillName;
    std::string statusMessage;

    auto renderer = Renderer([&] {
        std::vector<ProcessInfo> processes;
        CpuStats cpu;
        MemoryStats mem;
        std::vector<DiskVolume> diskVolumes;
        std::vector<DiskIoStats> diskIo;
        std::vector<GpuInfo> gpus;
        NetworkStats network;
        std::vector<ProcessConnection> connections;
        std::vector<ProcessInterfaceBandwidth> bandwidth;
        {
            std::lock_guard<std::mutex> lock(state.mutex);
            processes = state.processes;
            cpu = state.cpu;
            mem = state.mem;
            diskVolumes = state.diskVolumes;
            diskIo = state.diskIo;
            gpus = state.gpus;
            network = state.network;
            connections = state.connections;
            bandwidth = state.bandwidth;
        }

        // --- header: title + meter bars (persistent across all tabs) --
        int termWidth = Terminal::Size().dimx;
        int barWidth = std::max(10, std::min(30, termWidth / 3));

        Elements headerLines;
        Elements tabLabels;
        for (int i = 0; i < tabCount; ++i) {
            std::string label = " [" + std::to_string(i + 1) + "] " + kTabNames[i] + " ";
            if (i == activeTab) tabLabels.push_back(text(label) | bold | bgcolor(Color::Blue) | color(Color::White));
            else tabLabels.push_back(text(label) | dim);
        }
        headerLines.push_back(
            hbox({
                text(" TaskManager ") | bold | bgcolor(Color::Blue) | color(Color::White),
                text(" "),
                hbox(tabLabels),
            })
        );
        headerLines.push_back(separator());

        headerLines.push_back(meterBar("CPU", cpu.totalPercent, barWidth));
        if (!cpu.perCore.empty()) {
            // Show every core, laid out in as many columns as fit the
            // current terminal width -- the body area below shrinks
            // automatically to make room (tableHeight is computed from
            // headerLines.size() at render time).
            int perCoreBarWidth = std::max(6, std::min(15, termWidth / 8));
            int cellWidth = 7 /* label */ + 2 /* brackets */ + perCoreBarWidth + 2 /* spacing */;
            int colsPerRow = std::max(1, termWidth / cellWidth);

            int totalCores = static_cast<int>(cpu.perCore.size());
            for (int i = 0; i < totalCores; i += colsPerRow) {
                Elements row;
                for (int j = i; j < std::min(i + colsPerRow, totalCores); ++j) {
                    char label[8];
                    std::snprintf(label, sizeof(label), "Core%d", cpu.perCore[j].coreIndex);
                    row.push_back(meterBar(label, cpu.perCore[j].percent, perCoreBarWidth));
                    row.push_back(text("  "));
                }
                headerLines.push_back(hbox(row));
            }
        }
        headerLines.push_back(meterBar("Mem", mem.usedPercent(), barWidth));
        headerLines.push_back(
            text("  " + FormatUtils::bytes(mem.usedBytes) + " / " + FormatUtils::bytes(mem.totalBytes)) | dim
        );
        headerLines.push_back(separator());

        int bodyHeight = Terminal::Size().dimy - static_cast<int>(headerLines.size()) - 4;
        bodyHeight = std::max(3, bodyHeight);

        Element body;
        if (activeTab == 0) {
            // --- filter -----------------------------------------------
            std::string needle = toLower(filterText);
            if (!needle.empty()) {
                std::vector<ProcessInfo> filtered;
                filtered.reserve(processes.size());
                for (auto& p : processes) {
                    std::string haystack = toLower(p.name + " " + p.user + " " + std::to_string(p.pid));
                    if (haystack.find(needle) != std::string::npos) filtered.push_back(p);
                }
                processes = std::move(filtered);
            }
            sortProcesses(processes, sortBy, sortDescending);
            if (selected >= static_cast<int>(processes.size())) {
                selected = std::max(0, static_cast<int>(processes.size()) - 1);
            }

            Elements rows;
            rows.push_back(text("   PID USER         NI   VIRT    RES  %CPU  %MEM S COMMAND") | bold);

            int tableHeight = std::max(3, bodyHeight - 1);
            int scrollOffset = 0;
            if (selected >= tableHeight) scrollOffset = selected - tableHeight + 1;
            int visibleEnd = std::min(static_cast<int>(processes.size()), scrollOffset + tableHeight);

            for (int i = scrollOffset; i < visibleEnd; ++i) {
                const ProcessInfo& p = processes[i];
                double memPercent = mem.totalBytes ? (double(p.memRssBytes) / double(mem.totalBytes)) * 100.0 : 0.0;

                char left[64];
                std::snprintf(left, sizeof(left), "%6lld %-12.12s %3d %6s %6s",
                              static_cast<long long>(p.pid), p.user.c_str(), p.niceValue,
                              compactBytes(p.memVirtBytes).c_str(), compactBytes(p.memRssBytes).c_str());

                char cpuBuf[16];
                std::snprintf(cpuBuf, sizeof(cpuBuf), "%5.1f", p.cpuPercent);
                char memBuf[16];
                std::snprintf(memBuf, sizeof(memBuf), "%5.1f", memPercent);

                char stateChar = p.state.empty() ? '?' : p.state[0];
                std::string command = p.commandLine.empty() ? p.name : p.commandLine;

                Element row = hbox({
                    text(left),
                    text(" "),
                    text(cpuBuf) | color(colorForPercent(p.cpuPercent)),
                    text(" "),
                    text(memBuf),
                    text(" "),
                    text(std::string(1, stateChar)),
                    text(" "),
                    text(command) | flex,
                });

                if (i == selected) row = row | bgcolor(Color::Blue) | color(Color::White);
                rows.push_back(row);
            }
            body = vbox(rows) | flex;
        } else if (activeTab == 1) {
            body = buildPerformanceView(mem, diskVolumes, diskIo, gpus);
        } else if (activeTab == 2) {
            body = buildNetworkView(network);
        } else if (activeTab == 3) {
            body = buildConnectionsView(connections, processes, filterText, scrollOffsets[3], bodyHeight - 2);
        } else {
            body = buildBandwidthView(bandwidth, processes, bandwidthNote, scrollOffsets[4], bodyHeight - 2);
        }

        // --- footer: key hints or active prompt ------------------------
        Element footer;
        if (confirmKillPending) {
            footer = text(" Kill " + confirmKillName + " (PID " + std::to_string(confirmKillPid) + ")? [y/n] ")
                     | bgcolor(Color::Red) | color(Color::White) | bold;
        } else if (filterMode) {
            footer = text(" Search: " + filterText + "_ (Enter/Esc to exit) ") | bgcolor(Color::Yellow) | color(Color::Black);
        } else if (!statusMessage.empty()) {
            footer = text(" " + statusMessage + " ") | bgcolor(Color::Yellow) | color(Color::Black);
        } else if (activeTab == 0) {
            footer = hbox({
                text(" q:Quit ") | bold,
                text(" 1-" + std::to_string(tabCount) + ":Tabs "),
                text(" /:Search "),
                text(" k:Kill "),
                text(" c:SortCPU "),
                text(" m:SortMem "),
                text(" p:SortPID "),
                text(" Up/Down/PgUp/PgDn:Navigate "),
            }) | dim;
        } else if (activeTab == 3) {
            footer = hbox({
                text(" q:Quit ") | bold,
                text(" 1-" + std::to_string(tabCount) + ":Tabs "),
                text(" /:Search "),
                text(" Up/Down/PgUp/PgDn:Scroll "),
            }) | dim;
        } else {
            footer = hbox({
                text(" q:Quit ") | bold,
                text(" 1-" + std::to_string(tabCount) + ":Tabs "),
                text(" Up/Down/PgUp/PgDn:Scroll "),
            }) | dim;
        }

        return vbox({
            vbox(headerLines),
            body,
            separator(),
            footer,
        });
    });

    auto component = CatchEvent(renderer, [&](Event event) -> bool {
        // Latest sorted+filtered Processes snapshot for index-based
        // actions (kill, selection bounds) -- cheap enough to recompute
        // on demand here since it only runs once per keypress.
        auto processSnapshot = [&]() {
            std::vector<ProcessInfo> processes;
            {
                std::lock_guard<std::mutex> lock(state.mutex);
                processes = state.processes;
            }
            std::string needle = toLower(filterText);
            if (!needle.empty()) {
                std::vector<ProcessInfo> filtered;
                for (auto& p : processes) {
                    std::string haystack = toLower(p.name + " " + p.user + " " + std::to_string(p.pid));
                    if (haystack.find(needle) != std::string::npos) filtered.push_back(p);
                }
                processes = std::move(filtered);
            }
            sortProcesses(processes, sortBy, sortDescending);
            return processes;
        };

        // --- kill confirmation prompt takes priority over everything --
        if (confirmKillPending) {
            if (event == Event::Character('y') || event == Event::Character('Y')) {
                processCollector.killProcess(confirmKillPid);
                statusMessage = "Sent terminate signal to PID " + std::to_string(confirmKillPid);
                confirmKillPending = false;
                return true;
            }
            confirmKillPending = false;
            return true;
        }

        // --- search/filter input mode (Processes + Connections tabs) --
        if (filterMode) {
            if (event == Event::Return || event == Event::Escape) {
                filterMode = false;
                return true;
            }
            if (event == Event::Backspace) {
                if (!filterText.empty()) filterText.pop_back();
                return true;
            }
            if (event.is_character()) {
                filterText += event.character();
                return true;
            }
            return true; // swallow all other keys while typing
        }

        statusMessage.clear();

        if (event == Event::Character('q') || event == Event::CtrlC) {
            running = false;
            screen.Exit();
            return true;
        }

        // --- tab switching ---------------------------------------------
        for (int i = 0; i < tabCount; ++i) {
            if (event == Event::Character(static_cast<char>('1' + i))) {
                activeTab = i;
                return true;
            }
        }
        if (event == Event::Character('5') && !enableBandwidthTracking) {
            statusMessage = "Bandwidth tab needs --track-bandwidth on the command line.";
            return true;
        }

        if (activeTab == 0) {
            if (event == Event::Character('/')) { filterMode = true; return true; }
            if (event == Event::Character('c')) { sortBy = SortBy::Cpu; sortDescending = true; return true; }
            if (event == Event::Character('m')) { sortBy = SortBy::Mem; sortDescending = true; return true; }
            if (event == Event::Character('p')) { sortBy = SortBy::Pid; sortDescending = false; return true; }

            auto processes = processSnapshot();
            int count = static_cast<int>(processes.size());

            if (event == Event::ArrowUp) { selected = std::max(0, selected - 1); return true; }
            if (event == Event::ArrowDown) { selected = std::min(count - 1, selected + 1); return true; }
            if (event == Event::PageUp) { selected = std::max(0, selected - 20); return true; }
            if (event == Event::PageDown) { selected = std::min(count - 1, selected + 20); return true; }
            if (event == Event::Home) { selected = 0; return true; }
            if (event == Event::End) { selected = std::max(0, count - 1); return true; }

            if (event == Event::Character('k')) {
                if (selected >= 0 && selected < count) {
                    confirmKillPending = true;
                    confirmKillPid = processes[selected].pid;
                    confirmKillName = processes[selected].name;
                }
                return true;
            }
            return false;
        }

        // --- scrollable read-only tabs (Network has no scroll state --
        // it's typically short enough to fit; Connections/Bandwidth do) --
        if (activeTab == 3 && event == Event::Character('/')) { filterMode = true; return true; }

        int& offset = scrollOffsets[activeTab];
        if (event == Event::ArrowUp) { offset = std::max(0, offset - 1); return true; }
        if (event == Event::ArrowDown) { offset = offset + 1; return true; }
        if (event == Event::PageUp) { offset = std::max(0, offset - 20); return true; }
        if (event == Event::PageDown) { offset = offset + 20; return true; }
        if (event == Event::Home) { offset = 0; return true; }

        return false;
    });

    screen.Loop(component);

    running = false;
    poller.join();
    return 0;
}
