#include "network/network_state/feature_normalizer.h"

#include <cmath>

namespace aafr::netstate {

std::optional<double> ToCanonical(double value, const std::string& unit, Unit target) {
  if (!std::isfinite(value)) return std::nullopt;
  switch (target) {
    case Unit::Milliseconds:
      if (unit == "ms") return value;
      if (unit == "s") return value * 1000.0;
      if (unit == "us") return value / 1000.0;
      if (unit == "ns") return value / 1e6;
      return std::nullopt;
    case Unit::Mbps:
      if (unit == "Mbps") return value;
      if (unit == "kbps") return value / 1000.0;
      if (unit == "bps") return value / 1e6;
      if (unit == "Gbps") return value * 1000.0;
      return std::nullopt;
    case Unit::Ratio:
      if (unit == "ratio") return value;
      if (unit == "percent") return value / 100.0;
      return std::nullopt;
    case Unit::Percent:
      if (unit == "percent") return value;
      if (unit == "ratio") return value * 100.0;
      return std::nullopt;
    case Unit::Bytes:
      if (unit == "bytes") return value;
      if (unit == "kB") return value * 1000.0;
      return std::nullopt;
    case Unit::Count:
      // V0.3 names what is counted; any of these is a plain count.
      if (unit == "packets" || unit == "segments" || unit == "responses" || unit == "count" ||
          unit == "connections") {
        return value;
      }
      return std::nullopt;
  }
  return std::nullopt;
}

std::optional<double> SafeRatio(double numerator, double denominator) {
  if (!std::isfinite(numerator) || !std::isfinite(denominator) || denominator <= 0.0) {
    return std::nullopt;
  }
  return numerator / denominator;
}

std::optional<double> BytesToMbps(double bytes, double duration_ms) {
  const auto bytes_per_ms = SafeRatio(bytes, duration_ms);
  if (!bytes_per_ms) return std::nullopt;
  // bytes/ms * 8 bits * 1000 ms/s / 10^6 = Mbit/s.
  return *bytes_per_ms * 8.0 * 1000.0 / 1e6;
}

bool InUnitRange(double value, Unit unit) {
  if (!std::isfinite(value) || value < 0.0) return false;
  if (unit == Unit::Ratio) return value <= 1.0;
  if (unit == Unit::Percent) return value <= 100.0;
  return true;
}

}  // namespace aafr::netstate
