// Per-process network bandwidth, broken down by interface.
//
// TCP: Netlink socket-diag (NETLINK_SOCK_DIAG) with the TCP_INFO
// extension -- the same mechanism `ss -i` uses, reading
// tcpi_bytes_acked/tcpi_bytes_received per socket. No elevated privileges
// needed for your own processes' sockets. Interface attribution comes
// from mapping each connection's local IP address (reported directly by
// the kernel in the diag response) to an interface via getifaddrs() --
// valid for TCP because the kernel resolves a concrete source IP for an
// established connection even if the application originally bound to
// the wildcard address (0.0.0.0).
//
// UDP: no per-socket cumulative byte counter exists in the kernel for
// UDP, so this uses a raw AF_PACKET capture matched to locally-open UDP
// ports -- REQUIRES root or CAP_NET_RAW. Interface attribution here uses
// the ACTUAL interface each packet was observed on (via AF_PACKET's
// sockaddr_ll), not an IP-to-interface guess -- UDP sockets very
// commonly bind to the wildcard address, so unlike TCP there is no
// single "local IP" to map; the interface a given packet really
// travelled over is only known at capture time.

#include "ProcessBandwidthCollector.h"

#include <dirent.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <fstream>
#include <sstream>
#include <map>
#include <chrono>

#include <sys/socket.h>
#include <sys/types.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <poll.h>
#include <vector>
#include <thread>
#include <mutex>
#include <atomic>
#include <utility>
#include <cstring>
#include <cerrno>

#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <linux/sock_diag.h>
#include <linux/inet_diag.h>
#include <linux/tcp.h>
#include <linux/if_ether.h>
#include <linux/if_packet.h>

namespace {

constexpr size_t kNetlinkRecvBufSize = 32 * 1024;
constexpr size_t kCaptureBufSize = 65536;
constexpr int kCapturePollTimeoutMs = 500;

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
            try {
                uint64_t inode = std::stoull(target.substr(8, target.size() - 9));
                result[inode] = pid;
            } catch (...) {}
        }
        closedir(fdDir);
    }
    closedir(procDir);
    return result;
}

// Maps a local IP address string ("192.168.1.5", "::1", ...) to the
// interface name that owns it, via getifaddrs(). Refreshed by the caller
// periodically, not on every single poll (interface addresses rarely
// change, and getifaddrs() is comparatively cheap but no need to call it
// every cycle either).
std::map<std::string, std::string> buildIpToInterfaceMap() {
    std::map<std::string, std::string> result;
    struct ifaddrs* ifaddr = nullptr;
    if (getifaddrs(&ifaddr) != 0) return result;

    for (struct ifaddrs* ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr) continue;
        char buf[INET6_ADDRSTRLEN] = {0};
        if (ifa->ifa_addr->sa_family == AF_INET) {
            auto* sa = reinterpret_cast<struct sockaddr_in*>(ifa->ifa_addr);
            if (inet_ntop(AF_INET, &sa->sin_addr, buf, sizeof(buf))) result[buf] = ifa->ifa_name;
        } else if (ifa->ifa_addr->sa_family == AF_INET6) {
            auto* sa6 = reinterpret_cast<struct sockaddr_in6*>(ifa->ifa_addr);
            if (inet_ntop(AF_INET6, &sa6->sin6_addr, buf, sizeof(buf))) result[buf] = ifa->ifa_name;
        }
    }
    freeifaddrs(ifaddr);
    return result;
}

// ---------------------------------------------------------------------------
// TCP: Netlink socket-diag with TCP_INFO
// ---------------------------------------------------------------------------
struct SocketBandwidth {
    uint64_t bytesAcked = 0;
    uint64_t bytesReceived = 0;
    std::string localIp; // for interface attribution
};

