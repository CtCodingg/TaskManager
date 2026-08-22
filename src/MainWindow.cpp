#include "MainWindow.h"
#include "ProcessModel.h"
#include "HistoryChartWidget.h"
#include "FormatUtils.h"
#include "UiTheme.h"
#include "SettingsDialog.h"
#include "AboutDialog.h"

#include <QTableView>
#include <QHeaderView>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QLineEdit>
#include <QLabel>
#include <QProgressBar>
#include <QTabWidget>
#include <QTableWidget>
#include <QSortFilterProxyModel>
#include <QMenu>
#include <QMenuBar>
#include <QAction>
#include <QMessageBox>
#include <QGroupBox>
#include <QSplitter>
#include <QScrollArea>
#include <QStatusBar>
#include <QPushButton>
#include <QTableWidgetItem>
#include <QStyle>
#include <QBrush>
#include <QColor>
#include <QSet>
#include <QSettings>

namespace {
constexpr int kDefaultRefreshMs = 1000;
}

MainWindow::MainWindow(bool enableBandwidthTracking, QWidget* parent)
    : QMainWindow(parent), m_bandwidthTrackingEnabled(enableBandwidthTracking) {
    setWindowTitle("Task Manager");
    resize(1200, 800);
    buildUi();

    loadAndApplySettings(); // sets m_refreshRateMs and FormatUtils rate unit before timers start

    connect(&m_processTimer, &QTimer::timeout, this, &MainWindow::pollProcesses);
    connect(&m_statsTimer, &QTimer::timeout, this, &MainWindow::pollSystemStats);
    connect(&m_connectionsTimer, &QTimer::timeout, this, &MainWindow::pollConnections);
    m_processTimer.start(m_refreshRateMs);
    m_statsTimer.start(m_refreshRateMs);
    m_connectionsTimer.start(m_refreshRateMs);

    // Immediate first poll so the UI isn't empty on launch
    pollProcesses();
    pollSystemStats();
    pollConnections();

    if (m_bandwidthTrackingEnabled) {
        m_bandwidthCollector = new ProcessBandwidthCollector();
        if (m_bandwidthCollector->start()) {
            // start() can succeed "partially" -- e.g. on Linux, TCP works
            // without elevation but UDP needs CAP_NET_RAW/root and may not
            // be available. lastError() carries that note even on success.
            m_bandwidthAvailabilityNote = m_bandwidthCollector->lastError();
            connect(&m_bandwidthTimer, &QTimer::timeout, this, &MainWindow::pollBandwidth);
            m_bandwidthTimer.start(m_refreshRateMs);
            pollBandwidth();
        } else if (m_bandwidthStatusLabel) {
            m_bandwidthStatusLabel->setText(
                "Bandwidth tracking could not start: " + m_bandwidthCollector->lastError());
        }
    }

    statusBar()->showMessage("Ready");
}

MainWindow::~MainWindow() {
    delete m_bandwidthCollector;
}

void MainWindow::loadAndApplySettings() {
    QSettings settings;
    bool useBits = settings.value("network/rateUnitIsBits", true).toBool();
    FormatUtils::setRateUnit(useBits ? FormatUtils::RateUnit::Bits : FormatUtils::RateUnit::Bytes);

    m_refreshRateMs = settings.value("general/refreshRateMs", kDefaultRefreshMs).toInt();

    // Re-apply to already-running timers too (called again after the
    // Settings dialog closes, not just at startup). setInterval() on a
    // live QTimer takes effect for its next firing -- no restart needed.
    m_processTimer.setInterval(m_refreshRateMs);
    m_statsTimer.setInterval(m_refreshRateMs);
    m_connectionsTimer.setInterval(m_refreshRateMs);
    if (m_bandwidthTimer.isActive()) {
        m_bandwidthTimer.setInterval(m_refreshRateMs);
    }
}

void MainWindow::onOpenSettings() {
    SettingsDialog dialog(this);
    if (dialog.exec() != QDialog::Accepted) return;

    loadAndApplySettings();

    // Refresh every visible table/label immediately so the new rate unit
    // is visible right away, rather than waiting for the next poll tick.
    pollSystemStats();
    if (m_connectionsTable) pollConnections();
    if (m_bandwidthTable && m_bandwidthCollector && m_bandwidthCollector->isRunning()) {
        pollBandwidth();
    }
}

void MainWindow::onOpenAbout() {
    AboutDialog dialog(this);
    dialog.exec();
}

