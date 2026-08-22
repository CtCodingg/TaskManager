// Windows implementation of ProcessBandwidthCollector.
//
// TCP: the TCP Extended Statistics (EStats) API
// (Set/GetPerTcpConnectionEStats, tcpestats.h), reading cumulative
// DataBytesOut/DataBytesIn per IPv4 TCP connection. Simple, well-documented
// public API surface.
//
// UDP: Windows has no EStats-equivalent for UDP (it's connectionless, so
// there's no per-flow kernel structure to query). The only way to get real
// per-process UDP byte counts is to consume Event Tracing for Windows
// (ETW) events from the Microsoft-Windows-Kernel-Network provider -- the
// same mechanism Task Manager's own per-process Network column uses. This
// is genuinely the most complex, hardest-to-verify-without-a-real-machine
// code in this project: it involves starting a real-time kernel trace
// session, consuming events on a background thread, and dynamically
// decoding event properties by name via TDH (Trace Data Helper), because
// hardcoding numeric event/property layouts would be fragile across
// Windows versions.
//
// BOTH mechanisms require the process to run elevated (Administrator).
// main.cpp handles prompting for elevation (a UAC dialog) when
// --track-bandwidth is passed; this collector reports failure via
// lastError() if, for whatever reason, it still isn't elevated.
//
// Scope: TCP is IPv4-only (EStats API limitation). UDP covers both IPv4
// and IPv6 (the ETW provider doesn't distinguish the way EStats does).

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif

#include "ProcessBandwidthCollector.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <tcpestats.h>
#include <evntrace.h>
#include <evntcons.h>
#include <tdh.h>
#include <QDateTime>
#include <QString>
#include <QStringList>
#include <vector>
#include <thread>
#include <mutex>

#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "tdh.lib")

