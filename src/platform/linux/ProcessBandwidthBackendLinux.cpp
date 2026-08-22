// Linux implementation of ProcessBandwidthCollector.
//
// TCP: Netlink socket-diag (NETLINK_SOCK_DIAG) with the TCP_INFO extension
// -- the same mechanism `ss -i` uses, reading tcpi_bytes_acked/
// tcpi_bytes_received per socket. No elevated privileges needed for your
// own processes' sockets.
//
// UDP: the kernel does NOT expose a per-socket cumulative byte counter
// for UDP the way it does for TCP, so there is no Netlink-based
// equivalent. The only way to get real UDP byte counts per process is to
// capture packets and match them to locally-open UDP sockets by port --
// this uses a raw AF_PACKET socket (kernel API, no libpcap dependency),
// which REQUIRES root or the CAP_NET_RAW capability
// (`sudo setcap cap_net_raw+ep <binary>` is the recommended way to grant
// this to just the TaskManager binary, rather than running the whole app
// as root).
//
// Both mechanisms attribute traffic to a PID via the same technique:
// matching a socket's inode (from /proc/net/{tcp,udp}[6]) to a PID by
// scanning /proc/<pid>/fd/* symlinks for "socket:[<inode>]" targets.

#include "ProcessBandwidthCollector.h"

#include <QDir>
#include <QFile>
#include <QTextStream>
#include <QMap>
#include <QStringList>
#include <QDateTime>

#include <sys/socket.h>
#include <sys/types.h>
#include <arpa/inet.h>          // htons
#include <unistd.h>
#include <poll.h>
#include <vector>
#include <thread>
#include <mutex>
#include <atomic>
#include <cstring>
#include <cerrno>

#include <linux/netlink.h>
#include <linux/rtnetlink.h>   // RTA_* macros (generic netlink attribute format)
#include <linux/sock_diag.h>   // SOCK_DIAG_BY_FAMILY
#include <linux/inet_diag.h>   // inet_diag_req_v2 / inet_diag_msg / INET_DIAG_INFO
#include <linux/tcp.h>         // struct tcp_info (tcpi_bytes_acked/received)
#include <linux/if_ether.h>    // ETH_P_IP / ETH_P_IPV6
#include <linux/if_packet.h>   // AF_PACKET socket type support

namespace {

constexpr size_t kNetlinkRecvBufSize = 32 * 1024;
constexpr size_t kCaptureBufSize = 65536;
constexpr int kCapturePollTimeoutMs = 500; // how often the capture thread re-checks the stop flag

// ---------------------------------------------------------------------------
// Shared: socket inode -> owning PID, by scanning /proc/<pid>/fd/*
// ---------------------------------------------------------------------------
QMap<quint64, qint64> buildInodeToPidMap() {
    QMap<quint64, qint64> result;
    QDir procDir("/proc");
    const QStringList entries = procDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot);

    for (const QString& entry : entries) {
        bool isPid = false;
        qint64 pid = entry.toLongLong(&isPid);
        if (!isPid) continue;

        QDir fdDir(QString("/proc/%1/fd").arg(pid));
        const QStringList fds = fdDir.entryList(QDir::Files | QDir::System | QDir::NoDotAndDotDot);
        for (const QString& fd : fds) {
            QString target = QFile::symLinkTarget(fdDir.filePath(fd));
            if (target.isEmpty() || !target.startsWith("socket:[")) continue;
            QString inodeStr = target.mid(8, target.size() - 9);
            bool ok = false;
            quint64 inode = inodeStr.toULongLong(&ok);
            if (ok) result[inode] = pid;
        }
    }
    return result;
}

// ---------------------------------------------------------------------------
// TCP: Netlink socket-diag with TCP_INFO
// ---------------------------------------------------------------------------
struct SocketBandwidth {
    quint64 bytesAcked = 0;
    quint64 bytesReceived = 0;
};

