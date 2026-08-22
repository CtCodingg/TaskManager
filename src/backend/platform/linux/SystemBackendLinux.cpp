// CPU, memory, and disk statistics via /proc and statvfs.

#include "SystemStatsCollector.h"

#include <fstream>
#include <sstream>
#include <map>
#include <chrono>
#include <algorithm>
#include <dirent.h>
#include <sys/statvfs.h>
#include <mntent.h>
#include <cstdio>

namespace {

int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

std::vector<std::string> splitWs(const std::string& s) {
    std::vector<std::string> out;
    std::istringstream iss(s);
    std::string tok;
    while (iss >> tok) out.push_back(tok);
    return out;
}

struct CpuTimes {
    uint64_t user = 0, nice = 0, system = 0, idle = 0, iowait = 0, irq = 0, softirq = 0, steal = 0;
    uint64_t total() const { return user + nice + system + idle + iowait + irq + softirq + steal; }
    uint64_t active() const { return total() - idle - iowait; }
};

std::map<int, CpuTimes> readProcStatAllCores(CpuTimes* aggregateOut) {
    std::map<int, CpuTimes> perCore;
    std::ifstream f("/proc/stat");
    if (!f) return perCore;

    std::string line;
    while (std::getline(f, line)) {
        if (line.rfind("cpu", 0) != 0) break;
        auto parts = splitWs(line);
        if (parts.empty()) continue;
        std::string label = parts[0];

        auto val = [&](size_t idx) -> uint64_t {
            return (idx < parts.size()) ? std::stoull(parts[idx]) : 0;
        };
        CpuTimes t;
        t.user = val(1); t.nice = val(2); t.system = val(3); t.idle = val(4);
        t.iowait = val(5); t.irq = val(6); t.softirq = val(7); t.steal = val(8);

        if (label == "cpu") {
            if (aggregateOut) *aggregateOut = t;
        } else {
            int coreIdx = std::stoi(label.substr(3));
            perCore[coreIdx] = t;
        }
    }
    return perCore;
}

double readCoreFrequencyMHz(int coreIndex) {
    std::string path = "/sys/devices/system/cpu/cpu" + std::to_string(coreIndex) + "/cpufreq/scaling_cur_freq";
    std::ifstream f(path);
    if (!f) return 0.0;
    double khz = 0.0;
    f >> khz;
    return f.fail() ? 0.0 : khz / 1000.0;
}

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    size_t b = s.find_last_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    return s.substr(a, b - a + 1);
}

std::string toLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}

double readCpuTemperatureC() {
    DIR* dir = opendir("/sys/class/thermal");
    if (!dir) return -1.0;
    double result = -1.0;
    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        std::string name = entry->d_name;
        if (name.rfind("thermal_zone", 0) != 0) continue;
        std::string base = "/sys/class/thermal/" + name;
        std::ifstream typeFile(base + "/type");
        if (!typeFile) continue;
        std::string type;
        std::getline(typeFile, type);
        type = toLower(trim(type));
        if (type.find("cpu") != std::string::npos || type.find("soc") != std::string::npos ||
            type.find("x86_pkg_temp") != std::string::npos || type.find("tj") != std::string::npos) {
            std::ifstream tempFile(base + "/temp");
            if (tempFile) {
                double milliC = 0.0;
                tempFile >> milliC;
                if (!tempFile.fail()) { result = milliC / 1000.0; break; }
            }
        }
    }
    closedir(dir);
    return result;
}

} // namespace

class SystemStatsCollector::Impl {
public:
    CpuTimes prevAggregate;
    std::map<int, CpuTimes> prevPerCore;
    bool havePrev = false;

    std::map<std::string, uint64_t> prevReadSectors;
    std::map<std::string, uint64_t> prevWriteSectors;
    std::map<std::string, uint64_t> prevIoTimeMs;
    int64_t prevIoSampleMs = 0;
};

SystemStatsCollector::SystemStatsCollector() : m_impl(new Impl()) {}
SystemStatsCollector::~SystemStatsCollector() { delete m_impl; }

CpuStats SystemStatsCollector::collectCpu() {
    CpuStats stats;
    CpuTimes aggregate;
    std::map<int, CpuTimes> perCore = readProcStatAllCores(&aggregate);

    if (m_impl->havePrev) {
        uint64_t totalDelta = aggregate.total() - m_impl->prevAggregate.total();
        uint64_t activeDelta = aggregate.active() - m_impl->prevAggregate.active();
        stats.totalPercent = totalDelta ? (double(activeDelta) / double(totalDelta)) * 100.0 : 0.0;

        for (auto& [idx, times] : perCore) {
            CpuCoreLoad core;
            core.coreIndex = idx;
            core.frequencyMHz = readCoreFrequencyMHz(idx);
            auto prevIt = m_impl->prevPerCore.find(idx);
            if (prevIt != m_impl->prevPerCore.end()) {
                uint64_t td = times.total() - prevIt->second.total();
                uint64_t ad = times.active() - prevIt->second.active();
                core.percent = td ? (double(ad) / double(td)) * 100.0 : 0.0;
            }
            stats.perCore.push_back(core);
        }
    }

    stats.temperatureC = readCpuTemperatureC();

    m_impl->prevAggregate = aggregate;
    m_impl->prevPerCore = perCore;
    m_impl->havePrev = true;
    return stats;
}

