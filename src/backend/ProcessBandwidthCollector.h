#pragma once

#include "Types.h"
#include <map>
#include <vector>
#include <string>

class ProcessBandwidthCollector {
public:
    ProcessBandwidthCollector();
    ~ProcessBandwidthCollector();

    bool start();
    void stop();
    bool isRunning() const;
    std::string lastError() const;

    // Per-(PID, interface) snapshot: rxBytesPerSec/txBytesPerSec are rates
    // since the previous call; rxBytesTotal/txBytesTotal are cumulative
    // since start(). Only entries with currently-tracked TCP/UDP traffic
    // appear.
    std::vector<ProcessInterfaceBandwidth> collect();

private:
    class Impl;
    Impl* m_impl;
};
