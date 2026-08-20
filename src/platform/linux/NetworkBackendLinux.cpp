// Linux implementation of NetworkStatsCollector.
//
// Bandwidth, packets, errors and drops per interface come from
// /proc/net/dev (always available, no extra deps). Link speed and
// duplex/carrier state are queried via SIOCETHTOOL / SIOCGIFFLAGS ioctls
// on a throwaway UDP socket -- both are part of the Linux kernel headers
// (linux/ethtool.h, sys/ioctl.h), not an external library, so this keeps
// us dependency-free while still giving "deep" info like ethtool would.

#include "NetworkStatsCollector.h"

#include <QFile>
#include <QTextStream>
#include <QStringList>
#include <QDateTime>
#include <QDir>

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

struct RawIfCounters {
    QString name;
    quint64 rxBytes = 0, rxPackets = 0, rxErrors = 0, rxDropped = 0;
    quint64 txBytes = 0, txPackets = 0, txErrors = 0, txDropped = 0;
};

QVector<RawIfCounters> readProcNetDev() {
    QVector<RawIfCounters> result;
    QFile f("/proc/net/dev");
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return result;

    QTextStream ts(&f);
    QString line;
    int lineNo = 0;
    while (ts.readLineInto(&line)) {
        ++lineNo;
        if (lineNo <= 2) continue; // header lines
        int colon = line.indexOf(':');
        if (colon < 0) continue;
        QString name = line.left(colon).trimmed();
        QStringList vals = line.mid(colon + 1).split(' ', Qt::SkipEmptyParts);
        if (vals.size() < 16) continue;

        RawIfCounters c;
        c.name = name;
        c.rxBytes = vals[0].toULongLong();
        c.rxPackets = vals[1].toULongLong();
        c.rxErrors = vals[2].toULongLong();
        c.rxDropped = vals[3].toULongLong();
        c.txBytes = vals[8].toULongLong();
        c.txPackets = vals[9].toULongLong();
        c.txErrors = vals[10].toULongLong();
        c.txDropped = vals[11].toULongLong();
        result.push_back(c);
    }
    return result;
}

// Queries link speed (Mbps) and up/carrier state via ioctl. Best-effort:
// returns 0 speed if the driver doesn't report it (e.g. some USB/virtual NICs).
struct LinkInfo {
    quint64 speedMbps = 0;
    bool up = false;
    QString mac;
    QString ipv4;
};

LinkInfo queryLinkInfo(const QString& ifName) {
    LinkInfo info;
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) return info;

    struct ifreq ifr{};
    std::strncpy(ifr.ifr_name, ifName.toLocal8Bit().constData(), IFNAMSIZ - 1);

    // Flags (up/down)
    if (ioctl(sock, SIOCGIFFLAGS, &ifr) == 0) {
        info.up = (ifr.ifr_flags & IFF_UP) && (ifr.ifr_flags & IFF_RUNNING);
    }

    // MAC address
    std::strncpy(ifr.ifr_name, ifName.toLocal8Bit().constData(), IFNAMSIZ - 1);
    if (ioctl(sock, SIOCGIFHWADDR, &ifr) == 0) {
        unsigned char* mac = reinterpret_cast<unsigned char*>(ifr.ifr_hwaddr.sa_data);
        char buf[18];
        std::snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
                      mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        info.mac = QString::fromLatin1(buf);
    }

    // IPv4 address
    std::strncpy(ifr.ifr_name, ifName.toLocal8Bit().constData(), IFNAMSIZ - 1);
    if (ioctl(sock, SIOCGIFADDR, &ifr) == 0) {
        struct sockaddr_in* addr = reinterpret_cast<struct sockaddr_in*>(&ifr.ifr_addr);
        info.ipv4 = QString::fromLatin1(inet_ntoa(addr->sin_addr));
    }

    // Link speed via ethtool
    struct ethtool_cmd ecmd{};
    ecmd.cmd = ETHTOOL_GSET;
    std::strncpy(ifr.ifr_name, ifName.toLocal8Bit().constData(), IFNAMSIZ - 1);
    ifr.ifr_data = reinterpret_cast<char*>(&ecmd);
    if (ioctl(sock, SIOCETHTOOL, &ifr) == 0) {
        quint32 speed = ecmd.speed | (static_cast<quint32>(ecmd.speed_hi) << 16);
        if (speed != 0xFFFF && speed != 0xFFFFFFFF) info.speedMbps = speed;
    }

    close(sock);
    return info;
}

} // namespace

class NetworkStatsCollector::Impl {
public:
    // link info doesn't change every poll; cache it and refresh occasionally
    QMap<QString, LinkInfo> linkCache;
    int pollCountSinceLinkRefresh = 1000; // force refresh on first call
};

