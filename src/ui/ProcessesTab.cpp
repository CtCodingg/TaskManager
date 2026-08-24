#include "ProcessesTab.h"
#include "../Theme.h"
#include "../backend/FormatUtils.h"

#include <algorithm>
#include <cctype>

namespace {

std::string toLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}

bool matchesFilter(const ProcessInfo& p, const std::string& needleLower) {
    if (needleLower.empty()) return true;
    if (toLower(p.name).find(needleLower) != std::string::npos) return true;
    if (toLower(p.user).find(needleLower) != std::string::npos) return true;
    if (std::to_string(p.pid).find(needleLower) != std::string::npos) return true;
    return false;
}

ImVec4 toImVec4(Theme::Rgb c, float a = 1.0f) { return ImVec4(c.r, c.g, c.b, a); }

// Colors a process row's state text by what it signals.
Theme::Rgb colorForState(const std::string& state) {
    if (state == "Running") return {0.204f, 0.827f, 0.600f};   // good/green
    if (state == "Zombie" || state == "Dead") return {0.973f, 0.443f, 0.443f}; // critical/red
    if (state == "Stopped" || state == "Tracing Stop") return {0.984f, 0.749f, 0.141f}; // warn/amber
    return {0.576f, 0.608f, 0.690f}; // textSecondary/neutral
}

} // namespace

void ProcessesTab::updateData(std::vector<ProcessInfo> processes) {
    m_processes = std::move(processes);
    // Re-apply the last-known sort to this fresh data -- see the member
    // comment in ProcessesTab.h for why this is necessary (ImGui's
    // SpecsDirty flag doesn't fire just because the data changed).
    if (m_sortColumn >= 0) applySort(m_sortColumn, m_sortAscending);
}

std::string ProcessesTab::nameForPid(int64_t pid) const {
    for (const auto& p : m_processes) {
        if (p.pid == pid) return p.name;
    }
    return "(pid " + std::to_string(pid) + ")";
}

void ProcessesTab::applySort(int column, bool ascending) {
    // Pure "x's key < y's key" relation, used as-is for ascending and with
    // swapped arguments for descending. Negating this instead (as an
    // earlier version did) breaks strict weak ordering whenever two
    // elements tie on the sort key -- e.g. two processes both at 0.0%
    // CPU, which is common right after startup -- and std::stable_sort
    // (or MSVC's debug STL specifically) can throw "invalid comparator"
    // as a result.
    auto keyLess = [column](const ProcessInfo& x, const ProcessInfo& y) -> bool {
        switch (column) {
            case 0: return x.pid < y.pid;
            case 1: return x.name < y.name;
            case 2: return x.user < y.user;
            case 3: return x.state < y.state;
            case 4: return x.cpuPercent < y.cpuPercent;
            case 5: return x.memRssBytes < y.memRssBytes;
            case 6: return x.threadCount < y.threadCount;
            default: return x.pid < y.pid;
        }
    };

    std::stable_sort(m_processes.begin(), m_processes.end(),
        [&](const ProcessInfo& a, const ProcessInfo& b) {
            return ascending ? keyLess(a, b) : keyLess(b, a);
        });
}

