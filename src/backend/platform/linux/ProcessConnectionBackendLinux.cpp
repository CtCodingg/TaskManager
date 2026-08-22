// Per-process network connections via /proc/net/{tcp,udp}[6] + fd inode matching.

#include "ProcessConnectionCollector.h"

#include <dirent.h>
#include <fstream>
#include <sstream>
#include <map>
#include <unistd.h>

namespace {

std::vector<std::string> splitWs(const std::string& s) {
    std::vector<std::string> out;
    std::istringstream iss(s);
    std::string tok;
    while (iss >> tok) out.push_back(tok);
    return out;
}

std::string tcpStateName(int stateHex) {
    switch (stateHex) {
        case 0x01: return "ESTABLISHED";
        case 0x02: return "SYN_SENT";
        case 0x03: return "SYN_RECV";
        case 0x04: return "FIN_WAIT1";
        case 0x05: return "FIN_WAIT2";
        case 0x06: return "TIME_WAIT";
        case 0x07: return "CLOSE";
        case 0x08: return "CLOSE_WAIT";
        case 0x09: return "LAST_ACK";
        case 0x0A: return "LISTEN";
        case 0x0B: return "CLOSING";
        default:   return "UNKNOWN";
    }
}

std::string parseIPv4(const std::string& hex) {
    if (hex.size() != 8) return {};
    uint8_t b[4];
    for (int i = 0; i < 4; ++i) b[i] = static_cast<uint8_t>(std::stoul(hex.substr(i * 2, 2), nullptr, 16));
    return std::to_string(b[3]) + "." + std::to_string(b[2]) + "." + std::to_string(b[1]) + "." + std::to_string(b[0]);
}

std::string parseIPv6(const std::string& hex) {
    if (hex.size() != 32) return {};
    uint16_t groups[8];
    for (int word = 0; word < 4; ++word) {
        std::string wordHex = hex.substr(word * 8, 8);
        uint8_t b[4];
        for (int i = 0; i < 4; ++i) b[i] = static_cast<uint8_t>(std::stoul(wordHex.substr(i * 2, 2), nullptr, 16));
        groups[word * 2] = (uint16_t(b[3]) << 8) | b[2];
        groups[word * 2 + 1] = (uint16_t(b[1]) << 8) | b[0];
    }

    int bestStart = -1, bestLen = 0, curStart = -1, curLen = 0;
    for (int i = 0; i < 8; ++i) {
        if (groups[i] == 0) {
            if (curStart < 0) curStart = i;
            ++curLen;
            if (curLen > bestLen) { bestLen = curLen; bestStart = curStart; }
        } else { curStart = -1; curLen = 0; }
    }

    char buf[8];
    std::ostringstream oss;
    if (bestLen >= 2) {
        for (int i = 0; i < bestStart; ++i) { std::snprintf(buf, sizeof(buf), "%x", groups[i]); oss << buf << (i < bestStart - 1 ? ":" : ""); }
        oss << "::";
        for (int i = bestStart + bestLen; i < 8; ++i) { std::snprintf(buf, sizeof(buf), "%x", groups[i]); oss << buf << (i < 7 ? ":" : ""); }
        return oss.str();
    }
    for (int i = 0; i < 8; ++i) { std::snprintf(buf, sizeof(buf), "%x", groups[i]); oss << buf << (i < 7 ? ":" : ""); }
    return oss.str();
}

struct RawConn {
    std::string localAddr;
    uint16_t localPort = 0;
    std::string remoteAddr;
    uint16_t remotePort = 0;
    int stateHex = 0;
    uint64_t inode = 0;
};

std::vector<RawConn> parseProcNetFile(const std::string& path, bool isIPv6) {
    std::vector<RawConn> result;
    std::ifstream f(path);
    if (!f) return result;

    std::string line;
    int lineNo = 0;
    while (std::getline(f, line)) {
        if (++lineNo == 1) continue;
        auto parts = splitWs(line);
        if (parts.size() < 10) continue;

        size_t colon1 = parts[1].find(':');
        size_t colon2 = parts[2].find(':');
        if (colon1 == std::string::npos || colon2 == std::string::npos) continue;

        RawConn conn;
        conn.localAddr = isIPv6 ? parseIPv6(parts[1].substr(0, colon1)) : parseIPv4(parts[1].substr(0, colon1));
        conn.localPort = static_cast<uint16_t>(std::stoul(parts[1].substr(colon1 + 1), nullptr, 16));
        conn.remoteAddr = isIPv6 ? parseIPv6(parts[2].substr(0, colon2)) : parseIPv4(parts[2].substr(0, colon2));
        conn.remotePort = static_cast<uint16_t>(std::stoul(parts[2].substr(colon2 + 1), nullptr, 16));
        conn.stateHex = std::stoi(parts[3], nullptr, 16);
        conn.inode = std::stoull(parts[9]);

        result.push_back(conn);
    }
    return result;
}

std::map<uint64_t, int64_t> buildInodeToPidMap() {
    std::map<uint64_t, int64_t> result;
    DIR* procDir = opendir("/proc");
    if (!procDir) return result;

    struct dirent* entry;
    while ((entry = readdir(procDir)) != nullptr) {
        std::string name = entry->d_name;
        bool isPid = !name.empty();
        for (char c : name) if (!isdigit(static_cast<unsigned char>(c))) { isPid = false; break; }
        if (!isPid) continue;
        int64_t pid = std::stoll(name);

        std::string fdDirPath = "/proc/" + name + "/fd";
        DIR* fdDir = opendir(fdDirPath.c_str());
        if (!fdDir) continue;

        struct dirent* fdEntry;
        while ((fdEntry = readdir(fdDir)) != nullptr) {
            std::string fdName = fdEntry->d_name;
            if (fdName == "." || fdName == "..") continue;
            std::string linkPath = fdDirPath + "/" + fdName;
            char buf[64];
            ssize_t len = readlink(linkPath.c_str(), buf, sizeof(buf) - 1);
            if (len <= 0) continue;
            buf[len] = '\0';
            std::string target = buf;
            if (target.rfind("socket:[", 0) != 0) continue;
            std::string inodeStr = target.substr(8, target.size() - 9);
            try {
                uint64_t inode = std::stoull(inodeStr);
                result[inode] = pid;
            } catch (...) {}
        }
        closedir(fdDir);
    }
    closedir(procDir);
    return result;
}

} // namespace