// ---------------------------------------------------------------------------
// UI construction
// ---------------------------------------------------------------------------
void MainWindow::buildUi() {
    QAction* settingsAction = menuBar()->addAction("⚙ Settings");
    connect(settingsAction, &QAction::triggered, this, &MainWindow::onOpenSettings);

    QAction* aboutAction = menuBar()->addAction("ℹ About");
    connect(aboutAction, &QAction::triggered, this, &MainWindow::onOpenAbout);

    auto* tabs = new QTabWidget(this);
    tabs->addTab(buildProcessesTab(), "Processes");
    tabs->addTab(buildPerformanceTab(), "Performance");
    tabs->addTab(buildNetworkTab(), "Network");
    tabs->addTab(buildConnectionsTab(), "Connections");
    if (m_bandwidthTrackingEnabled) {
        tabs->addTab(buildBandwidthTab(), "Bandwidth");
    }
    setCentralWidget(tabs);
}

QWidget* MainWindow::buildProcessesTab() {
    auto* container = new QWidget();
    auto* layout = new QVBoxLayout(container);

    auto* topBar = new QHBoxLayout();
    m_filterEdit = new QLineEdit();
    m_filterEdit->setPlaceholderText("Filter by name, user, or PID...");
    connect(m_filterEdit, &QLineEdit::textChanged, this, &MainWindow::onProcessFilterChanged);

    auto* killButton = new QPushButton("End Task");
    killButton->setObjectName("endTaskButton");
    connect(killButton, &QPushButton::clicked, this, &MainWindow::onKillSelectedProcess);

    auto* searchLabel = new QLabel("Search:");
    searchLabel->setObjectName("metricSubtle");
    topBar->addWidget(searchLabel);
    topBar->addWidget(m_filterEdit, 1);
    topBar->addWidget(killButton);
    layout->addLayout(topBar);

    m_processModel = new ProcessModel(this);
    m_processProxy = new QSortFilterProxyModel(this);
    m_processProxy->setSourceModel(m_processModel);
    m_processProxy->setFilterCaseSensitivity(Qt::CaseInsensitive);
    m_processProxy->setFilterKeyColumn(-1); // search all columns

    m_processTable = new QTableView();
    m_processTable->setModel(m_processProxy);
    m_processTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_processTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_processTable->setSortingEnabled(true);
    m_processTable->sortByColumn(ProcessModel::ColCpu, Qt::DescendingOrder);
    m_processTable->horizontalHeader()->setStretchLastSection(true);
    m_processTable->verticalHeader()->setVisible(false);
    m_processTable->setAlternatingRowColors(true);
    m_processTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_processTable->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_processTable, &QTableView::customContextMenuRequested,
            this, &MainWindow::onProcessContextMenu);

    layout->addWidget(m_processTable, 1);

    m_processSummaryLabel = new QLabel("0 processes");
    m_processSummaryLabel->setObjectName("metricSubtle");
    layout->addWidget(m_processSummaryLabel);

    return container;
}