namespace {

// ---------------------------------------------------------------------------
// TCP: EStats (unchanged mechanism from the original TCP-only implementation)
// ---------------------------------------------------------------------------
struct ConnKey {
    DWORD localAddr = 0, localPort = 0, remoteAddr = 0, remotePort = 0;
    bool operator<(const ConnKey& o) const {
        if (localAddr != o.localAddr) return localAddr < o.localAddr;
        if (localPort != o.localPort) return localPort < o.localPort;
        if (remoteAddr != o.remoteAddr) return remoteAddr < o.remoteAddr;
        return remotePort < o.remotePort;
    }
};

struct ConnBandwidth {
    quint64 bytesOut = 0;
    quint64 bytesIn = 0;
};

MIB_TCPROW toPlainRow(const MIB_TCPROW_OWNER_PID& row) {
    MIB_TCPROW plain{};
    plain.dwState = row.dwState;
    plain.dwLocalAddr = row.dwLocalAddr;
    plain.dwLocalPort = row.dwLocalPort;
    plain.dwRemoteAddr = row.dwRemoteAddr;
    plain.dwRemotePort = row.dwRemotePort;
    return plain;
}

bool enumerateTcp4(std::vector<MIB_TCPROW_OWNER_PID>& out, QString& errorOut) {
    ULONG size = 0;
    GetExtendedTcpTable(nullptr, &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_ALL, 0);
    if (size == 0) return true;

    std::vector<BYTE> buffer(size);
    auto* table = reinterpret_cast<MIB_TCPTABLE_OWNER_PID*>(buffer.data());
    DWORD result = GetExtendedTcpTable(table, &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_ALL, 0);
    if (result != NO_ERROR) {
        errorOut = QString("GetExtendedTcpTable failed (error %1)").arg(result);
        return false;
    }

    out.assign(table->table, table->table + table->dwNumEntries);
    return true;
}

// ---------------------------------------------------------------------------
// UDP: ETW consumption from the Microsoft-Windows-Kernel-Network provider
// ---------------------------------------------------------------------------
constexpr wchar_t kSessionName[] = L"TaskManagerNetTrace";

// {7DD42A49-5329-4832-8DFD-43D979153A88} -- Microsoft-Windows-Kernel-Network.
// Defined locally (not via DEFINE_GUID/initguid.h) since it's only used in
// this one translation unit -- avoids any multiple-definition risk.
const GUID kKernelNetworkProviderGuid = {
    0x7dd42a49, 0x5329, 0x4832, {0x8d, 0xfd, 0x43, 0xd9, 0x79, 0x15, 0x3a, 0x88}
};

// Shared state between the ETW consumer thread (writer) and collect() on
// the main thread (reader/drainer), passed through EVENT_TRACE_LOGFILEW's
// Context field so the callback (which must be a plain function pointer,
// not a member function) can reach it via EventRecord->UserContext.
struct UdpEtwState {
    std::mutex mutex;
    QMap<qint64, quint64> rxAccum; // bytes received, accumulated since last drain, per PID
    QMap<qint64, quint64> txAccum; // bytes sent, accumulated since last drain, per PID
};

// Extracts a named ULONG property from an event via TDH. Returns false if
// the named property doesn't exist on this particular event (common --
// most Kernel-Network events aren't data-transfer events and won't have
// "size", for example).
bool getUint32Property(PEVENT_RECORD record, PTRACE_EVENT_INFO info, const wchar_t* name, ULONG* outValue) {
    for (ULONG i = 0; i < info->TopLevelPropertyCount; ++i) {
        auto* propInfo = &info->EventPropertyInfoArray[i];
        auto* propName = reinterpret_cast<LPWSTR>(reinterpret_cast<BYTE*>(info) + propInfo->NameOffset);
        if (_wcsicmp(propName, name) != 0) continue;

        PROPERTY_DATA_DESCRIPTOR desc{};
        desc.PropertyName = reinterpret_cast<ULONGLONG>(propName);
        desc.ArrayIndex = ULONG_MAX;

        ULONG propSize = 0;
        if (TdhGetPropertySize(record, 0, nullptr, 1, &desc, &propSize) != ERROR_SUCCESS) return false;
        if (propSize < sizeof(ULONG)) return false;

        std::vector<BYTE> buf(propSize);
        if (TdhGetProperty(record, 0, nullptr, 1, &desc, propSize, buf.data()) != ERROR_SUCCESS) return false;

        *outValue = *reinterpret_cast<ULONG*>(buf.data());
        return true;
    }
    return false;
}

// ETW callback -- runs on the consumer thread inside ProcessTrace(). Must
// stay fast and never block: this identifies UDP data-transfer events by
// Task/Opcode NAME (e.g. task "KERNEL_NETWORK_TASK_UDPIP", opcode
// containing "send"/"recv") rather than hardcoded numeric IDs, since
// exact event IDs are not a stable, documented public contract and can
// differ across Windows versions -- name-based matching is more resilient.
void WINAPI EventRecordCallback(PEVENT_RECORD record) {
    if (!record || !record->UserContext) return;
    auto* state = reinterpret_cast<UdpEtwState*>(record->UserContext);

    ULONG bufferSize = 0;
    ULONG status = TdhGetEventInformation(record, 0, nullptr, nullptr, &bufferSize);
    if (status != ERROR_INSUFFICIENT_BUFFER) return;

    std::vector<BYTE> infoBuf(bufferSize);
    auto* info = reinterpret_cast<TRACE_EVENT_INFO*>(infoBuf.data());
    if (TdhGetEventInformation(record, 0, nullptr, info, &bufferSize) != ERROR_SUCCESS) return;

    if (info->TaskNameOffset == 0 || info->OpcodeNameOffset == 0) return;

    auto* taskNamePtr = reinterpret_cast<LPWSTR>(reinterpret_cast<BYTE*>(info) + info->TaskNameOffset);
    auto* opcodeNamePtr = reinterpret_cast<LPWSTR>(reinterpret_cast<BYTE*>(info) + info->OpcodeNameOffset);

    QString task = QString::fromWCharArray(taskNamePtr);
    if (!task.contains("udp", Qt::CaseInsensitive)) return;

    QString opcode = QString::fromWCharArray(opcodeNamePtr);
    bool isSend = opcode.contains("send", Qt::CaseInsensitive);
    bool isRecv = !isSend && (opcode.contains("recv", Qt::CaseInsensitive) || opcode.contains("receive", Qt::CaseInsensitive));
    if (!isSend && !isRecv) return;

    ULONG pid = 0, size = 0;
    if (!getUint32Property(record, info, L"PID", &pid)) {
        // Fallback: the event header's process ID, correct as long as the
        // event fires in the owning process's context (true for the
        // Kernel-Network provider's per-packet events).
        pid = record->EventHeader.ProcessId;
    }
    if (pid == 0) return;
    if (!getUint32Property(record, info, L"size", &size)) return;
    if (size == 0) return;

    std::lock_guard<std::mutex> lock(state->mutex);
    if (isSend) state->txAccum[static_cast<qint64>(pid)] += size;
    else state->rxAccum[static_cast<qint64>(pid)] += size;
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
    QMap<ConnKey, bool> enabledConns;
    QMap<ConnKey, ConnBandwidth> prevByConn;

    // --- UDP (ETW) state ---
    UdpEtwState udpState;
    TRACEHANDLE sessionHandle = 0;
    TRACEHANDLE traceHandle = INVALID_PROCESSTRACE_HANDLE;
    std::thread consumerThread;
    EVENT_TRACE_PROPERTIES* sessionProps = nullptr;
    bool udpSessionActive = false;

    // Running totals per PID, shared across TCP+UDP -- only ever
    // incremented by newly-computed deltas each poll, never overwritten.
    QMap<qint64, ProcessBandwidthStats> cumulativeByPid;

    ~Impl() {
        stopUdpSession();
    }

    bool startUdpSession(QString& errorOut) {
        ULONG bufferSize = sizeof(EVENT_TRACE_PROPERTIES) + sizeof(kSessionName);
        sessionProps = reinterpret_cast<EVENT_TRACE_PROPERTIES*>(malloc(bufferSize));
        if (!sessionProps) {
            errorOut = "Out of memory starting ETW trace session";
            return false;
        }

        auto initProps = [&]() {
            ZeroMemory(sessionProps, bufferSize);
            sessionProps->Wnode.BufferSize = bufferSize;
            sessionProps->Wnode.Flags = WNODE_FLAG_TRACED_GUID;
            sessionProps->Wnode.ClientContext = 1; // QPC timer resolution
            sessionProps->LogFileMode = EVENT_TRACE_REAL_TIME_MODE;
            sessionProps->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);
        };

        // Best-effort: stop any stale session left over from a previous
        // crashed run. Ignore the result -- it most likely just doesn't exist.
        initProps();
        ControlTraceW(static_cast<TRACEHANDLE>(0), kSessionName, sessionProps, EVENT_TRACE_CONTROL_STOP);

        initProps();
        ULONG result = StartTraceW(&sessionHandle, kSessionName, sessionProps);
        if (result != ERROR_SUCCESS) {
            errorOut = (result == ERROR_ACCESS_DENIED)
                ? "Administrator privileges required for UDP bandwidth tracking (ETW trace session)."
                : QString("StartTraceW failed (error %1)").arg(result);
            free(sessionProps);
            sessionProps = nullptr;
            return false;
        }

        result = EnableTraceEx2(sessionHandle, &kKernelNetworkProviderGuid,
            EVENT_CONTROL_CODE_ENABLE_PROVIDER, TRACE_LEVEL_INFORMATION,
            0, 0, 0, nullptr);
        if (result != ERROR_SUCCESS) {
            errorOut = QString("EnableTraceEx2 failed (error %1)").arg(result);
            ControlTraceW(sessionHandle, kSessionName, sessionProps, EVENT_TRACE_CONTROL_STOP);
            free(sessionProps);
            sessionProps = nullptr;
            sessionHandle = 0;
            return false;
        }

        EVENT_TRACE_LOGFILEW logFile{};
        logFile.LoggerName = const_cast<LPWSTR>(kSessionName);
        logFile.ProcessTraceMode = PROCESS_TRACE_MODE_REAL_TIME | PROCESS_TRACE_MODE_EVENT_RECORD;
        logFile.EventRecordCallback = EventRecordCallback;
        logFile.Context = &udpState;

        traceHandle = OpenTraceW(&logFile);
        if (traceHandle == INVALID_PROCESSTRACE_HANDLE) {
            errorOut = "OpenTraceW failed to open the real-time trace session.";
            ControlTraceW(sessionHandle, kSessionName, sessionProps, EVENT_TRACE_CONTROL_STOP);
            free(sessionProps);
            sessionProps = nullptr;
            sessionHandle = 0;
            return false;
        }

        udpSessionActive = true;
        consumerThread = std::thread([this]() {
            // Blocks until the session is stopped (ControlTraceW below,
            // which is the documented, non-racy way to unblock a
            // real-time ProcessTrace() call) or a fatal read error occurs.
            ProcessTrace(&traceHandle, 1, nullptr, nullptr);
        });

        return true;
    }

