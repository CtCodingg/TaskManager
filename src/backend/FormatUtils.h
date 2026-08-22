#pragma once

#include <cstdint>
#include <string>

namespace FormatUtils {

std::string bytes(uint64_t value);
std::string bytesPerSec(uint64_t value);
std::string bitsPerSec(uint64_t bytesPerSecValue);

enum class RateUnit { Bits, Bytes };
void setRateUnit(RateUnit unit);
RateUnit rateUnit();
std::string rate(uint64_t bytesPerSecValue);

std::string percent(double value, int decimals = 1);
std::string duration(int64_t seconds);

} // namespace FormatUtils
