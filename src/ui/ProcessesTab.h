#pragma once

#include "backend/Types.h"
#include "backend/ProcessCollector.h"
#include "imgui.h"
#include <vector>
#include <string>

// Fully-worked reference tab: sortable/filterable process table via
// ImGui::BeginTable, replacing the Qt edition's ProcessModel +
// QSortFilterProxyModel + QTableView. This is the pattern the other four
// tabs (Performance, Network, Connections, Bandwidth) should follow.
class ProcessesTab {
public:
    void updateData(std::vector<ProcessInfo> processes);
    void draw(ProcessCollector& collector);

private:
    std::vector<ProcessInfo> m_processes;
    char m_filterBuf[128] = "";
    int m_selectedRow = -1;

    void applySort(ImGuiTableSortSpecs* sortSpecs);
};