bool queryTcpInfoByFamily(int family, std::map<uint64_t, SocketBandwidth>& out, std::string& errorOut) {
    int fd = socket(AF_NETLINK, SOCK_DGRAM, NETLINK_SOCK_DIAG);
    if (fd < 0) {
        errorOut = std::string("Failed to open NETLINK_SOCK_DIAG socket: ") + strerror(errno);
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
        errorOut = std::string("Failed to send Netlink request: ") + strerror(errno);
        close(fd);
        return false;
    }

    std::vector<char> buf(kNetlinkRecvBufSize);
    bool done = false;
    while (!done) {
        ssize_t recvLen = recv(fd, buf.data(), buf.size(), 0);
        if (recvLen < 0) {
            errorOut = std::string("Failed to receive Netlink response: ") + strerror(errno);
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
                errorOut = std::string("Netlink error: ") + strerror(-nlErr->error);
                close(fd);
                return false;
            }

            auto* diag = reinterpret_cast<struct inet_diag_msg*>(NLMSG_DATA(nlh));

            // Local IP address, straight from the diag response -- valid
            // for interface attribution since TCP has a concrete resolved
            // source IP once a connection is established.
            char ipBuf[INET6_ADDRSTRLEN] = {0};
            if (family == AF_INET) {
                struct in_addr addr{};
                addr.s_addr = diag->id.idiag_src[0];
                inet_ntop(AF_INET, &addr, ipBuf, sizeof(ipBuf));
            } else {
                inet_ntop(AF_INET6, &diag->id.idiag_src, ipBuf, sizeof(ipBuf));
            }

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
                sb.localIp = ipBuf;
                out[diag->idiag_inode] = sb;
            }
        }
    }

    close(fd);
    return true;
}

// ---------------------------------------------------------------------------
// UDP: raw AF_PACKET capture, attributed by the interface each packet was
// actually observed on (not by IP address -- see file header comment).
// ---------------------------------------------------------------------------
struct PortIfaceKey {
    uint16_t port = 0;
    std::string iface;
    bool operator<(const PortIfaceKey& o) const {
        if (port != o.port) return port < o.port;
        return iface < o.iface;
    }
};

struct UdpAccum {
    std::mutex mutex;
    std::map<PortIfaceKey, uint64_t> bytesAsSourcePort; // candidate TX
    std::map<PortIfaceKey, uint64_t> bytesAsDestPort;   // candidate RX
};

void udpCaptureLoop(int fd, bool isIPv6, UdpAccum* accum, std::atomic<bool>* stopFlag) {
    std::vector<unsigned char> buf(kCaptureBufSize);

    while (!stopFlag->load()) {
        struct pollfd pfd{};
        pfd.fd = fd;
        pfd.events = POLLIN;
        int pr = poll(&pfd, 1, kCapturePollTimeoutMs);
        if (pr <= 0) continue;
        if (!(pfd.revents & POLLIN)) continue;

        struct sockaddr_ll llAddr{};
        socklen_t llLen = sizeof(llAddr);
        ssize_t n = recvfrom(fd, buf.data(), buf.size(), 0,
                              reinterpret_cast<struct sockaddr*>(&llAddr), &llLen);
        if (n <= 0) {
            if (errno == EINTR) continue;
            break;
        }

        char ifNameBuf[IF_NAMESIZE] = {0};
        std::string ifaceName = if_indextoname(static_cast<unsigned>(llAddr.sll_ifindex), ifNameBuf)
                                     ? ifNameBuf : "unknown";

        const unsigned char* data = buf.data();
        size_t len = static_cast<size_t>(n);
        size_t udpOffset;

        if (!isIPv6) {
            if (len < 20) continue;
            unsigned char ihl = data[0] & 0x0F;
            size_t ipHeaderLen = size_t(ihl) * 4;
            if (ipHeaderLen < 20 || len < ipHeaderLen + 8) continue;
            if (data[9] != 17) continue;
            udpOffset = ipHeaderLen;
        } else {
            if (len < 40 + 8) continue;
            if (data[6] != 17) continue;
            udpOffset = 40;
        }

        uint16_t srcPort = (uint16_t(data[udpOffset]) << 8) | data[udpOffset + 1];
        uint16_t dstPort = (uint16_t(data[udpOffset + 2]) << 8) | data[udpOffset + 3];

        std::lock_guard<std::mutex> lock(accum->mutex);
        accum->bytesAsSourcePort[{srcPort, ifaceName}] += len;
        accum->bytesAsDestPort[{dstPort, ifaceName}] += len;
    }
}

