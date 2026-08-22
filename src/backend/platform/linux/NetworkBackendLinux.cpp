// Per-interface network statistics via /proc/net/dev and ethtool ioctls.

#include "NetworkStatsCollector.h"

#include <fstream>
#include <sstream>
#include <map>
#include <chrono>
#include <algorithm>

#include <sys/socket.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <linux/ethtool.h>
#include <linux/sockios.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>

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

struct RawIfCounters {
    std::string name;
    uint64_t rxBytes = 0, rxPackets = 0, rxErrors = 0, rxDropped = 0;
    uint64_t txBytes = 0, txPackets = 0, txErrors = 0, txDropped = 0;
};

std::vector<RawIfCounters> readProcNetDev() {
    std::vector<RawIfCounters> result;
    std::ifstream f("/proc/net/dev");
    if (!f) return result;

    std::string line;
    int lineNo = 0;
    while (std::getline(f, line)) {
        if (++lineNo <= 2) continue;
        size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string name = line.substr(0, colon);
        size_t a = name.find_first_not_of(" \t");
        size_t b = name.find_last_not_of(" \t");
        if (a != std::string::npos) name = name.substr(a, b - a + 1);

        auto vals = splitWs(line.substr(colon + 1));
        if (vals.size() < 16) continue;

        RawIfCounters c;
        c.name = name;
        c.rxBytes = std::stoull(vals[0]);
        c.rxPackets = std::stoull(vals[1]);
        c.rxErrors = std::stoull(vals[2]);
        c.rxDropped = std::stoull(vals[3]);
        c.txBytes = std::stoull(vals[8]);
        c.txPackets = std::stoull(vals[9]);
        c.txErrors = std::stoull(vals[10]);
        c.txDropped = std::stoull(vals[11]);
        result.push_back(c);
    }
    return result;
}

struct LinkInfo {
    uint64_t speedMbps = 0;
    bool up = false;
    std::string mac;
    std::string ipv4;
};

LinkInfo queryLinkInfo(const std::string& ifName) {
    LinkInfo info;
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) return info;

    struct ifreq ifr{};
    std::strncpy(ifr.ifr_name, ifName.c_str(), IFNAMSIZ - 1);

    if (ioctl(sock, SIOCGIFFLAGS, &ifr) == 0) {
        info.up = (ifr.ifr_flags & IFF_UP) && (ifr.ifr_flags & IFF_RUNNING);
    }

    std::strncpy(ifr.ifr_name, ifName.c_str(), IFNAMSIZ - 1);
    if (ioctl(sock, SIOCGIFHWADDR, &ifr) == 0) {
        unsigned char* mac = reinterpret_cast<unsigned char*>(ifr.ifr_hwaddr.sa_data);
        char buf[18];
        std::snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
                      mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        info.mac = buf;
    }

    std::strncpy(ifr.ifr_name, ifName.c_str(), IFNAMSIZ - 1);
    if (ioctl(sock, SIOCGIFADDR, &ifr) == 0) {
        struct sockaddr_in* addr = reinterpret_cast<struct sockaddr_in*>(&ifr.ifr_addr);
        info.ipv4 = inet_ntoa(addr->sin_addr);
    }

    struct ethtool_cmd ecmd{};
    ecmd.cmd = ETHTOOL_GSET;
    std::strncpy(ifr.ifr_name, ifName.c_str(), IFNAMSIZ - 1);
    ifr.ifr_data = reinterpret_cast<char*>(&ecmd);
    if (ioctl(sock, SIOCETHTOOL, &ifr) == 0) {
        uint32_t speed = ecmd.speed | (static_cast<uint32_t>(ecmd.speed_hi) << 16);
        if (speed != 0xFFFF && speed != 0xFFFFFFFF) info.speedMbps = speed;
    }

    close(sock);
    return info;
}

} // namespace

class NetworkStatsCollector::Impl {
public:
    std::map<std::string, LinkInfo> linkCache;
    int pollCountSinceLinkRefresh = 1000;
};

NetworkStatsCollector::NetworkStatsCollector() : m_impl(new Impl()) {}
NetworkStatsCollector::~NetworkStatsCollector() { delete m_impl; }

