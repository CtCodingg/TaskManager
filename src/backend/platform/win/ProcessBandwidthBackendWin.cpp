// Per-process network bandwidth: TCP via the TCP Extended Statistics
// (EStats) API (Set/GetPerTcpConnectionEStats), UDP via ETW consumption
// from the Microsoft-Windows-Kernel-Network provider -- both REQUIRE
// Administrator rights, enforced by main.cpp's elevation prompt.

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
#include <chrono>
#include <vector>
#include <thread>
#include <mutex>
#include <map>
#include <string>
#include <cwchar>

#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "tdh.lib")

namespace {

int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

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
    uint64_t bytesOut = 0;
    uint64_t bytesIn = 0;
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

bool enumerateTcp4(std::vector<MIB_TCPROW_OWNER_PID>& out, std::string& errorOut) {
    ULONG size = 0;
    GetExtendedTcpTable(nullptr, &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_ALL, 0);
    if (size == 0) return true;

    std::vector<BYTE> buffer(size);
    auto* table = reinterpret_cast<MIB_TCPTABLE_OWNER_PID*>(buffer.data());
    DWORD result = GetExtendedTcpTable(table, &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_ALL, 0);
    if (result != NO_ERROR) {
        errorOut = "GetExtendedTcpTable failed (error " + std::to_string(result) + ")";
        return false;
    }
    out.assign(table->table, table->table + table->dwNumEntries);
    return true;
}

// --- UDP via ETW -----------------------------------------------------
constexpr wchar_t kSessionName[] = L"CtTaskManagerNetTrace";

const GUID kKernelNetworkProviderGuid = {
    0x7dd42a49, 0x5329, 0x4832, {0x8d, 0xfd, 0x43, 0xd9, 0x79, 0x15, 0x3a, 0x88}
};

struct UdpEtwState {
    std::mutex mutex;
    std::map<int64_t, uint64_t> rxAccum;
    std::map<int64_t, uint64_t> txAccum;
};

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

bool containsCI(const std::wstring& haystack, const wchar_t* needle) {
    std::wstring h = haystack;
    for (auto& c : h) c = towlower(c);
    std::wstring n = needle;
    for (auto& c : n) c = towlower(c);
    return h.find(n) != std::wstring::npos;
}

void WINAPI EventRecordCallback(PEVENT_RECORD record) {
    if (!record || !record->UserContext) return;
    auto* state = reinterpret_cast<UdpEtwState*>(record->UserContext);

    ULONG bufferSize = 0;
    if (TdhGetEventInformation(record, 0, nullptr, nullptr, &bufferSize) != ERROR_INSUFFICIENT_BUFFER) return;

    std::vector<BYTE> infoBuf(bufferSize);
    auto* info = reinterpret_cast<TRACE_EVENT_INFO*>(infoBuf.data());
    if (TdhGetEventInformation(record, 0, nullptr, info, &bufferSize) != ERROR_SUCCESS) return;

    if (info->TaskNameOffset == 0 || info->OpcodeNameOffset == 0) return;

    auto* taskNamePtr = reinterpret_cast<LPWSTR>(reinterpret_cast<BYTE*>(info) + info->TaskNameOffset);
    auto* opcodeNamePtr = reinterpret_cast<LPWSTR>(reinterpret_cast<BYTE*>(info) + info->OpcodeNameOffset);

    if (!containsCI(taskNamePtr, L"udp")) return;

    bool isSend = containsCI(opcodeNamePtr, L"send");
    bool isRecv = !isSend && (containsCI(opcodeNamePtr, L"recv") || containsCI(opcodeNamePtr, L"receive"));
    if (!isSend && !isRecv) return;

    ULONG pid = 0, size = 0;
    if (!getUint32Property(record, info, L"PID", &pid)) pid = record->EventHeader.ProcessId;
    if (pid == 0) return;
    if (!getUint32Property(record, info, L"size", &size)) return;
    if (size == 0) return;

    std::lock_guard<std::mutex> lock(state->mutex);
    if (isSend) state->txAccum[static_cast<int64_t>(pid)] += size;
    else state->rxAccum[static_cast<int64_t>(pid)] += size;
}

} // namespace

class ProcessBandwidthCollector::Impl {
public:
    bool running = false;
    bool tcpAvailable = false;
    bool udpAvailable = false;
    std::string error;
    int64_t prevSampleMs = 0;

    std::map<ConnKey, bool> enabledConns;
    std::map<ConnKey, ConnBandwidth> prevByConn;

    UdpEtwState udpState;
    TRACEHANDLE sessionHandle = 0;
    TRACEHANDLE traceHandle = INVALID_PROCESSTRACE_HANDLE;
    std::thread consumerThread;
    EVENT_TRACE_PROPERTIES* sessionProps = nullptr;
    bool udpSessionActive = false;

    std::map<int64_t, ProcessBandwidthStats> cumulativeByPid;

    ~Impl() { stopUdpSession(); }

    bool startUdpSession(std::string& errorOut) {
        ULONG bufferSize = sizeof(EVENT_TRACE_PROPERTIES) + sizeof(kSessionName);
        sessionProps = reinterpret_cast<EVENT_TRACE_PROPERTIES*>(malloc(bufferSize));
        if (!sessionProps) { errorOut = "Out of memory starting ETW trace session"; return false; }

        auto initProps = [&]() {
            ZeroMemory(sessionProps, bufferSize);
            sessionProps->Wnode.BufferSize = bufferSize;
            sessionProps->Wnode.Flags = WNODE_FLAG_TRACED_GUID;
            sessionProps->Wnode.ClientContext = 1;
            sessionProps->LogFileMode = EVENT_TRACE_REAL_TIME_MODE;
            sessionProps->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);
        };

