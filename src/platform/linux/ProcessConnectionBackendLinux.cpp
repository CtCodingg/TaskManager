// Linux implementation of ProcessConnectionCollector.
//
// The kernel exposes all TCP/UDP sockets system-wide in /proc/net/{tcp,tcp6,
// udp,udp6} (fixed-width text tables, no special privileges needed to read
// your own connections). Each line includes a socket inode number but NOT
// the owning PID. To find the PID, we scan every /proc/<pid>/fd/* symlink
// looking for targets of the form "socket:[<inode>]" and build an
// inode -> pid map (this is the same technique lsof/ss/nethogs use under
// the hood). Connections whose fd we can't see (another user's process,
// when not running as root) are simply skipped -- same behavior as `ss`
// run as a normal user.
//
// This gives connection-level detail (who's connected to what) but NOT
// byte counters -- /proc/net/tcp's tx_queue/rx_queue fields are queued
// bytes at this instant, not cumulative transferred bytes. True
// per-process bandwidth would require Netlink INET_DIAG with the
// TCP_INFO extension (tcpi_bytes_acked/tcpi_bytes_received), which is a
// planned future addition.

#include "ProcessConnectionCollector.h"

#include <QDir>
#include <QFile>
#include <QTextStream>
#include <QStringList>
#include <QMap>

namespace {

QString tcpStateName(int stateHex) {
    switch (stateHex) {
        case 0x01: return "ESTABLISHED";
        case 0x02: return "SYN_SENT";
        case 0x03: return "SYN_RECV";
        case 0x04: return "FIN_WAIT1";
        case 0x05: return "FIN_WAIT2";
        case 0x06: return "TIME_WAIT";
        case 0x07: return "CLOSE";
        case 0x08: return "CLOSE_WAIT";
        case 0x09: return "LAST_ACK";
        case 0x0A: return "LISTEN";
        case 0x0B: return "CLOSING";
        default:   return "UNKNOWN";
    }
}

// Parses the 8-hex-char IPv4 address field from /proc/net/tcp (stored as a
// little-endian uint32, so the byte order in the string is reversed
// relative to normal dotted-quad reading order).
QString parseIPv4(const QString& hex) {
    if (hex.size() != 8) return QString();
    bool ok = true;
    quint8 b[4];
    for (int i = 0; i < 4; ++i) {
        b[i] = static_cast<quint8>(hex.mid(i * 2, 2).toUInt(&ok, 16));
        if (!ok) return QString();
    }
    // Little-endian: reverse byte order to get the actual address.
    return QString("%1.%2.%3.%4").arg(b[3]).arg(b[2]).arg(b[1]).arg(b[0]);
}

// Parses the 32-hex-char IPv6 address field (4 little-endian 32-bit words).
// Produces a plain 8-group hex form with a minimal "::" compression for the
// single longest run of zero groups -- not full RFC 5952 canonicalization,
// but readable and correct.
QString parseIPv6(const QString& hex) {
    if (hex.size() != 32) return QString();
    quint16 groups[8];
    bool ok = true;
    for (int word = 0; word < 4; ++word) {
        QString wordHex = hex.mid(word * 8, 8);
        quint8 b[4];
        for (int i = 0; i < 4; ++i) {
            b[i] = static_cast<quint8>(wordHex.mid(i * 2, 2).toUInt(&ok, 16));
            if (!ok) return QString();
        }
        // Each 32-bit word is stored little-endian; after byte-reversal the
        // word splits into two big-endian 16-bit groups.
        groups[word * 2]     = (quint16(b[3]) << 8) | b[2];
        groups[word * 2 + 1] = (quint16(b[1]) << 8) | b[0];
    }

    // Find the longest run (length >= 2) of zero groups to collapse to "::"
    int bestStart = -1, bestLen = 0;
    int curStart = -1, curLen = 0;
    for (int i = 0; i < 8; ++i) {
        if (groups[i] == 0) {
            if (curStart < 0) curStart = i;
            ++curLen;
            if (curLen > bestLen) { bestLen = curLen; bestStart = curStart; }
        } else {
            curStart = -1;
            curLen = 0;
        }
    }

    QStringList parts;
    if (bestLen >= 2) {
        for (int i = 0; i < bestStart; ++i) parts << QString::number(groups[i], 16);
        QString result = parts.join(':');
        result += "::";
        parts.clear();
        for (int i = bestStart + bestLen; i < 8; ++i) parts << QString::number(groups[i], 16);
        result += parts.join(':');
        return result;
    }

    for (int i = 0; i < 8; ++i) parts << QString::number(groups[i], 16);
    return parts.join(':');
}

struct RawConn {
    QString localAddr;
    quint16 localPort = 0;
    QString remoteAddr;
    quint16 remotePort = 0;
    int stateHex = 0;
    quint64 inode = 0;
};

QVector<RawConn> parseProcNetFile(const QString& path, bool isIPv6) {
    QVector<RawConn> result;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return result;

    QTextStream ts(&f);
    QString line;
    int lineNo = 0;
    while (ts.readLineInto(&line)) {
        ++lineNo;
        if (lineNo == 1) continue; // header
        QStringList parts = line.trimmed().split(' ', Qt::SkipEmptyParts);
        if (parts.size() < 10) continue;

        // parts[1] = "localhex:porthex", parts[2] = "remotehex:porthex",
        // parts[3] = state (hex), parts[9] = inode
        QStringList localParts = parts[1].split(':');
        QStringList remoteParts = parts[2].split(':');
        if (localParts.size() != 2 || remoteParts.size() != 2) continue;

        RawConn conn;
        conn.localAddr = isIPv6 ? parseIPv6(localParts[0]) : parseIPv4(localParts[0]);
        bool ok = false;
        conn.localPort = static_cast<quint16>(localParts[1].toUInt(&ok, 16));
        conn.remoteAddr = isIPv6 ? parseIPv6(remoteParts[0]) : parseIPv4(remoteParts[0]);
        conn.remotePort = static_cast<quint16>(remoteParts[1].toUInt(&ok, 16));
        conn.stateHex = parts[3].toInt(&ok, 16);
        conn.inode = parts[9].toULongLong();

        result.push_back(conn);
    }
    return result;
}

// Scans every /proc/<pid>/fd/* symlink and builds inode -> pid. Skips PIDs
// we don't have permission to read (belonging to other users) silently,
// same as running `ss`/`lsof` without root.
QMap<quint64, qint64> buildInodeToPidMap() {
    QMap<quint64, qint64> result;
    QDir procDir("/proc");
    const QStringList entries = procDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot);