QWidget* MainWindow::buildPerformanceTab() {
    auto* scroll = new QScrollArea();
    scroll->setWidgetResizable(true);

    auto* container = new QWidget();
    auto* layout = new QVBoxLayout(container);

    // --- CPU group ---
    auto* cpuGroup = new QGroupBox("CPU");
    cpuGroup->setObjectName("cpuCard");
    auto* cpuLayout = new QVBoxLayout(cpuGroup);
    auto* cpuHeaderRow = new QHBoxLayout();
    m_cpuTotalLabel = new QLabel("0.0%");
    m_cpuTotalLabel->setObjectName("metricValue");
    m_cpuTempLabel = new QLabel("Temp: n/a");
    m_cpuTempLabel->setObjectName("metricSubtle");
    cpuHeaderRow->addWidget(m_cpuTotalLabel);
    cpuHeaderRow->addStretch();
    cpuHeaderRow->addWidget(m_cpuTempLabel);
    cpuLayout->addLayout(cpuHeaderRow);

    m_cpuChart = new HistoryChartWidget("CPU Load (%)", 120);
    m_cpuChart->setYRange(0, 100);
    m_cpuChart->addSeries("Total", UiTheme::accentCpu());
    cpuLayout->addWidget(m_cpuChart);

    m_coreBarContainer = new QWidget();
    auto* coreGrid = new QGridLayout(m_coreBarContainer);
    coreGrid->setSpacing(6);
    cpuLayout->addWidget(m_coreBarContainer);

    layout->addWidget(cpuGroup);

    // --- Memory group ---
    auto* memGroup = new QGroupBox("Memory");
    memGroup->setObjectName("memCard");
    auto* memLayout = new QVBoxLayout(memGroup);
    m_memLabel = new QLabel("0 / 0 GB");
    m_memLabel->setObjectName("metricValue");
    m_memBar = new QProgressBar();
    m_memBar->setRange(0, 100);
    m_swapLabel = new QLabel("Swap: 0 / 0 GB");
    m_swapLabel->setObjectName("metricSubtle");
    memLayout->addWidget(m_memLabel);
    memLayout->addWidget(m_memBar);
    memLayout->addWidget(m_swapLabel);

    m_memChart = new HistoryChartWidget("Memory Usage (%)", 120);
    m_memChart->setYRange(0, 100);
    m_memChart->addSeries("Used", UiTheme::accentMemory());
    memLayout->addWidget(m_memChart);

    layout->addWidget(memGroup);

    // --- GPU group ---
    auto* gpuGroup = new QGroupBox("GPU");
    gpuGroup->setObjectName("gpuCard");
    auto* gpuLayout = new QVBoxLayout(gpuGroup);
    m_gpuContainer = new QWidget();
    auto* gpuGrid = new QVBoxLayout(m_gpuContainer);
    gpuLayout->addWidget(m_gpuContainer);

    m_gpuChart = new HistoryChartWidget("GPU Load (%)", 120);
    m_gpuChart->setYRange(0, 100);
    gpuLayout->addWidget(m_gpuChart);

    layout->addWidget(gpuGroup);

    // --- Disk group ---
    auto* diskGroup = new QGroupBox("Disks");
    diskGroup->setObjectName("diskCard");
    auto* diskLayout = new QVBoxLayout(diskGroup);

    m_diskTable = new QTableWidget(0, 5);
    m_diskTable->setHorizontalHeaderLabels({"Mount", "Filesystem", "Used", "Total", "Usage"});
    m_diskTable->horizontalHeader()->setStretchLastSection(true);
    m_diskTable->verticalHeader()->setVisible(false);
    m_diskTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_diskTable->setAlternatingRowColors(true);
    m_diskTable->setMinimumHeight(140);
    auto* volumesLabel = new QLabel("Volumes");
    volumesLabel->setObjectName("sectionHint");
    diskLayout->addWidget(volumesLabel);
    diskLayout->addWidget(m_diskTable);

    m_diskIoTable = new QTableWidget(0, 4);
    m_diskIoTable->setHorizontalHeaderLabels({"Device", "Read/s", "Write/s", "Utilization"});
    m_diskIoTable->horizontalHeader()->setStretchLastSection(true);
    m_diskIoTable->verticalHeader()->setVisible(false);
    m_diskIoTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_diskIoTable->setAlternatingRowColors(true);
    m_diskIoTable->setMinimumHeight(120);
    auto* ioLabel = new QLabel("I/O Activity");
    ioLabel->setObjectName("sectionHint");
    diskLayout->addWidget(ioLabel);
    diskLayout->addWidget(m_diskIoTable);

    layout->addWidget(diskGroup);

    layout->addStretch();
    scroll->setWidget(container);
    return scroll;
}

QWidget* MainWindow::buildNetworkTab() {
    auto* container = new QWidget();
    auto* layout = new QVBoxLayout(container);

    m_networkTotalsLabel = new QLabel("Total: ↓ 0 B/s   ↑ 0 B/s");
    m_networkTotalsLabel->setObjectName("metricValue");
    layout->addWidget(m_networkTotalsLabel);

    m_networkChart = new HistoryChartWidget("Total Bandwidth", 120);
    m_networkChart->setYAutoScale(true);
    m_netRxSeriesIndex = m_networkChart->addSeries("Download", UiTheme::accentNetRx());
    m_netTxSeriesIndex = m_networkChart->addSeries("Upload", UiTheme::accentNetTx());
    layout->addWidget(m_networkChart);

    m_networkTable = new QTableWidget(0, 11);
    m_networkTable->setHorizontalHeaderLabels({
        "Interface", "Status", "IPv4", "Link Speed",
        "Down", "Up", "Utilization",
        "RX Drop %", "TX Drop %", "RX Err %", "TX Err %"
    });
    m_networkTable->horizontalHeader()->setStretchLastSection(true);
    m_networkTable->verticalHeader()->setVisible(false);
    m_networkTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_networkTable->setAlternatingRowColors(true);
    layout->addWidget(m_networkTable, 1);

    auto* hint = new QLabel(
        "Drop % / Error % are computed per poll interval as "
        "(dropped or errored packets) / (successful + dropped) × 100.");
    hint->setObjectName("sectionHint");
    hint->setWordWrap(true);
    layout->addWidget(hint);

    return container;
}

