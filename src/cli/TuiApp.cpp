// htop-style terminal UI for --tui mode. Runs the same backend
// collectors as the graphical mode (src/backend/) on a background
// polling thread; this file is purely the view + keyboard-input layer.
//
// Fully implemented: Processes view (sortable/filterable table, CPU/
// memory meter bars, kill-with-confirmation) -- this is the view that
// actually defines "looks like htop". Tabs 2-5 (Performance/Network/
// Connections/Bandwidth) are reachable via number keys but currently
// just show a placeholder; they'd follow the exact same pattern as
// Processes (collector -> poll thread -> render), just not written yet.

#include "TuiApp.h"

#include "../backend/ProcessCollector.h"
#include "../backend/SystemStatsCollector.h"
#include "../backend/GpuStatsCollector.h"
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
// Negating the ascending result instead (an earlier version did this)
// breaks strict weak ordering whenever two elements tie on the sort key
// -- e.g. two processes both at 0.0% CPU, which is common right after
// startup, since CPU% needs two polls before it's meaningful -- and
// std::sort can throw "invalid comparator" as a result (this is exactly
// what caused --tui to crash immediately: the very first frame sorts
// descending by CPU with almost every process still tied at 0.0%).
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

// --- shared state between the poll thread and the render thread ----------

struct TuiState {
    std::mutex mutex;
    std::vector<ProcessInfo> processes;
    CpuStats cpu;
    MemoryStats mem;
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

} // namespace

int runTuiApp(bool enableBandwidthTracking) {
    // Bandwidth tracking isn't wired into the TUI's tab set yet (see file
    // header) -- accepted for CLI-flag symmetry with the graphical mode,
    // silently unused for now rather than pretending it does something.
    (void)enableBandwidthTracking;

    ProcessCollector processCollector;
    SystemStatsCollector systemCollector;
    GpuStatsCollector gpuCollector;
    (void)gpuCollector; // reserved for the Performance tab once it's ported here

    TuiState state;
    std::atomic<bool> running{true};

    auto screen = ScreenInteractive::Fullscreen();

    std::thread poller([&] {
        while (running.load()) {
            auto processes = processCollector.collect();
            auto cpu = systemCollector.collectCpu();
            auto mem = systemCollector.collectMemory();
            {
                std::lock_guard<std::mutex> lock(state.mutex);
                state.processes = std::move(processes);
                state.cpu = cpu;
                state.mem = mem;
            }
            screen.PostEvent(Event::Custom);

            for (int i = 0; i < 15 && running.load(); ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }
    });

    // --- interactive state -------------------------------------------
    int selected = 0;
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
        {
            std::lock_guard<std::mutex> lock(state.mutex);
            processes = state.processes;
            cpu = state.cpu;
            mem = state.mem;
        }

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

        // --- sort ---------------------------------------------------
        sortProcesses(processes, sortBy, sortDescending);

        if (selected >= static_cast<int>(processes.size())) {
            selected = std::max(0, static_cast<int>(processes.size()) - 1);
        }

        // --- header: title + meter bars ------------------------------
        int termWidth = Terminal::Size().dimx;
        int barWidth = std::max(10, std::min(30, termWidth / 3));

        Elements headerLines;
        headerLines.push_back(
            hbox({
                text(" TaskManager ") | bold | bgcolor(Color::Blue) | color(Color::White),
                text(" -- htop-style TUI mode ") | dim,
                filler(),
                text("[1] Processes  [2-5] other tabs (not yet in TUI)") | dim,
            })
        );
        headerLines.push_back(separator());

        headerLines.push_back(meterBar("CPU", cpu.totalPercent, barWidth));
        if (!cpu.perCore.empty()) {
            // Show every core, laid out in as many columns as fit the
            // current terminal width -- no cap. The process table below
            // shrinks automatically to make room (tableHeight is computed
            // from headerLines.size() at render time), so this stays
            // correct on machines with many cores; it just uses more of
            // the screen for the header on those systems.
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

        // --- process table --------------------------------------------
        Elements rows;
        rows.push_back(
            text("   PID USER         NI   VIRT    RES  %CPU  %MEM S COMMAND") | bold
        );

        int tableHeight = Terminal::Size().dimy - static_cast<int>(headerLines.size()) - 6;
        tableHeight = std::max(3, tableHeight);

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

        // --- footer: key hints or active prompt ------------------------
        Element footer;
        if (confirmKillPending) {
            footer = text(" Kill " + confirmKillName + " (PID " + std::to_string(confirmKillPid) + ")? [y/n] ")
                     | bgcolor(Color::Red) | color(Color::White) | bold;
        } else if (filterMode) {
            footer = text(" Search: " + filterText + "_ (Enter/Esc to exit) ") | bgcolor(Color::Yellow) | color(Color::Black);
        } else if (!statusMessage.empty()) {
            footer = text(" " + statusMessage + " ") | bgcolor(Color::Yellow) | color(Color::Black);
        } else {
            footer = hbox({
                text(" q:Quit ") | bold,
                text(" /:Search "),
                text(" k:Kill "),
                text(" c:SortCPU "),
                text(" m:SortMem "),
                text(" p:SortPID "),
                text(" Up/Down/PgUp/PgDn:Navigate "),
            }) | dim;
        }

        return vbox({
            vbox(headerLines),
            separator(),
            vbox(rows) | flex,
            separator(),
            footer,
        });
    });

    auto component = CatchEvent(renderer, [&](Event event) -> bool {
        // Latest sorted+filtered snapshot for index-based actions (kill,
        // selection bounds) -- cheap enough to recompute on demand here
        // since it only runs once per keypress, not per frame.
        auto snapshot = [&]() {
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

        // --- search/filter input mode ---------------------------------
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
        if (event == Event::Character('/')) {
            filterMode = true;
            return true;
        }
        if (event == Event::Character('c')) { sortBy = SortBy::Cpu; sortDescending = true; return true; }
        if (event == Event::Character('m')) { sortBy = SortBy::Mem; sortDescending = true; return true; }
        if (event == Event::Character('p')) { sortBy = SortBy::Pid; sortDescending = false; return true; }
        if (event == Event::Character('1')) { return true; }
        if (event == Event::Character('2') || event == Event::Character('3') ||
            event == Event::Character('4') || event == Event::Character('5')) {
            statusMessage = "That tab isn't implemented in TUI mode yet -- press 1 for Processes.";
            return true;
        }

        auto processes = snapshot();
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
    });

    screen.Loop(component);

    running = false;
    poller.join();
    return 0;
}
