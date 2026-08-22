#include "ConnectionsTab.h"
#include "../Theme.h"
#include "imgui.h"

#include <algorithm>
#include <set>

namespace {
ImVec4 toImVec4(Theme::Rgb c, float a = 1.0f) { return ImVec4(c.r, c.g, c.b, a); }

std::string toLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}

Theme::Rgb colorForState(const std::string& state) {
    if (state == "ESTABLISHED") return {0.220f, 0.741f, 0.973f}; // accent
    if (state == "LISTEN") return {0.220f, 0.741f, 0.973f};
    if (state == "-") return {0.576f, 0.608f, 0.690f};
    if (state == "SYN_SENT" || state == "SYN_RECV") return {0.984f, 0.749f, 0.141f};
    return {0.373f, 0.416f, 0.502f};
}
}

void ConnectionsTab::updateData(std::vector<ProcessConnection> connections) {
    m_connections = std::move(connections);
}

void ConnectionsTab::draw(const std::function<std::string(int64_t)>& pidToName) {
    ImGui::PushItemWidth(300);
    ImGui::InputTextWithHint("##connFilter", "Filter by process, address, port, or state...", m_filterBuf, sizeof(m_filterBuf));
    ImGui::PopItemWidth();

    std::string needle = toLower(m_filterBuf);

    ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                             ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY |
                             ImGuiTableFlags_Sortable | ImGuiTableFlags_SizingStretchProp;
    ImVec2 size(0, ImGui::GetContentRegionAvail().y - 30);

    std::set<int64_t> distinctPids;
    int visibleCount = 0;

    if (ImGui::BeginTable("ConnectionsTable", 7, flags, size)) {
        ImGui::TableSetupColumn("Process");
        ImGui::TableSetupColumn("PID", ImGuiTableColumnFlags_WidthFixed, 70);
        ImGui::TableSetupColumn("Protocol", ImGuiTableColumnFlags_WidthFixed, 70);
        ImGui::TableSetupColumn("Local Address");
        ImGui::TableSetupColumn("Remote Address");
        ImGui::TableSetupColumn("State");
        ImGui::TableSetupColumn("IP Version", ImGuiTableColumnFlags_WidthFixed, 80);
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();

        for (const auto& c : m_connections) {
            std::string processName = pidToName(c.pid);
            std::string localEndpoint = c.localAddress + ":" + std::to_string(c.localPort);
            std::string remoteEndpoint = (c.remoteAddress.empty() || c.remotePort == 0)
                ? "-" : c.remoteAddress + ":" + std::to_string(c.remotePort);

            if (!needle.empty()) {
                std::string haystack = toLower(processName + " " + std::to_string(c.pid) + " " +
                    c.protocol + " " + localEndpoint + " " + remoteEndpoint + " " + c.state);
                if (haystack.find(needle) == std::string::npos) continue;
            }

            distinctPids.insert(c.pid);
            ++visibleCount;

            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextUnformatted(processName.c_str());
            ImGui::TableNextColumn(); ImGui::Text("%lld", static_cast<long long>(c.pid));
            ImGui::TableNextColumn(); ImGui::TextUnformatted(c.protocol.c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(localEndpoint.c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(remoteEndpoint.c_str());
            ImGui::TableNextColumn();
            ImGui::TextColored(toImVec4(colorForState(c.state)), "%s", c.state.c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(c.isIPv6 ? "IPv6" : "IPv4");
        }
        ImGui::EndTable();
    }

    ImGui::Text("%d connections across %zu processes", visibleCount, distinctPids.size());
}
