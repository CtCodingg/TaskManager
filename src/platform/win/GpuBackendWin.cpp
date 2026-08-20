// Windows GPU backend using the built-in PDH "GPU Engine" and
// "GPU Adapter Memory" performance counter sets (available on Windows
// 10/11 out of the box, vendor-agnostic -- works for NVIDIA/AMD/Intel
// alike without installing NVML or any vendor SDK).

#include "GpuStatsCollector.h"

#include <windows.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <QMap>
#include <QRegularExpression>
#include <QtGlobal>
#include <cwchar>

#pragma comment(lib, "pdh.lib")

namespace {

// GPU Engine instance names look like:
//   pid_1234_luid_0x... _phys_0_eng_0_engtype_3D
// We sum "Utilization Percentage" across engines per physical GPU (luid),
// and read dedicated/shared memory from "GPU Adapter Memory".
QString extractLuid(const QString& instanceName) {
    QRegularExpression re("luid_(0x[0-9A-Fa-f]+_0x[0-9A-Fa-f]+)");
    auto m = re.match(instanceName);
    return m.hasMatch() ? m.captured(1) : QString();
}

} // namespace

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

QVector<GpuInfo> GpuStatsCollector::collect() {
    QVector<GpuInfo> result;
    if (!m_impl->initialized) return result;

    // "GPU Engine" utilization: expand the wildcard counter path fresh each
    // call since engine instances can appear/disappear as processes launch.
    PDH_HQUERY tempQuery = nullptr;
    if (PdhOpenQuery(nullptr, 0, &tempQuery) != ERROR_SUCCESS) return result;

    DWORD pathListSize = 0;
    PdhExpandWildCardPathW(nullptr, L"\\GPU Engine(*)\\Utilization Percentage",
                            nullptr, &pathListSize, 0);
    QVector<wchar_t> pathList(pathListSize > 0 ? pathListSize : 1);
    if (pathListSize > 0) {
        PdhExpandWildCardPathW(nullptr, L"\\GPU Engine(*)\\Utilization Percentage",
                                pathList.data(), &pathListSize, 0);
    }

    QMap<QString, PDH_HCOUNTER> counters;
    if (pathListSize > 0) {
        const wchar_t* p = pathList.data();
        while (*p) {
            PDH_HCOUNTER c = nullptr;
            if (PdhAddEnglishCounterW(tempQuery, p, 0, &c) == ERROR_SUCCESS) {
                counters[QString::fromWCharArray(p)] = c;
            }
            p += wcslen(p) + 1;
        }
    }

    PdhCollectQueryData(tempQuery);
    Sleep(200); // GPU Engine counters need two samples with a delay for a valid rate
    PdhCollectQueryData(tempQuery);

    QMap<QString, double> loadByLuid;
    for (auto it = counters.constBegin(); it != counters.constEnd(); ++it) {
        QString luid = extractLuid(it.key());
        if (luid.isEmpty()) continue;
        PDH_FMT_COUNTERVALUE val{};
        if (PdhGetFormattedCounterValue(it.value(), PDH_FMT_DOUBLE, nullptr, &val) == ERROR_SUCCESS) {
            loadByLuid[luid] += val.doubleValue;
        }
    }
    PdhCloseQuery(tempQuery);

    // "GPU Adapter Memory" -- dedicated usage per adapter
    PDH_HQUERY memQuery = nullptr;
    QMap<QString, quint64> memUsedByLuid;
    if (PdhOpenQuery(nullptr, 0, &memQuery) == ERROR_SUCCESS) {
        DWORD memPathSize = 0;
        PdhExpandWildCardPathW(nullptr, L"\\GPU Adapter Memory(*)\\Dedicated Usage",
                                nullptr, &memPathSize, 0);
        QVector<wchar_t> memPaths(memPathSize > 0 ? memPathSize : 1);
        if (memPathSize > 0) {
            PdhExpandWildCardPathW(nullptr, L"\\GPU Adapter Memory(*)\\Dedicated Usage",
                                    memPaths.data(), &memPathSize, 0);
            QMap<QString, PDH_HCOUNTER> memCounters;
            const wchar_t* p = memPaths.data();
            while (*p) {
                PDH_HCOUNTER c = nullptr;
                if (PdhAddEnglishCounterW(memQuery, p, 0, &c) == ERROR_SUCCESS) {
                    memCounters[QString::fromWCharArray(p)] = c;
                }
                p += wcslen(p) + 1;
            }
            PdhCollectQueryData(memQuery);
            for (auto it = memCounters.constBegin(); it != memCounters.constEnd(); ++it) {
                QString luid = extractLuid(it.key());
                if (luid.isEmpty()) continue;
                PDH_FMT_COUNTERVALUE val{};
                if (PdhGetFormattedCounterValue(it.value(), PDH_FMT_LARGE, nullptr, &val) == ERROR_SUCCESS) {
                    memUsedByLuid[luid] = static_cast<quint64>(val.largeValue);
                }
            }
        }
        PdhCloseQuery(memQuery);
    }

    int idx = 0;
    for (auto it = loadByLuid.constBegin(); it != loadByLuid.constEnd(); ++it, ++idx) {
        GpuInfo info;
        info.name = QString("GPU %1").arg(idx);
        info.vendor = "Unknown";
        info.loadPercent = qMin(100.0, it.value());
        info.memUsedBytes = memUsedByLuid.value(it.key(), 0);
        info.memTotalBytes = 0; // Total VRAM requires DXGI adapter enumeration; left as extension point
        result.push_back(info);
    }

    return result;
}
