#pragma once

#include "backend/ProcessCollector.h"
#include "backend/SystemStatsCollector.h"
#include "backend/GpuStatsCollector.h"
#include "backend/NetworkStatsCollector.h"
#include "backend/ProcessConnectionCollector.h"
#include "backend/ProcessBandwidthCollector.h"

#include "ui/ProcessesTab.h"
#include "ui/PerformanceTab.h"
#include "ui/NetworkTab.h"
#include "ui/ConnectionsTab.h"
#include "ui/BandwidthTab.h"
#include "ui/SettingsWindow.h"
#include "ui/AboutWindow.h"

#include <memory>
#include <string>

// Top-level application state and per-frame draw. enableBandwidthTracking mirrors the --track-bandwidth CLI
// flag (see main.cpp): when false, the Bandwidth tab and its collector
// don't exist at all, so normal launches never need admin/elevated rights.
class App {
public:
    explicit App(bool enableBandwidthTracking);

    void draw();

private:
    bool m_bandwidthTrackingEnabled;

    // --- Collectors ---
    ProcessCollector m_processCollector;
    SystemStatsCollector m_systemCollector;
    GpuStatsCollector m_gpuCollector;
    NetworkStatsCollector m_networkCollector;
    ProcessConnectionCollector m_connectionCollector;
    std::unique_ptr<ProcessBandwidthCollector> m_bandwidthCollector; // only constructed if enabled

    // --- Tabs ---
    ProcessesTab m_processesTab;
    PerformanceTab m_performanceTab;
    NetworkTab m_networkTab;
    ConnectionsTab m_connectionsTab;
    BandwidthTab m_bandwidthTab;

    // --- Dialogs ---
    SettingsWindow m_settingsWindow;
    AboutWindow m_aboutWindow;

    // --- Poll timing ---
    double m_lastProcessPoll = 0.0;
    double m_lastStatsPoll = 0.0;
    double m_lastConnectionsPoll = 0.0;
    double m_lastBandwidthPoll = 0.0;
    double m_refreshRateSec = 1.0; // driven by SettingsWindow::refreshRateMs()

    void pollIfDue();
    std::string pidToName(int64_t pid) const;
};
