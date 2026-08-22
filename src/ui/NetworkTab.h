#pragma once

#include "backend/Types.h"
#include "HistoryChart.h"

class NetworkTab {
public:
    NetworkTab();

    void updateData(const NetworkStats& stats);
    void draw();

private:
    NetworkStats m_stats;
    HistoryChart m_chart;
    int m_rxSeriesIdx = -1;
    int m_txSeriesIdx = -1;
};
