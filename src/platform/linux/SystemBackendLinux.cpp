// Linux implementation of SystemStatsCollector: CPU load (total + per-core),
// RAM/swap via /proc/meminfo, disk space via statvfs, disk I/O via /proc/diskstats.

#include "SystemStatsCollector.h"

#include <QFile>
#include <QTextStream>
#include <QStringList>
#include <QDateTime>
#include <QMap>
#include <QDir>

#include <sys/statvfs.h>
#include <mntent.h>
#include <cstdio>

namespace {

struct CpuTimes {
    quint64 user = 0, nice = 0, system = 0, idle = 0, iowait = 0, irq = 0, softirq = 0, steal = 0;
    quint64 total() const { return user + nice + system + idle + iowait + irq + softirq + steal; }
    quint64 active() const { return total() - idle - iowait; }
};

QMap<int, CpuTimes> readProcStatAllCores(CpuTimes* aggregateOut) {
    QMap<int, CpuTimes> perCore;
    QFile f("/proc/stat");
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return perCore;

    QTextStream ts(&f);
    QString line;
    while (ts.readLineInto(&line)) {
        if (!line.startsWith("cpu")) break;
        QStringList parts = line.split(' ', Qt::SkipEmptyParts);
        if (parts.isEmpty()) continue;
        QString label = parts[0]; // "cpu" or "cpu0", "cpu1", ...

        CpuTimes t;
        auto val = [&](int idx) -> quint64 {
            return (idx < parts.size()) ? parts[idx].toULongLong() : 0;
        };
        t.user = val(1); t.nice = val(2); t.system = val(3); t.idle = val(4);
        t.iowait = val(5); t.irq = val(6); t.softirq = val(7); t.steal = val(8);

        if (label == "cpu") {
            if (aggregateOut) *aggregateOut = t;
        } else {
            int coreIdx = label.mid(3).toInt();
            perCore[coreIdx] = t;
        }
    }
    return perCore;
}

double readCoreFrequencyMHz(int coreIndex) {
    QString path = QString("/sys/devices/system/cpu/cpu%1/cpufreq/scaling_cur_freq").arg(coreIndex);
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return 0.0;
    QByteArray data = f.readAll().trimmed();
    bool ok = false;
    double khz = data.toDouble(&ok);
    return ok ? khz / 1000.0 : 0.0;
}

double readCpuTemperatureC() {
    // Try common thermal zone paths; works on x86 and most ARM/Jetson boards.
    QDir thermalDir("/sys/class/thermal");
    const QStringList zones = thermalDir.entryList(QStringList() << "thermal_zone*", QDir::Dirs);
    for (const QString& zone : zones) {
        QFile typeFile("/sys/class/thermal/" + zone + "/type");
        QFile tempFile("/sys/class/thermal/" + zone + "/temp");
        if (!typeFile.open(QIODevice::ReadOnly | QIODevice::Text)) continue;
        QString type = QString::fromUtf8(typeFile.readAll()).trimmed().toLower();
        if (type.contains("cpu") || type.contains("soc") || type.contains("x86_pkg_temp") || type.contains("tj")) {
            if (tempFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
                bool ok = false;
                double milliC = QString::fromUtf8(tempFile.readAll()).trimmed().toDouble(&ok);
                if (ok) return milliC / 1000.0;
            }
        }
    }
    return -1.0;
}

} // namespace

class SystemStatsCollector::Impl {
public:
    CpuTimes prevAggregate;
    QMap<int, CpuTimes> prevPerCore;
    bool havePrev = false;

    QMap<QString, quint64> prevReadSectors;
    QMap<QString, quint64> prevWriteSectors;
    QMap<QString, quint64> prevIoTimeMs;
    qint64 prevIoSampleMs = 0;
};

SystemStatsCollector::SystemStatsCollector() : m_impl(new Impl()) {}
SystemStatsCollector::~SystemStatsCollector() { delete m_impl; }