    void stopUdpSession() {
        if (!udpSessionActive) return;
        udpSessionActive = false;

        if (sessionHandle) {
            ControlTraceW(sessionHandle, kSessionName, sessionProps, EVENT_TRACE_CONTROL_STOP);
        }
        if (traceHandle != INVALID_PROCESSTRACE_HANDLE) {
            CloseTrace(traceHandle);
            traceHandle = INVALID_PROCESSTRACE_HANDLE;
        }
        if (consumerThread.joinable()) consumerThread.join();

        if (sessionProps) {
            free(sessionProps);
            sessionProps = nullptr;
        }
        sessionHandle = 0;
    }
};

ProcessBandwidthCollector::ProcessBandwidthCollector() : m_impl(new Impl()) {}
ProcessBandwidthCollector::~ProcessBandwidthCollector() { delete m_impl; }

bool ProcessBandwidthCollector::start() {
    // --- TCP: structural check only ---
    // Elevation (Administrator) is already guaranteed by main.cpp before
    // this collector is ever started with --track-bandwidth active, so we
    // don't probe a specific connection here to "confirm" TCP works --
    // that used to pick an arbitrary connection (e.g. conns[0]) and call
    // SetPerTcpConnectionEStats on it, which can fail for reasons that
    // have nothing to do with admin rights (a connection already in
    // TIME_WAIT/CLOSE_WAIT, a protected system connection, etc.), wrongly
    // marking TCP as globally unavailable for the whole session even
    // though every OTHER connection would have worked fine. Per-connection
    // failures are already tolerated gracefully inside collect() (a
    // connection that can't be enabled is just skipped that cycle).
    std::vector<MIB_TCPROW_OWNER_PID> conns;
    QString tcpErr;
    bool tcpAvailable = enumerateTcp4(conns, tcpErr);

    // --- UDP: ETW session ---
    QString udpErr;
    bool udpAvailable = m_impl->startUdpSession(udpErr);

    if (!tcpAvailable && !udpAvailable) {
        m_impl->error = !tcpErr.isEmpty() ? tcpErr : udpErr;
        m_impl->running = false;
        return false;
    }

    QStringList notes;
    if (!tcpAvailable) notes << QString("TCP tracking unavailable: %1").arg(tcpErr);
    if (!udpAvailable) notes << QString("UDP tracking unavailable: %1").arg(udpErr);
    m_impl->error = notes.join(QStringLiteral("  |  "));

    m_impl->tcpAvailable = tcpAvailable;
    m_impl->udpAvailable = udpAvailable;
    m_impl->running = true;
    m_impl->enabledConns.clear();
    m_impl->prevByConn.clear();
    m_impl->cumulativeByPid.clear();
    m_impl->prevSampleMs = QDateTime::currentMSecsSinceEpoch();
    return true;
}