        initProps();
        ControlTraceW(static_cast<TRACEHANDLE>(0), kSessionName, sessionProps, EVENT_TRACE_CONTROL_STOP);

        initProps();
        ULONG result = StartTraceW(&sessionHandle, kSessionName, sessionProps);
        if (result != ERROR_SUCCESS) {
            errorOut = (result == ERROR_ACCESS_DENIED)
                ? "Administrator privileges required for UDP bandwidth tracking (ETW trace session)."
                : "StartTraceW failed (error " + std::to_string(result) + ")";
            free(sessionProps);
            sessionProps = nullptr;
            return false;
        }

        result = EnableTraceEx2(sessionHandle, &kKernelNetworkProviderGuid,
            EVENT_CONTROL_CODE_ENABLE_PROVIDER, TRACE_LEVEL_INFORMATION, 0, 0, 0, nullptr);
        if (result != ERROR_SUCCESS) {
            errorOut = "EnableTraceEx2 failed (error " + std::to_string(result) + ")";
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
            ProcessTrace(&traceHandle, 1, nullptr, nullptr);
        });
        return true;
    }

    void stopUdpSession() {
        if (!udpSessionActive) return;
        udpSessionActive = false;

        if (sessionHandle) ControlTraceW(sessionHandle, kSessionName, sessionProps, EVENT_TRACE_CONTROL_STOP);
        if (traceHandle != INVALID_PROCESSTRACE_HANDLE) { CloseTrace(traceHandle); traceHandle = INVALID_PROCESSTRACE_HANDLE; }
        if (consumerThread.joinable()) consumerThread.join();

        if (sessionProps) { free(sessionProps); sessionProps = nullptr; }
        sessionHandle = 0;
    }
};

ProcessBandwidthCollector::ProcessBandwidthCollector() : m_impl(new Impl()) {}
ProcessBandwidthCollector::~ProcessBandwidthCollector() { delete m_impl; }

bool ProcessBandwidthCollector::start() {
    std::vector<MIB_TCPROW_OWNER_PID> conns;
    std::string tcpErr;
    bool tcpAvailable = enumerateTcp4(conns, tcpErr);

    std::string udpErr;
    bool udpAvailable = m_impl->startUdpSession(udpErr);

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
    m_impl->enabledConns.clear();
    m_impl->prevByConn.clear();
    m_impl->cumulativeByPid.clear();
    m_impl->prevSampleMs = nowMs();
    return true;
}

void ProcessBandwidthCollector::stop() {
    m_impl->running = false;
    m_impl->stopUdpSession();
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
        std::vector<MIB_TCPROW_OWNER_PID> conns;
        std::string err;
        if (enumerateTcp4(conns, err)) {
            std::map<ConnKey, ConnBandwidth> currentByConn;

            for (const auto& ownerRow : conns) {
                ConnKey key{ownerRow.dwLocalAddr, ownerRow.dwLocalPort, ownerRow.dwRemoteAddr, ownerRow.dwRemotePort};
                MIB_TCPROW plainRow = toPlainRow(ownerRow);

                if (!m_impl->enabledConns.count(key)) {
                    TCP_ESTATS_DATA_RW_v0 rw{};
                    rw.EnableCollection = TRUE;
                    SetPerTcpConnectionEStats(&plainRow, TcpConnectionEstatsData,
                        reinterpret_cast<PUCHAR>(&rw), 0, sizeof(rw), 0);
                    m_impl->enabledConns[key] = true;
                    continue;
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

                int64_t pid = static_cast<int64_t>(ownerRow.dwOwningPid);

                uint64_t prevOut = 0, prevIn = 0;
                auto prevIt = m_impl->prevByConn.find(key);
                if (prevIt != m_impl->prevByConn.end()) {
                    prevOut = prevIt->second.bytesOut;
                    prevIn = prevIt->second.bytesIn;
                }

                uint64_t txDelta = cb.bytesOut >= prevOut ? cb.bytesOut - prevOut : 0;
                uint64_t rxDelta = cb.bytesIn >= prevIn ? cb.bytesIn - prevIn : 0;

                rateThisPoll[pid].txBytesPerSec += uint64_t(txDelta / dtSec);
                rateThisPoll[pid].rxBytesPerSec += uint64_t(rxDelta / dtSec);

                auto& cum = m_impl->cumulativeByPid[pid];
                cum.txBytesTotal += txDelta;
                cum.rxBytesTotal += rxDelta;
            }

            m_impl->prevByConn = currentByConn;
        }
    }

    if (m_impl->udpAvailable) {
        std::map<int64_t, uint64_t> rxAccum, txAccum;
        {
            std::lock_guard<std::mutex> lock(m_impl->udpState.mutex);
            rxAccum = m_impl->udpState.rxAccum;
            txAccum = m_impl->udpState.txAccum;
            m_impl->udpState.rxAccum.clear();
            m_impl->udpState.txAccum.clear();
        }

        for (auto& kv : rxAccum) {
            rateThisPoll[kv.first].rxBytesPerSec += uint64_t(kv.second / dtSec);
            m_impl->cumulativeByPid[kv.first].rxBytesTotal += kv.second;
        }
        for (auto& kv : txAccum) {
            rateThisPoll[kv.first].txBytesPerSec += uint64_t(kv.second / dtSec);
            m_impl->cumulativeByPid[kv.first].txBytesTotal += kv.second;
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
