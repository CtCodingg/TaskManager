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

    // Remembers the user's last chosen sort column/direction so it can be
    // RE-APPLIED every time updateData() replaces m_processes with fresh,
    // unsorted data from the collector (roughly once a second). ImGui's
    // own SpecsDirty flag only fires when the USER changes the sort
    // column/direction, not when the underlying data changes -- without
    // this, the table would revert to raw collector order right after
    // each poll, undoing the sort within a second of clicking a header.
    // -1 = not yet established (picks up the CPU-descending default sort
    // via ImGuiTableColumnFlags_DefaultSort on the first frame).
    int m_sortColumn = -1;
    bool m_sortAscending = false;

    void applySort(int column, bool ascending);
};
