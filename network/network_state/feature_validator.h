#pragma once

// Validation of telemetry measurements before they enter a NetworkState.
// Plain C++17. A rejected measurement is counted and reported; it is never
// repaired, clamped or replaced.

#include <optional>
#include <string>

#include "network/network_state/network_state.h"
#include "network/telemetry/telemetry_event.h"

namespace aafr::netstate {

enum class ValidationIssue {
  None,
  MissingValue,    // The event has no value or reports itself unavailable.
  NonFinite,       // NaN or infinity.
  Negative,
  UnexpectedUnit,  // The unit does not measure this metric's quantity.
  OutOfRange,      // Above the metric's sanity bound.
  NotAnInteger,    // A count with a fractional part.
};

// What a known metric must look like.
struct MetricContract {
  const char* metric;
  Unit unit;
  // Sanity bound in the canonical unit. Values above it are measurement or
  // parsing faults, not network conditions:
  //   durations 3 600 000 ms (one hour, far beyond TCP and DNS timeouts);
  //   throughput 1 000 000 Mbps; counts 2^53 (exact in a double).
  double max_value;
};

// nullptr for metrics that carry no NetworkState input.
const MetricContract* FindMetricContract(const std::string& metric);

struct ValidationResult {
  ValidationIssue issue{ValidationIssue::None};
  std::string detail;
  // The value in the contract's canonical unit, set when the issue is None.
  std::optional<double> canonical_value;

  bool Ok() const { return issue == ValidationIssue::None; }
};

// Checks `event` against the contract of its metric. Precondition: the metric
// has a contract.
ValidationResult ValidateMeasurement(const telemetry::TelemetryEvent& event,
                                     const MetricContract& contract);

const char* ToString(ValidationIssue value);

}  // namespace aafr::netstate
