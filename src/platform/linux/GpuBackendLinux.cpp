// Linux GPU backend covering three cases, all without hard link-time deps:
//
//  1. NVIDIA discrete GPUs: NVML is dlopen()'d at runtime. If libnvidia-ml.so
//     isn't installed (e.g. no NVIDIA driver), we silently skip this path.
//  2. Jetson/Tegra integrated GPU: read load/frequency straight from sysfs
//     (/sys/devices/gpu.0/load or /sys/class/devfreq/*.gpu/*), the same
//     files tegrastats itself reads. No CUDA/NVML required, works on a
//     stock JetPack image.
//  3. Generic fallback (Intel/AMD iGPU on x86): best-effort busy % from
//     /sys/class/drm/card*/device/gpu_busy_percent (amdgpu exposes this;
//     i915 does not always, so it may simply be unavailable).

#include "GpuStatsCollector.h"

#include <QDir>
#include <QFile>
#include <QDebug>
#include <dlfcn.h>
#include <cstring>

// ---------------------------------------------------------------------------
// Minimal NVML declarations (subset) so we don't need the NVML dev headers.
// ---------------------------------------------------------------------------
namespace nvml_min {
using nvmlReturn_t = int;
using nvmlDevice_t = void*;
constexpr nvmlReturn_t NVML_SUCCESS = 0;

struct nvmlMemory_t { unsigned long long total; unsigned long long free; unsigned long long used; };
struct nvmlUtilization_t { unsigned int gpu; unsigned int memory; };

using fn_nvmlInit_v2 = nvmlReturn_t(*)();
using fn_nvmlShutdown = nvmlReturn_t(*)();
using fn_nvmlDeviceGetCount_v2 = nvmlReturn_t(*)(unsigned int*);
using fn_nvmlDeviceGetHandleByIndex_v2 = nvmlReturn_t(*)(unsigned int, nvmlDevice_t*);
using fn_nvmlDeviceGetName = nvmlReturn_t(*)(nvmlDevice_t, char*, unsigned int);
using fn_nvmlDeviceGetUtilizationRates = nvmlReturn_t(*)(nvmlDevice_t, nvmlUtilization_t*);
using fn_nvmlDeviceGetMemoryInfo = nvmlReturn_t(*)(nvmlDevice_t, nvmlMemory_t*);
using fn_nvmlDeviceGetTemperature = nvmlReturn_t(*)(nvmlDevice_t, unsigned int, unsigned int*);
using fn_nvmlDeviceGetPowerUsage = nvmlReturn_t(*)(nvmlDevice_t, unsigned int*);
using fn_nvmlDeviceGetClockInfo = nvmlReturn_t(*)(nvmlDevice_t, unsigned int, unsigned int*);
} // namespace nvml_min

class NvmlBackend {
public:
    NvmlBackend() { tryLoad(); }
    ~NvmlBackend() {
        if (m_loaded && m_shutdown) m_shutdown();
        if (m_handle) dlclose(m_handle);
    }

    bool available() const { return m_loaded; }

    QVector<GpuInfo> collect() {
        QVector<GpuInfo> result;
        if (!m_loaded) return result;

        unsigned int count = 0;
        if (m_getCount(&count) != nvml_min::NVML_SUCCESS) return result;

        for (unsigned int i = 0; i < count; ++i) {
            nvml_min::nvmlDevice_t dev = nullptr;
            if (m_getHandle(i, &dev) != nvml_min::NVML_SUCCESS) continue;

            GpuInfo info;
            info.vendor = "NVIDIA";

            char name[128] = {0};
            if (m_getName(dev, name, sizeof(name)) == nvml_min::NVML_SUCCESS) {
                info.name = QString::fromLocal8Bit(name);
            } else {
                info.name = QString("NVIDIA GPU %1").arg(i);
            }

            nvml_min::nvmlUtilization_t util{};
            if (m_getUtil(dev, &util) == nvml_min::NVML_SUCCESS) {
                info.loadPercent = util.gpu;
            }

            nvml_min::nvmlMemory_t mem{};
            if (m_getMem(dev, &mem) == nvml_min::NVML_SUCCESS) {
                info.memTotalBytes = mem.total;
                info.memUsedBytes = mem.used;
            }

            unsigned int tempC = 0;
            if (m_getTemp && m_getTemp(dev, /*NVML_TEMPERATURE_GPU=*/0, &tempC) == nvml_min::NVML_SUCCESS) {
                info.temperatureC = tempC;
            }

            unsigned int mw = 0;
            if (m_getPower && m_getPower(dev, &mw) == nvml_min::NVML_SUCCESS) {
                info.powerWatts = mw / 1000.0;
            }

            unsigned int clk = 0;
            if (m_getClock && m_getClock(dev, /*NVML_CLOCK_GRAPHICS=*/0, &clk) == nvml_min::NVML_SUCCESS) {
                info.clockMHz = clk;
            }

            result.push_back(info);
        }
        return result;
    }

private:
    void* m_handle = nullptr;
    bool m_loaded = false;

