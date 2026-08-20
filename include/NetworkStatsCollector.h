#pragma once

#include "Types.h"
#include <QMap>
#include <QString>

// Collects per-interface network statistics: throughput (rx/tx bandwidth),
// packet rates, error rates and drop rates (as percentages), link speed and
// utilization. Rates are computed as deltas between successive collect()
// calls, so call it on a regular interval (see MainWindow's poll timer).
class NetworkStatsCollector {
public:
    NetworkStatsCollector();
    ~NetworkStatsCollector();

    NetworkStats collect();

private:
    struct PrevCounters {
        quint64 rxBytes = 0, txBytes = 0;
        quint64 rxPackets = 0, txPackets = 0;
        quint64 rxDropped = 0, txDropped = 0;
        quint64 rxErrors = 0, txErrors = 0;
        qint64  sampledAtMs = 0;
        bool    valid = false;
    };

    QMap<QString, PrevCounters> m_prev;

    class Impl;
    Impl* m_impl;
};
