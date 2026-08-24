#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Shared data structs: std::string/std::vector/std::map, no external
// dependencies.

struct ThreadInfo {
    int64_t tid = 0;
    std::string name;
    std::string state;
    double cpuPercent = 0.0;
    int priority = 0;
};

struct ProcessInfo {
    int64_t pid = 0;
    int64_t ppid = 0;
    std::string name;
    std::string user;
    std::string state;
    double cpuPercent = 0.0;
    uint64_t memRssBytes = 0;
    uint64_t memVirtBytes = 0;
    int threadCount = 0;
    int niceValue = 0;
    std::string commandLine;
    std::vector<ThreadInfo> threads;
};

struct CpuCoreLoad {
    int coreIndex = 0;
    double percent = 0.0;
    double frequencyMHz = 0.0;
};

struct CpuStats {
    double totalPercent = 0.0;
    std::vector<CpuCoreLoad> perCore;
    double temperatureC = -1.0;
};

struct MemoryStats {
    uint64_t totalBytes = 0;
    uint64_t usedBytes = 0;
    uint64_t availableBytes = 0;
    uint64_t cachedBytes = 0;
    uint64_t swapTotalBytes = 0;
    uint64_t swapUsedBytes = 0;

    double usedPercent() const {
        return totalBytes ? (double(usedBytes) / double(totalBytes)) * 100.0 : 0.0;
    }
};

struct DiskVolume {
    std::string device;
    std::string mountPoint;
    std::string fsType;
    uint64_t totalBytes = 0;
    uint64_t usedBytes = 0;
    uint64_t freeBytes = 0;

    double usedPercent() const {
        return totalBytes ? (double(usedBytes) / double(totalBytes)) * 100.0 : 0.0;
    }
};

struct DiskIoStats {
    std::string device;
    uint64_t readBytesPerSec = 0;
    uint64_t writeBytesPerSec = 0;
    double utilizationPercent = 0.0;
};

struct NetworkInterfaceStats {
    std::string name;
    std::string ipv4Address;
    std::string macAddress;
    bool isUp = false;
    uint64_t linkSpeedMbps = 0;

    uint64_t rxBytesPerSec = 0;
    uint64_t txBytesPerSec = 0;
    uint64_t rxPacketsPerSec = 0;
    uint64_t txPacketsPerSec = 0;

    uint64_t rxBytesTotal = 0;
    uint64_t txBytesTotal = 0;
    uint64_t rxPacketsTotal = 0;
    uint64_t txPacketsTotal = 0;

    uint64_t rxErrorsTotal = 0;
    uint64_t txErrorsTotal = 0;
    uint64_t rxDroppedTotal = 0;
    uint64_t txDroppedTotal = 0;

    double rxDropPercent = 0.0;
    double txDropPercent = 0.0;
    double rxErrorPercent = 0.0;
    double txErrorPercent = 0.0;

    double utilizationPercent = 0.0;
};

struct NetworkStats {
    std::vector<NetworkInterfaceStats> interfaces;
    uint64_t totalRxBytesPerSec = 0;
    uint64_t totalTxBytesPerSec = 0;
};

struct ProcessConnection {
    int64_t pid = 0;
    std::string protocol;
    std::string localAddress;
    uint16_t localPort = 0;
    std::string remoteAddress;
    uint16_t remotePort = 0;
    std::string state;
    bool isIPv6 = false;
};

struct ProcessBandwidthStats {
    uint64_t rxBytesPerSec = 0;
    uint64_t txBytesPerSec = 0;
    uint64_t rxBytesTotal = 0;
    uint64_t txBytesTotal = 0;
};

// Per-process bandwidth broken down by network interface. interfaceName
// is the OS interface name ("eth0", "wlan0", "Ethernet"...) when it could
// be determined, or "(unattributed)" when it couldn't -- e.g. Windows UDP
// traffic via ETW, where the provider doesn't reliably expose which
// interface a packet used (see ProcessBandwidthBackendWin.cpp).
struct ProcessInterfaceBandwidth {
    int64_t pid = 0;
    std::string interfaceName;
    ProcessBandwidthStats stats;
};

struct GpuInfo {
    std::string name;
    std::string vendor;
    double loadPercent = -1.0;
    uint64_t memTotalBytes = 0;
    uint64_t memUsedBytes = 0;
    double temperatureC = -1.0;
    double powerWatts = -1.0;
    double clockMHz = -1.0;

    double memUsedPercent() const {
        return memTotalBytes ? (double(memUsedBytes) / double(memTotalBytes)) * 100.0 : 0.0;
    }
};

struct SystemSnapshot {
    CpuStats cpu;
    MemoryStats memory;
    std::vector<DiskVolume> diskVolumes;
    std::vector<DiskIoStats> diskIo;
    NetworkStats network;
    std::vector<GpuInfo> gpus;
    int64_t timestampMs = 0;
};