    nvml_min::fn_nvmlShutdown m_shutdown = nullptr;
    nvml_min::fn_nvmlDeviceGetCount_v2 m_getCount = nullptr;
    nvml_min::fn_nvmlDeviceGetHandleByIndex_v2 m_getHandle = nullptr;
    nvml_min::fn_nvmlDeviceGetName m_getName = nullptr;
    nvml_min::fn_nvmlDeviceGetUtilizationRates m_getUtil = nullptr;
    nvml_min::fn_nvmlDeviceGetMemoryInfo m_getMem = nullptr;
    nvml_min::fn_nvmlDeviceGetTemperature m_getTemp = nullptr;
    nvml_min::fn_nvmlDeviceGetPowerUsage m_getPower = nullptr;
    nvml_min::fn_nvmlDeviceGetClockInfo m_getClock = nullptr;

    void tryLoad() {
        m_handle = dlopen("libnvidia-ml.so.1", RTLD_LAZY);
        if (!m_handle) m_handle = dlopen("libnvidia-ml.so", RTLD_LAZY);
        if (!m_handle) return; // no NVIDIA driver present -- not an error

        auto sym = [this](const char* n) { return dlsym(m_handle, n); };

        auto init = reinterpret_cast<nvml_min::fn_nvmlInit_v2>(sym("nvmlInit_v2"));
        m_shutdown = reinterpret_cast<nvml_min::fn_nvmlShutdown>(sym("nvmlShutdown"));
        m_getCount = reinterpret_cast<nvml_min::fn_nvmlDeviceGetCount_v2>(sym("nvmlDeviceGetCount_v2"));
        m_getHandle = reinterpret_cast<nvml_min::fn_nvmlDeviceGetHandleByIndex_v2>(sym("nvmlDeviceGetHandleByIndex_v2"));
        m_getName = reinterpret_cast<nvml_min::fn_nvmlDeviceGetName>(sym("nvmlDeviceGetName"));
        m_getUtil = reinterpret_cast<nvml_min::fn_nvmlDeviceGetUtilizationRates>(sym("nvmlDeviceGetUtilizationRates"));
        m_getMem = reinterpret_cast<nvml_min::fn_nvmlDeviceGetMemoryInfo>(sym("nvmlDeviceGetMemoryInfo"));
        m_getTemp = reinterpret_cast<nvml_min::fn_nvmlDeviceGetTemperature>(sym("nvmlDeviceGetTemperature"));
        m_getPower = reinterpret_cast<nvml_min::fn_nvmlDeviceGetPowerUsage>(sym("nvmlDeviceGetPowerUsage"));
        m_getClock = reinterpret_cast<nvml_min::fn_nvmlDeviceGetClockInfo>(sym("nvmlDeviceGetClockInfo"));

        if (!init || !m_shutdown || !m_getCount || !m_getHandle) {
            dlclose(m_handle);
            m_handle = nullptr;
            return;
        }
        m_loaded = (init() == nvml_min::NVML_SUCCESS);
    }
};

