// Per-interface network statistics via GetIfTable2.

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif

#include "NetworkStatsCollector.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <netioapi.h>
#include <chrono>
#include <algorithm>
#include <cstdio>

#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "ws2_32.lib")

namespace {
int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string wideToUtf8(const wchar_t* w) {
    if (!w) return {};
    int size = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 0) return {};
    std::string out(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, out.data(), size, nullptr, nullptr);
    if (!out.empty() && out.back() == '\0') out.pop_back();
    return out;
}
}

class NetworkStatsCollector::Impl {};

NetworkStatsCollector::NetworkStatsCollector() : m_impl(new Impl()) {}
NetworkStatsCollector::~NetworkStatsCollector() { delete m_impl; }

NetworkStats NetworkStatsCollector::collect() {
    NetworkStats stats;

    PMIB_IF_TABLE2 table = nullptr;
    if (GetIfTable2(&table) != NO_ERROR || !table) return stats;

    int64_t now = nowMs();

    for (ULONG i = 0; i < table->NumEntries; ++i) {
        const MIB_IF_ROW2& row = table->Table[i];

        if (row.Type == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        if (row.InterfaceAndOperStatusFlags.FilterInterface) continue;
        if (row.OperStatus == IfOperStatusNotPresent) continue;

        std::string name = wideToUtf8(row.Alias);
        if (name.empty()) name = wideToUtf8(row.Description);

        NetworkInterfaceStats ifs;
        ifs.name = name;
        ifs.isUp = (row.OperStatus == IfOperStatusUp);
        ifs.linkSpeedMbps = row.TransmitLinkSpeed ? row.TransmitLinkSpeed / 1'000'000ULL : 0;

        if (row.PhysicalAddressLength >= 6) {
            char buf[18];
            std::snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
                          row.PhysicalAddress[0], row.PhysicalAddress[1], row.PhysicalAddress[2],
                          row.PhysicalAddress[3], row.PhysicalAddress[4], row.PhysicalAddress[5]);
            ifs.macAddress = buf;
        }

        ifs.rxBytesTotal = row.InOctets;
        ifs.txBytesTotal = row.OutOctets;
        ifs.rxPacketsTotal = row.InUcastPkts + row.InNUcastPkts;
        ifs.txPacketsTotal = row.OutUcastPkts + row.OutNUcastPkts;
        ifs.rxErrorsTotal = row.InErrors;
        ifs.txErrorsTotal = row.OutErrors;
        ifs.rxDroppedTotal = row.InDiscards;
        ifs.txDroppedTotal = row.OutDiscards;

        std::string key = std::to_string(row.InterfaceIndex);
        auto prevIt = m_prev.find(key);
        if (prevIt != m_prev.end() && prevIt->second.valid) {
            double dtSec = (now - prevIt->second.sampledAtMs) / 1000.0;
            if (dtSec <= 0) dtSec = 1.0;

            const auto& prev = prevIt->second;
            uint64_t rxBytesDelta = ifs.rxBytesTotal >= prev.rxBytes ? ifs.rxBytesTotal - prev.rxBytes : 0;
            uint64_t txBytesDelta = ifs.txBytesTotal >= prev.txBytes ? ifs.txBytesTotal - prev.txBytes : 0;
            uint64_t rxPktDelta = ifs.rxPacketsTotal >= prev.rxPackets ? ifs.rxPacketsTotal - prev.rxPackets : 0;
            uint64_t txPktDelta = ifs.txPacketsTotal >= prev.txPackets ? ifs.txPacketsTotal - prev.txPackets : 0;
            uint64_t rxDropDelta = ifs.rxDroppedTotal >= prev.rxDropped ? ifs.rxDroppedTotal - prev.rxDropped : 0;
            uint64_t txDropDelta = ifs.txDroppedTotal >= prev.txDropped ? ifs.txDroppedTotal - prev.txDropped : 0;
            uint64_t rxErrDelta = ifs.rxErrorsTotal >= prev.rxErrors ? ifs.rxErrorsTotal - prev.rxErrors : 0;
            uint64_t txErrDelta = ifs.txErrorsTotal >= prev.txErrors ? ifs.txErrorsTotal - prev.txErrors : 0;

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
                ifs.utilizationPercent = (std::min)(100.0, (usedBps / capacityBps) * 100.0);
            }

            stats.totalRxBytesPerSec += ifs.rxBytesPerSec;
            stats.totalTxBytesPerSec += ifs.txBytesPerSec;
        }

        PrevCounters newPrev;
        newPrev.rxBytes = ifs.rxBytesTotal; newPrev.txBytes = ifs.txBytesTotal;
        newPrev.rxPackets = ifs.rxPacketsTotal; newPrev.txPackets = ifs.txPacketsTotal;
        newPrev.rxDropped = ifs.rxDroppedTotal; newPrev.txDropped = ifs.txDroppedTotal;
        newPrev.rxErrors = ifs.rxErrorsTotal; newPrev.txErrors = ifs.txErrorsTotal;
        newPrev.sampledAtMs = now;
        newPrev.valid = true;
        m_prev[key] = newPrev;

        stats.interfaces.push_back(ifs);
    }

    FreeMibTable(table);
    return stats;
}