QWidget* MainWindow::buildConnectionsTab() {
    auto* container = new QWidget();
    auto* layout = new QVBoxLayout(container);

    auto* topBar = new QHBoxLayout();
    auto* searchLabel = new QLabel("Search:");
    searchLabel->setObjectName("metricSubtle");
    m_connectionsFilterEdit = new QLineEdit();
    m_connectionsFilterEdit->setPlaceholderText("Filter by process, address, port, or state...");
    connect(m_connectionsFilterEdit, &QLineEdit::textChanged, this, &MainWindow::onConnectionsFilterChanged);
    topBar->addWidget(searchLabel);
    topBar->addWidget(m_connectionsFilterEdit, 1);
    layout->addLayout(topBar);

    m_connectionsTable = new QTableWidget(0, 7);
    m_connectionsTable->setHorizontalHeaderLabels({
        "Process", "PID", "Protocol", "Local Address", "Remote Address", "State", "IP Version"
    });
    m_connectionsTable->horizontalHeader()->setStretchLastSection(true);
    m_connectionsTable->verticalHeader()->setVisible(false);
    m_connectionsTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_connectionsTable->setAlternatingRowColors(true);
    m_connectionsTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_connectionsTable->setSortingEnabled(true);
    layout->addWidget(m_connectionsTable, 1);

    m_connectionsSummaryLabel = new QLabel("0 connections");
    m_connectionsSummaryLabel->setObjectName("metricSubtle");
    layout->addWidget(m_connectionsSummaryLabel);

    auto* hint = new QLabel(
        "This is a connection-level view (who's connected to what, and how) -- "
        "it does not show bytes sent/received per process. Launch with "
        "--track-bandwidth for a per-process download/upload view (see the "
        "Bandwidth tab; requires Administrator on Windows).");
    hint->setObjectName("sectionHint");
    hint->setWordWrap(true);
    layout->addWidget(hint);

    return container;
}

QWidget* MainWindow::buildBandwidthTab() {
    auto* container = new QWidget();
    auto* layout = new QVBoxLayout(container);

    m_bandwidthStatusLabel = new QLabel("Starting bandwidth tracking...");
    m_bandwidthStatusLabel->setObjectName("sectionHint");
    m_bandwidthStatusLabel->setWordWrap(true);
    layout->addWidget(m_bandwidthStatusLabel);

    m_bandwidthTable = new QTableWidget(0, 5);
    m_bandwidthTable->setHorizontalHeaderLabels({
        "Process", "PID", "Download", "Upload", "Total (session)"
    });
    m_bandwidthTable->horizontalHeader()->setStretchLastSection(true);
    m_bandwidthTable->verticalHeader()->setVisible(false);
    m_bandwidthTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_bandwidthTable->setAlternatingRowColors(true);
    m_bandwidthTable->setSortingEnabled(true);
    layout->addWidget(m_bandwidthTable, 1);

    auto* hint = new QLabel(
        "TCP + UDP tracking (Linux: Netlink socket-diag + raw packet capture; "
        "Windows: TCP Extended Statistics API + ETW). Traffic in the brief "
        "window between a connection closing and the next poll is not "
        "counted (slightly under-, never over-counted). On Windows, TCP "
        "tracking covers IPv4 only.");
    hint->setObjectName("sectionHint");
    hint->setWordWrap(true);
    layout->addWidget(hint);

    return container;
}

// ---------------------------------------------------------------------------
// Polling
// ---------------------------------------------------------------------------
void MainWindow::pollProcesses() {
    QVector<ProcessInfo> processes = m_processCollector.collect();
    m_processModel->updateProcesses(processes);

    double totalCpu = 0;
    quint64 totalMem = 0;
    for (const auto& p : processes) { totalCpu += p.cpuPercent; totalMem += p.memRssBytes; }

    m_processSummaryLabel->setText(QString("%1 processes  •  Σ CPU %2  •  Σ RSS %3")
        .arg(processes.size())
        .arg(FormatUtils::percent(totalCpu))
        .arg(FormatUtils::bytes(totalMem)));

    // Process names may have changed (or new PIDs appeared) since the last
    // connections poll; re-render with cached connection data so the
    // Connections tab's process-name column stays current without waiting
    // for its own (slower) poll cycle.
    if (m_connectionsTable) {
        renderConnectionsTable(m_connectionsFilterEdit ? m_connectionsFilterEdit->text() : QString());
    }
}