void ProcessBandwidthCollector::stop() {
    m_impl->running = false;
    m_impl->stopUdpSession();
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

    // --- TCP (EStats) ---
    if (m_impl->tcpAvailable) {
        std::vector<MIB_TCPROW_OWNER_PID> conns;
        QString err;
        if (enumerateTcp4(conns, err)) {
            QMap<ConnKey, ConnBandwidth> currentByConn;

            for (const auto& ownerRow : conns) {
                ConnKey key{ownerRow.dwLocalAddr, ownerRow.dwLocalPort, ownerRow.dwRemoteAddr, ownerRow.dwRemotePort};
                MIB_TCPROW plainRow = toPlainRow(ownerRow);

                if (!m_impl->enabledConns.value(key, false)) {
                    TCP_ESTATS_DATA_RW_v0 rw{};
                    rw.EnableCollection = TRUE;
                    SetPerTcpConnectionEStats(&plainRow, TcpConnectionEstatsData,
                        reinterpret_cast<PUCHAR>(&rw), 0, sizeof(rw), 0);
                    m_impl->enabledConns[key] = true;
                    continue; // needs at least one interval before reading anything meaningful
                }

                TCP_ESTATS_DATA_ROD_v0 rod{};
                DWORD result = GetPerTcpConnectionEStats(&plainRow, TcpConnectionEstatsData,
                    nullptr, 0, 0, nullptr, 0, 0,
                    reinterpret_cast<PUCHAR>(&rod), 0, sizeof(rod));
                if (result != NO_ERROR) continue;

                ConnBandwidth cb;
                cb.bytesOut = rod.DataBytesOut;
                cb.bytesIn = rod.DataBytesIn;
                currentByConn[key] = cb;

                qint64 pid = static_cast<qint64>(ownerRow.dwOwningPid);

                quint64 prevOut = 0, prevIn = 0;
                auto prevIt = m_impl->prevByConn.constFind(key);
                if (prevIt != m_impl->prevByConn.constEnd()) {
                    prevOut = prevIt->bytesOut;
                    prevIn = prevIt->bytesIn;
                }

                quint64 txDelta = cb.bytesOut >= prevOut ? cb.bytesOut - prevOut : 0;
                quint64 rxDelta = cb.bytesIn >= prevIn ? cb.bytesIn - prevIn : 0;

                rateThisPoll[pid].txBytesPerSec += quint64(txDelta / dtSec);
                rateThisPoll[pid].rxBytesPerSec += quint64(rxDelta / dtSec);

                ProcessBandwidthStats& cum = m_impl->cumulativeByPid[pid];
                cum.txBytesTotal += txDelta;
                cum.rxBytesTotal += rxDelta;
            }

            m_impl->prevByConn = currentByConn;
        }
    }

    // --- UDP (ETW, drained from the background consumer thread) ---
    if (m_impl->udpAvailable) {
        QMap<qint64, quint64> rxAccum, txAccum;
        {
            std::lock_guard<std::mutex> lock(m_impl->udpState.mutex);
            rxAccum = m_impl->udpState.rxAccum;
            txAccum = m_impl->udpState.txAccum;
            m_impl->udpState.rxAccum.clear();
            m_impl->udpState.txAccum.clear();
        }

        for (auto it = rxAccum.constBegin(); it != rxAccum.constEnd(); ++it) {
            rateThisPoll[it.key()].rxBytesPerSec += quint64(it.value() / dtSec);
            m_impl->cumulativeByPid[it.key()].rxBytesTotal += it.value();
        }
        for (auto it = txAccum.constBegin(); it != txAccum.constEnd(); ++it) {
            rateThisPoll[it.key()].txBytesPerSec += quint64(it.value() / dtSec);
            m_impl->cumulativeByPid[it.key()].txBytesTotal += it.value();
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
