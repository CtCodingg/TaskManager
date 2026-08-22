#pragma once

#include "Types.h"
#include <vector>

class GpuStatsCollector {
public:
    GpuStatsCollector();
    ~GpuStatsCollector();

    std::vector<GpuInfo> collect();

private:
    class Impl;
    Impl* m_impl;
};
