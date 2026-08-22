#pragma once

#include <QDialog>
#include "FormatUtils.h"

class QComboBox;
class QSpinBox;

// A small preferences dialog covering the two user-configurable settings
// this app has: how rate values (network/disk/bandwidth throughput) are
// displayed, and how often the UI polls for fresh data. Persists to
// QSettings on accept (OK); MainWindow re-reads QSettings and re-applies
// the effects (FormatUtils::setRateUnit + QTimer intervals) after the
// dialog closes with QDialog::Accepted.
class SettingsDialog : public QDialog {
    Q_OBJECT
public:
    explicit SettingsDialog(QWidget* parent = nullptr);

    FormatUtils::RateUnit selectedRateUnit() const;
    int selectedRefreshRateMs() const;

private:
    QComboBox* m_unitCombo = nullptr;
    QSpinBox* m_refreshSpin = nullptr;
};
