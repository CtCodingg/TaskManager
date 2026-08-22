// Process enumeration and control via /proc.

#include "ProcessCollector.h"

#include <dirent.h>
#include <sys/stat.h>
#include <pwd.h>
#include <unistd.h>
#include <signal.h>
#include <sys/resource.h>
#include <fstream>
#include <sstream>
#include <chrono>
#include <thread>
#include <cstring>

namespace {

int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

long clockTicksPerSec() {
    static long hz = sysconf(_SC_CLK_TCK);
    return hz > 0 ? hz : 100;
}

struct ProcStatFields {
    std::string comm;
    char state = '?';
    int64_t ppid = 0;
    uint64_t utime = 0, stime = 0;
    int64_t numThreads = 0;
    int64_t priority = 0, nice = 0;
    bool ok = false;
};

std::vector<std::string> splitWs(const std::string& s) {
    std::vector<std::string> out;
    std::istringstream iss(s);
    std::string tok;
    while (iss >> tok) out.push_back(tok);
    return out;
}

ProcStatFields parseStat(const std::string& path) {
    ProcStatFields f;
    std::ifstream file(path, std::ios::binary);
    if (!file) return f;
    std::string data((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

    size_t open = data.find('(');
    size_t close = data.rfind(')');
    if (open == std::string::npos || close == std::string::npos || close < open) return f;

    f.comm = data.substr(open + 1, close - open - 1);
    std::string rest = data.substr(close + 2);
    std::vector<std::string> parts = splitWs(rest);
    if (parts.size() < 22) return f;

    f.state = parts[0].empty() ? '?' : parts[0][0];
    f.ppid = std::stoll(parts[1]);
    f.utime = std::stoull(parts[11]);
    f.stime = std::stoull(parts[12]);
    f.priority = std::stoll(parts[15]);
    f.nice = std::stoll(parts[16]);
    f.numThreads = std::stoll(parts[17]);
    f.ok = true;
    return f;
}

uint64_t readVmRssBytes(const std::string& statusPath, uint64_t* vmSizeOut) {
    std::ifstream f(statusPath);
    uint64_t rss = 0, vsize = 0;
    std::string line;
    while (std::getline(f, line)) {
        if (line.rfind("VmRSS:", 0) == 0) {
            auto parts = splitWs(line.substr(6));
            if (!parts.empty()) rss = std::stoull(parts[0]) * 1024;
        } else if (line.rfind("VmSize:", 0) == 0) {
            auto parts = splitWs(line.substr(7));
            if (!parts.empty()) vsize = std::stoull(parts[0]) * 1024;
        }
    }
    if (vmSizeOut) *vmSizeOut = vsize;
    return rss;
}

std::string userNameForUid(uid_t uid) {
    struct passwd* pw = getpwuid(uid);
    return pw ? std::string(pw->pw_name) : std::to_string(uid);
}

std::string readCmdline(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    std::string data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    for (char& c : data) if (c == '\0') c = ' ';
    // trim
    size_t start = data.find_first_not_of(" \t\n");
    size_t end = data.find_last_not_of(" \t\n");
    if (start == std::string::npos) return {};
    return data.substr(start, end - start + 1);
}

std::string stateToText(char c) {
    switch (c) {
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

uint64_t readSystemTotalTicks() {
    std::ifstream f("/proc/stat");
    std::string line;
    std::getline(f, line);
    auto parts = splitWs(line);
    uint64_t total = 0;
    for (size_t i = 1; i < parts.size(); ++i) total += std::stoull(parts[i]);
    return total;
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

    uint64_t systemTotalNow = readSystemTotalTicks();
    uint64_t systemDelta = (systemTotalNow > m_prevSystemTotalTicks)
                                ? (systemTotalNow - m_prevSystemTotalTicks) : 1;
    int64_t now = nowMs();

    long nproc = static_cast<long>(std::thread::hardware_concurrency());
    if (nproc <= 0) nproc = 1;

    DIR* procDir = opendir("/proc");
    if (!procDir) return result;

    std::map<int64_t, CpuTimeSample> newSamples;

    struct dirent* entry;
    while ((entry = readdir(procDir)) != nullptr) {
        std::string name = entry->d_name;
        bool isPid = !name.empty();
        for (char c : name) if (!isdigit(static_cast<unsigned char>(c))) { isPid = false; break; }
        if (!isPid) continue;
        int64_t pid = std::stoll(name);

        std::string base = "/proc/" + name;
        ProcStatFields stat = parseStat(base + "/stat");
        if (!stat.ok) continue;

        ProcessInfo info;
        info.pid = pid;
        info.ppid = stat.ppid;
        info.name = stat.comm;
        info.state = stateToText(stat.state);
        info.threadCount = static_cast<int>(stat.numThreads);
        info.niceValue = static_cast<int>(stat.nice);
        info.commandLine = readCmdline(base + "/cmdline");
        if (info.commandLine.empty()) info.commandLine = "[" + stat.comm + "]";

        struct stat st{};
        if (::stat(base.c_str(), &st) == 0) {
            info.user = userNameForUid(st.st_uid);
        }

        uint64_t vsize = 0;
        info.memRssBytes = readVmRssBytes(base + "/status", &vsize);
        info.memVirtBytes = vsize;

        uint64_t totalTicks = stat.utime + stat.stime;
        newSamples[pid] = CpuTimeSample{totalTicks, now};

        auto prevIt = m_prevSamples.find(pid);
        if (prevIt != m_prevSamples.end() && systemDelta > 0) {
            uint64_t procDelta = (totalTicks >= prevIt->second.totalTimeTicks)
                                      ? (totalTicks - prevIt->second.totalTimeTicks) : 0;
            info.cpuPercent = (double(procDelta) / double(systemDelta)) * 100.0 * nproc;
        }

        result.push_back(std::move(info));
    }
    closedir(procDir);

    m_prevSamples = std::move(newSamples);
    m_prevSystemTotalTicks = systemTotalNow;
    m_prevSampleMs = now;

    return result;
}

std::vector<ThreadInfo> ProcessCollector::collectThreads(int64_t pid) {
    std::vector<ThreadInfo> threads;
    std::string taskDir = "/proc/" + std::to_string(pid) + "/task";
    DIR* dir = opendir(taskDir.c_str());
    if (!dir) return threads;

    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        std::string tidStr = entry->d_name;
        bool isTid = !tidStr.empty();
        for (char c : tidStr) if (!isdigit(static_cast<unsigned char>(c))) { isTid = false; break; }
        if (!isTid) continue;

        ProcStatFields stat = parseStat(taskDir + "/" + tidStr + "/stat");
        if (!stat.ok) continue;

        ThreadInfo t;
        t.tid = std::stoll(tidStr);
        t.name = stat.comm;
        t.state = stateToText(stat.state);
        t.priority = static_cast<int>(stat.priority);
        t.cpuPercent = 0.0; // per-thread deltas not tracked
        threads.push_back(std::move(t));
    }
    closedir(dir);
    return threads;
}

bool ProcessCollector::killProcess(int64_t pid) {
    return ::kill(static_cast<pid_t>(pid), SIGTERM) == 0;
}

bool ProcessCollector::setPriority(int64_t pid, int niceValue) {
    return ::setpriority(PRIO_PROCESS, static_cast<id_t>(pid), niceValue) == 0;
}
