#include "SettingsDialog.h"

#include <QComboBox>
#include <QSpinBox>
#include <QLabel>
#include <QFormLayout>
#include <QVBoxLayout>
#include <QDialogButtonBox>
#include <QSettings>

namespace {
constexpr int kMinRefreshMs = 200;
constexpr int kMaxRefreshMs = 10000;
constexpr int kDefaultRefreshMs = 1000;
}

SettingsDialog::SettingsDialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle("Settings");
    setModal(true);
    setMinimumWidth(380);

    QSettings settings;
    bool useBits = settings.value("network/rateUnitIsBits", true).toBool();
    int refreshMs = settings.value("general/refreshRateMs", kDefaultRefreshMs).toInt();

    auto* form = new QFormLayout();
    form->setSpacing(10);

    m_unitCombo = new QComboBox();
    m_unitCombo->addItem("Bits (bit/s, kbit/s, Mbit/s, Gbit/s)", static_cast<int>(FormatUtils::RateUnit::Bits));
    m_unitCombo->addItem("Bytes (B/s, KB/s, MB/s, GB/s)", static_cast<int>(FormatUtils::RateUnit::Bytes));
    m_unitCombo->setCurrentIndex(useBits ? 0 : 1);
    form->addRow("Data rate unit:", m_unitCombo);

    m_refreshSpin = new QSpinBox();
    m_refreshSpin->setRange(kMinRefreshMs, kMaxRefreshMs);
    m_refreshSpin->setSingleStep(100);
    m_refreshSpin->setSuffix(" ms");
    m_refreshSpin->setValue(refreshMs);
    form->addRow("Refresh rate:", m_refreshSpin);

    auto* unitHint = new QLabel(
        "Applies to all rate displays: network throughput, disk I/O, and "
        "per-process bandwidth. Cumulative totals (memory used, disk space, "
        "session totals) always stay in bytes.");
    unitHint->setObjectName("sectionHint");
    unitHint->setWordWrap(true);

    auto* refreshHint = new QLabel(
        "Applies to Processes, Performance, Network, Connections, and "
        "Bandwidth polling. Lower values update faster but use more CPU.");
    refreshHint->setObjectName("sectionHint");
    refreshHint->setWordWrap(true);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, this, [this]() {
        QSettings s;
        s.setValue("network/rateUnitIsBits",
            m_unitCombo->currentData().toInt() == static_cast<int>(FormatUtils::RateUnit::Bits));
        s.setValue("general/refreshRateMs", m_refreshSpin->value());
        accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &SettingsDialog::reject);

    auto* layout = new QVBoxLayout(this);
    layout->addLayout(form);
    layout->addWidget(unitHint);
    layout->addSpacing(4);
    layout->addWidget(refreshHint);
    layout->addStretch();
    layout->addWidget(buttons);
}

FormatUtils::RateUnit SettingsDialog::selectedRateUnit() const {
    return static_cast<FormatUtils::RateUnit>(m_unitCombo->currentData().toInt());
}

int SettingsDialog::selectedRefreshRateMs() const {
    return m_refreshSpin->value();
}
