#pragma once

#include "Types.h"
#include <vector>

class SystemStatsCollector {
public:
    SystemStatsCollector();
    ~SystemStatsCollector();

    CpuStats collectCpu();
    MemoryStats collectMemory();
    std::vector<DiskVolume> collectDiskVolumes();
    std::vector<DiskIoStats> collectDiskIo();

private:
    class Impl;
    Impl* m_impl;
};
