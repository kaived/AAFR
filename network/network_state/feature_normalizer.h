#pragma once

// Deterministic unit normalization for NetworkState features. Plain C++17.
//
// Normalization here means converting a reported value to the feature's
// canonical unit. There is no statistical scaling (no min-max, no z-score):
// V0.4 has no justified reference ranges, so values keep physical units.

#include <optional>
#include <string>

#include "network/network_state/network_state.h"

namespace aafr::netstate {

// Bump when a conversion factor or a canonical unit changes.
inline constexpr int kNormalizationVersion = 1;

// Converts `value` in `reported_unit` (as written in a telemetry event, e.g.
// "ms", "s", "Mbps", "packets") to `target`. Returns nullopt when the reported
// unit does not measure the same quantity as the target.
std::optional<double> ToCanonical(double value, const std::string& reported_unit, Unit target);

// numerator / denominator, or nullopt when the denominator is not positive
// or either input is not finite. A ratio is never fabricated from no data.
std::optional<double> SafeRatio(double numerator, double denominator);

// Megabits per second for `bytes` transferred in `duration_ms`.
std::optional<double> BytesToMbps(double bytes, double duration_ms);

// Whether `value` lies in the range its unit allows: finite and nonnegative,
// at most 1 for Ratio and at most 100 for Percent.
bool InUnitRange(double value, Unit unit);

}  // namespace aafr::netstate
