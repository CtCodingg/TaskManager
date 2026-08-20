#include "MainWindow.h"
#include "ProcessModel.h"
#include "HistoryChartWidget.h"
#include "FormatUtils.h"

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
#include <QAction>
#include <QMessageBox>
#include <QGroupBox>
#include <QSplitter>
#include <QScrollArea>
#include <QStatusBar>
#include <QPushButton>
#include <QTableWidgetItem>

namespace {
constexpr int kProcessPollMs = 1500;
constexpr int kStatsPollMs = 1000;

QColor colorForIndex(int i) {
    static const QColor palette[] = {
        QColor("#4C9AFF"), QColor("#F97066"), QColor("#36B37E"),
        QColor("#FFAB00"), QColor("#9C6ADE"), QColor("#00B8D9")
    };
    return palette[i % 6];
}
}

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    setWindowTitle("Task Manager");
    resize(1200, 800);
    buildUi();

    connect(&m_processTimer, &QTimer::timeout, this, &MainWindow::pollProcesses);
    connect(&m_statsTimer, &QTimer::timeout, this, &MainWindow::pollSystemStats);
    m_processTimer.start(kProcessPollMs);
    m_statsTimer.start(kStatsPollMs);

    // Immediate first poll so the UI isn't empty on launch
    pollProcesses();
    pollSystemStats();

    statusBar()->showMessage("Ready");
}

MainWindow::~MainWindow() = default;

// ---------------------------------------------------------------------------
// UI construction
// ---------------------------------------------------------------------------
void MainWindow::buildUi() {
    auto* tabs = new QTabWidget(this);
    tabs->addTab(buildProcessesTab(), "Processes");
    tabs->addTab(buildPerformanceTab(), "Performance");
    tabs->addTab(buildNetworkTab(), "Network");
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
    connect(killButton, &QPushButton::clicked, this, &MainWindow::onKillSelectedProcess);

    topBar->addWidget(new QLabel("Search:"));
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
    auto* cpuLayout = new QVBoxLayout(cpuGroup);
    auto* cpuHeaderRow = new QHBoxLayout();
    m_cpuTotalLabel = new QLabel("0.0%");
    m_cpuTotalLabel->setStyleSheet("font-size: 20px; font-weight: 600;");
    m_cpuTempLabel = new QLabel("Temp: n/a");
    cpuHeaderRow->addWidget(m_cpuTotalLabel);
    cpuHeaderRow->addStretch();
    cpuHeaderRow->addWidget(m_cpuTempLabel);
    cpuLayout->addLayout(cpuHeaderRow);

    m_cpuChart = new HistoryChartWidget("CPU Load (%)", 120);
    m_cpuChart->setYRange(0, 100);
    m_cpuChart->addSeries("Total", colorForIndex(0));
    cpuLayout->addWidget(m_cpuChart);

    m_coreBarContainer = new QWidget();
    auto* coreGrid = new QGridLayout(m_coreBarContainer);
    coreGrid->setSpacing(4);
    cpuLayout->addWidget(m_coreBarContainer);

    layout->addWidget(cpuGroup);

    // --- Memory group ---
    auto* memGroup = new QGroupBox("Memory");
    auto* memLayout = new QVBoxLayout(memGroup);
    m_memLabel = new QLabel("0 / 0 GB");
    m_memLabel->setStyleSheet("font-size: 16px; font-weight: 600;");
    m_memBar = new QProgressBar();
    m_memBar->setRange(0, 100);
    m_swapLabel = new QLabel("Swap: 0 / 0 GB");
    memLayout->addWidget(m_memLabel);
    memLayout->addWidget(m_memBar);
    memLayout->addWidget(m_swapLabel);

    m_memChart = new HistoryChartWidget("Memory Usage (%)", 120);
    m_memChart->setYRange(0, 100);
    m_memChart->addSeries("Used", colorForIndex(1));
    memLayout->addWidget(m_memChart);

    layout->addWidget(memGroup);

    // --- GPU group ---
    auto* gpuGroup = new QGroupBox("GPU");
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
    auto* diskLayout = new QVBoxLayout(diskGroup);

    m_diskTable = new QTableWidget(0, 5);
    m_diskTable->setHorizontalHeaderLabels({"Mount", "Filesystem", "Used", "Total", "Usage"});
    m_diskTable->horizontalHeader()->setStretchLastSection(true);
    m_diskTable->verticalHeader()->setVisible(false);
    m_diskTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_diskTable->setMinimumHeight(140);
    diskLayout->addWidget(new QLabel("Volumes"));
    diskLayout->addWidget(m_diskTable);

    m_diskIoTable = new QTableWidget(0, 4);
    m_diskIoTable->setHorizontalHeaderLabels({"Device", "Read/s", "Write/s", "Utilization"});
    m_diskIoTable->horizontalHeader()->setStretchLastSection(true);
    m_diskIoTable->verticalHeader()->setVisible(false);
    m_diskIoTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_diskIoTable->setMinimumHeight(120);
    diskLayout->addWidget(new QLabel("I/O Activity"));
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
    m_networkTotalsLabel->setStyleSheet("font-size: 16px; font-weight: 600;");
    layout->addWidget(m_networkTotalsLabel);

    m_networkChart = new HistoryChartWidget("Total Bandwidth", 120);
    m_networkChart->setYAutoScale(true);
    m_netRxSeriesIndex = m_networkChart->addSeries("Download (B/s)", colorForIndex(0));
    m_netTxSeriesIndex = m_networkChart->addSeries("Upload (B/s)", colorForIndex(1));
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
    hint->setStyleSheet("color: gray;");
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
    }
}

