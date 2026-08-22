#pragma once

#include <vector>
#include <string>
#include "implot.h"

// Rolling-buffer line chart built on ImPlot. Same shape: addSeries()
// once, pushValue() each poll, draw() each frame.
class HistoryChart {
public:
    explicit HistoryChart(std::string title, int maxPoints = 120)
        : m_title(std::move(title)), m_maxPoints(maxPoints) {}

    int addSeries(const std::string& name, ImVec4 color) {
        Series s;
        s.name = name;
        s.color = color;
        s.values.assign(m_maxPoints, 0.0);
        m_series.push_back(std::move(s));
        return static_cast<int>(m_series.size()) - 1;
    }

    void pushValue(int seriesIndex, double value) {
        if (seriesIndex < 0 || seriesIndex >= static_cast<int>(m_series.size())) return;
        auto& v = m_series[seriesIndex].values;
        v.push_back(value);
        if (static_cast<int>(v.size()) > m_maxPoints) v.erase(v.begin());
    }

    void setYRange(double minVal, double maxVal) {
        m_autoScaleY = false;
        m_yMin = minVal;
        m_yMax = maxVal;
    }

    void setYAutoScale(bool autoScale) { m_autoScaleY = autoScale; }

    void draw(float height = 150.0f) {
        double yMin = m_yMin, yMax = m_yMax;
        if (m_autoScaleY) {
            yMax = 1.0;
            for (const auto& s : m_series)
                for (double v : s.values) yMax = (v > yMax) ? v : yMax;
            yMax *= 1.15;
            yMin = 0.0;
        }

        if (ImPlot::BeginPlot(m_title.c_str(), ImVec2(-1, height),
                               ImPlotFlags_NoTitle | ImPlotFlags_NoMouseText | ImPlotFlags_NoMenus)) {
            ImPlot::SetupAxes(nullptr, nullptr,
                               ImPlotAxisFlags_NoTickLabels | ImPlotAxisFlags_Lock,
                               ImPlotAxisFlags_None);
            ImPlot::SetupAxisLimits(ImAxis_X1, 0, m_maxPoints, ImGuiCond_Always);
            ImPlot::SetupAxisLimits(ImAxis_Y1, yMin, yMax, ImGuiCond_Always);

            for (const auto& s : m_series) {
                ImPlot::PushStyleColor(ImPlotCol_Line, s.color);
                ImPlot::PlotLine(s.name.c_str(), s.values.data(), static_cast<int>(s.values.size()));
                ImPlot::PopStyleColor();
            }
            ImPlot::EndPlot();
        }
        ImGui::TextUnformatted(m_title.c_str());
    }

private:
    struct Series {
        std::string name;
        ImVec4 color;
        std::vector<double> values;
    };

    std::string m_title;
    int m_maxPoints;
    std::vector<Series> m_series;
    bool m_autoScaleY = false;
    double m_yMin = 0.0;
    double m_yMax = 100.0;
};