CpuStats SystemStatsCollector::collectCpu() {
    CpuStats stats;
    CpuTimes aggregate;
    QMap<int, CpuTimes> perCore = readProcStatAllCores(&aggregate);

    if (m_impl->havePrev) {
        quint64 totalDelta = aggregate.total() - m_impl->prevAggregate.total();
        quint64 activeDelta = aggregate.active() - m_impl->prevAggregate.active();
        stats.totalPercent = totalDelta ? (double(activeDelta) / double(totalDelta)) * 100.0 : 0.0;

        for (auto it = perCore.constBegin(); it != perCore.constEnd(); ++it) {
            int idx = it.key();
            CpuCoreLoad core;
            core.coreIndex = idx;
            core.frequencyMHz = readCoreFrequencyMHz(idx);
            if (m_impl->prevPerCore.contains(idx)) {
                const CpuTimes& prevT = m_impl->prevPerCore[idx];
                quint64 td = it.value().total() - prevT.total();
                quint64 ad = it.value().active() - prevT.active();
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
    QFile f("/proc/meminfo");
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return mem;

    QMap<QString, quint64> kv;
    QTextStream ts(&f);
    QString line;
    while (ts.readLineInto(&line)) {
        QStringList parts = line.split(':', Qt::SkipEmptyParts);
        if (parts.size() < 2) continue;
        QString key = parts[0].trimmed();
        QString valPart = parts[1].trimmed().split(' ').value(0);
        kv[key] = valPart.toULongLong() * 1024; // kB -> bytes
    }

    mem.totalBytes = kv.value("MemTotal");
    quint64 memFree = kv.value("MemFree");
    mem.availableBytes = kv.value("MemAvailable", memFree);
    mem.cachedBytes = kv.value("Cached") + kv.value("Buffers");
    mem.usedBytes = (mem.totalBytes > mem.availableBytes) ? (mem.totalBytes - mem.availableBytes) : 0;
    mem.swapTotalBytes = kv.value("SwapTotal");
    quint64 swapFree = kv.value("SwapFree");
    mem.swapUsedBytes = (mem.swapTotalBytes > swapFree) ? (mem.swapTotalBytes - swapFree) : 0;

    return mem;
}

QVector<DiskVolume> SystemStatsCollector::collectDiskVolumes() {
    QVector<DiskVolume> volumes;

    FILE* mtab = setmntent("/proc/mounts", "r");
    if (!mtab) return volumes;

    struct mntent* ent;
    while ((ent = getmntent(mtab)) != nullptr) {
        QString device = QString::fromLocal8Bit(ent->mnt_fsname);
        QString mountPoint = QString::fromLocal8Bit(ent->mnt_dir);
        QString fsType = QString::fromLocal8Bit(ent->mnt_type);

        // Skip pseudo/virtual filesystems for a clean, top(1)-like list.
        static const QStringList skipTypes = {
            "proc", "sysfs", "devtmpfs", "tmpfs", "devpts", "cgroup", "cgroup2",
            "pstore", "bpf", "tracefs", "debugfs", "securityfs", "mqueue",
            "hugetlbfs", "overlay", "squashfs", "autofs", "fusectl", "configfs", "binfmt_misc"
        };
        if (skipTypes.contains(fsType) || device.startsWith("/dev/loop")) continue;

        struct statvfs vfs{};
        if (statvfs(ent->mnt_dir, &vfs) != 0) continue;
        if (vfs.f_blocks == 0) continue;

        DiskVolume vol;
        vol.device = device;
        vol.mountPoint = mountPoint;
        vol.fsType = fsType;
        vol.totalBytes = quint64(vfs.f_blocks) * vfs.f_frsize;
        vol.freeBytes = quint64(vfs.f_bfree) * vfs.f_frsize;
        vol.usedBytes = vol.totalBytes - vol.freeBytes;
        volumes.push_back(vol);
    }
    endmntent(mtab);
    return volumes;
}

QVector<DiskIoStats> SystemStatsCollector::collectDiskIo() {
    QVector<DiskIoStats> result;
    QFile f("/proc/diskstats");
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return result;

    qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    double dtSec = m_impl->prevIoSampleMs
                       ? (nowMs - m_impl->prevIoSampleMs) / 1000.0
                       : 0.0;
    if (dtSec <= 0) dtSec = 1.0;

    QTextStream ts(&f);
    QString line;
    while (ts.readLineInto(&line)) {
        QStringList p = line.split(' ', Qt::SkipEmptyParts);
        if (p.size() < 14) continue;
        QString name = p[2];

        // Only real block devices (skip partitions like sda1, loopN, ram disks)
        if (name.startsWith("loop") || name.startsWith("ram")) continue;
        static const QStringList wanted = {"sd", "nvme", "mmcblk", "vd", "hd"};
        bool looksLikeDisk = false;
        for (const QString& pfx : wanted) if (name.startsWith(pfx)) { looksLikeDisk = true; break; }
        if (!looksLikeDisk) continue;
        // Skip partitions (e.g. sda1, nvme0n1p1) -- keep only whole-device entries
        if (name.startsWith("sd") && name.back().isDigit()) continue;
        if (name.startsWith("nvme") && name.contains('p')) continue;
        if (name.startsWith("mmcblk") && name.contains('p')) continue;

        quint64 readSectors = p[5].toULongLong();
        quint64 writeSectors = p[9].toULongLong();
        quint64 ioTimeMs = p[12].toULongLong();

        DiskIoStats io;
        io.device = name;

        if (m_impl->prevReadSectors.contains(name)) {
            quint64 rDelta = readSectors - m_impl->prevReadSectors[name];
            quint64 wDelta = writeSectors - m_impl->prevWriteSectors[name];
            quint64 ioDelta = ioTimeMs - m_impl->prevIoTimeMs[name];
            io.readBytesPerSec = quint64((rDelta * 512) / dtSec);
            io.writeBytesPerSec = quint64((wDelta * 512) / dtSec);
            io.utilizationPercent = qMin(100.0, (ioDelta / (dtSec * 1000.0)) * 100.0);
        }

        m_impl->prevReadSectors[name] = readSectors;
        m_impl->prevWriteSectors[name] = writeSectors;
        m_impl->prevIoTimeMs[name] = ioTimeMs;

        result.push_back(io);
    }

    m_impl->prevIoSampleMs = nowMs;
    return result;
}