NetworkStatsCollector::NetworkStatsCollector() : m_impl(new Impl()) {}
NetworkStatsCollector::~NetworkStatsCollector() { delete m_impl; }

NetworkStats NetworkStatsCollector::collect() {
    NetworkStats stats;
    QVector<RawIfCounters> raw = readProcNetDev();
    qint64 nowMs = QDateTime::currentMSecsSinceEpoch();

    bool refreshLink = (++m_impl->pollCountSinceLinkRefresh >= 5); // every ~5 polls
    if (refreshLink) m_impl->pollCountSinceLinkRefresh = 0;

    for (const RawIfCounters& c : raw) {
        if (c.name == "lo") continue; // skip loopback, not useful for "network load"

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

        if (refreshLink || !m_impl->linkCache.contains(c.name)) {
            m_impl->linkCache[c.name] = queryLinkInfo(c.name);
        }
        const LinkInfo& link = m_impl->linkCache[c.name];
        ifs.isUp = link.up;
        ifs.macAddress = link.mac;
        ifs.ipv4Address = link.ipv4;
        ifs.linkSpeedMbps = link.speedMbps;

        auto prevIt = m_prev.constFind(c.name);
        if (prevIt != m_prev.constEnd() && prevIt->valid) {
            double dtSec = (nowMs - prevIt->sampledAtMs) / 1000.0;
            if (dtSec <= 0) dtSec = 1.0;

            quint64 rxBytesDelta = c.rxBytes >= prevIt->rxBytes ? c.rxBytes - prevIt->rxBytes : 0;
            quint64 txBytesDelta = c.txBytes >= prevIt->txBytes ? c.txBytes - prevIt->txBytes : 0;
            quint64 rxPktDelta = c.rxPackets >= prevIt->rxPackets ? c.rxPackets - prevIt->rxPackets : 0;
            quint64 txPktDelta = c.txPackets >= prevIt->txPackets ? c.txPackets - prevIt->txPackets : 0;
            quint64 rxDropDelta = c.rxDropped >= prevIt->rxDropped ? c.rxDropped - prevIt->rxDropped : 0;
            quint64 txDropDelta = c.txDropped >= prevIt->txDropped ? c.txDropped - prevIt->txDropped : 0;
            quint64 rxErrDelta = c.rxErrors >= prevIt->rxErrors ? c.rxErrors - prevIt->rxErrors : 0;
            quint64 txErrDelta = c.txErrors >= prevIt->txErrors ? c.txErrors - prevIt->txErrors : 0;

            ifs.rxBytesPerSec = quint64(rxBytesDelta / dtSec);
            ifs.txBytesPerSec = quint64(txBytesDelta / dtSec);
            ifs.rxPacketsPerSec = quint64(rxPktDelta / dtSec);
            ifs.txPacketsPerSec = quint64(txPktDelta / dtSec);

            quint64 rxTotalPkts = rxPktDelta + rxDropDelta;
            quint64 txTotalPkts = txPktDelta + txDropDelta;
            ifs.rxDropPercent = rxTotalPkts ? (double(rxDropDelta) / double(rxTotalPkts)) * 100.0 : 0.0;
            ifs.txDropPercent = txTotalPkts ? (double(txDropDelta) / double(txTotalPkts)) * 100.0 : 0.0;
            ifs.rxErrorPercent = rxTotalPkts ? (double(rxErrDelta) / double(rxTotalPkts)) * 100.0 : 0.0;
            ifs.txErrorPercent = txTotalPkts ? (double(txErrDelta) / double(txTotalPkts)) * 100.0 : 0.0;

            if (ifs.linkSpeedMbps > 0) {
                double usedBps = double(ifs.rxBytesPerSec + ifs.txBytesPerSec) * 8.0;
                double capacityBps = double(ifs.linkSpeedMbps) * 1'000'000.0;
                ifs.utilizationPercent = qMin(100.0, (usedBps / capacityBps) * 100.0);
            }

            stats.totalRxBytesPerSec += ifs.rxBytesPerSec;
            stats.totalTxBytesPerSec += ifs.txBytesPerSec;
        }

        PrevCounters newPrev;
        newPrev.rxBytes = c.rxBytes; newPrev.txBytes = c.txBytes;
        newPrev.rxPackets = c.rxPackets; newPrev.txPackets = c.txPackets;
        newPrev.rxDropped = c.rxDropped; newPrev.txDropped = c.txDropped;
        newPrev.rxErrors = c.rxErrors; newPrev.txErrors = c.txErrors;
        newPrev.sampledAtMs = nowMs;
        newPrev.valid = true;
        m_prev[c.name] = newPrev;

        stats.interfaces.push_back(ifs);
    }

    return stats;
}