bool queryTcpInfoByFamily(int family, QMap<quint64, SocketBandwidth>& out, QString& errorOut) {
    int fd = socket(AF_NETLINK, SOCK_DGRAM, NETLINK_SOCK_DIAG);
    if (fd < 0) {
        errorOut = QString("Failed to open NETLINK_SOCK_DIAG socket: %1").arg(strerror(errno));
        return false;
    }

    struct {
        struct nlmsghdr nlh;
        struct inet_diag_req_v2 req;
    } request{};

    request.nlh.nlmsg_len = NLMSG_LENGTH(sizeof(request.req));
    request.nlh.nlmsg_type = SOCK_DIAG_BY_FAMILY;
    request.nlh.nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
    request.nlh.nlmsg_seq = 1;
    request.nlh.nlmsg_pid = 0;

    request.req.sdiag_family = static_cast<__u8>(family);
    request.req.sdiag_protocol = IPPROTO_TCP;
    request.req.idiag_ext = (1 << (INET_DIAG_INFO - 1));
    request.req.idiag_states = 0xFFFFFFFFu;

    if (send(fd, &request, request.nlh.nlmsg_len, 0) < 0) {
        errorOut = QString("Failed to send Netlink request: %1").arg(strerror(errno));
        close(fd);
        return false;
    }

    std::vector<char> buf(kNetlinkRecvBufSize);
    bool done = false;
    while (!done) {
        ssize_t recvLen = recv(fd, buf.data(), buf.size(), 0);
        if (recvLen < 0) {
            errorOut = QString("Failed to receive Netlink response: %1").arg(strerror(errno));
            close(fd);
            return false;
        }
        if (recvLen == 0) break;

        int len = static_cast<int>(recvLen);
        auto* nlh = reinterpret_cast<struct nlmsghdr*>(buf.data());

        for (; NLMSG_OK(nlh, len); nlh = NLMSG_NEXT(nlh, len)) {
            if (nlh->nlmsg_type == NLMSG_DONE) { done = true; break; }
            if (nlh->nlmsg_type == NLMSG_ERROR) {
                auto* nlErr = reinterpret_cast<struct nlmsgerr*>(NLMSG_DATA(nlh));
                errorOut = QString("Netlink error: %1").arg(strerror(-nlErr->error));
                close(fd);
                return false;
            }

            auto* diag = reinterpret_cast<struct inet_diag_msg*>(NLMSG_DATA(nlh));
            int payloadLen = static_cast<int>(nlh->nlmsg_len) - static_cast<int>(NLMSG_LENGTH(sizeof(struct inet_diag_msg)));
            if (payloadLen <= 0) continue;

            auto* rta = reinterpret_cast<struct rtattr*>(
                reinterpret_cast<char*>(diag) + sizeof(struct inet_diag_msg));

            for (; RTA_OK(rta, payloadLen); rta = RTA_NEXT(rta, payloadLen)) {
                if (rta->rta_type != INET_DIAG_INFO) continue;
                if (RTA_PAYLOAD(rta) < sizeof(struct tcp_info)) continue;

                auto* info = reinterpret_cast<struct tcp_info*>(RTA_DATA(rta));
                SocketBandwidth sb;
                sb.bytesAcked = info->tcpi_bytes_acked;
                sb.bytesReceived = info->tcpi_bytes_received;
                out[diag->idiag_inode] = sb;
            }
        }
    }

    close(fd);
    return true;
}

// ---------------------------------------------------------------------------
// UDP: raw AF_PACKET capture + port-based attribution
// ---------------------------------------------------------------------------
struct UdpPortAccum {
    std::mutex mutex;
    QMap<quint16, quint64> bytesAsSourcePort; // packet's source port was this -- candidate TX
    QMap<quint16, quint64> bytesAsDestPort;   // packet's destination port was this -- candidate RX
};