std::map<uint16_t, int64_t> buildUdpPortToPidMap() {
    std::map<uint16_t, int64_t> result;
    std::map<uint64_t, int64_t> inodeToPid = buildInodeToPidMap();

    static const char* const kPaths[] = {"/proc/net/udp", "/proc/net/udp6"};
    for (const char* path : kPaths) {
        std::ifstream f(path);
        if (!f) continue;

        std::string line;
        int lineNo = 0;
        while (std::getline(f, line)) {
            if (++lineNo == 1) continue;
            auto parts = splitWs(line);
            if (parts.size() < 10) continue;

            size_t colon = parts[1].find(':');
            if (colon == std::string::npos) continue;

            try {
                uint16_t port = static_cast<uint16_t>(std::stoul(parts[1].substr(colon + 1), nullptr, 16));
                uint64_t inode = std::stoull(parts[9]);
                auto pidIt = inodeToPid.find(inode);
                if (pidIt != inodeToPid.end()) result[port] = pidIt->second;
            } catch (...) {}
        }
    }
    return result;
}

// --- Fast-refreshing port->pid cache with a short TTL ---------------------
//
// buildUdpPortToPidMap() above reflects only currently-open sockets. Many
// UDP senders (heartbeats, telemetry, one-shot lookups) open a socket,
// send, and close it again within milliseconds -- far faster than the
// ~2 second collect() poll interval. If we only ever looked up ports at
// collect() time, that traffic would be captured (the packet was seen)
// but end up unattributable (the socket is already gone by the time we
// scan /proc/net/udp), and silently vanish from the results.
//
// Fix: a dedicated thread refreshes a port->pid map every ~400ms
// (independent of collect()'s ~2s cadence) and remembers each mapping
// for a short grace period (kPortCacheTtlMs) after the socket itself
// disappears from /proc/net/udp. This catches short-lived sockets that
// existed for at least one ~400ms scan window, at the minor cost of
// very rarely attributing a just-reused ephemeral port to the previous
// (now-closed) owner for a few seconds -- an acceptable trade-off for a
// monitoring tool, not an exact accounting system.
constexpr int kPortCacheScanIntervalMs = 400;
constexpr int kPortCacheTtlMs = 8000;

struct CachedPidEntry {
    int64_t pid = 0;
    std::chrono::steady_clock::time_point lastSeenAlive;
};

struct UdpPortPidCache {
    std::mutex mutex;
    std::map<uint16_t, CachedPidEntry> entries;
};

