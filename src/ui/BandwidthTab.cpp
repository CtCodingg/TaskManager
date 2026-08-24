#include "BandwidthTab.h"
#include "../backend/FormatUtils.h"
#include "imgui.h"

#include <algorithm>
#include <map>
#include <cstdint>

namespace {

// One process's aggregate (summed across all its interfaces) plus the
// per-interface breakdown that gets shown when the row is expanded.
struct ProcessGroup {
    int64_t pid = 0;
    std::string name;
    ProcessBandwidthStats total;
    std::vector<ProcessInterfaceBandwidth> perInterface;
};

std::vector<ProcessGroup> groupByProcess(const std::vector<ProcessInterfaceBandwidth>& entries,
                                          const std::function<std::string(int64_t)>& pidToName) {
    std::map<int64_t, ProcessGroup> groups;

    for (const auto& e : entries) {
        auto& g = groups[e.pid];
        g.pid = e.pid;
        g.total.rxBytesPerSec += e.stats.rxBytesPerSec;
        g.total.txBytesPerSec += e.stats.txBytesPerSec;
        g.total.rxBytesTotal += e.stats.rxBytesTotal;
        g.total.txBytesTotal += e.stats.txBytesTotal;
        g.perInterface.push_back(e);
    }

    std::vector<ProcessGroup> result;
    result.reserve(groups.size());
    for (auto& kv : groups) {
        kv.second.name = pidToName(kv.second.pid);
        // Interfaces sorted alphabetically within each process for stable ordering.
        std::sort(kv.second.perInterface.begin(), kv.second.perInterface.end(),
                  [](const ProcessInterfaceBandwidth& a, const ProcessInterfaceBandwidth& b) {
                      return a.interfaceName < b.interfaceName;
                  });
        result.push_back(std::move(kv.second));
    }

    std::sort(result.begin(), result.end(), [](const ProcessGroup& a, const ProcessGroup& b) {
        if (a.name != b.name) return a.name < b.name;
        return a.pid < b.pid;
    });
    return result;
}

} // namespace

void BandwidthTab::updateData(std::vector<ProcessInterfaceBandwidth> entries) {
    m_entries = std::move(entries);
}

void BandwidthTab::draw(const std::function<std::string(int64_t)>& pidToName) {
    if (!m_availabilityNote.empty()) {
        ImGui::TextColored(ImVec4(0.984f, 0.749f, 0.141f, 1.0f), "%s", m_availabilityNote.c_str());
        ImGui::Spacing();
    }

    std::vector<ProcessGroup> groups = groupByProcess(m_entries, pidToName);

    if (groups.empty()) {
        ImGui::TextUnformatted("Tracking active. No processes with measurable traffic yet.");
    } else {
        ImGui::Text("Tracking %zu process(es).", groups.size());
    }

    ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                             ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY |
                             ImGuiTableFlags_SizingStretchProp;
    ImVec2 size(0, ImGui::GetContentRegionAvail().y - 60);
    if (ImGui::BeginTable("BandwidthTable", 5, flags, size)) {
        ImGui::TableSetupColumn("Process / Interface");
        ImGui::TableSetupColumn("PID", ImGuiTableColumnFlags_WidthFixed, 70);
        ImGui::TableSetupColumn("Download");
        ImGui::TableSetupColumn("Upload");
        ImGui::TableSetupColumn("Total (session)");
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();

        for (const auto& group : groups) {
            // --- Parent row: process, summed over every interface -----
            ImGui::TableNextRow();

            // Subtle background tint so the summary row visually stands
            // out from the (unindented but plain) interface rows under it.
            ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, IM_COL32(38, 47, 61, 255));

            ImGui::TableNextColumn();
            ImGuiTreeNodeFlags nodeFlags = ImGuiTreeNodeFlags_SpanAllColumns |
                                            ImGuiTreeNodeFlags_FramePadding;
            if (group.perInterface.size() <= 1) nodeFlags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
            bool open = ImGui::TreeNodeEx(reinterpret_cast<void*>(static_cast<intptr_t>(group.pid)),
                                           nodeFlags, "%s", group.name.c_str());

            ImGui::TableNextColumn();
            ImGui::Text("%lld", static_cast<long long>(group.pid));

            ImGui::TableNextColumn();
            ImGui::TextUnformatted(FormatUtils::rate(group.total.rxBytesPerSec).c_str());

            ImGui::TableNextColumn();
            ImGui::TextUnformatted(FormatUtils::rate(group.total.txBytesPerSec).c_str());

            ImGui::TableNextColumn();
            ImGui::Text("v %s  ^ %s",
                        FormatUtils::bytes(group.total.rxBytesTotal).c_str(),
                        FormatUtils::bytes(group.total.txBytesTotal).c_str());

            // --- Child rows: one per interface, only while expanded ----
            // (skipped entirely for single-interface processes, since the
            // parent row above -- marked Leaf/NoTreePushOnOpen -- already
            // shows that one interface's numbers with nothing to add.)
            if (open && group.perInterface.size() > 1) {
                for (const auto& e : group.perInterface) {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TreeNodeEx(e.interfaceName.c_str(),
                                       ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen |
                                       ImGuiTreeNodeFlags_Bullet | ImGuiTreeNodeFlags_SpanAllColumns,
                                       "%s", e.interfaceName.c_str());

                    ImGui::TableNextColumn(); // PID column left blank for detail rows

                    ImGui::TableNextColumn();
                    ImGui::TextDisabled("%s", FormatUtils::rate(e.stats.rxBytesPerSec).c_str());

                    ImGui::TableNextColumn();
                    ImGui::TextDisabled("%s", FormatUtils::rate(e.stats.txBytesPerSec).c_str());

                    ImGui::TableNextColumn();
                    ImGui::TextDisabled("v %s  ^ %s",
                                         FormatUtils::bytes(e.stats.rxBytesTotal).c_str(),
                                         FormatUtils::bytes(e.stats.txBytesTotal).c_str());
                }
                ImGui::TreePop();
            }
        }
        ImGui::EndTable();
    }

    ImGui::TextDisabled(
        "TCP + UDP tracking (Linux: Netlink socket-diag + raw packet capture; "
        "Windows: TCP Extended Statistics API + ETW). \"(unattributed)\" means "
        "the interface for that traffic couldn't be determined -- always the "
        "case for UDP on Windows, and occasionally for TCP right after an "
        "adapter change. Click a process to see its per-interface breakdown. "
        "Traffic in the brief window between a connection closing and the "
        "next poll is not counted.");
}