// Runs on a dedicated thread per address family. Uses poll() with a
// timeout so it re-checks *stopFlag periodically instead of blocking
// forever on recv() -- this makes shutdown clean (join before close, no
// race on the fd) rather than relying on close() to unblock a pending
// recv() from another thread.
void udpCaptureLoop(int fd, bool isIPv6, UdpPortAccum* accum, std::atomic<bool>* stopFlag) {
    std::vector<unsigned char> buf(kCaptureBufSize);

    while (!stopFlag->load()) {
        struct pollfd pfd{};
        pfd.fd = fd;
        pfd.events = POLLIN;
        int pr = poll(&pfd, 1, kCapturePollTimeoutMs);
        if (pr <= 0) continue; // timeout or interrupted -- loop back and re-check stopFlag
        if (!(pfd.revents & POLLIN)) continue;

        ssize_t n = recv(fd, buf.data(), buf.size(), 0);
        if (n <= 0) {
            if (errno == EINTR) continue;
            break; // socket closed or hard error
        }

        const unsigned char* data = buf.data();
        size_t len = static_cast<size_t>(n);
        size_t udpOffset;

        if (!isIPv6) {
            // SOCK_DGRAM AF_PACKET delivers the packet starting at the IP
            // header (no Ethernet header).
            if (len < 20) continue;
            unsigned char ihl = data[0] & 0x0F;
            size_t ipHeaderLen = size_t(ihl) * 4;
            if (ipHeaderLen < 20 || len < ipHeaderLen + 8) continue;
            unsigned char proto = data[9];
            if (proto != 17) continue; // not UDP
            udpOffset = ipHeaderLen;
        } else {
            // Fixed 40-byte IPv6 header; extension headers are not walked
            // (a reasonable simplification -- the vast majority of UDP/
            // IPv6 traffic has no extension headers before the UDP header).
            if (len < 40 + 8) continue;
            unsigned char nextHeader = data[6];
            if (nextHeader != 17) continue; // not UDP
            udpOffset = 40;
        }

        quint16 srcPort = (quint16(data[udpOffset]) << 8) | data[udpOffset + 1];
        quint16 dstPort = (quint16(data[udpOffset + 2]) << 8) | data[udpOffset + 3];

        std::lock_guard<std::mutex> lock(accum->mutex);
        accum->bytesAsSourcePort[srcPort] += len;
        accum->bytesAsDestPort[dstPort] += len;
    }
}

// Builds local UDP port -> owning PID by parsing /proc/net/udp[6] and
// cross-referencing socket inodes against buildInodeToPidMap().
QMap<quint16, qint64> buildUdpPortToPidMap() {
    QMap<quint16, qint64> result;
    QMap<quint64, qint64> inodeToPid = buildInodeToPidMap();

    static const char* const kPaths[] = {"/proc/net/udp", "/proc/net/udp6"};
    for (const char* path : kPaths) {
        QFile f(QString::fromLatin1(path));
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) continue;

        QTextStream ts(&f);
        QString line;
        int lineNo = 0;
        while (ts.readLineInto(&line)) {
            if (++lineNo == 1) continue; // header
            QStringList parts = line.trimmed().split(' ', Qt::SkipEmptyParts);
            if (parts.size() < 10) continue;

            QStringList localParts = parts[1].split(':');
            if (localParts.size() != 2) continue;

            bool ok = false;
            quint16 port = static_cast<quint16>(localParts[1].toUInt(&ok, 16));
            if (!ok) continue;
            quint64 inode = parts[9].toULongLong();

            auto pidIt = inodeToPid.constFind(inode);
            if (pidIt != inodeToPid.constEnd()) {
                result[port] = pidIt.value();
            }
        }
    }
    return result;
}

} // namespace

class ProcessBandwidthCollector::Impl {
public:
    bool running = false;
    bool tcpAvailable = false;
    bool udpAvailable = false;
    QString error;
    qint64 prevSampleMs = 0;

    // --- TCP state ---
    QMap<quint64, SocketBandwidth> prevBySocket; // previous cumulative bytes per socket inode

    // --- UDP state ---
    int udpFd4 = -1;
    int udpFd6 = -1;
    std::atomic<bool> udpStopFlag{false};
    std::thread udpThread4;
    std::thread udpThread6;
    UdpPortAccum udpAccum;