void MainWindow::pollSystemStats() {
    CpuStats cpu = m_systemCollector.collectCpu();
    updateCpuUi(cpu);

    MemoryStats mem = m_systemCollector.collectMemory();
    updateMemoryUi(mem);

    QVector<DiskVolume> volumes = m_systemCollector.collectDiskVolumes();
    QVector<DiskIoStats> io = m_systemCollector.collectDiskIo();
    updateDiskUi(volumes, io);

    QVector<GpuInfo> gpus = m_gpuCollector.collect();
    updateGpuUi(gpus);

    NetworkStats net = m_networkCollector.collect();
    updateNetworkUi(net);
}

void MainWindow::pollConnections() {
    m_lastConnections = m_connectionCollector.collect();
    renderConnectionsTable(m_connectionsFilterEdit ? m_connectionsFilterEdit->text() : QString());
}

void MainWindow::pollBandwidth() {
    if (!m_bandwidthCollector || !m_bandwidthCollector->isRunning()) return;
    QMap<qint64, ProcessBandwidthStats> stats = m_bandwidthCollector->collect();
    updateBandwidthUi(stats);
}

// ---------------------------------------------------------------------------
// UI update helpers
// ---------------------------------------------------------------------------
void MainWindow::updateCpuUi(const CpuStats& cpu) {
    m_cpuTotalLabel->setText(FormatUtils::percent(cpu.totalPercent));
    m_cpuTempLabel->setText(cpu.temperatureC >= 0
        ? QString("Temp: %1°C").arg(cpu.temperatureC, 0, 'f', 1)
        : "Temp: n/a");
    m_cpuChart->pushValue(0, cpu.totalPercent);

    if (m_coreBars.size() != cpu.perCore.size()) {
        // Rebuild the per-core bar grid if core count changed (first run)
        auto* grid = qobject_cast<QGridLayout*>(m_coreBarContainer->layout());
        for (auto* bar : m_coreBars) bar->deleteLater();
        m_coreBars.clear();

        QLayoutItem* item;
        while ((item = grid->takeAt(0)) != nullptr) delete item;

        int cols = 4;
        for (int i = 0; i < cpu.perCore.size(); ++i) {
            auto* bar = new QProgressBar();
            bar->setRange(0, 100);
            bar->setFormat(QString("Core %1: %p%").arg(i));
            m_coreBars.push_back(bar);
            grid->addWidget(bar, i / cols, i % cols);
        }
    }
    for (int i = 0; i < cpu.perCore.size() && i < m_coreBars.size(); ++i) {
        m_coreBars[i]->setValue(int(cpu.perCore[i].percent));
        applyBarLevel(m_coreBars[i], cpu.perCore[i].percent);
    }
}

void MainWindow::updateMemoryUi(const MemoryStats& mem) {
    m_memLabel->setText(QString("%1 / %2  (%3)")
        .arg(FormatUtils::bytes(mem.usedBytes))
        .arg(FormatUtils::bytes(mem.totalBytes))
        .arg(FormatUtils::percent(mem.usedPercent())));
    m_memBar->setValue(int(mem.usedPercent()));
    applyBarLevel(m_memBar, mem.usedPercent());
    m_swapLabel->setText(QString("Swap: %1 / %2")
        .arg(FormatUtils::bytes(mem.swapUsedBytes))
        .arg(FormatUtils::bytes(mem.swapTotalBytes)));
    m_memChart->pushValue(0, mem.usedPercent());
}

