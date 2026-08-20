#pragma once

#include <QString>
#include <QVector>
#include <cstdint>

// ----------------------------------------------------------------------------
// Process / thread info
// ----------------------------------------------------------------------------
struct ThreadInfo {
    qint64  tid = 0;
    QString name;
    QString state;      // Running / Sleeping / Waiting / etc.
    double  cpuPercent = 0.0;
    int     priority = 0;
};

struct ProcessInfo {
    qint64  pid = 0;
    qint64  ppid = 0;
    QString name;
    QString user;
    QString state;          // R, S, D, Z, T ...
    double  cpuPercent = 0.0;
    quint64 memRssBytes = 0;
    quint64 memVirtBytes = 0;
    int     threadCount = 0;
    int     niceValue = 0;
    QString commandLine;
    QVector<ThreadInfo> threads;
};

// ----------------------------------------------------------------------------
// CPU
// ----------------------------------------------------------------------------
struct CpuCoreLoad {
    int    coreIndex = 0;
    double percent = 0.0;
    double frequencyMHz = 0.0;
};

struct CpuStats {
    double totalPercent = 0.0;
    QVector<CpuCoreLoad> perCore;
    double temperatureC = -1.0;      // -1 = unavailable
};

// ----------------------------------------------------------------------------
// Memory
// ----------------------------------------------------------------------------
struct MemoryStats {
    quint64 totalBytes = 0;
    quint64 usedBytes = 0;
    quint64 availableBytes = 0;
    quint64 cachedBytes = 0;
    quint64 swapTotalBytes = 0;
    quint64 swapUsedBytes = 0;

    double usedPercent() const {
        return totalBytes ? (double(usedBytes) / double(totalBytes)) * 100.0 : 0.0;
    }
};

// ----------------------------------------------------------------------------
// Disk
// ----------------------------------------------------------------------------
struct DiskVolume {
    QString device;
    QString mountPoint;
    QString fsType;
    quint64 totalBytes = 0;
    quint64 usedBytes = 0;
    quint64 freeBytes = 0;

    double usedPercent() const {
        return totalBytes ? (double(usedBytes) / double(totalBytes)) * 100.0 : 0.0;
    }
};

struct DiskIoStats {
    QString device;
    quint64 readBytesPerSec = 0;
    quint64 writeBytesPerSec = 0;
    double  utilizationPercent = 0.0;  // time spent doing I/O, 0-100
};

// ----------------------------------------------------------------------------
// Network -- the "deep information" requirement
// ----------------------------------------------------------------------------
struct NetworkInterfaceStats {
    QString name;
    QString ipv4Address;
    QString macAddress;
    bool    isUp = false;
    quint64 linkSpeedMbps = 0;        // negotiated link speed, 0 if unknown

    // Bandwidth (computed as delta between polls)
    quint64 rxBytesPerSec = 0;
    quint64 txBytesPerSec = 0;
    quint64 rxPacketsPerSec = 0;
    quint64 txPacketsPerSec = 0;

    // Cumulative counters (since boot / interface reset)
    quint64 rxBytesTotal = 0;
    quint64 txBytesTotal = 0;
    quint64 rxPacketsTotal = 0;
    quint64 txPacketsTotal = 0;

    // Errors / drops
    quint64 rxErrorsTotal = 0;
    quint64 txErrorsTotal = 0;
    quint64 rxDroppedTotal = 0;
    quint64 txDroppedTotal = 0;

    // Instantaneous drop-rate stats (since last poll) as percentages
    double rxDropPercent = 0.0;   // rxDropped(delta) / (rxPackets(delta)+rxDropped(delta)) * 100
    double txDropPercent = 0.0;
    double rxErrorPercent = 0.0;
    double txErrorPercent = 0.0;

    double utilizationPercent = 0.0; // (rx+tx bytes/s * 8) / linkSpeed, if link speed known
};

struct NetworkStats {
    QVector<NetworkInterfaceStats> interfaces;
    quint64 totalRxBytesPerSec = 0;
    quint64 totalTxBytesPerSec = 0;
};

// ----------------------------------------------------------------------------
// GPU
// ----------------------------------------------------------------------------
struct GpuInfo {
    QString name;
    QString vendor;               // NVIDIA / Intel / AMD / Tegra
    double  loadPercent = -1.0;   // -1 = unavailable
    quint64 memTotalBytes = 0;
    quint64 memUsedBytes = 0;
    double  temperatureC = -1.0;
    double  powerWatts = -1.0;
    double  clockMHz = -1.0;

    double memUsedPercent() const {
        return memTotalBytes ? (double(memUsedBytes) / double(memTotalBytes)) * 100.0 : 0.0;
    }
};

// ----------------------------------------------------------------------------
// Aggregate snapshot delivered to the UI once per poll cycle
// ----------------------------------------------------------------------------
struct SystemSnapshot {
    CpuStats cpu;
    MemoryStats memory;
    QVector<DiskVolume> diskVolumes;
    QVector<DiskIoStats> diskIo;
    NetworkStats network;
    QVector<GpuInfo> gpus;
    qint64 timestampMs = 0;
};