void MainWindow::updateMemoryUi(const MemoryStats& mem) {
    m_memLabel->setText(QString("%1 / %2  (%3)")
        .arg(FormatUtils::bytes(mem.usedBytes))
        .arg(FormatUtils::bytes(mem.totalBytes))
        .arg(FormatUtils::percent(mem.usedPercent())));
    m_memBar->setValue(int(mem.usedPercent()));
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
        m_diskTable->setItem(i, 4, new QTableWidgetItem(FormatUtils::percent(v.usedPercent())));
    }

    m_diskIoTable->setRowCount(io.size());
    for (int i = 0; i < io.size(); ++i) {
        const DiskIoStats& d = io[i];
        m_diskIoTable->setItem(i, 0, new QTableWidgetItem(d.device));
        m_diskIoTable->setItem(i, 1, new QTableWidgetItem(FormatUtils::bytesPerSec(d.readBytesPerSec)));
        m_diskIoTable->setItem(i, 2, new QTableWidgetItem(FormatUtils::bytesPerSec(d.writeBytesPerSec)));
        m_diskIoTable->setItem(i, 3, new QTableWidgetItem(FormatUtils::percent(d.utilizationPercent)));
    }
}

void MainWindow::updateGpuUi(const QVector<GpuInfo>& gpus) {
    if (gpus.isEmpty()) {
        if (m_gpuPanels.isEmpty()) {
            auto* layout = qobject_cast<QVBoxLayout*>(m_gpuContainer->layout());
            auto* label = new QLabel("No supported GPU backend detected on this system.");
            label->setStyleSheet("color: gray;");
            layout->addWidget(label);
            m_gpuPanels.push_back(label);
        }
        return;
    }

    if (m_gpuChartSeriesIndex < 0) {
        m_gpuChartSeriesIndex = m_gpuChart->addSeries(gpus[0].name, colorForIndex(2));
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
        .arg(FormatUtils::bytesPerSec(net.totalRxBytesPerSec))
        .arg(FormatUtils::bytesPerSec(net.totalTxBytesPerSec)));

    m_networkChart->pushValue(m_netRxSeriesIndex, double(net.totalRxBytesPerSec));
    m_networkChart->pushValue(m_netTxSeriesIndex, double(net.totalTxBytesPerSec));

    m_networkTable->setRowCount(net.interfaces.size());
    for (int i = 0; i < net.interfaces.size(); ++i) {
        const NetworkInterfaceStats& ifs = net.interfaces[i];
        int col = 0;
        m_networkTable->setItem(i, col++, new QTableWidgetItem(ifs.name));
        m_networkTable->setItem(i, col++, new QTableWidgetItem(ifs.isUp ? "Up" : "Down"));
        m_networkTable->setItem(i, col++, new QTableWidgetItem(ifs.ipv4Address));
        m_networkTable->setItem(i, col++, new QTableWidgetItem(
            ifs.linkSpeedMbps ? QString("%1 Mbps").arg(ifs.linkSpeedMbps) : "n/a"));
        m_networkTable->setItem(i, col++, new QTableWidgetItem(FormatUtils::bytesPerSec(ifs.rxBytesPerSec)));
        m_networkTable->setItem(i, col++, new QTableWidgetItem(FormatUtils::bytesPerSec(ifs.txBytesPerSec)));
        m_networkTable->setItem(i, col++, new QTableWidgetItem(
            ifs.linkSpeedMbps ? FormatUtils::percent(ifs.utilizationPercent) : "n/a"));
        m_networkTable->setItem(i, col++, new QTableWidgetItem(FormatUtils::percent(ifs.rxDropPercent, 3)));
        m_networkTable->setItem(i, col++, new QTableWidgetItem(FormatUtils::percent(ifs.txDropPercent, 3)));
        m_networkTable->setItem(i, col++, new QTableWidgetItem(FormatUtils::percent(ifs.rxErrorPercent, 3)));
        m_networkTable->setItem(i, col++, new QTableWidgetItem(FormatUtils::percent(ifs.txErrorPercent, 3)));
    }
}

// ---------------------------------------------------------------------------
// Interaction
// ---------------------------------------------------------------------------
void MainWindow::onProcessFilterChanged(const QString& text) {
    m_processProxy->setFilterFixedString(text);
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
