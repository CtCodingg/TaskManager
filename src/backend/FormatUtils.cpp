#include "FormatUtils.h"
#include <cstdio>

namespace FormatUtils {

namespace {

std::string scaledUnits(double value, const char* const* units, int unitCount, double base) {
    int unitIndex = 0;
    while (value >= base && unitIndex < unitCount - 1) {
        value /= base;
        ++unitIndex;
    }
    char buf[64];
    if (unitIndex == 0) {
        std::snprintf(buf, sizeof(buf), "%.0f %s", value, units[unitIndex]);
    } else {
        std::snprintf(buf, sizeof(buf), "%.2f %s", value, units[unitIndex]);
    }
    return std::string(buf);
}

RateUnit g_rateUnit = RateUnit::Bits;

} // namespace

std::string bytes(uint64_t value) {
    static const char* const units[] = {"B", "KB", "MB", "GB", "TB", "PB"};
    return scaledUnits(static_cast<double>(value), units, 6, 1024.0);
}

std::string bytesPerSec(uint64_t value) {
    return bytes(value) + "/s";
}

std::string bitsPerSec(uint64_t bytesPerSecValue) {
    static const char* const units[] = {"bit/s", "kbit/s", "Mbit/s", "Gbit/s"};
    double bits = static_cast<double>(bytesPerSecValue) * 8.0;
    return scaledUnits(bits, units, 4, 1000.0);
}

void setRateUnit(RateUnit unit) { g_rateUnit = unit; }
RateUnit rateUnit() { return g_rateUnit; }

std::string rate(uint64_t bytesPerSecValue) {
    return (g_rateUnit == RateUnit::Bits) ? bitsPerSec(bytesPerSecValue) : bytesPerSec(bytesPerSecValue);
}

std::string percent(double value, int decimals) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.*f%%", decimals, value);
    return std::string(buf);
}

std::string duration(int64_t seconds) {
    if (seconds < 0) seconds = 0;
    int64_t h = seconds / 3600;
    int64_t m = (seconds % 3600) / 60;
    int64_t s = seconds % 60;
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%02lld:%02lld:%02lld",
                  static_cast<long long>(h), static_cast<long long>(m), static_cast<long long>(s));
    return std::string(buf);
}

} // namespace FormatUtils
