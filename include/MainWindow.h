#pragma once

#include <QMainWindow>
#include <QTimer>
#include <QMap>

#include "Types.h"
#include "ProcessCollector.h"
#include "SystemStatsCollector.h"
#include "NetworkStatsCollector.h"
#include "GpuStatsCollector.h"

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
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

private slots:
    void pollProcesses();
    void pollSystemStats();
    void onProcessFilterChanged(const QString& text);
    void onKillSelectedProcess();
    void onProcessContextMenu(const QPoint& pos);

private:
    // --- Data collectors (platform-agnostic front, platform impl behind) ---
    ProcessCollector m_processCollector;
    SystemStatsCollector m_systemCollector;
    NetworkStatsCollector m_networkCollector;
    GpuStatsCollector m_gpuCollector;

    QTimer m_processTimer;
    QTimer m_statsTimer;

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

    void buildUi();
    QWidget* buildProcessesTab();
    QWidget* buildPerformanceTab();
    QWidget* buildNetworkTab();

    void updateCpuUi(const CpuStats& cpu);
    void updateMemoryUi(const MemoryStats& mem);
    void updateDiskUi(const QVector<DiskVolume>& volumes, const QVector<DiskIoStats>& io);
    void updateGpuUi(const QVector<GpuInfo>& gpus);
    void updateNetworkUi(const NetworkStats& net);
};
