#pragma once

#include <QString>
#include <cstdint>

namespace FormatUtils {

// "1.23 GB", "456 MB", etc. (binary, 1024-based) -- used for cumulative
// amounts (memory used, disk space, session totals), which are NOT
// affected by the rate-unit setting below.
QString bytes(quint64 value);

// "1.23 MB/s" -- always bytes, regardless of the rate-unit setting. Prefer
// rate() below for anything the user sees; this is kept for callers that
// specifically need bytes.
QString bytesPerSec(quint64 value);

// "12.3 Mbit/s" (bits, decimal/1000-based, standard networking convention)
// -- always bits, regardless of the rate-unit setting.
QString bitsPerSec(quint64 bytesPerSecValue);

// User-facing preference for how *rate* values (things measured per
// second: network throughput, disk I/O, per-process bandwidth) are
// displayed. Does NOT affect cumulative byte amounts (bytes() above) --
// "500 kbit downloaded this session" isn't how anyone reads a data total,
// so those always stay in bytes/KB/MB/GB.
enum class RateUnit {
    Bits,   // bit/s, kbit/s, Mbit/s, Gbit/s (the default)
    Bytes,  // B/s, KB/s, MB/s, GB/s
};

// Sets/gets the current process-wide rate unit preference. Call
// setRateUnit() once at startup (from persisted settings) and again
// whenever the user changes it in the Settings dialog; rate() picks it up
// immediately on the next call, no restart needed.
void setRateUnit(RateUnit unit);
RateUnit rateUnit();

// Formats a bytes-per-second value using the CURRENT rate unit preference
// (see setRateUnit()). This is what every rate display in the UI
// (network throughput, disk I/O, per-process bandwidth) should call.
QString rate(quint64 bytesPerSecValue);

// "42.3%"
QString percent(double value, int decimals = 1);

// "00:12:34" from milliseconds uptime etc.
QString duration(qint64 seconds);

} // namespace FormatUtils
