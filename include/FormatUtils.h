#pragma once

#include <QString>
#include <cstdint>

namespace FormatUtils {

// "1.23 GB", "456 MB", etc. (binary, 1024-based)
QString bytes(quint64 value);

// "1.23 MB/s"
QString bytesPerSec(quint64 value);

// "12.3 Mbps" (bits, decimal, for link speeds / bandwidth in networking convention)
QString bitsPerSec(quint64 bytesPerSecValue);

// "42.3%"
QString percent(double value, int decimals = 1);

// "00:12:34" from milliseconds uptime etc.
QString duration(qint64 seconds);

} // namespace FormatUtils
