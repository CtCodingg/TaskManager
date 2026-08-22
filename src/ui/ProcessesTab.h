#pragma once

#include "backend/Types.h"
#include "backend/ProcessCollector.h"
#include "imgui.h"
#include <vector>
#include <string>

// Fully-worked reference tab: sortable/filterable process table via
// ImGui::BeginTable + ImGuiListClipper. This is the pattern the other
// tabs (Performance, Network, Connections, Bandwidth) follow.
class ProcessesTab {
public:
    void updateData(std::vector<ProcessInfo> processes);
    void draw(ProcessCollector& collector);

    // Looks up a process name by PID from the most recently collected
    // list -- used by ConnectionsTab/BandwidthTab so they don't need
    // their own process enumeration just to show a name.
    std::string nameForPid(int64_t pid) const;

private:
    std::vector<ProcessInfo> m_processes;
    char m_filterBuf[128] = "";
    int m_selectedRow = -1;

    void applySort(ImGuiTableSortSpecs* sortSpecs);
};
