// Windows implementation of ProcessCollector using Toolhelp32 snapshots +
// PSAPI/QueryFullProcessImageName. No third-party dependencies -- only
// system libraries linked via CMake (psapi).

#include "ProcessCollector.h"

#include <QDateTime>

#include <windows.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <thread>

namespace {

quint64 fileTimeToTicks(const FILETIME& ft) {
    ULARGE_INTEGER uli;
    uli.LowPart = ft.dwLowDateTime;
    uli.HighPart = ft.dwHighDateTime;
    return uli.QuadPart; // 100-ns units
}

QString processNameFromEntry(const PROCESSENTRY32W& pe) {
    return QString::fromWCharArray(pe.szExeFile);
}

quint64 readSystemTotalTicks() {
    FILETIME idle, kernel, user;
    if (!GetSystemTimes(&idle, &kernel, &user)) return 0;
    return fileTimeToTicks(kernel) + fileTimeToTicks(user);
}

} // namespace

class ProcessCollector::Impl {};

ProcessCollector::ProcessCollector() : m_impl(new Impl()) {
    m_prevSystemTotalTicks = readSystemTotalTicks();
    m_prevSampleMs = QDateTime::currentMSecsSinceEpoch();
}

ProcessCollector::~ProcessCollector() { delete m_impl; }

QVector<ProcessInfo> ProcessCollector::collect() {
    QVector<ProcessInfo> result;

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return result;

    quint64 systemTotalNow = readSystemTotalTicks();
    quint64 systemDelta = (systemTotalNow > m_prevSystemTotalTicks)
                               ? (systemTotalNow - m_prevSystemTotalTicks) : 1;
    qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    unsigned int nproc = std::thread::hardware_concurrency();
    if (nproc == 0) nproc = 1;

    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);

    QMap<qint64, CpuTimeSample> newSamples;

    if (Process32FirstW(snap, &pe)) {
        do {
            ProcessInfo info;
            info.pid = pe.th32ProcessID;
            info.ppid = pe.th32ParentProcessID;
            info.name = processNameFromEntry(pe);
            info.threadCount = static_cast<int>(pe.cntThreads);
            info.niceValue = pe.pcPriClassBase;
            info.state = "Running";

            HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ,
                                        FALSE, pe.th32ProcessID);
            if (hProc) {
                // Memory
                PROCESS_MEMORY_COUNTERS_EX pmc{};
                if (GetProcessMemoryInfo(hProc, reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc))) {
                    info.memRssBytes = pmc.WorkingSetSize;
                    info.memVirtBytes = pmc.PrivateUsage;
                }

                // Command line via full image path (full cmdline needs NtQueryInformationProcess,
                // omitted to avoid undocumented API usage; image path is still very useful)
                wchar_t pathBuf[MAX_PATH];
                DWORD pathLen = MAX_PATH;
                if (QueryFullProcessImageNameW(hProc, 0, pathBuf, &pathLen)) {
                    info.commandLine = QString::fromWCharArray(pathBuf, pathLen);
                }

                // CPU time
                FILETIME creationTime, exitTime, kernelTime, userTime;
                if (GetProcessTimes(hProc, &creationTime, &exitTime, &kernelTime, &userTime)) {
                    quint64 totalTicks = fileTimeToTicks(kernelTime) + fileTimeToTicks(userTime);
                    CpuTimeSample sample{totalTicks, nowMs};
                    newSamples[info.pid] = sample;

                    auto prevIt = m_prevSamples.constFind(info.pid);
                    if (prevIt != m_prevSamples.constEnd() && systemDelta > 0) {
                        quint64 procDelta = (totalTicks >= prevIt->totalTimeTicks)
                                                 ? (totalTicks - prevIt->totalTimeTicks) : 0;
                        info.cpuPercent = (double(procDelta) / double(systemDelta)) * 100.0 * nproc;
                    }
                }

                CloseHandle(hProc);
            }

            if (info.commandLine.isEmpty()) info.commandLine = info.name;

            result.push_back(info);
        } while (Process32NextW(snap, &pe));
    }

    CloseHandle(snap);

    m_prevSamples = newSamples;
    m_prevSystemTotalTicks = systemTotalNow;
    m_prevSampleMs = nowMs;

    return result;
}

QVector<ThreadInfo> ProcessCollector::collectThreads(qint64 pid) {
    QVector<ThreadInfo> threads;

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return threads;

    THREADENTRY32 te{};
    te.dwSize = sizeof(te);

    if (Thread32First(snap, &te)) {
        do {
            if (static_cast<qint64>(te.th32OwnerProcessID) != pid) continue;
            ThreadInfo t;
            t.tid = te.th32ThreadID;
            t.name = QString("Thread %1").arg(t.tid);
            t.priority = te.tpBasePri;
            t.state = "Running";
            t.cpuPercent = 0.0; // per-thread CPU delta omitted for brevity
            threads.push_back(t);
        } while (Thread32Next(snap, &te));
    }

    CloseHandle(snap);
    return threads;
}

bool ProcessCollector::killProcess(qint64 pid) {
    HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, static_cast<DWORD>(pid));
    if (!h) return false;
    bool ok = TerminateProcess(h, 1);
    CloseHandle(h);
    return ok;
}

bool ProcessCollector::setPriority(qint64 pid, int niceValue) {
    HANDLE h = OpenProcess(PROCESS_SET_INFORMATION, FALSE, static_cast<DWORD>(pid));
    if (!h) return false;
    // Map a -20..19 "nice"-like scale onto Windows priority classes
    DWORD priorityClass = NORMAL_PRIORITY_CLASS;
    if (niceValue <= -15) priorityClass = HIGH_PRIORITY_CLASS;
    else if (niceValue <= -5) priorityClass = ABOVE_NORMAL_PRIORITY_CLASS;
    else if (niceValue >= 15) priorityClass = IDLE_PRIORITY_CLASS;
    else if (niceValue >= 5) priorityClass = BELOW_NORMAL_PRIORITY_CLASS;
    bool ok = SetPriorityClass(h, priorityClass);
    CloseHandle(h);
    return ok;
}
