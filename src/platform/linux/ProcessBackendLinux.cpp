// Linux implementation of ProcessCollector using /proc.
// Works identically on x86_64 and armv8 (including Jetson), since /proc's
// layout is architecture-independent.

#include "ProcessCollector.h"

#include <QDir>
#include <QFile>
#include <QTextStream>
#include <QDateTime>
#include <QCoreApplication>
#include <QThread>

#include <sys/stat.h>
#include <pwd.h>
#include <unistd.h>
#include <signal.h>
#include <sys/resource.h>

namespace {

long clockTicksPerSec() {
    static long hz = sysconf(_SC_CLK_TCK);
    return hz > 0 ? hz : 100;
}

// Reads a single /proc/<pid>/stat-style line and extracts the fields we need.
// Handles the "comm" field safely even if it contains spaces/parentheses.
struct ProcStatFields {
    QString comm;
    QChar   state;
    qint64  ppid = 0;
    quint64 utime = 0, stime = 0;
    qint64  numThreads = 0;
    qint64  priority = 0, nice = 0;
    quint64 starttime = 0;
    bool    ok = false;
};

ProcStatFields parseStat(const QString& path) {
    ProcStatFields f;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return f;

    QByteArray data = file.readAll();
    int open = data.indexOf('(');
    int close = data.lastIndexOf(')');
    if (open < 0 || close < 0 || close < open) return f;

    f.comm = QString::fromUtf8(data.mid(open + 1, close - open - 1));

    QString rest = QString::fromUtf8(data.mid(close + 2)); // skip ") "
    QStringList parts = rest.split(' ', Qt::SkipEmptyParts);
    // Fields after comm, 1-indexed from the man page starting at field 3:
    // 0: state, 1: ppid, ... 11: utime, 12: stime, ... 17: priority, 18: nice,
    // 19: num_threads, ... 21: starttime
    if (parts.size() < 22) return f;

    f.state = parts[0].isEmpty() ? QChar('?') : parts[0][0];
    f.ppid = parts[1].toLongLong();
    f.utime = parts[11].toULongLong();
    f.stime = parts[12].toULongLong();
    f.priority = parts[15].toLongLong();
    f.nice = parts[16].toLongLong();
    f.numThreads = parts[17].toLongLong();
    f.starttime = parts[19].toULongLong();
    f.ok = true;
    return f;
}

quint64 readVmRssBytes(const QString& statusPath, quint64* vmSizeOut) {
    QFile f(statusPath);
    quint64 rss = 0, vsize = 0;
    if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QTextStream ts(&f);
        QString line;
        while (ts.readLineInto(&line)) {
            if (line.startsWith("VmRSS:")) {
                rss = line.split(' ', Qt::SkipEmptyParts).value(1).toULongLong() * 1024;
            } else if (line.startsWith("VmSize:")) {
                vsize = line.split(' ', Qt::SkipEmptyParts).value(1).toULongLong() * 1024;
            }
        }
    }
    if (vmSizeOut) *vmSizeOut = vsize;
    return rss;
}

QString userNameForUid(uid_t uid) {
    struct passwd* pw = getpwuid(uid);
    return pw ? QString::fromLocal8Bit(pw->pw_name) : QString::number(uid);
}

QString readCmdline(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return QString();
    QByteArray data = f.readAll();
    data.replace('\0', ' ');
    return QString::fromUtf8(data).trimmed();
}

QString stateToText(QChar c) {
    switch (c.toLatin1()) {
        case 'R': return "Running";
        case 'S': return "Sleeping";
        case 'D': return "Disk Wait";
        case 'Z': return "Zombie";
        case 'T': return "Stopped";
        case 't': return "Tracing Stop";
        case 'X': return "Dead";
        case 'I': return "Idle";
        default:  return "Unknown";
    }
}

quint64 readSystemTotalTicks() {
    QFile f("/proc/stat");
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return 0;
    QString line = QString::fromUtf8(f.readLine());
    QStringList parts = line.split(' ', Qt::SkipEmptyParts);
    quint64 total = 0;
    for (int i = 1; i < parts.size(); ++i) total += parts[i].toULongLong();
    return total;
}

} // namespace