void udpPortScanLoop(UdpPortPidCache* cache, std::atomic<bool>* stopFlag) {
    while (!stopFlag->load()) {
        auto live = buildUdpPortToPidMap();
        auto now = std::chrono::steady_clock::now();
        {
            std::lock_guard<std::mutex> lock(cache->mutex);
            for (auto& kv : live) {
                cache->entries[kv.first] = CachedPidEntry{kv.second, now};
            }
            for (auto it = cache->entries.begin(); it != cache->entries.end();) {
                if (now - it->second.lastSeenAlive > std::chrono::milliseconds(kPortCacheTtlMs)) {
                    it = cache->entries.erase(it);
                } else {
                    ++it;
                }
            }
        }

        for (int i = 0; i < kPortCacheScanIntervalMs / 100 && !stopFlag->load(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
}

} // namespace

class ProcessBandwidthCollector::Impl {
public:
    bool running = false;
    bool tcpAvailable = false;
    bool udpAvailable = false;
    std::string error;
    int64_t prevSampleMs = 0;

    std::map<uint64_t, SocketBandwidth> prevBySocket;

    int udpFd4 = -1;
    int udpFd6 = -1;
    std::atomic<bool> udpStopFlag{false};
    std::thread udpThread4;
    std::thread udpThread6;
    UdpAccum udpAccum;

    // Fast-refreshing port->pid cache with a short TTL -- see the
    // udpPortScanLoop() comment above for why this exists (catching
    // short-lived UDP sockets that close faster than collect()'s poll
    // interval). Runs on its own thread, independent of the packet
    // capture threads and of collect()'s cadence.
    UdpPortPidCache udpPortCache;
    std::thread udpPortScanThread;

    // Running totals per (pid, interface) -- only ever incremented by
    // newly-computed deltas each poll, never overwritten.
    std::map<std::pair<int64_t, std::string>, ProcessBandwidthStats> cumulativeByKey;

    ~Impl() { stopUdpCapture(); }

    bool startUdpCapture(std::string& errorOut) {
        udpFd4 = socket(AF_PACKET, SOCK_DGRAM, htons(ETH_P_IP));
        udpFd6 = socket(AF_PACKET, SOCK_DGRAM, htons(ETH_P_IPV6));

        if (udpFd4 < 0 && udpFd6 < 0) {
            errorOut = (errno == EPERM || errno == EACCES)
                ? "Root or CAP_NET_RAW required for UDP bandwidth tracking. "
                  "Try: sudo setcap cap_net_raw+ep <path-to-binary>, or run with sudo."
                : std::string("Failed to open raw capture socket: ") + strerror(errno);
            if (udpFd4 >= 0) { close(udpFd4); udpFd4 = -1; }
            if (udpFd6 >= 0) { close(udpFd6); udpFd6 = -1; }
            return false;
        }

        udpStopFlag.store(false);
        if (udpFd4 >= 0) udpThread4 = std::thread(udpCaptureLoop, udpFd4, false, &udpAccum, &udpStopFlag);
        if (udpFd6 >= 0) udpThread6 = std::thread(udpCaptureLoop, udpFd6, true, &udpAccum, &udpStopFlag);
        udpPortScanThread = std::thread(udpPortScanLoop, &udpPortCache, &udpStopFlag);
        return true;
    }

    void stopUdpCapture() {
        udpStopFlag.store(true);
        if (udpThread4.joinable()) udpThread4.join();
        if (udpThread6.joinable()) udpThread6.join();
        if (udpPortScanThread.joinable()) udpPortScanThread.join();
        if (udpFd4 >= 0) { close(udpFd4); udpFd4 = -1; }
        if (udpFd6 >= 0) { close(udpFd6); udpFd6 = -1; }
    }
};

ProcessBandwidthCollector::ProcessBandwidthCollector() : m_impl(new Impl()) {}
ProcessBandwidthCollector::~ProcessBandwidthCollector() { delete m_impl; }

bool ProcessBandwidthCollector::start() {
    std::map<uint64_t, SocketBandwidth> probe;
    std::string tcpErr4, tcpErr6;
    bool tcpOk4 = queryTcpInfoByFamily(AF_INET, probe, tcpErr4);
    bool tcpOk6 = queryTcpInfoByFamily(AF_INET6, probe, tcpErr6);
    bool tcpAvailable = tcpOk4 || tcpOk6;
    std::string tcpErr = !tcpErr4.empty() ? tcpErr4 : tcpErr6;

    std::string udpErr;
    bool udpAvailable = m_impl->startUdpCapture(udpErr);

    if (!tcpAvailable && !udpAvailable) {
        m_impl->error = !tcpErr.empty() ? tcpErr : udpErr;
        m_impl->running = false;
        return false;
    }

    std::string notes;
    if (!tcpAvailable) notes += "TCP tracking unavailable: " + tcpErr;
    if (!udpAvailable) {
        if (!notes.empty()) notes += "  |  ";
        notes += "UDP tracking unavailable: " + udpErr;
    }
    m_impl->error = notes;

    m_impl->tcpAvailable = tcpAvailable;
    m_impl->udpAvailable = udpAvailable;
    m_impl->running = true;
    m_impl->prevBySocket.clear();
    m_impl->cumulativeByKey.clear();
    m_impl->prevSampleMs = nowMs();
    return true;
}

void ProcessBandwidthCollector::stop() {
    m_impl->running = false;
    m_impl->stopUdpCapture();
}

bool ProcessBandwidthCollector::isRunning() const { return m_impl->running; }
std::string ProcessBandwidthCollector::lastError() const { return m_impl->error; }

std::vector<ProcessInterfaceBandwidth> ProcessBandwidthCollector::collect() {
    std::vector<ProcessInterfaceBandwidth> result;
    if (!m_impl->running) return result;

    int64_t now = nowMs();
    double dtSec = m_impl->prevSampleMs ? (now - m_impl->prevSampleMs) / 1000.0 : 1.0;
    if (dtSec <= 0) dtSec = 1.0;

    using Key = std::pair<int64_t, std::string>; // (pid, interface)
    std::map<Key, ProcessBandwidthStats> rateThisPoll;

    // --- TCP (Netlink), attributed by local-IP -> interface ---
    if (m_impl->tcpAvailable) {
        std::map<uint64_t, SocketBandwidth> current;
        std::string err;
        bool ok4 = queryTcpInfoByFamily(AF_INET, current, err);
        bool ok6 = queryTcpInfoByFamily(AF_INET6, current, err);

        if (ok4 || ok6) {
            std::map<uint64_t, int64_t> inodeToPid = buildInodeToPidMap();
            std::map<std::string, std::string> ipToIface = buildIpToInterfaceMap();

            for (auto& kv : current) {
                uint64_t inode = kv.first;
                const SocketBandwidth& sb = kv.second;

                auto pidIt = inodeToPid.find(inode);
                if (pidIt == inodeToPid.end()) continue;
                int64_t pid = pidIt->second;

                std::string iface = "(unattributed)";
                auto ifaceIt = ipToIface.find(sb.localIp);
                if (ifaceIt != ipToIface.end()) iface = ifaceIt->second;

                uint64_t prevAcked = 0, prevReceived = 0;
                auto prevIt = m_impl->prevBySocket.find(inode);
                if (prevIt != m_impl->prevBySocket.end()) {
                    prevAcked = prevIt->second.bytesAcked;
                    prevReceived = prevIt->second.bytesReceived;
                }

                uint64_t txDelta = sb.bytesAcked >= prevAcked ? sb.bytesAcked - prevAcked : 0;
                uint64_t rxDelta = sb.bytesReceived >= prevReceived ? sb.bytesReceived - prevReceived : 0;

                Key key{pid, iface};
                rateThisPoll[key].txBytesPerSec += uint64_t(txDelta / dtSec);
                rateThisPoll[key].rxBytesPerSec += uint64_t(rxDelta / dtSec);

                auto& cum = m_impl->cumulativeByKey[key];
                cum.txBytesTotal += txDelta;
                cum.rxBytesTotal += rxDelta;
            }

            m_impl->prevBySocket = current;
        }
    }

    // --- UDP (raw capture), attributed by observed capture interface ---
    if (m_impl->udpAvailable) {
        std::map<PortIfaceKey, uint64_t> asSrc, asDst;
        {
            std::lock_guard<std::mutex> lock(m_impl->udpAccum.mutex);
            asSrc = m_impl->udpAccum.bytesAsSourcePort;
            asDst = m_impl->udpAccum.bytesAsDestPort;
            m_impl->udpAccum.bytesAsSourcePort.clear();
            m_impl->udpAccum.bytesAsDestPort.clear();
        }

        std::map<uint16_t, int64_t> portToPid;
        {
            std::lock_guard<std::mutex> lock(m_impl->udpPortCache.mutex);
            for (auto& kv : m_impl->udpPortCache.entries) portToPid[kv.first] = kv.second.pid;
        }

        for (auto& kv : asDst) {
            auto pidIt = portToPid.find(kv.first.port);
            if (pidIt == portToPid.end()) continue;
            Key key{pidIt->second, kv.first.iface};
            rateThisPoll[key].rxBytesPerSec += uint64_t(kv.second / dtSec);
            m_impl->cumulativeByKey[key].rxBytesTotal += kv.second;
        }
        for (auto& kv : asSrc) {
            auto pidIt = portToPid.find(kv.first.port);
            if (pidIt == portToPid.end()) continue;
            Key key{pidIt->second, kv.first.iface};
            rateThisPoll[key].txBytesPerSec += uint64_t(kv.second / dtSec);
            m_impl->cumulativeByKey[key].txBytesTotal += kv.second;
        }
    }

    m_impl->prevSampleMs = now;

    for (auto& kv : m_impl->cumulativeByKey) {
        ProcessInterfaceBandwidth entry;
        entry.pid = kv.first.first;
        entry.interfaceName = kv.first.second;
        entry.stats = kv.second;

        auto rateIt = rateThisPoll.find(kv.first);
        if (rateIt != rateThisPoll.end()) {
            entry.stats.rxBytesPerSec = rateIt->second.rxBytesPerSec;
            entry.stats.txBytesPerSec = rateIt->second.txBytesPerSec;
        } else {
            entry.stats.rxBytesPerSec = 0;
            entry.stats.txBytesPerSec = 0;
        }
        result.push_back(entry);
    }

    return result;
}
