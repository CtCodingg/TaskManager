#pragma once

#include "Types.h"
#include <QObject>
#include <QVector>
#include <QMap>

// Collects the process list (and, on demand, thread lists) for the current
// platform. Implementation lives in src/platform/{linux,win}/ProcessBackend*.cpp
// so this header has zero platform-specific code.
class ProcessCollector {
public:
    ProcessCollector();
    ~ProcessCollector();

    // Returns the full process list with fresh CPU% (computed from deltas
    // against the previous call) and memory figures.
    QVector<ProcessInfo> collect();

    // Populates .threads for a single process (called lazily when the user
    // expands a process row, to keep the main poll cheap).
    QVector<ThreadInfo> collectThreads(qint64 pid);

    bool killProcess(qint64 pid);
    bool setPriority(qint64 pid, int niceValue);

private:
    struct CpuTimeSample {
        quint64 totalTimeTicks = 0; // process CPU ticks (user+sys) at sample time
        qint64  sampledAtMs = 0;
    };

    QMap<qint64, CpuTimeSample> m_prevSamples;
    quint64 m_prevSystemTotalTicks = 0;
    qint64  m_prevSampleMs = 0;

    class Impl;
    Impl* m_impl;
};
