// Windows implementation of NetworkStatsCollector using IP Helper API
// (GetIfTable2 / MIB_IF_ROW2), which exposes rich per-adapter counters
// including discards and errors -- equivalent to what /proc/net/dev gives
// on Linux. System library only (iphlpapi.lib).
//
// GetIfTable2/MIB_IF_TABLE2 are Vista+ APIs: they are only declared by the
// Windows SDK headers when _WIN32_WINNT is set to 0x0600 or higher BEFORE
// <windows.h> is included. Winsock2 must also be included before windows.h
// to avoid it pulling in the legacy winsock.h and causing redefinition
// conflicts with iphlpapi.h.

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601 // Windows 7+; covers Windows 10/11 targets too
#endif

#include "NetworkStatsCollector.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <netioapi.h>
#include <QDateTime>
#include <algorithm>

#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "ws2_32.lib")

class NetworkStatsCollector::Impl {};

NetworkStatsCollector::NetworkStatsCollector() : m_impl(new Impl()) {}
NetworkStatsCollector::~NetworkStatsCollector() { delete m_impl; }

NetworkStats NetworkStatsCollector::collect() {
    NetworkStats stats;

    PMIB_IF_TABLE2 table = nullptr;
    if (GetIfTable2(&table) != NO_ERROR || !table) return stats;

    qint64 nowMs = QDateTime::currentMSecsSinceEpoch();

    for (ULONG i = 0; i < table->NumEntries; ++i) {
        const MIB_IF_ROW2& row = table->Table[i];

        // Skip loopback and non-hardware interfaces for a clean list
        if (row.Type == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        if (row.InterfaceAndOperStatusFlags.FilterInterface) continue;
        if (row.OperStatus == IfOperStatusNotPresent) continue;

        QString name = QString::fromWCharArray(row.Alias);
        if (name.isEmpty()) name = QString::fromWCharArray(row.Description);

        NetworkInterfaceStats ifs;
        ifs.name = name;
        ifs.isUp = (row.OperStatus == IfOperStatusUp);
        ifs.linkSpeedMbps = row.TransmitLinkSpeed ? row.TransmitLinkSpeed / 1'000'000ULL : 0;

        // MAC address
        if (row.PhysicalAddressLength >= 6) {
            char buf[18];
            std::snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
                row.PhysicalAddress[0], row.PhysicalAddress[1], row.PhysicalAddress[2],
                row.PhysicalAddress[3], row.PhysicalAddress[4], row.PhysicalAddress[5]);
            ifs.macAddress = QString::fromLatin1(buf);
        }

        ifs.rxBytesTotal = row.InOctets;
        ifs.txBytesTotal = row.OutOctets;
        ifs.rxPacketsTotal = row.InUcastPkts + row.InNUcastPkts;
        ifs.txPacketsTotal = row.OutUcastPkts + row.OutNUcastPkts;
        ifs.rxErrorsTotal = row.InErrors;
        ifs.txErrorsTotal = row.OutErrors;
        ifs.rxDroppedTotal = row.InDiscards;
        ifs.txDroppedTotal = row.OutDiscards;

        QString key = QString::number(row.InterfaceIndex);
        auto prevIt = m_prev.constFind(key);
        if (prevIt != m_prev.constEnd() && prevIt->valid) {
            double dtSec = (nowMs - prevIt->sampledAtMs) / 1000.0;
            if (dtSec <= 0) dtSec = 1.0;

            quint64 rxBytesDelta = ifs.rxBytesTotal >= prevIt->rxBytes ? ifs.rxBytesTotal - prevIt->rxBytes : 0;
            quint64 txBytesDelta = ifs.txBytesTotal >= prevIt->txBytes ? ifs.txBytesTotal - prevIt->txBytes : 0;
            quint64 rxPktDelta = ifs.rxPacketsTotal >= prevIt->rxPackets ? ifs.rxPacketsTotal - prevIt->rxPackets : 0;
            quint64 txPktDelta = ifs.txPacketsTotal >= prevIt->txPackets ? ifs.txPacketsTotal - prevIt->txPackets : 0;
            quint64 rxDropDelta = ifs.rxDroppedTotal >= prevIt->rxDropped ? ifs.rxDroppedTotal - prevIt->rxDropped : 0;
            quint64 txDropDelta = ifs.txDroppedTotal >= prevIt->txDropped ? ifs.txDroppedTotal - prevIt->txDropped : 0;
            quint64 rxErrDelta = ifs.rxErrorsTotal >= prevIt->rxErrors ? ifs.rxErrorsTotal - prevIt->rxErrors : 0;
            quint64 txErrDelta = ifs.txErrorsTotal >= prevIt->txErrors ? ifs.txErrorsTotal - prevIt->txErrors : 0;

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
                ifs.utilizationPercent = std::min(100.0, (usedBps / capacityBps) * 100.0);
            }

            stats.totalRxBytesPerSec += ifs.rxBytesPerSec;
            stats.totalTxBytesPerSec += ifs.txBytesPerSec;
        }

        PrevCounters newPrev;
        newPrev.rxBytes = ifs.rxBytesTotal; newPrev.txBytes = ifs.txBytesTotal;
        newPrev.rxPackets = ifs.rxPacketsTotal; newPrev.txPackets = ifs.txPacketsTotal;
        newPrev.rxDropped = ifs.rxDroppedTotal; newPrev.txDropped = ifs.txDroppedTotal;
        newPrev.rxErrors = ifs.rxErrorsTotal; newPrev.txErrors = ifs.txErrorsTotal;
        newPrev.sampledAtMs = nowMs;
        newPrev.valid = true;
        m_prev[key] = newPrev;

        stats.interfaces.push_back(ifs);
    }

    FreeMibTable(table);
    return stats;
}