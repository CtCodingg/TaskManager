#include "PerformanceTab.h"
#include "../Theme.h"
#include "../backend/FormatUtils.h"
#include "imgui.h"

namespace {
ImVec4 toImVec4(Theme::Rgb c, float a = 1.0f) { return ImVec4(c.r, c.g, c.b, a); }

void colorBar(const char* label, float fraction, Theme::Rgb color, const char* overlay) {
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, toImVec4(color));
    ImGui::ProgressBar(fraction, ImVec2(-1, 0), overlay);
    ImGui::PopStyleColor();
}
}

PerformanceTab::PerformanceTab()
    : m_cpuChart("CPU Load (%)", 120), m_memChart("Memory Usage (%)", 120), m_gpuChart("GPU Load (%)", 120) {
    m_cpuChart.setYRange(0, 100);
    m_cpuSeriesIdx = m_cpuChart.addSeries("Total", toImVec4(Theme::accentCpu()));

    m_memChart.setYRange(0, 100);
    m_memSeriesIdx = m_memChart.addSeries("Used", toImVec4(Theme::accentMemory()));

    m_gpuChart.setYRange(0, 100);
}

void PerformanceTab::updateData(const CpuStats& cpu, const MemoryStats& mem,
                                 const std::vector<DiskVolume>& disks, const std::vector<DiskIoStats>& diskIo,
                                 const std::vector<GpuInfo>& gpus) {
    m_cpu = cpu;
    m_mem = mem;
    m_disks = disks;
    m_diskIo = diskIo;
    m_gpus = gpus;

    m_cpuChart.pushValue(m_cpuSeriesIdx, cpu.totalPercent);
    m_memChart.pushValue(m_memSeriesIdx, mem.usedPercent());

    if (!gpus.empty() && gpus[0].loadPercent >= 0) {
        if (m_gpuSeriesIdx < 0) m_gpuSeriesIdx = m_gpuChart.addSeries(gpus[0].name, toImVec4(Theme::accentGpu()));
        m_gpuChart.pushValue(m_gpuSeriesIdx, gpus[0].loadPercent);
    }
}

void PerformanceTab::draw() {
    // --- CPU ---
    ImGui::TextColored(toImVec4(Theme::accentCpu()), "CPU");
    ImGui::Separator();
    char cpuLabel[32];
    std::snprintf(cpuLabel, sizeof(cpuLabel), "%s", FormatUtils::percent(m_cpu.totalPercent).c_str());
    ImGui::Text("Total: %s", cpuLabel);
    if (m_cpu.temperatureC >= 0) ImGui::SameLine(), ImGui::Text("  Temp: %.1f C", m_cpu.temperatureC);
    m_cpuChart.draw();

    if (ImGui::BeginTable("CoreBars", 4, ImGuiTableFlags_SizingStretchSame)) {
        for (size_t i = 0; i < m_cpu.perCore.size(); ++i) {
            ImGui::TableNextColumn();
            const auto& core = m_cpu.perCore[i];
            char label[64];
            std::snprintf(label, sizeof(label), "Core %d: %.0f%%", core.coreIndex, core.percent);
            Theme::Rgb color = Theme::colorForPercent(core.percent);
            colorBar("##core", static_cast<float>(core.percent / 100.0), color, label);
        }
        ImGui::EndTable();
    }
    ImGui::Spacing();

    // --- Memory ---
    ImGui::TextColored(toImVec4(Theme::accentMemory()), "Memory");
    ImGui::Separator();
    ImGui::Text("%s / %s (%s)",
                FormatUtils::bytes(m_mem.usedBytes).c_str(),
                FormatUtils::bytes(m_mem.totalBytes).c_str(),
                FormatUtils::percent(m_mem.usedPercent()).c_str());
    colorBar("##mem", static_cast<float>(m_mem.usedPercent() / 100.0), Theme::colorForPercent(m_mem.usedPercent()), nullptr);
    ImGui::Text("Swap: %s / %s", FormatUtils::bytes(m_mem.swapUsedBytes).c_str(), FormatUtils::bytes(m_mem.swapTotalBytes).c_str());
    m_memChart.draw();
    ImGui::Spacing();

    // --- GPU ---
    ImGui::TextColored(toImVec4(Theme::accentGpu()), "GPU");
    ImGui::Separator();
    if (m_gpus.empty()) {
        ImGui::TextDisabled("No supported GPU backend detected on this system.");
    } else {
        for (const auto& g : m_gpus) {
            ImGui::Text("%s (%s)", g.name.c_str(), g.vendor.c_str());
            ImGui::Text("Load: %s   Memory: %s / %s",
                        g.loadPercent >= 0 ? FormatUtils::percent(g.loadPercent).c_str() : "n/a",
                        FormatUtils::bytes(g.memUsedBytes).c_str(),
                        g.memTotalBytes ? FormatUtils::bytes(g.memTotalBytes).c_str() : "n/a");
            if (g.temperatureC >= 0) { ImGui::SameLine(); ImGui::Text("  Temp: %.1f C", g.temperatureC); }
            if (g.powerWatts >= 0) { ImGui::SameLine(); ImGui::Text("  Power: %.1f W", g.powerWatts); }
        }
        m_gpuChart.draw();
    }
    ImGui::Spacing();

    // --- Disks ---
    ImGui::TextColored(toImVec4(Theme::accentDisk()), "Disks");
    ImGui::Separator();
    ImGui::TextDisabled("Volumes");
    if (ImGui::BeginTable("DiskVolumes", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Mount");
        ImGui::TableSetupColumn("Filesystem");
        ImGui::TableSetupColumn("Used");
        ImGui::TableSetupColumn("Total");
        ImGui::TableSetupColumn("Usage");
        ImGui::TableHeadersRow();
        for (const auto& v : m_disks) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextUnformatted(v.mountPoint.c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(v.fsType.c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(FormatUtils::bytes(v.usedBytes).c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(FormatUtils::bytes(v.totalBytes).c_str());
            ImGui::TableNextColumn();
            ImGui::TextColored(toImVec4(Theme::colorForPercent(v.usedPercent())), "%s", FormatUtils::percent(v.usedPercent()).c_str());
        }
        ImGui::EndTable();
    }

    ImGui::TextDisabled("I/O Activity");
    if (ImGui::BeginTable("DiskIo", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Device");
        ImGui::TableSetupColumn("Read/s");
        ImGui::TableSetupColumn("Write/s");
        ImGui::TableSetupColumn("Utilization");
        ImGui::TableHeadersRow();
        for (const auto& d : m_diskIo) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextUnformatted(d.device.c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(FormatUtils::rate(d.readBytesPerSec).c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(FormatUtils::rate(d.writeBytesPerSec).c_str());
            ImGui::TableNextColumn();
            ImGui::TextColored(toImVec4(Theme::colorForPercent(d.utilizationPercent)), "%s", FormatUtils::percent(d.utilizationPercent).c_str());
        }
        ImGui::EndTable();
    }
}
