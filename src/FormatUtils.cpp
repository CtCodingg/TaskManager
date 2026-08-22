#include "FormatUtils.h"
#include <QString>
#include <cmath>

namespace FormatUtils {

static QString scaledUnits(double value, const char* const* units, int unitCount, double base) {
    int unitIndex = 0;
    while (value >= base && unitIndex < unitCount - 1) {
        value /= base;
        ++unitIndex;
    }
    return QString::number(value, 'f', (unitIndex == 0) ? 0 : 2) + " " + units[unitIndex];
}

QString bytes(quint64 value) {
    static const char* const units[] = {"B", "KB", "MB", "GB", "TB", "PB"};
    return scaledUnits(static_cast<double>(value), units, 6, 1024.0);
}

QString bytesPerSec(quint64 value) {
    return bytes(value) + "/s";
}

QString bitsPerSec(quint64 bytesPerSecValue) {
    static const char* const units[] = {"bit/s", "kbit/s", "Mbit/s", "Gbit/s"};
    double bits = static_cast<double>(bytesPerSecValue) * 8.0;
    return scaledUnits(bits, units, 4, 1000.0);
}

namespace {
RateUnit g_rateUnit = RateUnit::Bits; // default: bits, per the user-facing default
}

void setRateUnit(RateUnit unit) {
    g_rateUnit = unit;
}

RateUnit rateUnit() {
    return g_rateUnit;
}

QString rate(quint64 bytesPerSecValue) {
    return (g_rateUnit == RateUnit::Bits) ? bitsPerSec(bytesPerSecValue) : bytesPerSec(bytesPerSecValue);
}

QString percent(double value, int decimals) {
    return QString::number(value, 'f', decimals) + "%";
}

QString duration(qint64 seconds) {
    if (seconds < 0) seconds = 0;
    qint64 h = seconds / 3600;
    qint64 m = (seconds % 3600) / 60;
    qint64 s = seconds % 60;
    return QString("%1:%2:%3")
        .arg(h, 2, 10, QChar('0'))
        .arg(m, 2, 10, QChar('0'))
        .arg(s, 2, 10, QChar('0'));
}

} // namespace FormatUtils
