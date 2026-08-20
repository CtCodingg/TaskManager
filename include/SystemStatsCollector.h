#pragma once

#include "Types.h"
#include <QVector>

class SystemStatsCollector {
public:
    SystemStatsCollector();
    ~SystemStatsCollector();

    CpuStats collectCpu();
    MemoryStats collectMemory();
    QVector<DiskVolume> collectDiskVolumes();
    QVector<DiskIoStats> collectDiskIo();

private:
    class Impl;
    Impl* m_impl;
};