    // Running totals per PID, shared across TCP+UDP -- only ever
    // incremented by newly-computed deltas each poll, never overwritten.
    QMap<qint64, ProcessBandwidthStats> cumulativeByPid;

    ~Impl() {
        stopUdpCapture();
    }

    bool startUdpCapture(QString& errorOut) {
        udpFd4 = socket(AF_PACKET, SOCK_DGRAM, htons(ETH_P_IP));
        udpFd6 = socket(AF_PACKET, SOCK_DGRAM, htons(ETH_P_IPV6));

        if (udpFd4 < 0 && udpFd6 < 0) {
            errorOut = (errno == EPERM || errno == EACCES)
                ? "Root or CAP_NET_RAW required for UDP bandwidth tracking. "
                  "Try: sudo setcap cap_net_raw+ep <path-to-TaskManager>, or run with sudo."
                : QString("Failed to open raw capture socket: %1").arg(strerror(errno));
            if (udpFd4 >= 0) { close(udpFd4); udpFd4 = -1; }
            if (udpFd6 >= 0) { close(udpFd6); udpFd6 = -1; }
            return false;
        }

        udpStopFlag.store(false);
        if (udpFd4 >= 0) udpThread4 = std::thread(udpCaptureLoop, udpFd4, false, &udpAccum, &udpStopFlag);
        if (udpFd6 >= 0) udpThread6 = std::thread(udpCaptureLoop, udpFd6, true, &udpAccum, &udpStopFlag);
        return true;
    }

    void stopUdpCapture() {
        udpStopFlag.store(true);
        if (udpThread4.joinable()) udpThread4.join();
        if (udpThread6.joinable()) udpThread6.join();
        if (udpFd4 >= 0) { close(udpFd4); udpFd4 = -1; }
        if (udpFd6 >= 0) { close(udpFd6); udpFd6 = -1; }
    }
};

ProcessBandwidthCollector::ProcessBandwidthCollector() : m_impl(new Impl()) {}
ProcessBandwidthCollector::~ProcessBandwidthCollector() { delete m_impl; }

bool ProcessBandwidthCollector::start() {
    // --- TCP probe ---
    QMap<quint64, SocketBandwidth> probe;
    QString tcpErr4, tcpErr6;
    bool tcpOk4 = queryTcpInfoByFamily(AF_INET, probe, tcpErr4);
    bool tcpOk6 = queryTcpInfoByFamily(AF_INET6, probe, tcpErr6);
    bool tcpAvailable = tcpOk4 || tcpOk6;
    QString tcpErr = !tcpErr4.isEmpty() ? tcpErr4 : tcpErr6;

    // --- UDP capture start ---
    QString udpErr;
    bool udpAvailable = m_impl->startUdpCapture(udpErr);

    if (!tcpAvailable && !udpAvailable) {
        m_impl->error = !tcpErr.isEmpty() ? tcpErr : udpErr;
        m_impl->running = false;
        return false;
    }

    QStringList notes;
    if (!tcpAvailable) notes << QString("TCP tracking unavailable: %1").arg(tcpErr);
    if (!udpAvailable) notes << QString("UDP tracking unavailable: %1").arg(udpErr);
    m_impl->error = notes.join("  |  ");

    m_impl->tcpAvailable = tcpAvailable;
    m_impl->udpAvailable = udpAvailable;
    m_impl->running = true;
    m_impl->prevBySocket.clear();
    m_impl->cumulativeByPid.clear();
    m_impl->prevSampleMs = QDateTime::currentMSecsSinceEpoch();
    return true;
}

void ProcessBandwidthCollector::stop() {
    m_impl->running = false;
    m_impl->stopUdpCapture();
}

bool ProcessBandwidthCollector::isRunning() const {
    return m_impl->running;
}

QString ProcessBandwidthCollector::lastError() const {
    return m_impl->error;
}