class ProcessCollector::Impl {
public:
    // no persistent state needed beyond what's in ProcessCollector itself
};

ProcessCollector::ProcessCollector() : m_impl(new Impl()) {
    m_prevSystemTotalTicks = readSystemTotalTicks();
    m_prevSampleMs = QDateTime::currentMSecsSinceEpoch();
}

ProcessCollector::~ProcessCollector() {
    delete m_impl;
}

QVector<ProcessInfo> ProcessCollector::collect() {
    QVector<ProcessInfo> result;

    quint64 systemTotalNow = readSystemTotalTicks();
    quint64 systemDelta = (systemTotalNow > m_prevSystemTotalTicks)
                               ? (systemTotalNow - m_prevSystemTotalTicks)
                               : 1;
    qint64 nowMs = QDateTime::currentMSecsSinceEpoch();

    long nproc = QThread::idealThreadCount();
    if (nproc <= 0) nproc = 1;

    QDir procDir("/proc");
    const QStringList entries = procDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot);

    QMap<qint64, CpuTimeSample> newSamples;

    for (const QString& entry : entries) {
        bool isPid = false;
        qint64 pid = entry.toLongLong(&isPid);
        if (!isPid) continue;

        QString base = "/proc/" + entry;
        ProcStatFields stat = parseStat(base + "/stat");
        if (!stat.ok) continue; // process may have exited between listdir and read

        ProcessInfo info;
        info.pid = pid;
        info.ppid = stat.ppid;
        info.name = stat.comm;
        info.state = stateToText(stat.state);
        info.threadCount = static_cast<int>(stat.numThreads);
        info.niceValue = static_cast<int>(stat.nice);
        info.commandLine = readCmdline(base + "/cmdline");
        if (info.commandLine.isEmpty()) info.commandLine = "[" + stat.comm + "]";

        struct stat st{};
        if (::stat(base.toLocal8Bit().constData(), &st) == 0) {
            info.user = userNameForUid(st.st_uid);
        }

        quint64 vsize = 0;
        info.memRssBytes = readVmRssBytes(base + "/status", &vsize);
        info.memVirtBytes = vsize;

        quint64 totalTicks = stat.utime + stat.stime;
        CpuTimeSample sample{totalTicks, nowMs};
        newSamples[pid] = sample;

        auto prevIt = m_prevSamples.constFind(pid);
        if (prevIt != m_prevSamples.constEnd() && systemDelta > 0) {
            quint64 procDelta = (totalTicks >= prevIt->totalTimeTicks)
                                     ? (totalTicks - prevIt->totalTimeTicks)
                                     : 0;
            // CPU% normalized like `top`: (proc delta / system delta) * 100 * nproc
            info.cpuPercent = (double(procDelta) / double(systemDelta)) * 100.0 * nproc;
        } else {
            info.cpuPercent = 0.0;
        }

        result.push_back(info);
    }

    m_prevSamples = newSamples;
    m_prevSystemTotalTicks = systemTotalNow;
    m_prevSampleMs = nowMs;

    return result;
}

QVector<ThreadInfo> ProcessCollector::collectThreads(qint64 pid) {
    QVector<ThreadInfo> threads;
    QString taskDir = QString("/proc/%1/task").arg(pid);
    QDir dir(taskDir);
    if (!dir.exists()) return threads;

    const QStringList tids = dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString& tidStr : tids) {
        ProcStatFields stat = parseStat(taskDir + "/" + tidStr + "/stat");
        if (!stat.ok) continue;
        ThreadInfo t;
        t.tid = tidStr.toLongLong();
        t.name = stat.comm;
        t.state = stateToText(stat.state);
        t.priority = static_cast<int>(stat.priority);
        // Per-thread CPU% would require another delta sample keyed by tid;
        // omitted here for simplicity (shown as 0 unless extended).
        t.cpuPercent = 0.0;
        threads.push_back(t);
    }
    return threads;
}

bool ProcessCollector::killProcess(qint64 pid) {
    return ::kill(static_cast<pid_t>(pid), SIGTERM) == 0;
}

bool ProcessCollector::setPriority(qint64 pid, int niceValue) {
    return ::setpriority(PRIO_PROCESS, static_cast<id_t>(pid), niceValue) == 0;
}