void MainWindow::updateDiskUi(const QVector<DiskVolume>& volumes, const QVector<DiskIoStats>& io) {
    m_diskTable->setRowCount(volumes.size());
    for (int i = 0; i < volumes.size(); ++i) {
        const DiskVolume& v = volumes[i];
        m_diskTable->setItem(i, 0, new QTableWidgetItem(v.mountPoint));
        m_diskTable->setItem(i, 1, new QTableWidgetItem(v.fsType));
        m_diskTable->setItem(i, 2, new QTableWidgetItem(FormatUtils::bytes(v.usedBytes)));
        m_diskTable->setItem(i, 3, new QTableWidgetItem(FormatUtils::bytes(v.totalBytes)));
        auto* usageItem = new QTableWidgetItem(FormatUtils::percent(v.usedPercent()));
        usageItem->setForeground(QBrush(UiTheme::colorForPercent(v.usedPercent())));
        m_diskTable->setItem(i, 4, usageItem);
    }

    m_diskIoTable->setRowCount(io.size());
    for (int i = 0; i < io.size(); ++i) {
        const DiskIoStats& d = io[i];
        m_diskIoTable->setItem(i, 0, new QTableWidgetItem(d.device));
        m_diskIoTable->setItem(i, 1, new QTableWidgetItem(FormatUtils::rate(d.readBytesPerSec)));
        m_diskIoTable->setItem(i, 2, new QTableWidgetItem(FormatUtils::rate(d.writeBytesPerSec)));
        auto* utilItem = new QTableWidgetItem(FormatUtils::percent(d.utilizationPercent));
        utilItem->setForeground(QBrush(UiTheme::colorForPercent(d.utilizationPercent)));
        m_diskIoTable->setItem(i, 3, utilItem);
    }
}

void MainWindow::updateGpuUi(const QVector<GpuInfo>& gpus) {
    if (gpus.isEmpty()) {
        if (m_gpuPanels.isEmpty()) {
            auto* layout = qobject_cast<QVBoxLayout*>(m_gpuContainer->layout());
            auto* label = new QLabel("No supported GPU backend detected on this system.");
            label->setObjectName("metricSubtle");
            layout->addWidget(label);
            m_gpuPanels.push_back(label);
        }
        return;
    }

    if (m_gpuChartSeriesIndex < 0) {
        m_gpuChartSeriesIndex = m_gpuChart->addSeries(gpus[0].name, UiTheme::accentGpu());
    }

    if (m_gpuPanels.size() != gpus.size()) {
        auto* layout = qobject_cast<QVBoxLayout*>(m_gpuContainer->layout());
        for (auto* w : m_gpuPanels) w->deleteLater();
        m_gpuPanels.clear();

        for (const GpuInfo& g : gpus) {
            auto* panel = new QLabel();
            panel->setWordWrap(true);
            layout->addWidget(panel);
            m_gpuPanels.push_back(panel);
        }
    }

    for (int i = 0; i < gpus.size() && i < m_gpuPanels.size(); ++i) {
        const GpuInfo& g = gpus[i];
        auto* label = qobject_cast<QLabel*>(m_gpuPanels[i]);
        if (!label) continue;

        QString text = QString("<b>%1</b> (%2)<br>Load: %3   Memory: %4 / %5 (%6)")
            .arg(g.name.toHtmlEscaped())
            .arg(g.vendor)
            .arg(g.loadPercent >= 0 ? FormatUtils::percent(g.loadPercent) : "n/a")
            .arg(FormatUtils::bytes(g.memUsedBytes))
            .arg(g.memTotalBytes ? FormatUtils::bytes(g.memTotalBytes) : QString("n/a"))
            .arg(g.memTotalBytes ? FormatUtils::percent(g.memUsedPercent()) : QString("n/a"));

        if (g.temperatureC >= 0) text += QString("   Temp: %1°C").arg(g.temperatureC, 0, 'f', 1);
        if (g.powerWatts >= 0) text += QString("   Power: %1 W").arg(g.powerWatts, 0, 'f', 1);

        label->setText(text);
    }

    if (gpus[0].loadPercent >= 0) {
        m_gpuChart->pushValue(m_gpuChartSeriesIndex, gpus[0].loadPercent);
    }
}

