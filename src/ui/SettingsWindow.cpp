#include "SettingsWindow.h"
#include "../backend/FormatUtils.h"
#include "imgui.h"

#include <fstream>
#include <sstream>
#include <cstdlib>

namespace {
// Simple config file location: %APPDATA%\CtTaskManager_settings.cfg on
// Windows, ~/.ctTaskManager_settings.cfg on Linux. Hand-rolled and
// deliberately minimal (two key=value lines), not a general-purpose format.
std::string configPath() {
#ifdef _WIN32
    const char* appdata = std::getenv("APPDATA");
    std::string base = appdata ? appdata : ".";
    return base + "\\CtTaskManager_settings.cfg";
#else
    const char* home = std::getenv("HOME");
    std::string base = home ? home : ".";
    return base + "/.ctTaskManager_settings.cfg";
#endif
}
}

SettingsWindow::SettingsWindow() {
    load();
}

void SettingsWindow::load() {
    std::ifstream f(configPath());
    if (!f) return;
    std::string line;
    while (std::getline(f, line)) {
        std::istringstream iss(line);
        std::string key;
        if (std::getline(iss, key, '=')) {
            std::string value;
            std::getline(iss, value);
            if (key == "useBits") m_useBits = (value == "1");
            else if (key == "refreshRateMs") { try { m_refreshRateMs = std::stoi(value); } catch (...) {} }
        }
    }
    FormatUtils::setRateUnit(m_useBits ? FormatUtils::RateUnit::Bits : FormatUtils::RateUnit::Bytes);
}

void SettingsWindow::save() {
    std::ofstream f(configPath());
    if (!f) return;
    f << "useBits=" << (m_useBits ? "1" : "0") << "\n";
    f << "refreshRateMs=" << m_refreshRateMs << "\n";
}

void SettingsWindow::open() {
    m_shouldOpen = true;
}

bool SettingsWindow::draw() {
    if (m_shouldOpen) {
        ImGui::OpenPopup("Settings");
        m_shouldOpen = false;
    }

    bool changed = false;
    ImGui::SetNextWindowSize(ImVec2(420, 0), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("Settings", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Data rate unit:");
        if (ImGui::RadioButton("Bits (bit/s, kbit/s, Mbit/s, Gbit/s)", m_useBits)) m_useBits = true;
        if (ImGui::RadioButton("Bytes (B/s, KB/s, MB/s, GB/s)", !m_useBits)) m_useBits = false;

        ImGui::Spacing();
        ImGui::Text("Refresh rate (ms):");
        ImGui::SliderInt("##refresh", &m_refreshRateMs, 200, 10000);

        ImGui::Spacing();
        ImGui::TextDisabled(
            "Applies to network throughput, disk I/O, and per-process bandwidth.\n"
            "Cumulative totals (memory, disk space, session totals) always stay in bytes.");

        ImGui::Spacing();
        ImGui::Separator();
        if (ImGui::Button("OK", ImVec2(120, 0))) {
            FormatUtils::setRateUnit(m_useBits ? FormatUtils::RateUnit::Bits : FormatUtils::RateUnit::Bytes);
            save();
            changed = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 0))) {
            load(); // discard in-memory edits, reload from disk
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    return changed;
}
