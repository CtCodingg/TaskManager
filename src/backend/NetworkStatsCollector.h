#pragma once

#include "Types.h"
#include <map>
#include <string>

class NetworkStatsCollector {
public:
    NetworkStatsCollector();
    ~NetworkStatsCollector();

    NetworkStats collect();

private:
    struct PrevCounters {
        uint64_t rxBytes = 0, txBytes = 0;
        uint64_t rxPackets = 0, txPackets = 0;
        uint64_t rxDropped = 0, txDropped = 0;
        uint64_t rxErrors = 0, txErrors = 0;
        int64_t sampledAtMs = 0;
        bool valid = false;
    };

    std::map<std::string, PrevCounters> m_prev;

    class Impl;
    Impl* m_impl;
};