void MainWindow::updateNetworkUi(const NetworkStats& net) {
    m_networkTotalsLabel->setText(QString("Total: ↓ %1   ↑ %2")
        .arg(FormatUtils::rate(net.totalRxBytesPerSec))
        .arg(FormatUtils::rate(net.totalTxBytesPerSec)));

    m_networkChart->pushValue(m_netRxSeriesIndex, double(net.totalRxBytesPerSec));
    m_networkChart->pushValue(m_netTxSeriesIndex, double(net.totalTxBytesPerSec));

    // Drop/error percentages are usually 0 on a healthy link, so color is
    // reserved for when something actually needs attention: neutral gray
    // at 0%, escalating through warn/critical as the rate climbs.
    auto colorForRate = [](double percent) -> QColor {
        if (percent <= 0.0) return UiTheme::textSecondary();
        if (percent >= 1.0) return UiTheme::levelCritical();
        return UiTheme::levelWarn();
    };

    m_networkTable->setRowCount(net.interfaces.size());
    for (int i = 0; i < net.interfaces.size(); ++i) {
        const NetworkInterfaceStats& ifs = net.interfaces[i];
        int col = 0;
        m_networkTable->setItem(i, col++, new QTableWidgetItem(ifs.name));

        auto* statusItem = new QTableWidgetItem(ifs.isUp ? "Up" : "Down");
        statusItem->setForeground(QBrush(ifs.isUp ? UiTheme::levelGood() : UiTheme::textTertiary()));
        m_networkTable->setItem(i, col++, statusItem);

        m_networkTable->setItem(i, col++, new QTableWidgetItem(ifs.ipv4Address));
        m_networkTable->setItem(i, col++, new QTableWidgetItem(
            ifs.linkSpeedMbps ? QString("%1 Mbps").arg(ifs.linkSpeedMbps) : "n/a"));
        m_networkTable->setItem(i, col++, new QTableWidgetItem(FormatUtils::rate(ifs.rxBytesPerSec)));
        m_networkTable->setItem(i, col++, new QTableWidgetItem(FormatUtils::rate(ifs.txBytesPerSec)));
        m_networkTable->setItem(i, col++, new QTableWidgetItem(
            ifs.linkSpeedMbps ? FormatUtils::percent(ifs.utilizationPercent) : "n/a"));

        auto addRateCell = [&](double percent) {
            auto* item = new QTableWidgetItem(FormatUtils::percent(percent, 3));
            item->setForeground(QBrush(colorForRate(percent)));
            m_networkTable->setItem(i, col++, item);
        };
        addRateCell(ifs.rxDropPercent);
        addRateCell(ifs.txDropPercent);
        addRateCell(ifs.rxErrorPercent);
        addRateCell(ifs.txErrorPercent);
    }
}

namespace {
// TCP state -> health color. ESTABLISHED is the "everything normal" case
// (colored green like other healthy states elsewhere in the UI); LISTEN is
// informational/neutral; the various teardown states get a warm color
// since a connection stuck in TIME_WAIT/CLOSE_WAIT for a long time can
// indicate a leak; UDP's "-" placeholder stays neutral.
QColor colorForConnectionState(const QString& state) {
    if (state == "ESTABLISHED") return UiTheme::levelGood();
    if (state == "LISTEN") return UiTheme::accentBrand();
    if (state == "-") return UiTheme::textSecondary();
    if (state == "SYN_SENT" || state == "SYN_RECV") return UiTheme::levelWarn();
    return UiTheme::textTertiary(); // TIME_WAIT, CLOSE_WAIT, CLOSING, LAST_ACK, ...
}
}

void MainWindow::renderConnectionsTable(const QString& filterText) {
    if (!m_connectionsTable) return;

    const QString needle = filterText.trimmed();
    QVector<const ProcessConnection*> visible;
    visible.reserve(m_lastConnections.size());

    for (const ProcessConnection& c : m_lastConnections) {
        if (needle.isEmpty()) {
            visible.push_back(&c);
            continue;
        }
        const QString processName = m_processModel ? m_processModel->nameForPid(c.pid) : QString();
        const QString haystack = QString("%1 %2 %3 %4 %5 %6")
            .arg(processName)
            .arg(c.pid)
            .arg(c.protocol)
            .arg(c.localAddress)
            .arg(c.remoteAddress)
            .arg(c.state);
        if (haystack.contains(needle, Qt::CaseInsensitive)) {
            visible.push_back(&c);
        }
    }

    m_connectionsTable->setSortingEnabled(false);
    m_connectionsTable->setRowCount(visible.size());

    QSet<qint64> distinctPids;
    for (int i = 0; i < visible.size(); ++i) {
        const ProcessConnection& c = *visible[i];
        distinctPids.insert(c.pid);

        const QString processName = m_processModel ? m_processModel->nameForPid(c.pid) : QString::number(c.pid);
        const QString localEndpoint = QString("%1:%2").arg(c.localAddress).arg(c.localPort);
        const QString remoteEndpoint = c.remoteAddress.isEmpty() || c.remotePort == 0
            ? QString("-")
            : QString("%1:%2").arg(c.remoteAddress).arg(c.remotePort);

        int col = 0;
        m_connectionsTable->setItem(i, col++, new QTableWidgetItem(processName));

        auto* pidItem = new QTableWidgetItem();
        pidItem->setData(Qt::DisplayRole, c.pid);
        m_connectionsTable->setItem(i, col++, pidItem);

        m_connectionsTable->setItem(i, col++, new QTableWidgetItem(c.protocol));
        m_connectionsTable->setItem(i, col++, new QTableWidgetItem(localEndpoint));
        m_connectionsTable->setItem(i, col++, new QTableWidgetItem(remoteEndpoint));

        auto* stateItem = new QTableWidgetItem(c.state);
        stateItem->setForeground(QBrush(colorForConnectionState(c.state)));
        m_connectionsTable->setItem(i, col++, stateItem);

        m_connectionsTable->setItem(i, col++, new QTableWidgetItem(c.isIPv6 ? "IPv6" : "IPv4"));
    }
    m_connectionsTable->setSortingEnabled(true);

    m_connectionsSummaryLabel->setText(QString("%1 connections across %2 processes")
        .arg(visible.size())
        .arg(distinctPids.size()));
}

