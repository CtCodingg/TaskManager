// GPU statistics: NVIDIA via dlopen'd NVML, Jetson/Tegra via sysfs, generic fallback via DRM sysfs.

#include "GpuStatsCollector.h"

#include <fstream>
#include <sstream>
#include <dirent.h>
#include <dlfcn.h>
#include <algorithm>

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

    std::vector<GpuInfo> collect() {
        std::vector<GpuInfo> result;
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
                info.name = name;
            } else {
                info.name = "NVIDIA GPU " + std::to_string(i);
            }

            nvml_min::nvmlUtilization_t util{};
            if (m_getUtil(dev, &util) == nvml_min::NVML_SUCCESS) info.loadPercent = util.gpu;

            nvml_min::nvmlMemory_t mem{};
            if (m_getMem(dev, &mem) == nvml_min::NVML_SUCCESS) {
                info.memTotalBytes = mem.total;
                info.memUsedBytes = mem.used;
            }

            unsigned int tempC = 0;
            if (m_getTemp && m_getTemp(dev, 0, &tempC) == nvml_min::NVML_SUCCESS) info.temperatureC = tempC;

            unsigned int mw = 0;
            if (m_getPower && m_getPower(dev, &mw) == nvml_min::NVML_SUCCESS) info.powerWatts = mw / 1000.0;

            unsigned int clk = 0;
            if (m_getClock && m_getClock(dev, 0, &clk) == nvml_min::NVML_SUCCESS) info.clockMHz = clk;

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
        if (!m_handle) return;

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

bool fileExists(const std::string& path) {
    std::ifstream f(path);
    return f.good();
}

bool isJetson() {
    return fileExists("/etc/nv_tegra_release") || fileExists("/sys/devices/gpu.0/load");
}

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    size_t b = s.find_last_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    return s.substr(a, b - a + 1);
}

GpuInfo collectTegraGpu() {
    GpuInfo info;
    info.vendor = "NVIDIA (Tegra)";
    info.name = "Jetson Integrated GPU";

    std::ifstream loadFile("/sys/devices/gpu.0/load");
    if (loadFile) {
        int permille = 0;
        loadFile >> permille;
        if (!loadFile.fail()) info.loadPercent = permille / 10.0;
    } else {
        DIR* dir = opendir("/sys/class/devfreq");
        if (dir) {
            struct dirent* entry;
            while ((entry = readdir(dir)) != nullptr) {
                std::string node = entry->d_name;
                if (node.find("gpu") == std::string::npos && node.find("gv11b") == std::string::npos &&
                    node.find("gv1") == std::string::npos) continue;

                std::string base = "/sys/class/devfreq/" + node;
                std::ifstream loadNode(base + "/load");
                std::ifstream busyNode(base + "/device/load");
                double val = 0.0;
                if (loadNode) { loadNode >> val; if (!loadNode.fail()) info.loadPercent = val; }
                else if (busyNode) { busyNode >> val; if (!busyNode.fail()) info.loadPercent = val; }

                std::ifstream curFreq(base + "/cur_freq");
                if (curFreq) {
                    double hz = 0.0;
                    curFreq >> hz;
                    if (!curFreq.fail()) info.clockMHz = hz / 1'000'000.0;
                }
                break;
            }
            closedir(dir);
        }
    }

    DIR* thermalDir = opendir("/sys/class/thermal");
    if (thermalDir) {
        struct dirent* entry;
        while ((entry = readdir(thermalDir)) != nullptr) {
            std::string name = entry->d_name;
            if (name.rfind("thermal_zone", 0) != 0) continue;
            std::ifstream typeFile("/sys/class/thermal/" + name + "/type");
            if (!typeFile) continue;
            std::string type;
            std::getline(typeFile, type);
            std::string typeLower = type;
            std::transform(typeLower.begin(), typeLower.end(), typeLower.begin(), ::tolower);
            if (typeLower.find("gpu") != std::string::npos) {
                std::ifstream tempFile("/sys/class/thermal/" + name + "/temp");
                if (tempFile) {
                    double milliC = 0.0;
                    tempFile >> milliC;
                    if (!tempFile.fail()) info.temperatureC = milliC / 1000.0;
                }
                break;
            }
        }
        closedir(thermalDir);
    }

    return info;
}

std::vector<GpuInfo> collectDrmSysfsGpus() {
    std::vector<GpuInfo> result;
    DIR* dir = opendir("/sys/class/drm");
    if (!dir) return result;

    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        std::string name = entry->d_name;
        if (name.size() < 5 || name.rfind("card", 0) != 0) continue;
        if (!isdigit(static_cast<unsigned char>(name[4]))) continue;

        std::string busyPath = "/sys/class/drm/" + name + "/device/gpu_busy_percent";
        std::ifstream busyFile(busyPath);
        if (!busyFile) continue;

        GpuInfo info;
        double busy = -1.0;
        busyFile >> busy;
        info.loadPercent = busyFile.fail() ? -1.0 : busy;

        std::ifstream vendorFile("/sys/class/drm/" + name + "/device/vendor");
        if (vendorFile) {
            std::string v;
            vendorFile >> v;
            if (v == "0x1002") info.vendor = "AMD";
            else if (v == "0x8086") info.vendor = "Intel";
            else info.vendor = "GPU";
        }
        info.name = info.vendor + " " + name;

        std::ifstream vramTotal("/sys/class/drm/" + name + "/device/mem_info_vram_total");
        std::ifstream vramUsed("/sys/class/drm/" + name + "/device/mem_info_vram_used");
        if (vramTotal) vramTotal >> info.memTotalBytes;
        if (vramUsed) vramUsed >> info.memUsedBytes;

        result.push_back(info);
    }
    closedir(dir);
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

std::vector<GpuInfo> GpuStatsCollector::collect() {
    std::vector<GpuInfo> result;

    if (m_impl->nvml.available()) result = m_impl->nvml.collect();
    if (m_impl->jetson) result.push_back(collectTegraGpu());
    if (result.empty()) result = collectDrmSysfsGpus();

    return result;
}
