#include "App.h"
#include "imgui.h"
#include <GLFW/glfw3.h>

namespace {
void drawStubTab(const char* name, const char* note) {
    ImGui::TextDisabled("%s", name);
    ImGui::Separator();
    ImGui::Spacing();
    ImGui::TextWrapped(
        "Not yet ported from the Qt edition. %s", note);
    ImGui::TextWrapped(
        "Pattern to follow: see ui/ProcessesTab.h/.cpp for a fully worked "
        "example (collector -> poll timer -> ImGui::BeginTable with "
        "sorting/filtering).");
}
}

App::App() {}

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
    ImGui::Begin("TaskManagerRoot", nullptr, flags);
    ImGui::PopStyleVar(2);

    if (ImGui::BeginMenuBar()) {
        if (ImGui::MenuItem("Settings")) { /* TODO: settings modal */ }
        if (ImGui::MenuItem("About")) { /* TODO: about modal */ }
        ImGui::EndMenuBar();
    }

    // --- Poll on a timer, same cadence as the Qt edition's QTimer -------
    double now = glfwGetTime();
    if (now - m_lastProcessPollTime >= m_processPollIntervalSec) {
        m_processesTab.updateData(m_processCollector.collect());
        m_lastProcessPollTime = now;
    }

    if (ImGui::BeginTabBar("MainTabs")) {
        if (ImGui::BeginTabItem("Processes")) {
            m_processesTab.draw(m_processCollector);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Performance")) {
            drawStubTab("Performance", "Ports SystemStatsCollector + GpuStatsCollector; charts via ImPlot::PlotLine in a rolling buffer (see HistoryChartWidget.cpp in the Qt edition for the exact data-shape to replicate).");
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Network")) {
            drawStubTab("Network", "Ports NetworkStatsCollector; same ImGui::BeginTable pattern as Processes.");
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Connections")) {
            drawStubTab("Connections", "Ports ProcessConnectionCollector; same table pattern.");
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Bandwidth")) {
            drawStubTab("Bandwidth", "Ports ProcessBandwidthCollector (Netlink/ETW backends carry over almost unchanged -- they never used Qt types internally beyond QString/QMap, which are already swapped in Types.h's pattern).");
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

    ImGui::End();
}
