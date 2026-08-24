#include "BandwidthTab.h"
#include "../backend/FormatUtils.h"
#include "imgui.h"

#include <algorithm>
#include <set>
#include <map>

void BandwidthTab::updateData(std::vector<ProcessInterfaceBandwidth> entries) {
    m_entries = std::move(entries);
}

void BandwidthTab::draw(const std::function<std::string(int64_t)>& pidToName) {
    if (!m_availabilityNote.empty()) {
        ImGui::TextColored(ImVec4(0.984f, 0.749f, 0.141f, 1.0f), "%s", m_availabilityNote.c_str());
        ImGui::Spacing();
    }

    std::set<int64_t> distinctPids;
    for (const auto& e : m_entries) distinctPids.insert(e.pid);

    if (m_entries.empty()) {
        ImGui::TextUnformatted("Tracking active. No processes with measurable traffic yet.");
    } else {
        ImGui::Text("Tracking %zu process(es) across %zu (process, interface) pair(s).",
                     distinctPids.size(), m_entries.size());
    }

    // Sort by process name then interface, so each process's rows sit
    // together in the table.
    std::vector<ProcessInterfaceBandwidth> sorted = m_entries;
    std::sort(sorted.begin(), sorted.end(), [&](const ProcessInterfaceBandwidth& a, const ProcessInterfaceBandwidth& b) {
        std::string nameA = pidToName(a.pid);
        std::string nameB = pidToName(b.pid);
        if (nameA != nameB) return nameA < nameB;
        if (a.pid != b.pid) return a.pid < b.pid;
        return a.interfaceName < b.interfaceName;
    });

    ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                             ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY |
                             ImGuiTableFlags_Sortable | ImGuiTableFlags_SizingStretchProp;
    ImVec2 size(0, ImGui::GetContentRegionAvail().y - 60);
    if (ImGui::BeginTable("BandwidthTable", 6, flags, size)) {
        ImGui::TableSetupColumn("Process");
        ImGui::TableSetupColumn("PID", ImGuiTableColumnFlags_WidthFixed, 70);
        ImGui::TableSetupColumn("Interface", ImGuiTableColumnFlags_WidthFixed, 110);
        ImGui::TableSetupColumn("Download");
        ImGui::TableSetupColumn("Upload");
        ImGui::TableSetupColumn("Total (session)");
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();

        for (const auto& e : sorted) {
            std::string name = pidToName(e.pid);

            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextUnformatted(name.c_str());
            ImGui::TableNextColumn(); ImGui::Text("%lld", static_cast<long long>(e.pid));
            ImGui::TableNextColumn(); ImGui::TextUnformatted(e.interfaceName.c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(FormatUtils::rate(e.stats.rxBytesPerSec).c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(FormatUtils::rate(e.stats.txBytesPerSec).c_str());
            ImGui::TableNextColumn();
            ImGui::Text("v %s  ^ %s", FormatUtils::bytes(e.stats.rxBytesTotal).c_str(), FormatUtils::bytes(e.stats.txBytesTotal).c_str());
        }
        ImGui::EndTable();
    }

    ImGui::TextDisabled(
        "TCP + UDP tracking (Linux: Netlink socket-diag + raw packet capture; "
        "Windows: TCP Extended Statistics API + ETW). \"(unattributed)\" means "
        "the interface for that traffic couldn't be determined -- always the "
        "case for UDP on Windows, and occasionally for TCP right after an "
        "adapter change. Traffic in the brief window between a connection "
        "closing and the next poll is not counted.");
}