class ProcessConnectionCollector::Impl {};

ProcessConnectionCollector::ProcessConnectionCollector() : m_impl(new Impl()) {}
ProcessConnectionCollector::~ProcessConnectionCollector() { delete m_impl; }

std::vector<ProcessConnection> ProcessConnectionCollector::collect() {
    std::vector<ProcessConnection> result;

    std::map<uint64_t, int64_t> inodeToPid = buildInodeToPidMap();

    struct Source { const char* path; const char* protocol; bool isIPv6; };
    static const Source sources[] = {
        {"/proc/net/tcp",  "TCP", false},
        {"/proc/net/tcp6", "TCP", true},
        {"/proc/net/udp",  "UDP", false},
        {"/proc/net/udp6", "UDP", true},
    };

    for (const auto& src : sources) {
        auto raw = parseProcNetFile(src.path, src.isIPv6);
        for (const auto& c : raw) {
            auto pidIt = inodeToPid.find(c.inode);
            if (pidIt == inodeToPid.end()) continue;

            ProcessConnection pc;
            pc.pid = pidIt->second;
            pc.protocol = src.protocol;
            pc.localAddress = c.localAddr;
            pc.localPort = c.localPort;
            pc.remoteAddress = c.remoteAddr;
            pc.remotePort = c.remotePort;
            pc.isIPv6 = src.isIPv6;
            pc.state = (pc.protocol == "TCP") ? tcpStateName(c.stateHex) : "-";

            result.push_back(pc);
        }
    }

    return result;
}
