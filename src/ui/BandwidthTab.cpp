#include "BandwidthTab.h"
#include "../backend/FormatUtils.h"
#include "imgui.h"

void BandwidthTab::updateData(std::map<int64_t, ProcessBandwidthStats> stats) {
    m_stats = std::move(stats);
}

void BandwidthTab::draw(const std::function<std::string(int64_t)>& pidToName) {
    if (!m_availabilityNote.empty()) {
        ImGui::TextColored(ImVec4(0.984f, 0.749f, 0.141f, 1.0f), "%s", m_availabilityNote.c_str());
        ImGui::Spacing();
    }

    if (m_stats.empty()) {
        ImGui::TextUnformatted("Tracking active. No processes with measurable traffic yet.");
    } else {
        ImGui::Text("Tracking %zu process(es) with active traffic.", m_stats.size());
    }

    ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                             ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY |
                             ImGuiTableFlags_Sortable | ImGuiTableFlags_SizingStretchProp;
    ImVec2 size(0, ImGui::GetContentRegionAvail().y - 60);
    if (ImGui::BeginTable("BandwidthTable", 5, flags, size)) {
        ImGui::TableSetupColumn("Process");
        ImGui::TableSetupColumn("PID", ImGuiTableColumnFlags_WidthFixed, 70);
        ImGui::TableSetupColumn("Download");
        ImGui::TableSetupColumn("Upload");
        ImGui::TableSetupColumn("Total (session)");
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();

        for (const auto& kv : m_stats) {
            int64_t pid = kv.first;
            const auto& s = kv.second;
            std::string name = pidToName(pid);

            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextUnformatted(name.c_str());
            ImGui::TableNextColumn(); ImGui::Text("%lld", static_cast<long long>(pid));
            ImGui::TableNextColumn(); ImGui::TextUnformatted(FormatUtils::rate(s.rxBytesPerSec).c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(FormatUtils::rate(s.txBytesPerSec).c_str());
            ImGui::TableNextColumn();
            ImGui::Text("v %s  ^ %s", FormatUtils::bytes(s.rxBytesTotal).c_str(), FormatUtils::bytes(s.txBytesTotal).c_str());
        }
        ImGui::EndTable();
    }

    ImGui::TextDisabled(
        "TCP + UDP tracking (Linux: Netlink socket-diag + raw packet capture; "
        "Windows: TCP Extended Statistics API + ETW). Traffic in the brief "
        "window between a connection closing and the next poll is not counted.");
}