MemoryStats SystemStatsCollector::collectMemory() {
    MemoryStats mem;
    std::ifstream f("/proc/meminfo");
    if (!f) return mem;

    std::map<std::string, uint64_t> kv;
    std::string line;
    while (std::getline(f, line)) {
        size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string key = trim(line.substr(0, colon));
        std::string rest = trim(line.substr(colon + 1));
        std::istringstream iss(rest);
        uint64_t val = 0;
        iss >> val;
        kv[key] = val * 1024;
    }

    mem.totalBytes = kv.count("MemTotal") ? kv["MemTotal"] : 0;
    uint64_t memFree = kv.count("MemFree") ? kv["MemFree"] : 0;
    mem.availableBytes = kv.count("MemAvailable") ? kv["MemAvailable"] : memFree;
    mem.cachedBytes = (kv.count("Cached") ? kv["Cached"] : 0) + (kv.count("Buffers") ? kv["Buffers"] : 0);
    mem.usedBytes = (mem.totalBytes > mem.availableBytes) ? (mem.totalBytes - mem.availableBytes) : 0;
    mem.swapTotalBytes = kv.count("SwapTotal") ? kv["SwapTotal"] : 0;
    uint64_t swapFree = kv.count("SwapFree") ? kv["SwapFree"] : 0;
    mem.swapUsedBytes = (mem.swapTotalBytes > swapFree) ? (mem.swapTotalBytes - swapFree) : 0;

    return mem;
}

std::vector<DiskVolume> SystemStatsCollector::collectDiskVolumes() {
    std::vector<DiskVolume> volumes;

    FILE* mtab = setmntent("/proc/mounts", "r");
    if (!mtab) return volumes;

    static const std::vector<std::string> skipTypes = {
        "proc", "sysfs", "devtmpfs", "tmpfs", "devpts", "cgroup", "cgroup2",
        "pstore", "bpf", "tracefs", "debugfs", "securityfs", "mqueue",
        "hugetlbfs", "overlay", "squashfs", "autofs", "fusectl", "configfs", "binfmt_misc"
    };

    struct mntent* ent;
    while ((ent = getmntent(mtab)) != nullptr) {
        std::string device = ent->mnt_fsname;
        std::string mountPoint = ent->mnt_dir;
        std::string fsType = ent->mnt_type;

        if (std::find(skipTypes.begin(), skipTypes.end(), fsType) != skipTypes.end()) continue;
        if (device.rfind("/dev/loop", 0) == 0) continue;

        struct statvfs vfs{};
        if (statvfs(ent->mnt_dir, &vfs) != 0) continue;
        if (vfs.f_blocks == 0) continue;

        DiskVolume vol;
        vol.device = device;
        vol.mountPoint = mountPoint;
        vol.fsType = fsType;
        vol.totalBytes = uint64_t(vfs.f_blocks) * vfs.f_frsize;
        vol.freeBytes = uint64_t(vfs.f_bfree) * vfs.f_frsize;
        vol.usedBytes = vol.totalBytes - vol.freeBytes;
        volumes.push_back(vol);
    }
    endmntent(mtab);
    return volumes;
}

std::vector<DiskIoStats> SystemStatsCollector::collectDiskIo() {
    std::vector<DiskIoStats> result;
    std::ifstream f("/proc/diskstats");
    if (!f) return result;

    int64_t now = nowMs();
    double dtSec = m_impl->prevIoSampleMs ? (now - m_impl->prevIoSampleMs) / 1000.0 : 0.0;
    if (dtSec <= 0) dtSec = 1.0;

    std::string line;
    while (std::getline(f, line)) {
        auto p = splitWs(line);
        if (p.size() < 14) continue;
        std::string name = p[2];

        if (name.rfind("loop", 0) == 0 || name.rfind("ram", 0) == 0) continue;
        static const std::vector<std::string> wanted = {"sd", "nvme", "mmcblk", "vd", "hd"};
        bool looksLikeDisk = false;
        for (const auto& pfx : wanted) if (name.rfind(pfx, 0) == 0) { looksLikeDisk = true; break; }
        if (!looksLikeDisk) continue;
        if (name.rfind("sd", 0) == 0 && isdigit(static_cast<unsigned char>(name.back()))) continue;
        if (name.rfind("nvme", 0) == 0 && name.find('p') != std::string::npos) continue;
        if (name.rfind("mmcblk", 0) == 0 && name.find('p') != std::string::npos) continue;

        uint64_t readSectors = std::stoull(p[5]);
        uint64_t writeSectors = std::stoull(p[9]);
        uint64_t ioTimeMs = std::stoull(p[12]);

        DiskIoStats io;
        io.device = name;

        auto prevR = m_impl->prevReadSectors.find(name);
        if (prevR != m_impl->prevReadSectors.end()) {
            uint64_t rDelta = readSectors - prevR->second;
            uint64_t wDelta = writeSectors - m_impl->prevWriteSectors[name];
            uint64_t ioDelta = ioTimeMs - m_impl->prevIoTimeMs[name];
            io.readBytesPerSec = uint64_t((rDelta * 512) / dtSec);
            io.writeBytesPerSec = uint64_t((wDelta * 512) / dtSec);
            io.utilizationPercent = std::min(100.0, (ioDelta / (dtSec * 1000.0)) * 100.0);
        }

        m_impl->prevReadSectors[name] = readSectors;
        m_impl->prevWriteSectors[name] = writeSectors;
        m_impl->prevIoTimeMs[name] = ioTimeMs;

        result.push_back(io);
    }

    m_impl->prevIoSampleMs = now;
    return result;
}