    for (const QString& entry : entries) {
        bool isPid = false;
        qint64 pid = entry.toLongLong(&isPid);
        if (!isPid) continue;

        QDir fdDir(QString("/proc/%1/fd").arg(pid));
        const QStringList fds = fdDir.entryList(QDir::Files | QDir::System | QDir::NoDotAndDotDot);
        for (const QString& fd : fds) {
            QString linkPath = fdDir.filePath(fd);
            QString target = QFile::symLinkTarget(linkPath);
            if (target.isEmpty() || !target.startsWith("socket:[")) continue;

            QString inodeStr = target.mid(8, target.size() - 9); // strip "socket:[" and "]"
            bool ok = false;
            quint64 inode = inodeStr.toULongLong(&ok);
            if (ok) result[inode] = pid;
        }
    }
    return result;
}

} // namespace

class ProcessConnectionCollector::Impl {};

ProcessConnectionCollector::ProcessConnectionCollector() : m_impl(new Impl()) {}
ProcessConnectionCollector::~ProcessConnectionCollector() { delete m_impl; }

QVector<ProcessConnection> ProcessConnectionCollector::collect() {
    QVector<ProcessConnection> result;

    QMap<quint64, qint64> inodeToPid = buildInodeToPidMap();

    struct Source { QString path; QString protocol; bool isIPv6; };
    static const Source sources[] = {
        {"/proc/net/tcp",  "TCP", false},
        {"/proc/net/tcp6", "TCP", true},
        {"/proc/net/udp",  "UDP", false},
        {"/proc/net/udp6", "UDP", true},
    };

    for (const Source& src : sources) {
        QVector<RawConn> raw = parseProcNetFile(src.path, src.isIPv6);
        for (const RawConn& c : raw) {
            auto pidIt = inodeToPid.constFind(c.inode);
            if (pidIt == inodeToPid.constEnd()) continue; // not ours / no permission

            ProcessConnection pc;
            pc.pid = pidIt.value();
            pc.protocol = src.protocol;
            pc.localAddress = c.localAddr;
            pc.localPort = c.localPort;
            pc.remoteAddress = c.remoteAddr;
            pc.remotePort = c.remotePort;
            pc.isIPv6 = src.isIPv6;
            pc.state = (src.protocol == "TCP") ? tcpStateName(c.stateHex) : "-";

            result.push_back(pc);
        }
    }

    return result;
}