void ProcessesTab::draw(ProcessCollector& collector) {
    ImGui::PushItemWidth(300);
    ImGui::InputTextWithHint("##filter", "Filter by name, user, or PID...", m_filterBuf, sizeof(m_filterBuf));
    ImGui::PopItemWidth();
    ImGui::SameLine();

    bool killClicked = ImGui::Button("End Task");

    std::string needleLower = toLower(m_filterBuf);

    // Build the filtered view. For huge process counts this per-frame
    // filter is fine (a few hundred entries, negligible cost); if this
    // ever needs to scale further, cache the filtered index list and only
    // rebuild it when m_filterBuf or m_processes actually changes.
    std::vector<int> visibleIndices;
    visibleIndices.reserve(m_processes.size());
    for (int i = 0; i < static_cast<int>(m_processes.size()); ++i) {
        if (matchesFilter(m_processes[i], needleLower)) visibleIndices.push_back(i);
    }

    static ImGuiTableFlags tableFlags =
        ImGuiTableFlags_Sortable | ImGuiTableFlags_Resizable | ImGuiTableFlags_RowBg |
        ImGuiTableFlags_BordersOuter | ImGuiTableFlags_BordersV | ImGuiTableFlags_ScrollY |
        ImGuiTableFlags_SizingStretchProp;

    ImVec2 tableSize = ImVec2(0, ImGui::GetContentRegionAvail().y - 30);
    if (ImGui::BeginTable("ProcessTable", 8, tableFlags, tableSize)) {
        ImGui::TableSetupColumn("PID", ImGuiTableColumnFlags_WidthFixed, 70);
        ImGui::TableSetupColumn("Name");
        ImGui::TableSetupColumn("User");
        ImGui::TableSetupColumn("State");
        // DefaultSort + PreferSortDescending: matches the Qt edition's
        // default (sortByColumn(ColCpu, Qt::DescendingOrder)) -- the
        // table opens already sorted by CPU% descending, not raw
        // collector order.
        ImGui::TableSetupColumn("CPU %", ImGuiTableColumnFlags_WidthFixed |
                                 ImGuiTableColumnFlags_DefaultSort |
                                 ImGuiTableColumnFlags_PreferSortDescending, 80);
        ImGui::TableSetupColumn("Memory", ImGuiTableColumnFlags_WidthFixed, 90);
        ImGui::TableSetupColumn("Threads", ImGuiTableColumnFlags_WidthFixed, 70);
        ImGui::TableSetupColumn("Command");
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();

        if (ImGuiTableSortSpecs* sortSpecs = ImGui::TableGetSortSpecs()) {
            if (sortSpecs->SpecsDirty && sortSpecs->SpecsCount > 0) {
                const ImGuiTableColumnSortSpecs& spec = sortSpecs->Specs[0];
                m_sortColumn = spec.ColumnIndex;
                m_sortAscending = (spec.SortDirection == ImGuiSortDirection_Ascending);
                applySort(m_sortColumn, m_sortAscending);
                sortSpecs->SpecsDirty = false;
            }
        }

        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(visibleIndices.size()));
        while (clipper.Step()) {
            for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
                int idx = visibleIndices[row];
                const ProcessInfo& p = m_processes[idx];

                ImGui::TableNextRow();
                ImGui::TableNextColumn();

                bool selected = (m_selectedRow == idx);
                char label[32];
                std::snprintf(label, sizeof(label), "%lld", static_cast<long long>(p.pid));
                if (ImGui::Selectable(label, selected, ImGuiSelectableFlags_SpanAllColumns)) {
                    m_selectedRow = idx;
                }

                ImGui::TableNextColumn();
                ImGui::TextUnformatted(p.name.c_str());

                ImGui::TableNextColumn();
                ImGui::TextUnformatted(p.user.c_str());

                ImGui::TableNextColumn();
                Theme::Rgb stateColor = colorForState(p.state);
                ImGui::TextColored(toImVec4(stateColor), "%s", p.state.c_str());

                ImGui::TableNextColumn();
                Theme::Rgb cpuColor = Theme::colorForPercent(p.cpuPercent);
                ImGui::TextColored(toImVec4(cpuColor), "%s", FormatUtils::percent(p.cpuPercent).c_str());

                ImGui::TableNextColumn();
                ImGui::TextUnformatted(FormatUtils::bytes(p.memRssBytes).c_str());

                ImGui::TableNextColumn();
                ImGui::Text("%d", p.threadCount);

                ImGui::TableNextColumn();
                ImGui::TextUnformatted(p.commandLine.c_str());
            }
        }

        ImGui::EndTable();
    }

    ImGui::Text("%zu processes", m_processes.size());

    if (killClicked && m_selectedRow >= 0 && m_selectedRow < static_cast<int>(m_processes.size())) {
        collector.killProcess(m_processes[m_selectedRow].pid);
    }
}
