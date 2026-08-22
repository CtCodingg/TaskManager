// Per-process network connections via GetExtendedTcpTable/GetExtendedUdpTable.

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif

#include "ProcessConnectionCollector.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <vector>

#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "ws2_32.lib")

namespace {

std::string tcpStateName(DWORD state) {
    switch (state) {
        case MIB_TCP_STATE_CLOSED:     return "CLOSE";
        case MIB_TCP_STATE_LISTEN:     return "LISTEN";
        case MIB_TCP_STATE_SYN_SENT:   return "SYN_SENT";
        case MIB_TCP_STATE_SYN_RCVD:   return "SYN_RECV";
        case MIB_TCP_STATE_ESTAB:      return "ESTABLISHED";
        case MIB_TCP_STATE_FIN_WAIT1:  return "FIN_WAIT1";
        case MIB_TCP_STATE_FIN_WAIT2:  return "FIN_WAIT2";
        case MIB_TCP_STATE_CLOSE_WAIT: return "CLOSE_WAIT";
        case MIB_TCP_STATE_CLOSING:    return "CLOSING";
        case MIB_TCP_STATE_LAST_ACK:   return "LAST_ACK";
        case MIB_TCP_STATE_TIME_WAIT:  return "TIME_WAIT";
        case MIB_TCP_STATE_DELETE_TCB: return "DELETE";
        default:                       return "UNKNOWN";
    }
}

std::string ipv4ToString(DWORD netOrderAddr) {
    char buf[INET_ADDRSTRLEN] = {0};
    struct in_addr addr;
    addr.S_un.S_addr = netOrderAddr;
    if (inet_ntop(AF_INET, &addr, buf, sizeof(buf))) return buf;
    return {};
}

std::string ipv6ToString(const UCHAR bytes[16]) {
    char buf[INET6_ADDRSTRLEN] = {0};
    struct in6_addr addr;
    memcpy(addr.u.Byte, bytes, 16);
    if (inet_ntop(AF_INET6, &addr, buf, sizeof(buf))) return buf;
    return {};
}

void collectTcp4(std::vector<ProcessConnection>& out) {
    ULONG size = 0;
    GetExtendedTcpTable(nullptr, &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_ALL, 0);
    if (size == 0) return;
    std::vector<BYTE> buffer(size);
    auto* table = reinterpret_cast<MIB_TCPTABLE_OWNER_PID*>(buffer.data());
    if (GetExtendedTcpTable(table, &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_ALL, 0) != NO_ERROR) return;

    for (DWORD i = 0; i < table->dwNumEntries; ++i) {
        const auto& row = table->table[i];
        ProcessConnection pc;
        pc.pid = static_cast<int64_t>(row.dwOwningPid);
        pc.protocol = "TCP";
        pc.localAddress = ipv4ToString(row.dwLocalAddr);
        pc.localPort = ntohs(static_cast<u_short>(row.dwLocalPort));
        pc.remoteAddress = ipv4ToString(row.dwRemoteAddr);
        pc.remotePort = ntohs(static_cast<u_short>(row.dwRemotePort));
        pc.state = tcpStateName(row.dwState);
        pc.isIPv6 = false;
        out.push_back(pc);
    }
}

void collectTcp6(std::vector<ProcessConnection>& out) {
    ULONG size = 0;
    GetExtendedTcpTable(nullptr, &size, FALSE, AF_INET6, TCP_TABLE_OWNER_PID_ALL, 0);
    if (size == 0) return;
    std::vector<BYTE> buffer(size);
    auto* table = reinterpret_cast<MIB_TCP6TABLE_OWNER_PID*>(buffer.data());
    if (GetExtendedTcpTable(table, &size, FALSE, AF_INET6, TCP_TABLE_OWNER_PID_ALL, 0) != NO_ERROR) return;

    for (DWORD i = 0; i < table->dwNumEntries; ++i) {
        const auto& row = table->table[i];
        ProcessConnection pc;
        pc.pid = static_cast<int64_t>(row.dwOwningPid);
        pc.protocol = "TCP";
        pc.localAddress = ipv6ToString(row.ucLocalAddr);
        pc.localPort = ntohs(static_cast<u_short>(row.dwLocalPort));
        pc.remoteAddress = ipv6ToString(row.ucRemoteAddr);
        pc.remotePort = ntohs(static_cast<u_short>(row.dwRemotePort));
        pc.state = tcpStateName(row.dwState);
        pc.isIPv6 = true;
        out.push_back(pc);
    }
}

void collectUdp4(std::vector<ProcessConnection>& out) {
    ULONG size = 0;
    GetExtendedUdpTable(nullptr, &size, FALSE, AF_INET, UDP_TABLE_OWNER_PID, 0);
    if (size == 0) return;
    std::vector<BYTE> buffer(size);
    auto* table = reinterpret_cast<MIB_UDPTABLE_OWNER_PID*>(buffer.data());
    if (GetExtendedUdpTable(table, &size, FALSE, AF_INET, UDP_TABLE_OWNER_PID, 0) != NO_ERROR) return;

    for (DWORD i = 0; i < table->dwNumEntries; ++i) {
        const auto& row = table->table[i];
        ProcessConnection pc;
        pc.pid = static_cast<int64_t>(row.dwOwningPid);
        pc.protocol = "UDP";
        pc.localAddress = ipv4ToString(row.dwLocalAddr);
        pc.localPort = ntohs(static_cast<u_short>(row.dwLocalPort));
        pc.state = "-";
        pc.isIPv6 = false;
        out.push_back(pc);
    }
}

void collectUdp6(std::vector<ProcessConnection>& out) {
    ULONG size = 0;
    GetExtendedUdpTable(nullptr, &size, FALSE, AF_INET6, UDP_TABLE_OWNER_PID, 0);
    if (size == 0) return;
    std::vector<BYTE> buffer(size);
    auto* table = reinterpret_cast<MIB_UDP6TABLE_OWNER_PID*>(buffer.data());
    if (GetExtendedUdpTable(table, &size, FALSE, AF_INET6, UDP_TABLE_OWNER_PID, 0) != NO_ERROR) return;

    for (DWORD i = 0; i < table->dwNumEntries; ++i) {
        const auto& row = table->table[i];
        ProcessConnection pc;
        pc.pid = static_cast<int64_t>(row.dwOwningPid);
        pc.protocol = "UDP";
        pc.localAddress = ipv6ToString(row.ucLocalAddr);
        pc.localPort = ntohs(static_cast<u_short>(row.dwLocalPort));
        pc.state = "-";
        pc.isIPv6 = true;
        out.push_back(pc);
    }
}

} // namespace

class ProcessConnectionCollector::Impl {};

ProcessConnectionCollector::ProcessConnectionCollector() : m_impl(new Impl()) {}
ProcessConnectionCollector::~ProcessConnectionCollector() { delete m_impl; }

std::vector<ProcessConnection> ProcessConnectionCollector::collect() {
    std::vector<ProcessConnection> result;
    collectTcp4(result);
    collectTcp6(result);
    collectUdp4(result);
    collectUdp6(result);
    return result;
}
