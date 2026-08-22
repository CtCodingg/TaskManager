#pragma once

#include "backend/Types.h"
#include "backend/SystemStatsCollector.h"
#include "backend/GpuStatsCollector.h"
#include "HistoryChart.h"
#include <memory>

// CPU (total + per-core bars), Memory, GPU, Disk volumes/IO -- each with
// a rolling HistoryChart.
class PerformanceTab {
public:
    PerformanceTab();

    void updateData(const CpuStats& cpu, const MemoryStats& mem,
                     const std::vector<DiskVolume>& disks, const std::vector<DiskIoStats>& diskIo,
                     const std::vector<GpuInfo>& gpus);
    void draw();

private:
    HistoryChart m_cpuChart;
    HistoryChart m_memChart;
    HistoryChart m_gpuChart;
    int m_cpuSeriesIdx = -1;
    int m_memSeriesIdx = -1;
    int m_gpuSeriesIdx = -1;

    CpuStats m_cpu;
    MemoryStats m_mem;
    std::vector<DiskVolume> m_disks;
    std::vector<DiskIoStats> m_diskIo;
    std::vector<GpuInfo> m_gpus;
};