namespace {

bool isJetson() {
    return QFile::exists("/etc/nv_tegra_release") || QFile::exists("/sys/devices/gpu.0/load");
}

// Jetson/Tegra: GPU load is exposed as an integer 0-1000 (permille) in
// /sys/devices/gpu.0/load on older L4T, or under devfreq on newer JetPack.
GpuInfo collectTegraGpu() {
    GpuInfo info;
    info.vendor = "NVIDIA (Tegra)";
    info.name = "Jetson Integrated GPU";

    // Try classic path first
    QFile loadFile("/sys/devices/gpu.0/load");
    if (loadFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        bool ok = false;
        int permille = loadFile.readAll().trimmed().toInt(&ok);
        if (ok) info.loadPercent = permille / 10.0;
    } else {
        // Newer JetPack: find a devfreq node whose name contains "gpu"
        QDir devfreqDir("/sys/class/devfreq");
        const QStringList nodes = devfreqDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
        for (const QString& node : nodes) {
            if (!node.contains("gpu", Qt::CaseInsensitive) && !node.contains("gv11b") && !node.contains("gv1")) continue;
            QFile loadNode("/sys/class/devfreq/" + node + "/load");
            // Some kernels expose load via governor stats instead; try device/load
            QFile busyNode("/sys/class/devfreq/" + node + "/device/load");
            QFile* target = loadNode.exists() ? &loadNode : (busyNode.exists() ? &busyNode : nullptr);
            if (target && target->open(QIODevice::ReadOnly | QIODevice::Text)) {
                bool ok = false;
                double val = target->readAll().trimmed().toDouble(&ok);
                if (ok) { info.loadPercent = val; }
                target->close();
            }
            // Current frequency
            QFile curFreq("/sys/class/devfreq/" + node + "/cur_freq");
            if (curFreq.open(QIODevice::ReadOnly | QIODevice::Text)) {
                bool ok = false;
                double hz = curFreq.readAll().trimmed().toDouble(&ok);
                if (ok) info.clockMHz = hz / 1'000'000.0;
            }
            break;
        }
    }

    // Thermal zone often labeled "GPU-therm" on Jetson
    QDir thermalDir("/sys/class/thermal");
    const QStringList zones = thermalDir.entryList(QStringList() << "thermal_zone*", QDir::Dirs);
    for (const QString& zone : zones) {
        QFile typeFile("/sys/class/thermal/" + zone + "/type");
        if (!typeFile.open(QIODevice::ReadOnly | QIODevice::Text)) continue;
        QString type = QString::fromUtf8(typeFile.readAll()).trimmed();
        if (type.contains("GPU", Qt::CaseInsensitive)) {
            QFile tempFile("/sys/class/thermal/" + zone + "/temp");
            if (tempFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
                bool ok = false;
                double milliC = tempFile.readAll().trimmed().toDouble(&ok);
                if (ok) info.temperatureC = milliC / 1000.0;
            }
            break;
        }
    }

    // Jetson shares system RAM with the GPU (unified memory) -- memory
    // figures are therefore reported via the system RAM view, not here.
    return info;
}

// Generic fallback for Intel/AMD iGPU or dGPU exposing busy % via sysfs
// (mainly amdgpu). Returns empty QVector if nothing found.
QVector<GpuInfo> collectDrmSysfsGpus() {
    QVector<GpuInfo> result;
    QDir drmDir("/sys/class/drm");
    const QStringList cards = drmDir.entryList(QStringList() << "card[0-9]", QDir::Dirs);
    for (const QString& card : cards) {
        QString busyPath = "/sys/class/drm/" + card + "/device/gpu_busy_percent";
        QFile busyFile(busyPath);
        if (!busyFile.open(QIODevice::ReadOnly | QIODevice::Text)) continue;

        GpuInfo info;
        bool ok = false;
        double busy = busyFile.readAll().trimmed().toDouble(&ok);
        info.loadPercent = ok ? busy : -1.0;

        QFile vendorFile("/sys/class/drm/" + card + "/device/vendor");
        if (vendorFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
            QString v = vendorFile.readAll().trimmed();
            if (v == "0x1002") info.vendor = "AMD";
            else if (v == "0x8086") info.vendor = "Intel";
            else info.vendor = "GPU";
        }
        info.name = info.vendor + " " + card;

        QFile vramTotal("/sys/class/drm/" + card + "/device/mem_info_vram_total");
        QFile vramUsed("/sys/class/drm/" + card + "/device/mem_info_vram_used");
        if (vramTotal.open(QIODevice::ReadOnly | QIODevice::Text)) {
            info.memTotalBytes = vramTotal.readAll().trimmed().toULongLong();
        }
        if (vramUsed.open(QIODevice::ReadOnly | QIODevice::Text)) {
            info.memUsedBytes = vramUsed.readAll().trimmed().toULongLong();
        }

        result.push_back(info);
    }
    return result;
}

} // namespace

class GpuStatsCollector::Impl {
public:
    NvmlBackend nvml;
    bool jetson = isJetson();
};

GpuStatsCollector::GpuStatsCollector() : m_impl(new Impl()) {}
GpuStatsCollector::~GpuStatsCollector() { delete m_impl; }

QVector<GpuInfo> GpuStatsCollector::collect() {
    QVector<GpuInfo> result;

    if (m_impl->nvml.available()) {
        result = m_impl->nvml.collect();
    }

    if (m_impl->jetson) {
        result.push_back(collectTegraGpu());
    }

    if (result.isEmpty()) {
        result = collectDrmSysfsGpus();
    }

    return result;
}
