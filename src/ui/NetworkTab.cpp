#include "NetworkTab.h"
#include "../Theme.h"
#include "../backend/FormatUtils.h"
#include "imgui.h"

namespace {
ImVec4 toImVec4(Theme::Rgb c, float a = 1.0f) { return ImVec4(c.r, c.g, c.b, a); }
}

NetworkTab::NetworkTab() : m_chart("Total Bandwidth", 120) {
    m_chart.setYAutoScale(true);
    m_rxSeriesIdx = m_chart.addSeries("Download", toImVec4(Theme::accentNetRx()));
    m_txSeriesIdx = m_chart.addSeries("Upload", toImVec4(Theme::accentNetTx()));
}

void NetworkTab::updateData(const NetworkStats& stats) {
    m_stats = stats;
    m_chart.pushValue(m_rxSeriesIdx, static_cast<double>(stats.totalRxBytesPerSec));
    m_chart.pushValue(m_txSeriesIdx, static_cast<double>(stats.totalTxBytesPerSec));
}

void NetworkTab::draw() {
    ImGui::Text("Total: v %s   ^ %s",
                FormatUtils::rate(m_stats.totalRxBytesPerSec).c_str(),
                FormatUtils::rate(m_stats.totalTxBytesPerSec).c_str());

    m_chart.draw(180.0f);

    ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                             ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY |
                             ImGuiTableFlags_SizingStretchProp;
    ImVec2 size(0, ImGui::GetContentRegionAvail().y - 40);
    if (ImGui::BeginTable("NetworkTable", 11, flags, size)) {
        ImGui::TableSetupColumn("Interface");
        ImGui::TableSetupColumn("Status");
        ImGui::TableSetupColumn("IPv4");
        ImGui::TableSetupColumn("Link Speed");
        ImGui::TableSetupColumn("Down");
        ImGui::TableSetupColumn("Up");
        ImGui::TableSetupColumn("Utilization");
        ImGui::TableSetupColumn("RX Drop %");
        ImGui::TableSetupColumn("TX Drop %");
        ImGui::TableSetupColumn("RX Err %");
        ImGui::TableSetupColumn("TX Err %");
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();

        for (const auto& ifs : m_stats.interfaces) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextUnformatted(ifs.name.c_str());

            ImGui::TableNextColumn();
            ImGui::TextColored(toImVec4(ifs.isUp ? Theme::Rgb{0.204f, 0.827f, 0.600f} : Theme::Rgb{0.576f, 0.608f, 0.690f}),
                                "%s", ifs.isUp ? "Up" : "Down");

            ImGui::TableNextColumn(); ImGui::TextUnformatted(ifs.ipv4Address.c_str());
            ImGui::TableNextColumn();
            if (ifs.linkSpeedMbps) ImGui::Text("%llu Mbps", static_cast<unsigned long long>(ifs.linkSpeedMbps));
            else ImGui::TextUnformatted("n/a");

            ImGui::TableNextColumn(); ImGui::TextUnformatted(FormatUtils::rate(ifs.rxBytesPerSec).c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(FormatUtils::rate(ifs.txBytesPerSec).c_str());

            ImGui::TableNextColumn();
            if (ifs.linkSpeedMbps) ImGui::TextUnformatted(FormatUtils::percent(ifs.utilizationPercent).c_str());
            else ImGui::TextUnformatted("n/a");

            auto rateCell = [](double percent) {
                ImGui::TableNextColumn();
                Theme::Rgb color = percent <= 0.0 ? Theme::Rgb{0.576f, 0.608f, 0.690f}
                                  : percent >= 1.0 ? Theme::Rgb{0.973f, 0.443f, 0.443f}
                                                    : Theme::Rgb{0.984f, 0.749f, 0.141f};
                ImGui::TextColored(toImVec4(color), "%s", FormatUtils::percent(percent, 3).c_str());
            };
            rateCell(ifs.rxDropPercent);
            rateCell(ifs.txDropPercent);
            rateCell(ifs.rxErrorPercent);
            rateCell(ifs.txErrorPercent);
        }
        ImGui::EndTable();
    }

    ImGui::TextDisabled("Drop %% / Error %% computed per poll as (dropped or errored) / (successful + dropped) * 100.");
}
