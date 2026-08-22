// Per-process network bandwidth: TCP via Netlink socket-diag with the
// TCP_INFO extension (the same mechanism `ss -i` uses), no elevated
// privileges needed. UDP via raw AF_PACKET packet capture matched to
// locally-open UDP ports -- this REQUIRES root or the CAP_NET_RAW
// capability, since the kernel has no per-socket byte counter for UDP.

#include "ProcessBandwidthCollector.h"

#include <dirent.h>
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

struct SocketBandwidth {
    uint64_t bytesAcked = 0;
    uint64_t bytesReceived = 0;
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

struct UdpPortAccum {
    std::mutex mutex;
    std::map<uint16_t, uint64_t> bytesAsSourcePort;
    std::map<uint16_t, uint64_t> bytesAsDestPort;
};

void udpCaptureLoop(int fd, bool isIPv6, UdpPortAccum* accum, std::atomic<bool>* stopFlag) {
    std::vector<unsigned char> buf(kCaptureBufSize);

    while (!stopFlag->load()) {
        struct pollfd pfd{};
        pfd.fd = fd;
        pfd.events = POLLIN;
        int pr = poll(&pfd, 1, kCapturePollTimeoutMs);
        if (pr <= 0) continue;
        if (!(pfd.revents & POLLIN)) continue;

        ssize_t n = recv(fd, buf.data(), buf.size(), 0);
        if (n <= 0) {
            if (errno == EINTR) continue;
            break;
        }

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
        accum->bytesAsSourcePort[srcPort] += len;
        accum->bytesAsDestPort[dstPort] += len;
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
    UdpPortAccum udpAccum;

    std::map<int64_t, ProcessBandwidthStats> cumulativeByPid;

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
    m_impl->cumulativeByPid.clear();
    m_impl->prevSampleMs = nowMs();
    return true;
}

void ProcessBandwidthCollector::stop() {
    m_impl->running = false;
    m_impl->stopUdpCapture();
}

bool ProcessBandwidthCollector::isRunning() const { return m_impl->running; }
std::string ProcessBandwidthCollector::lastError() const { return m_impl->error; }

std::map<int64_t, ProcessBandwidthStats> ProcessBandwidthCollector::collect() {
    std::map<int64_t, ProcessBandwidthStats> result;
    if (!m_impl->running) return result;

    int64_t now = nowMs();
    double dtSec = m_impl->prevSampleMs ? (now - m_impl->prevSampleMs) / 1000.0 : 1.0;
    if (dtSec <= 0) dtSec = 1.0;

    std::map<int64_t, ProcessBandwidthStats> rateThisPoll;

    if (m_impl->tcpAvailable) {
        std::map<uint64_t, SocketBandwidth> current;
        std::string err;
        bool ok4 = queryTcpInfoByFamily(AF_INET, current, err);
        bool ok6 = queryTcpInfoByFamily(AF_INET6, current, err);

        if (ok4 || ok6) {
            std::map<uint64_t, int64_t> inodeToPid = buildInodeToPidMap();

            for (auto& kv : current) {
                uint64_t inode = kv.first;
                const SocketBandwidth& sb = kv.second;

                auto pidIt = inodeToPid.find(inode);
                if (pidIt == inodeToPid.end()) continue;
                int64_t pid = pidIt->second;

                uint64_t prevAcked = 0, prevReceived = 0;
                auto prevIt = m_impl->prevBySocket.find(inode);
                if (prevIt != m_impl->prevBySocket.end()) {
                    prevAcked = prevIt->second.bytesAcked;
                    prevReceived = prevIt->second.bytesReceived;
                }

                uint64_t txDelta = sb.bytesAcked >= prevAcked ? sb.bytesAcked - prevAcked : 0;
                uint64_t rxDelta = sb.bytesReceived >= prevReceived ? sb.bytesReceived - prevReceived : 0;

                rateThisPoll[pid].txBytesPerSec += uint64_t(txDelta / dtSec);
                rateThisPoll[pid].rxBytesPerSec += uint64_t(rxDelta / dtSec);

                auto& cum = m_impl->cumulativeByPid[pid];
                cum.txBytesTotal += txDelta;
                cum.rxBytesTotal += rxDelta;
            }

            m_impl->prevBySocket = current;
        }
    }

    if (m_impl->udpAvailable) {
        std::map<uint16_t, uint64_t> asSrc, asDst;
        {
            std::lock_guard<std::mutex> lock(m_impl->udpAccum.mutex);
            asSrc = m_impl->udpAccum.bytesAsSourcePort;
            asDst = m_impl->udpAccum.bytesAsDestPort;
            m_impl->udpAccum.bytesAsSourcePort.clear();
            m_impl->udpAccum.bytesAsDestPort.clear();
        }

        std::map<uint16_t, int64_t> portToPid = buildUdpPortToPidMap();

        for (auto& kv : portToPid) {
            uint16_t port = kv.first;
            int64_t pid = kv.second;

            uint64_t rxBytes = asDst.count(port) ? asDst[port] : 0;
            uint64_t txBytes = asSrc.count(port) ? asSrc[port] : 0;
            if (rxBytes == 0 && txBytes == 0) continue;

            rateThisPoll[pid].rxBytesPerSec += uint64_t(rxBytes / dtSec);
            rateThisPoll[pid].txBytesPerSec += uint64_t(txBytes / dtSec);

            auto& cum = m_impl->cumulativeByPid[pid];
            cum.rxBytesTotal += rxBytes;
            cum.txBytesTotal += txBytes;
        }
    }

    m_impl->prevSampleMs = now;

    for (auto& kv : m_impl->cumulativeByPid) {
        ProcessBandwidthStats s = kv.second;
        auto rateIt = rateThisPoll.find(kv.first);
        if (rateIt != rateThisPoll.end()) {
            s.rxBytesPerSec = rateIt->second.rxBytesPerSec;
            s.txBytesPerSec = rateIt->second.txBytesPerSec;
        } else {
            s.rxBytesPerSec = 0;
            s.txBytesPerSec = 0;
        }
        result[kv.first] = s;
    }

    return result;
}
