#pragma once

#include <QMainWindow>
#include <QTimer>
#include <QMap>

#include "Types.h"
#include "ProcessCollector.h"
#include "SystemStatsCollector.h"
#include "NetworkStatsCollector.h"
#include "GpuStatsCollector.h"
#include "ProcessConnectionCollector.h"
#include "ProcessBandwidthCollector.h"

class QTableView;
class QLineEdit;
class QLabel;
class QProgressBar;
class QTabWidget;
class QTableWidget;
class QTreeWidget;
class QSortFilterProxyModel;
class ProcessModel;
class HistoryChartWidget;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    // enableBandwidthTracking corresponds to the --track-bandwidth CLI flag
    // (see main.cpp). When false (the default), the Bandwidth tab doesn't
    // exist at all and no bandwidth-tracking OS mechanism is ever started
    // -- so normal launches never need admin/elevated rights.
    explicit MainWindow(bool enableBandwidthTracking = false, QWidget* parent = nullptr);
    ~MainWindow() override;

private slots:
    void pollProcesses();
    void pollSystemStats();
    void pollConnections();
    void pollBandwidth();
    void onProcessFilterChanged(const QString& text);
    void onKillSelectedProcess();
    void onProcessContextMenu(const QPoint& pos);
    void onConnectionsFilterChanged(const QString& text);

private:
    // --- Data collectors (platform-agnostic front, platform impl behind) ---
    ProcessCollector m_processCollector;
    SystemStatsCollector m_systemCollector;
    NetworkStatsCollector m_networkCollector;
    GpuStatsCollector m_gpuCollector;
    ProcessConnectionCollector m_connectionCollector;

    QTimer m_processTimer;
    QTimer m_statsTimer;
    QTimer m_connectionsTimer;

    // --- Processes tab ---
    ProcessModel* m_processModel = nullptr;
    QSortFilterProxyModel* m_processProxy = nullptr;
    QTableView* m_processTable = nullptr;
    QLineEdit* m_filterEdit = nullptr;
    QLabel* m_processSummaryLabel = nullptr;

    // --- Performance tab: CPU ---
    HistoryChartWidget* m_cpuChart = nullptr;
    QLabel* m_cpuTotalLabel = nullptr;
    QLabel* m_cpuTempLabel = nullptr;
    QVector<QProgressBar*> m_coreBars;
    QWidget* m_coreBarContainer = nullptr;

    // --- Performance tab: Memory ---
    HistoryChartWidget* m_memChart = nullptr;
    QLabel* m_memLabel = nullptr;
    QProgressBar* m_memBar = nullptr;
    QLabel* m_swapLabel = nullptr;

    // --- Performance tab: GPU ---
    QVector<QWidget*> m_gpuPanels;
    QWidget* m_gpuContainer = nullptr;
    HistoryChartWidget* m_gpuChart = nullptr;
    int m_gpuChartSeriesIndex = -1;

    // --- Performance tab: Disk ---
    QTableWidget* m_diskTable = nullptr;
    QTableWidget* m_diskIoTable = nullptr;

    // --- Network tab (deep info) ---
    QTableWidget* m_networkTable = nullptr;
    HistoryChartWidget* m_networkChart = nullptr;
    int m_netRxSeriesIndex = -1;
    int m_netTxSeriesIndex = -1;
    QLabel* m_networkTotalsLabel = nullptr;

    // --- Connections tab (per-process connection view; see
    // ProcessConnection in Types.h for scope -- connection-level detail,
    // not byte counters) ---
    QTableWidget* m_connectionsTable = nullptr;
    QLineEdit* m_connectionsFilterEdit = nullptr;
    QLabel* m_connectionsSummaryLabel = nullptr;
    QVector<ProcessConnection> m_lastConnections; // cached for re-filtering without a re-poll

    // --- Bandwidth tab (opt-in, only built/started when the
    // --track-bandwidth CLI flag is set -- see main.cpp) ---
    bool m_bandwidthTrackingEnabled = false;
    ProcessBandwidthCollector* m_bandwidthCollector = nullptr; // heap: only ever constructed when enabled
    QTimer m_bandwidthTimer;
    QTableWidget* m_bandwidthTable = nullptr;
    QLabel* m_bandwidthStatusLabel = nullptr;
    // Set once at startup if start() succeeded but only partially (e.g. TCP
    // works, UDP doesn't because CAP_NET_RAW/root is missing on Linux).
    // Kept separate from the per-poll "N processes tracked" status text so
    // this important caveat doesn't get silently overwritten on every poll.
    QString m_bandwidthAvailabilityNote;

    void buildUi();
    QWidget* buildProcessesTab();
    QWidget* buildPerformanceTab();
    QWidget* buildNetworkTab();
    QWidget* buildConnectionsTab();
    QWidget* buildBandwidthTab();

    void updateCpuUi(const CpuStats& cpu);
    void updateMemoryUi(const MemoryStats& mem);
    void updateDiskUi(const QVector<DiskVolume>& volumes, const QVector<DiskIoStats>& io);
    void updateGpuUi(const QVector<GpuInfo>& gpus);
    void updateNetworkUi(const NetworkStats& net);
    void renderConnectionsTable(const QString& filterText);
    void updateBandwidthUi(const QMap<qint64, ProcessBandwidthStats>& stats);

    // Sets the QProgressBar's "level" dynamic property (good/warn/critical)
    // from a 0-100 percentage and re-polishes it so the QSS
    // QProgressBar[level=...] color rule picks it up immediately.
    static void applyBarLevel(QProgressBar* bar, double percent);
};
