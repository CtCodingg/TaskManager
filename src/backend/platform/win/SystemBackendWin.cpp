// CPU, memory, and disk statistics via PDH and the Win32 API.

#include "SystemStatsCollector.h"

#include <windows.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <string>
#include <vector>

#pragma comment(lib, "pdh.lib")

namespace {
std::string driveTypeToFsGuess(UINT type) {
    switch (type) {
        case DRIVE_FIXED: return "NTFS";
        case DRIVE_REMOVABLE: return "FAT32/exFAT";
        case DRIVE_REMOTE: return "Network";
        case DRIVE_CDROM: return "CDFS";
        default: return "Unknown";
    }
}
}

class SystemStatsCollector::Impl {
public:
    PDH_HQUERY query = nullptr;
    PDH_HCOUNTER totalCpuCounter = nullptr;
    std::vector<PDH_HCOUNTER> perCoreCounters;
    bool initialized = false;

    void init() {
        if (initialized) return;
        if (PdhOpenQuery(nullptr, 0, &query) != ERROR_SUCCESS) return;

        PdhAddEnglishCounterW(query, L"\\Processor(_Total)\\% Processor Time", 0, &totalCpuCounter);

        SYSTEM_INFO sysInfo;
        GetSystemInfo(&sysInfo);
        for (DWORD i = 0; i < sysInfo.dwNumberOfProcessors; ++i) {
            PDH_HCOUNTER c = nullptr;
            std::wstring path = L"\\Processor(" + std::to_wstring(i) + L")\\% Processor Time";
            if (PdhAddEnglishCounterW(query, path.c_str(), 0, &c) == ERROR_SUCCESS) {
                perCoreCounters.push_back(c);
            }
        }

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

    for (size_t i = 0; i < m_impl->perCoreCounters.size(); ++i) {
        PDH_FMT_COUNTERVALUE cv{};
        CpuCoreLoad core;
        core.coreIndex = static_cast<int>(i);
        if (PdhGetFormattedCounterValue(m_impl->perCoreCounters[i], PDH_FMT_DOUBLE, nullptr, &cv) == ERROR_SUCCESS) {
            core.percent = cv.doubleValue;
        }
        stats.perCore.push_back(core);
    }

    stats.temperatureC = -1.0;
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
        uint64_t availPageFile = ms.ullAvailPageFile;
        mem.swapUsedBytes = (mem.swapTotalBytes > availPageFile) ? mem.swapTotalBytes - availPageFile : 0;
    }
    return mem;
}

std::vector<DiskVolume> SystemStatsCollector::collectDiskVolumes() {
    std::vector<DiskVolume> volumes;
    DWORD drives = GetLogicalDrives();

    for (int i = 0; i < 26; ++i) {
        if (!(drives & (1 << i))) continue;
        std::wstring root = std::wstring(1, wchar_t('A' + i)) + L":\\";
        UINT type = GetDriveTypeW(root.c_str());
        if (type != DRIVE_FIXED && type != DRIVE_REMOVABLE) continue;

        ULARGE_INTEGER freeBytesAvail, totalBytes, totalFreeBytes;
        if (!GetDiskFreeSpaceExW(root.c_str(), &freeBytesAvail, &totalBytes, &totalFreeBytes)) continue;
        if (totalBytes.QuadPart == 0) continue;

        DiskVolume vol;
        std::string rootA(root.begin(), root.end());
        vol.device = rootA;
        vol.mountPoint = rootA;
        vol.fsType = driveTypeToFsGuess(type);
        vol.totalBytes = totalBytes.QuadPart;
        vol.freeBytes = totalFreeBytes.QuadPart;
        vol.usedBytes = vol.totalBytes - vol.freeBytes;
        volumes.push_back(vol);
    }
    return volumes;
}

std::vector<DiskIoStats> SystemStatsCollector::collectDiskIo() {
    // Extension point: full per-disk PDH counters
    // (PhysicalDisk(*)\Disk Read Bytes/sec etc.) not wired up here.
    return {};
}
