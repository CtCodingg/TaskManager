#pragma once

#include "Types.h"
#include <QAbstractTableModel>
#include <QVector>

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

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;
    void sort(int column, Qt::SortOrder order) override;

private:
    QVector<ProcessInfo> m_processes;
    int m_sortColumn = ColCpu;
    Qt::SortOrder m_sortOrder = Qt::DescendingOrder;

    void applySort();
};