QMap<qint64, ProcessBandwidthStats> ProcessBandwidthCollector::collect() {
    QMap<qint64, ProcessBandwidthStats> result;
    if (!m_impl->running) return result;

    qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    double dtSec = m_impl->prevSampleMs ? (nowMs - m_impl->prevSampleMs) / 1000.0 : 1.0;
    if (dtSec <= 0) dtSec = 1.0;

    QMap<qint64, ProcessBandwidthStats> rateThisPoll;

    // --- TCP (Netlink) ---
    if (m_impl->tcpAvailable) {
        QMap<quint64, SocketBandwidth> current;
        QString err;
        bool ok4 = queryTcpInfoByFamily(AF_INET, current, err);
        bool ok6 = queryTcpInfoByFamily(AF_INET6, current, err);

        if (ok4 || ok6) {
            QMap<quint64, qint64> inodeToPid = buildInodeToPidMap();

            for (auto it = current.constBegin(); it != current.constEnd(); ++it) {
                quint64 inode = it.key();
                const SocketBandwidth& sb = it.value();

                auto pidIt = inodeToPid.constFind(inode);
                if (pidIt == inodeToPid.constEnd()) continue;
                qint64 pid = pidIt.value();

                quint64 prevAcked = 0, prevReceived = 0;
                auto prevIt = m_impl->prevBySocket.constFind(inode);
                if (prevIt != m_impl->prevBySocket.constEnd()) {
                    prevAcked = prevIt->bytesAcked;
                    prevReceived = prevIt->bytesReceived;
                }

                quint64 txDelta = sb.bytesAcked >= prevAcked ? sb.bytesAcked - prevAcked : 0;
                quint64 rxDelta = sb.bytesReceived >= prevReceived ? sb.bytesReceived - prevReceived : 0;

                rateThisPoll[pid].txBytesPerSec += quint64(txDelta / dtSec);
                rateThisPoll[pid].rxBytesPerSec += quint64(rxDelta / dtSec);

                ProcessBandwidthStats& cum = m_impl->cumulativeByPid[pid];
                cum.txBytesTotal += txDelta;
                cum.rxBytesTotal += rxDelta;
            }

            m_impl->prevBySocket = current;
        }
    }

    // --- UDP (raw capture, drained + attributed by local port) ---
    if (m_impl->udpAvailable) {
        QMap<quint16, quint64> asSrc, asDst;
        {
            std::lock_guard<std::mutex> lock(m_impl->udpAccum.mutex);
            asSrc = m_impl->udpAccum.bytesAsSourcePort;
            asDst = m_impl->udpAccum.bytesAsDestPort;
            m_impl->udpAccum.bytesAsSourcePort.clear();
            m_impl->udpAccum.bytesAsDestPort.clear();
        }

        QMap<quint16, qint64> portToPid = buildUdpPortToPidMap();

        for (auto it = portToPid.constBegin(); it != portToPid.constEnd(); ++it) {
            quint16 port = it.key();
            qint64 pid = it.value();

            quint64 rxBytes = asDst.value(port, 0);
            quint64 txBytes = asSrc.value(port, 0);
            if (rxBytes == 0 && txBytes == 0) continue;

            rateThisPoll[pid].rxBytesPerSec += quint64(rxBytes / dtSec);
            rateThisPoll[pid].txBytesPerSec += quint64(txBytes / dtSec);

            ProcessBandwidthStats& cum = m_impl->cumulativeByPid[pid];
            cum.rxBytesTotal += rxBytes;
            cum.txBytesTotal += txBytes;
        }
    }

    m_impl->prevSampleMs = nowMs;

    for (auto it = m_impl->cumulativeByPid.constBegin(); it != m_impl->cumulativeByPid.constEnd(); ++it) {
        ProcessBandwidthStats s = it.value();
        auto rateIt = rateThisPoll.constFind(it.key());
        if (rateIt != rateThisPoll.constEnd()) {
            s.rxBytesPerSec = rateIt->rxBytesPerSec;
            s.txBytesPerSec = rateIt->txBytesPerSec;
        } else {
            s.rxBytesPerSec = 0;
            s.txBytesPerSec = 0;
        }
        result[it.key()] = s;
    }

    return result;
}
