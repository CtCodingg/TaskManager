#pragma once

#include "Types.h"
#include <vector>
#include <map>

class ProcessCollector {
public:
    ProcessCollector();
    ~ProcessCollector();

    std::vector<ProcessInfo> collect();
    std::vector<ThreadInfo> collectThreads(int64_t pid);
    bool killProcess(int64_t pid);
    bool setPriority(int64_t pid, int niceValue);

private:
    struct CpuTimeSample {
        uint64_t totalTimeTicks = 0;
        int64_t sampledAtMs = 0;
    };

    std::map<int64_t, CpuTimeSample> m_prevSamples;
    uint64_t m_prevSystemTotalTicks = 0;
    int64_t m_prevSampleMs = 0;

    class Impl;
    Impl* m_impl;
};
