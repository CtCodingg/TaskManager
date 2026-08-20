#pragma once

#include "Types.h"
#include <QVector>

// Collects GPU load/memory/temperature information across:
//  - NVIDIA discrete GPUs (via NVML, dlopen'd at runtime -- no link dependency)
//  - Jetson/Tegra integrated GPU (via sysfs: /sys/devices/gpu.0 / /sys/class/devfreq)
//  - Windows GPUs (via PDH "GPU Engine" / "GPU Adapter Memory" performance counters)
// If no supported backend is found, returns an empty list gracefully.
class GpuStatsCollector {
public:
    GpuStatsCollector();
    ~GpuStatsCollector();

    QVector<GpuInfo> collect();

private:
    class Impl;
    Impl* m_impl;
};
