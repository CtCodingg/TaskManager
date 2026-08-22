#pragma once

#include "Types.h"
#include <map>
#include <string>

class ProcessBandwidthCollector {
public:
    ProcessBandwidthCollector();
    ~ProcessBandwidthCollector();

    bool start();
    void stop();
    bool isRunning() const;
    std::string lastError() const;

    std::map<int64_t, ProcessBandwidthStats> collect();

private:
    class Impl;
    Impl* m_impl;
};