NetworkStats NetworkStatsCollector::collect() {
    NetworkStats stats;
    std::vector<RawIfCounters> raw = readProcNetDev();
    int64_t now = nowMs();

    bool refreshLink = (++m_impl->pollCountSinceLinkRefresh >= 5);
    if (refreshLink) m_impl->pollCountSinceLinkRefresh = 0;

    for (const auto& c : raw) {
        if (c.name == "lo") continue;

        NetworkInterfaceStats ifs;
        ifs.name = c.name;
        ifs.rxBytesTotal = c.rxBytes;
        ifs.txBytesTotal = c.txBytes;
        ifs.rxPacketsTotal = c.rxPackets;
        ifs.txPacketsTotal = c.txPackets;
        ifs.rxErrorsTotal = c.rxErrors;
        ifs.txErrorsTotal = c.txErrors;
        ifs.rxDroppedTotal = c.rxDropped;
        ifs.txDroppedTotal = c.txDropped;

        if (refreshLink || !m_impl->linkCache.count(c.name)) {
            m_impl->linkCache[c.name] = queryLinkInfo(c.name);
        }
        const LinkInfo& link = m_impl->linkCache[c.name];
        ifs.isUp = link.up;
        ifs.macAddress = link.mac;
        ifs.ipv4Address = link.ipv4;
        ifs.linkSpeedMbps = link.speedMbps;

        auto prevIt = m_prev.find(c.name);
        if (prevIt != m_prev.end() && prevIt->second.valid) {
            double dtSec = (now - prevIt->second.sampledAtMs) / 1000.0;
            if (dtSec <= 0) dtSec = 1.0;

            const auto& prev = prevIt->second;
            uint64_t rxBytesDelta = c.rxBytes >= prev.rxBytes ? c.rxBytes - prev.rxBytes : 0;
            uint64_t txBytesDelta = c.txBytes >= prev.txBytes ? c.txBytes - prev.txBytes : 0;
            uint64_t rxPktDelta = c.rxPackets >= prev.rxPackets ? c.rxPackets - prev.rxPackets : 0;
            uint64_t txPktDelta = c.txPackets >= prev.txPackets ? c.txPackets - prev.txPackets : 0;
            uint64_t rxDropDelta = c.rxDropped >= prev.rxDropped ? c.rxDropped - prev.rxDropped : 0;
            uint64_t txDropDelta = c.txDropped >= prev.txDropped ? c.txDropped - prev.txDropped : 0;
            uint64_t rxErrDelta = c.rxErrors >= prev.rxErrors ? c.rxErrors - prev.rxErrors : 0;
            uint64_t txErrDelta = c.txErrors >= prev.txErrors ? c.txErrors - prev.txErrors : 0;

            ifs.rxBytesPerSec = uint64_t(rxBytesDelta / dtSec);
            ifs.txBytesPerSec = uint64_t(txBytesDelta / dtSec);
            ifs.rxPacketsPerSec = uint64_t(rxPktDelta / dtSec);
            ifs.txPacketsPerSec = uint64_t(txPktDelta / dtSec);

            uint64_t rxTotalPkts = rxPktDelta + rxDropDelta;
            uint64_t txTotalPkts = txPktDelta + txDropDelta;
            ifs.rxDropPercent = rxTotalPkts ? (double(rxDropDelta) / double(rxTotalPkts)) * 100.0 : 0.0;
            ifs.txDropPercent = txTotalPkts ? (double(txDropDelta) / double(txTotalPkts)) * 100.0 : 0.0;
            ifs.rxErrorPercent = rxTotalPkts ? (double(rxErrDelta) / double(rxTotalPkts)) * 100.0 : 0.0;
            ifs.txErrorPercent = txTotalPkts ? (double(txErrDelta) / double(txTotalPkts)) * 100.0 : 0.0;

            if (ifs.linkSpeedMbps > 0) {
                double usedBps = double(ifs.rxBytesPerSec + ifs.txBytesPerSec) * 8.0;
                double capacityBps = double(ifs.linkSpeedMbps) * 1'000'000.0;
                ifs.utilizationPercent = std::min(100.0, (usedBps / capacityBps) * 100.0);
            }

            stats.totalRxBytesPerSec += ifs.rxBytesPerSec;
            stats.totalTxBytesPerSec += ifs.txBytesPerSec;
        }

        PrevCounters newPrev;
        newPrev.rxBytes = c.rxBytes; newPrev.txBytes = c.txBytes;
        newPrev.rxPackets = c.rxPackets; newPrev.txPackets = c.txPackets;
        newPrev.rxDropped = c.rxDropped; newPrev.txDropped = c.txDropped;
        newPrev.rxErrors = c.rxErrors; newPrev.txErrors = c.txErrors;
        newPrev.sampledAtMs = now;
        newPrev.valid = true;
        m_prev[c.name] = newPrev;

        stats.interfaces.push_back(ifs);
    }

    return stats;
}
