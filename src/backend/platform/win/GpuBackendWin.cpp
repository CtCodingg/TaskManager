// GPU statistics via the built-in PDH "GPU Engine" / "GPU Adapter Memory" counters.

#include "GpuStatsCollector.h"

#include <windows.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <map>
#include <string>
#include <regex>
#include <algorithm>

#pragma comment(lib, "pdh.lib")

namespace {
std::string extractLuid(const std::wstring& instanceName) {
    std::wsmatch m;
    static const std::wregex re(L"luid_(0x[0-9A-Fa-f]+_0x[0-9A-Fa-f]+)");
    if (std::regex_search(instanceName, m, re)) {
        std::wstring w = m[1].str();
        return std::string(w.begin(), w.end());
    }
    return {};
}
}

class GpuStatsCollector::Impl {
public:
    PDH_HQUERY query = nullptr;
    bool initialized = false;

    void init() {
        if (initialized) return;
        if (PdhOpenQuery(nullptr, 0, &query) != ERROR_SUCCESS) return;
        initialized = true;
    }

    ~Impl() {
        if (query) PdhCloseQuery(query);
    }
};

GpuStatsCollector::GpuStatsCollector() : m_impl(new Impl()) {
    m_impl->init();
}

GpuStatsCollector::~GpuStatsCollector() { delete m_impl; }

std::vector<GpuInfo> GpuStatsCollector::collect() {
    std::vector<GpuInfo> result;
    if (!m_impl->initialized) return result;

    PDH_HQUERY tempQuery = nullptr;
    if (PdhOpenQuery(nullptr, 0, &tempQuery) != ERROR_SUCCESS) return result;

    DWORD pathListSize = 0;
    PdhExpandWildCardPathW(nullptr, L"\\GPU Engine(*)\\Utilization Percentage",
                            nullptr, &pathListSize, 0);
    std::vector<wchar_t> pathList(pathListSize > 0 ? pathListSize : 1);
    if (pathListSize > 0) {
        PdhExpandWildCardPathW(nullptr, L"\\GPU Engine(*)\\Utilization Percentage",
                                pathList.data(), &pathListSize, 0);
    }

    std::map<std::wstring, PDH_HCOUNTER> counters;
    if (pathListSize > 0) {
        const wchar_t* p = pathList.data();
        while (*p) {
            PDH_HCOUNTER c = nullptr;
            if (PdhAddEnglishCounterW(tempQuery, p, 0, &c) == ERROR_SUCCESS) {
                counters[p] = c;
            }
            p += wcslen(p) + 1;
        }
    }

    PdhCollectQueryData(tempQuery);
    Sleep(200);
    PdhCollectQueryData(tempQuery);

    std::map<std::string, double> loadByLuid;
    for (auto& kv : counters) {
        std::string luid = extractLuid(kv.first);
        if (luid.empty()) continue;
        PDH_FMT_COUNTERVALUE val{};
        if (PdhGetFormattedCounterValue(kv.second, PDH_FMT_DOUBLE, nullptr, &val) == ERROR_SUCCESS) {
            loadByLuid[luid] += val.doubleValue;
        }
    }
    PdhCloseQuery(tempQuery);

    PDH_HQUERY memQuery = nullptr;
    std::map<std::string, uint64_t> memUsedByLuid;
    if (PdhOpenQuery(nullptr, 0, &memQuery) == ERROR_SUCCESS) {
        DWORD memPathSize = 0;
        PdhExpandWildCardPathW(nullptr, L"\\GPU Adapter Memory(*)\\Dedicated Usage",
                                nullptr, &memPathSize, 0);
        std::vector<wchar_t> memPaths(memPathSize > 0 ? memPathSize : 1);
        if (memPathSize > 0) {
            PdhExpandWildCardPathW(nullptr, L"\\GPU Adapter Memory(*)\\Dedicated Usage",
                                    memPaths.data(), &memPathSize, 0);
            std::map<std::wstring, PDH_HCOUNTER> memCounters;
            const wchar_t* p = memPaths.data();
            while (*p) {
                PDH_HCOUNTER c = nullptr;
                if (PdhAddEnglishCounterW(memQuery, p, 0, &c) == ERROR_SUCCESS) {
                    memCounters[p] = c;
                }
                p += wcslen(p) + 1;
            }
            PdhCollectQueryData(memQuery);
            for (auto& kv : memCounters) {
                std::string luid = extractLuid(kv.first);
                if (luid.empty()) continue;
                PDH_FMT_COUNTERVALUE val{};
                if (PdhGetFormattedCounterValue(kv.second, PDH_FMT_LARGE, nullptr, &val) == ERROR_SUCCESS) {
                    memUsedByLuid[luid] = static_cast<uint64_t>(val.largeValue);
                }
            }
        }
        PdhCloseQuery(memQuery);
    }

    int idx = 0;
    for (auto& kv : loadByLuid) {
        GpuInfo info;
        info.name = "GPU " + std::to_string(idx++);
        info.vendor = "Unknown";
        info.loadPercent = (std::min)(100.0, kv.second);
        info.memUsedBytes = memUsedByLuid.count(kv.first) ? memUsedByLuid[kv.first] : 0;
        info.memTotalBytes = 0;
        result.push_back(info);
    }

    return result;
}
