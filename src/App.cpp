#include "App.h"
#include "imgui.h"
#include <GLFW/glfw3.h>

App::App(bool enableBandwidthTracking) : m_bandwidthTrackingEnabled(enableBandwidthTracking) {
    m_refreshRateSec = m_settingsWindow.refreshRateMs() / 1000.0;

    if (m_bandwidthTrackingEnabled) {
        m_bandwidthCollector = std::make_unique<ProcessBandwidthCollector>();
        if (m_bandwidthCollector->start()) {
            m_bandwidthTab.setAvailabilityNote(m_bandwidthCollector->lastError());
        } else {
            m_bandwidthTab.setAvailabilityNote("Bandwidth tracking could not start: " + m_bandwidthCollector->lastError());
        }
    }
}

std::string App::pidToName(int64_t pid) const {
    return m_processesTab.nameForPid(pid);
}

void App::pollIfDue() {
    double now = glfwGetTime();

    if (now - m_lastProcessPoll >= m_refreshRateSec) {
        m_processesTab.updateData(m_processCollector.collect());
        m_lastProcessPoll = now;
    }

    if (now - m_lastStatsPoll >= m_refreshRateSec) {
        CpuStats cpu = m_systemCollector.collectCpu();
        MemoryStats mem = m_systemCollector.collectMemory();
        auto disks = m_systemCollector.collectDiskVolumes();
        auto diskIo = m_systemCollector.collectDiskIo();
        auto gpus = m_gpuCollector.collect();
        m_performanceTab.updateData(cpu, mem, disks, diskIo, gpus);

        m_networkTab.updateData(m_networkCollector.collect());
        m_lastStatsPoll = now;
    }

    // Slower interval: on Linux this scans every process's /proc/<pid>/fd
    // table, more expensive than the other collectors.
    if (now - m_lastConnectionsPoll >= m_refreshRateSec * 3.0) {
        m_connectionsTab.updateData(m_connectionCollector.collect());
        m_lastConnectionsPoll = now;
    }

    if (m_bandwidthTrackingEnabled && m_bandwidthCollector && m_bandwidthCollector->isRunning()) {
        if (now - m_lastBandwidthPoll >= m_refreshRateSec * 2.0) {
            m_bandwidthTab.updateData(m_bandwidthCollector->collect());
            m_lastBandwidthPoll = now;
        }
    }
}

void App::draw() {
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    // No SetNextWindowViewport() call: that's part of the multi-viewport
    // (multiple OS windows) API, which requires
    // ImGuiConfigFlags_ViewportsEnable -- we don't use that, so it's both
    // unneeded and, without it enabled, not guaranteed to be available.

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
                              ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                              ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus |
                              ImGuiWindowFlags_MenuBar;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::Begin("CtTaskManagerRoot", nullptr, flags);
    ImGui::PopStyleVar(2);

    if (ImGui::BeginMenuBar()) {
        if (ImGui::MenuItem("Settings")) m_settingsWindow.open();
        if (ImGui::MenuItem("About")) m_aboutWindow.open();
        ImGui::EndMenuBar();
    }

    if (m_settingsWindow.draw()) {
        m_refreshRateSec = m_settingsWindow.refreshRateMs() / 1000.0;
    }
    m_aboutWindow.draw();

    pollIfDue();

    if (ImGui::BeginTabBar("MainTabs")) {
        if (ImGui::BeginTabItem("Processes")) {
            m_processesTab.draw(m_processCollector);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Performance")) {
            m_performanceTab.draw();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Network")) {
            m_networkTab.draw();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Connections")) {
            m_connectionsTab.draw([this](int64_t pid) { return pidToName(pid); });
            ImGui::EndTabItem();
        }
        if (m_bandwidthTrackingEnabled) {
            if (ImGui::BeginTabItem("Bandwidth")) {
                m_bandwidthTab.draw([this](int64_t pid) { return pidToName(pid); });
                ImGui::EndTabItem();
            }
        }
        ImGui::EndTabBar();
    }

    ImGui::End();
}
