#pragma once

#include "Types.h"
#include <QMap>
#include <QString>

// Per-process network bandwidth tracking -- an OPT-IN feature, only used
// when the --track-bandwidth command-line flag is passed (see main.cpp).
// See the ProcessBandwidthStats comment in Types.h for exactly what
// mechanism each platform uses and its real limitations (TCP-only on both
// platforms; requires Administrator on Windows).
class ProcessBandwidthCollector {
public:
    ProcessBandwidthCollector();
    ~ProcessBandwidthCollector();

    // Starts the underlying OS mechanism. Returns false if it couldn't be
    // started (most commonly: not running elevated on Windows) -- call
    // lastError() for a human-readable reason to show the user.
    bool start();
    void stop();
    bool isRunning() const;
    QString lastError() const;

    // Per-PID snapshot: rxBytesPerSec/txBytesPerSec are rates since the
    // previous call; rxBytesTotal/txBytesTotal are cumulative since
    // start(). Only processes with currently-tracked TCP traffic appear.
    QMap<qint64, ProcessBandwidthStats> collect();

private:
    class Impl;
    Impl* m_impl;
};