void MainWindow::updateBandwidthUi(const QMap<qint64, ProcessBandwidthStats>& stats) {
    if (!m_bandwidthTable) return;

    QString statusText = stats.isEmpty()
        ? "Tracking active. No processes with measurable traffic yet."
        : QString("Tracking %1 process(es) with active traffic.").arg(stats.size());
    if (!m_bandwidthAvailabilityNote.isEmpty()) {
        statusText = m_bandwidthAvailabilityNote + "\n" + statusText;
    }
    m_bandwidthStatusLabel->setText(statusText);

    m_bandwidthTable->setSortingEnabled(false);
    m_bandwidthTable->setRowCount(stats.size());

    int row = 0;
    for (auto it = stats.constBegin(); it != stats.constEnd(); ++it, ++row) {
        qint64 pid = it.key();
        const ProcessBandwidthStats& s = it.value();
        QString name = m_processModel ? m_processModel->nameForPid(pid) : QString::number(pid);

        int col = 0;
        m_bandwidthTable->setItem(row, col++, new QTableWidgetItem(name));

        auto* pidItem = new QTableWidgetItem();
        pidItem->setData(Qt::DisplayRole, pid);
        m_bandwidthTable->setItem(row, col++, pidItem);

        auto* downItem = new QTableWidgetItem(FormatUtils::rate(s.rxBytesPerSec));
        m_bandwidthTable->setItem(row, col++, downItem);

        auto* upItem = new QTableWidgetItem(FormatUtils::rate(s.txBytesPerSec));
        m_bandwidthTable->setItem(row, col++, upItem);

        QString totalText = QString("↓ %1  ↑ %2")
            .arg(FormatUtils::bytes(s.rxBytesTotal))
            .arg(FormatUtils::bytes(s.txBytesTotal));
        m_bandwidthTable->setItem(row, col++, new QTableWidgetItem(totalText));
    }
    m_bandwidthTable->setSortingEnabled(true);
}

// ---------------------------------------------------------------------------
// Interaction
// ---------------------------------------------------------------------------
void MainWindow::onProcessFilterChanged(const QString& text) {
    m_processProxy->setFilterFixedString(text);
}

void MainWindow::onConnectionsFilterChanged(const QString& text) {
    renderConnectionsTable(text);
}

void MainWindow::onKillSelectedProcess() {
    QModelIndexList sel = m_processTable->selectionModel()->selectedRows();
    if (sel.isEmpty()) return;
    QModelIndex sourceIndex = m_processProxy->mapToSource(sel.first());
    const ProcessInfo* p = m_processModel->processAt(sourceIndex.row());
    if (!p) return;

    auto reply = QMessageBox::question(this, "End Task",
        QString("Terminate process \"%1\" (PID %2)?").arg(p->name).arg(p->pid));
    if (reply != QMessageBox::Yes) return;

    if (!m_processCollector.killProcess(p->pid)) {
        QMessageBox::warning(this, "End Task", "Failed to terminate process (insufficient permissions?).");
    }
}

void MainWindow::onProcessContextMenu(const QPoint& pos) {
    QModelIndex idx = m_processTable->indexAt(pos);
    if (!idx.isValid()) return;
    m_processTable->selectRow(idx.row());

    QMenu menu(this);
    QAction* killAction = menu.addAction("End Task");
    QAction* chosen = menu.exec(m_processTable->viewport()->mapToGlobal(pos));
    if (chosen == killAction) onKillSelectedProcess();
}

void MainWindow::applyBarLevel(QProgressBar* bar, double percent) {
    if (!bar) return;
    const QString level = UiTheme::levelNameForPercent(percent);
    if (bar->property("level").toString() == level) return; // avoid needless re-polish
    bar->setProperty("level", level);
    bar->style()->unpolish(bar);
    bar->style()->polish(bar);
    bar->update();
}
