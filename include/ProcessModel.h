#pragma once

#include "Types.h"
#include <QAbstractTableModel>
#include <QVector>
#include <QMap>
#include <QString>

class ProcessModel : public QAbstractTableModel {
    Q_OBJECT
public:
    enum Column {
        ColPid = 0,
        ColName,
        ColUser,
        ColState,
        ColCpu,
        ColMemRss,
        ColThreads,
        ColNice,
        ColCommand,
        ColumnCount
    };

    explicit ProcessModel(QObject* parent = nullptr);

    void updateProcesses(const QVector<ProcessInfo>& processes);
    const ProcessInfo* processAt(int row) const;
    qint64 pidForRow(int row) const;

    // Looks up the process name for a PID from the most recently collected
    // process list. Used by the Connections tab to show a process name next
    // to each connection without any extra system calls. Returns a
    // placeholder like "(pid 1234)" if the PID isn't currently known (e.g.
    // the process exited between polls).
    QString nameForPid(qint64 pid) const;

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;
    void sort(int column, Qt::SortOrder order) override;

private:
    QVector<ProcessInfo> m_processes;
    QMap<qint64, QString> m_pidToName;
    int m_sortColumn = ColCpu;
    Qt::SortOrder m_sortOrder = Qt::DescendingOrder;

    void applySort();
};
