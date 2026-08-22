// Qt-free port of the Qt edition's ProcessBackendWin.cpp.

#include "ProcessCollector.h"

#include <windows.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <chrono>
#include <thread>

namespace {

int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

uint64_t fileTimeToTicks(const FILETIME& ft) {
    ULARGE_INTEGER uli;
    uli.LowPart = ft.dwLowDateTime;
    uli.HighPart = ft.dwHighDateTime;
    return uli.QuadPart;
}

std::string wideToUtf8(const wchar_t* w, int len = -1) {
    if (!w) return {};
    int size = WideCharToMultiByte(CP_UTF8, 0, w, len, nullptr, 0, nullptr, nullptr);
    if (size <= 0) return {};
    std::string out(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, len, out.data(), size, nullptr, nullptr);
    if (len < 0 && !out.empty() && out.back() == '\0') out.pop_back();
    return out;
}

uint64_t readSystemTotalTicks() {
    FILETIME idle, kernel, user;
    if (!GetSystemTimes(&idle, &kernel, &user)) return 0;
    return fileTimeToTicks(kernel) + fileTimeToTicks(user);
}

} // namespace

class ProcessCollector::Impl {};

ProcessCollector::ProcessCollector() : m_impl(new Impl()) {
    m_prevSystemTotalTicks = readSystemTotalTicks();
    m_prevSampleMs = nowMs();
}

ProcessCollector::~ProcessCollector() { delete m_impl; }

std::vector<ProcessInfo> ProcessCollector::collect() {
    std::vector<ProcessInfo> result;

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return result;

    uint64_t systemTotalNow = readSystemTotalTicks();
    uint64_t systemDelta = (systemTotalNow > m_prevSystemTotalTicks)
                                ? (systemTotalNow - m_prevSystemTotalTicks) : 1;
    int64_t now = nowMs();
    unsigned int nproc = std::thread::hardware_concurrency();
    if (nproc == 0) nproc = 1;

    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);

    std::map<int64_t, CpuTimeSample> newSamples;

    if (Process32FirstW(snap, &pe)) {
        do {
            ProcessInfo info;
            info.pid = pe.th32ProcessID;
            info.ppid = pe.th32ParentProcessID;
            info.name = wideToUtf8(pe.szExeFile);
            info.threadCount = static_cast<int>(pe.cntThreads);
            info.niceValue = pe.pcPriClassBase;
            info.state = "Running";

            HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ,
                                        FALSE, pe.th32ProcessID);
            if (hProc) {
                PROCESS_MEMORY_COUNTERS_EX pmc{};
                if (GetProcessMemoryInfo(hProc, reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc))) {
                    info.memRssBytes = pmc.WorkingSetSize;
                    info.memVirtBytes = pmc.PrivateUsage;
                }

                wchar_t pathBuf[MAX_PATH];
                DWORD pathLen = MAX_PATH;
                if (QueryFullProcessImageNameW(hProc, 0, pathBuf, &pathLen)) {
                    info.commandLine = wideToUtf8(pathBuf, static_cast<int>(pathLen));
                }

                FILETIME creationTime, exitTime, kernelTime, userTime;
                if (GetProcessTimes(hProc, &creationTime, &exitTime, &kernelTime, &userTime)) {
                    uint64_t totalTicks = fileTimeToTicks(kernelTime) + fileTimeToTicks(userTime);
                    newSamples[info.pid] = CpuTimeSample{totalTicks, now};

                    auto prevIt = m_prevSamples.find(info.pid);
                    if (prevIt != m_prevSamples.end() && systemDelta > 0) {
                        uint64_t procDelta = (totalTicks >= prevIt->second.totalTimeTicks)
                                                  ? (totalTicks - prevIt->second.totalTimeTicks) : 0;
                        info.cpuPercent = (double(procDelta) / double(systemDelta)) * 100.0 * nproc;
                    }
                }

                CloseHandle(hProc);
            }

            if (info.commandLine.empty()) info.commandLine = info.name;
            result.push_back(std::move(info));
        } while (Process32NextW(snap, &pe));
    }

    CloseHandle(snap);

    m_prevSamples = std::move(newSamples);
    m_prevSystemTotalTicks = systemTotalNow;
    m_prevSampleMs = now;

    return result;
}

std::vector<ThreadInfo> ProcessCollector::collectThreads(int64_t pid) {
    std::vector<ThreadInfo> threads;

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return threads;

    THREADENTRY32 te{};
    te.dwSize = sizeof(te);

    if (Thread32First(snap, &te)) {
        do {
            if (static_cast<int64_t>(te.th32OwnerProcessID) != pid) continue;
            ThreadInfo t;
            t.tid = te.th32ThreadID;
            t.name = "Thread " + std::to_string(t.tid);
            t.priority = te.tpBasePri;
            t.state = "Running";
            threads.push_back(std::move(t));
        } while (Thread32Next(snap, &te));
    }

    CloseHandle(snap);
    return threads;
}

bool ProcessCollector::killProcess(int64_t pid) {
    HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, static_cast<DWORD>(pid));
    if (!h) return false;
    bool ok = TerminateProcess(h, 1);
    CloseHandle(h);
    return ok;
}

bool ProcessCollector::setPriority(int64_t pid, int niceValue) {
    HANDLE h = OpenProcess(PROCESS_SET_INFORMATION, FALSE, static_cast<DWORD>(pid));
    if (!h) return false;
    DWORD priorityClass = NORMAL_PRIORITY_CLASS;
    if (niceValue <= -15) priorityClass = HIGH_PRIORITY_CLASS;
    else if (niceValue <= -5) priorityClass = ABOVE_NORMAL_PRIORITY_CLASS;
    else if (niceValue >= 15) priorityClass = IDLE_PRIORITY_CLASS;
    else if (niceValue >= 5) priorityClass = BELOW_NORMAL_PRIORITY_CLASS;
    bool ok = SetPriorityClass(h, priorityClass);
    CloseHandle(h);
    return ok;
}
