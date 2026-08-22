#include "ProcessModel.h"
#include "FormatUtils.h"
#include "UiTheme.h"

#include <QFont>
#include <QBrush>
#include <algorithm>

ProcessModel::ProcessModel(QObject* parent) : QAbstractTableModel(parent) {}

void ProcessModel::updateProcesses(const QVector<ProcessInfo>& processes) {
    beginResetModel();
    m_processes = processes;
    m_pidToName.clear();
    for (const ProcessInfo& p : m_processes) {
        m_pidToName[p.pid] = p.name;
    }
    applySort();
    endResetModel();
}

const ProcessInfo* ProcessModel::processAt(int row) const {
    if (row < 0 || row >= m_processes.size()) return nullptr;
    return &m_processes[row];
}

qint64 ProcessModel::pidForRow(int row) const {
    const ProcessInfo* p = processAt(row);
    return p ? p->pid : -1;
}

QString ProcessModel::nameForPid(qint64 pid) const {
    auto it = m_pidToName.constFind(pid);
    if (it != m_pidToName.constEnd()) return it.value();
    return QString("(pid %1)").arg(pid);
}

int ProcessModel::rowCount(const QModelIndex& parent) const {
    if (parent.isValid()) return 0;
    return m_processes.size();
}

int ProcessModel::columnCount(const QModelIndex& parent) const {
    if (parent.isValid()) return 0;
    return ColumnCount;
}

namespace {

// State text is platform-specific (Linux: Running/Sleeping/Zombie/...,
// Windows: mostly "Running") -- color by the parts of that vocabulary
// that signal something worth noticing, and leave everything else at the
// neutral secondary text color.
QColor colorForState(const QString& state) {
    if (state == "Running") return UiTheme::levelGood();
    if (state == "Zombie" || state == "Dead") return UiTheme::levelCritical();
    if (state == "Stopped" || state == "Tracing Stop") return UiTheme::levelWarn();
    return UiTheme::textSecondary(); // Sleeping, Disk Wait, Idle, Unknown, ...
}

} // namespace

QVariant ProcessModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() >= m_processes.size()) return QVariant();
    const ProcessInfo& p = m_processes[index.row()];

    if (role == Qt::TextAlignmentRole) {
        switch (index.column()) {
            case ColPid: case ColCpu: case ColMemRss: case ColThreads: case ColNice:
                return int(Qt::AlignRight | Qt::AlignVCenter);
            default:
                return int(Qt::AlignLeft | Qt::AlignVCenter);
        }
    }

    if (role == Qt::ForegroundRole) {
        switch (index.column()) {
            case ColCpu:
                return QBrush(UiTheme::colorForPercent(p.cpuPercent));
            case ColState:
                return QBrush(colorForState(p.state));
            default:
                return QVariant();
        }
    }

    if (role == Qt::FontRole) {
        switch (index.column()) {
            case ColPid: case ColCpu: case ColMemRss: case ColThreads: case ColNice: {
                QFont f("Consolas");
                f.setStyleHint(QFont::Monospace);
                return f;
            }
            default:
                return QVariant();
        }
    }

    if (role != Qt::DisplayRole) return QVariant();

    switch (index.column()) {
        case ColPid: return p.pid;
        case ColName: return p.name;
        case ColUser: return p.user;
        case ColState: return p.state;
        case ColCpu: return FormatUtils::percent(p.cpuPercent);
        case ColMemRss: return FormatUtils::bytes(p.memRssBytes);
        case ColThreads: return p.threadCount;
        case ColNice: return p.niceValue;
        case ColCommand: return p.commandLine;
        default: return QVariant();
    }
}

QVariant ProcessModel::headerData(int section, Qt::Orientation orientation, int role) const {
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole) return QVariant();
    switch (section) {
        case ColPid: return "PID";
        case ColName: return "Name";
        case ColUser: return "User";
        case ColState: return "State";
        case ColCpu: return "CPU %";
        case ColMemRss: return "Memory";
        case ColThreads: return "Threads";
        case ColNice: return "Priority";
        case ColCommand: return "Command";
        default: return QVariant();
    }
}

void ProcessModel::sort(int column, Qt::SortOrder order) {
    m_sortColumn = column;
    m_sortOrder = order;
    beginResetModel();
    applySort();
    endResetModel();
}

void ProcessModel::applySort() {
    const int col = m_sortColumn;
    const bool asc = (m_sortOrder == Qt::AscendingOrder);

    auto lessThan = [col](const ProcessInfo& a, const ProcessInfo& b) -> bool {
        switch (col) {
            case ColPid:     return a.pid < b.pid;
            case ColName:    return a.name.compare(b.name, Qt::CaseInsensitive) < 0;
            case ColUser:    return a.user.compare(b.user, Qt::CaseInsensitive) < 0;
            case ColState:   return a.state < b.state;
            case ColCpu:     return a.cpuPercent < b.cpuPercent;
            case ColMemRss:  return a.memRssBytes < b.memRssBytes;
            case ColThreads: return a.threadCount < b.threadCount;
            case ColNice:    return a.niceValue < b.niceValue;
            case ColCommand: return a.commandLine.compare(b.commandLine, Qt::CaseInsensitive) < 0;
            default:         return a.pid < b.pid;
        }
    };

    std::stable_sort(m_processes.begin(), m_processes.end(),
        [&lessThan, asc](const ProcessInfo& a, const ProcessInfo& b) {
            return asc ? lessThan(a, b) : lessThan(b, a);
        });
}
