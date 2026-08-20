// Windows implementation of SystemStatsCollector using PDH (Performance
// Data Helper) for CPU, GlobalMemoryStatusEx for RAM, and
// GetDiskFreeSpaceExW + GetLogicalDrives for disks. All are system libs
// (pdh.lib), no third-party dependency.

#include "SystemStatsCollector.h"

#include <windows.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <QVector>
#include <QString>
#include <string>

#pragma comment(lib, "pdh.lib")

namespace {

QString driveTypeToFsGuess(UINT type) {
    switch (type) {
        case DRIVE_FIXED: return "NTFS";
        case DRIVE_REMOVABLE: return "FAT32/exFAT";
        case DRIVE_REMOTE: return "Network";
        case DRIVE_CDROM: return "CDFS";
        default: return "Unknown";
    }
}

} // namespace

class SystemStatsCollector::Impl {
public:
    PDH_HQUERY query = nullptr;
    PDH_HCOUNTER totalCpuCounter = nullptr;
    QVector<PDH_HCOUNTER> perCoreCounters;
    bool initialized = false;

    void init() {
        if (initialized) return;
        if (PdhOpenQuery(nullptr, 0, &query) != ERROR_SUCCESS) return;

        PdhAddEnglishCounterW(query, L"\\Processor(_Total)\\% Processor Time", 0, &totalCpuCounter);

        // Enumerate per-core counters \Processor(0)\% Processor Time, etc.
        SYSTEM_INFO sysInfo;
        GetSystemInfo(&sysInfo);
        for (DWORD i = 0; i < sysInfo.dwNumberOfProcessors; ++i) {
            PDH_HCOUNTER c = nullptr;
            std::wstring path = L"\\Processor(" + std::to_wstring(i) + L")\\% Processor Time";
            if (PdhAddEnglishCounterW(query, path.c_str(), 0, &c) == ERROR_SUCCESS) {
                perCoreCounters.push_back(c);
            }
        }

        // First collect call establishes the baseline; values are only
        // meaningful from the second call onward.
        PdhCollectQueryData(query);
        initialized = true;
    }

    ~Impl() {
        if (query) PdhCloseQuery(query);
    }
};

SystemStatsCollector::SystemStatsCollector() : m_impl(new Impl()) {
    m_impl->init();
}

SystemStatsCollector::~SystemStatsCollector() { delete m_impl; }

CpuStats SystemStatsCollector::collectCpu() {
    CpuStats stats;
    if (!m_impl->initialized) return stats;

    PdhCollectQueryData(m_impl->query);

    PDH_FMT_COUNTERVALUE val{};
    if (PdhGetFormattedCounterValue(m_impl->totalCpuCounter, PDH_FMT_DOUBLE, nullptr, &val) == ERROR_SUCCESS) {
        stats.totalPercent = val.doubleValue;
    }

    for (int i = 0; i < m_impl->perCoreCounters.size(); ++i) {
        PDH_FMT_COUNTERVALUE cv{};
        CpuCoreLoad core;
        core.coreIndex = i;
        if (PdhGetFormattedCounterValue(m_impl->perCoreCounters[i], PDH_FMT_DOUBLE, nullptr, &cv) == ERROR_SUCCESS) {
            core.percent = cv.doubleValue;
        }
        stats.perCore.push_back(core);
    }

    stats.temperatureC = -1.0; // Requires WMI MSAcpi_ThermalZoneTemperature; often restricted/unavailable
    return stats;
}

MemoryStats SystemStatsCollector::collectMemory() {
    MemoryStats mem;
    MEMORYSTATUSEX ms{};
    ms.dwLength = sizeof(ms);
    if (GlobalMemoryStatusEx(&ms)) {
        mem.totalBytes = ms.ullTotalPhys;
        mem.availableBytes = ms.ullAvailPhys;
        mem.usedBytes = mem.totalBytes - mem.availableBytes;
        mem.swapTotalBytes = ms.ullTotalPageFile > ms.ullTotalPhys
                                  ? ms.ullTotalPageFile - ms.ullTotalPhys : 0;
        quint64 availPageFile = ms.ullAvailPageFile;
        mem.swapUsedBytes = (mem.swapTotalBytes > availPageFile) ? mem.swapTotalBytes - availPageFile : 0;
    }
    return mem;
}

QVector<DiskVolume> SystemStatsCollector::collectDiskVolumes() {
    QVector<DiskVolume> volumes;
    DWORD drives = GetLogicalDrives();

    for (int i = 0; i < 26; ++i) {
        if (!(drives & (1 << i))) continue;
        QString root = QString("%1:\\").arg(QChar('A' + i));
        UINT type = GetDriveTypeW(reinterpret_cast<LPCWSTR>(root.utf16()));
        if (type != DRIVE_FIXED && type != DRIVE_REMOVABLE) continue;

        ULARGE_INTEGER freeBytesAvail, totalBytes, totalFreeBytes;
        if (!GetDiskFreeSpaceExW(reinterpret_cast<LPCWSTR>(root.utf16()),
                                  &freeBytesAvail, &totalBytes, &totalFreeBytes)) continue;
        if (totalBytes.QuadPart == 0) continue;

        DiskVolume vol;
        vol.device = root;
        vol.mountPoint = root;
        vol.fsType = driveTypeToFsGuess(type);
        vol.totalBytes = totalBytes.QuadPart;
        vol.freeBytes = totalFreeBytes.QuadPart;
        vol.usedBytes = vol.totalBytes - vol.freeBytes;
        volumes.push_back(vol);
    }
    return volumes;
}

QVector<DiskIoStats> SystemStatsCollector::collectDiskIo() {
    // Full per-physical-disk throughput requires PDH counters on
    // "PhysicalDisk(*)\Disk Read Bytes/sec" etc. Left as a lightweight
    // extension point; returns empty here to keep the base implementation
    // dependency-free and simple. See README for how to extend.
    return {};
}
