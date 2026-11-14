#include "network/network_state/feature_validator.h"

#include <cmath>
#include <cstring>

#include "network/network_state/feature_normalizer.h"

namespace aafr::netstate {
namespace {

constexpr double kMaxDurationMs = 3'600'000.0;
constexpr double kMaxMbps = 1'000'000.0;
constexpr double kMaxCount = 9007199254740992.0;  // 2^53

const MetricContract kContracts[] = {
    {"net.tcp.handshake_rtt.min", Unit::Milliseconds, kMaxDurationMs},
    {"net.tcp.handshake_rtt.mean", Unit::Milliseconds, kMaxDurationMs},
    {"net.tcp.handshake_rtt.max", Unit::Milliseconds, kMaxDurationMs},
    {"net.tcp.ack_rtt.mean", Unit::Milliseconds, kMaxDurationMs},
    {"net.tcp.ack_rtt.jitter", Unit::Milliseconds, kMaxDurationMs},
    {"net.dns.response_time.mean", Unit::Milliseconds, kMaxDurationMs},
    {"net.dns.response_time.max", Unit::Milliseconds, kMaxDurationMs},
    {"net.dns.responses", Unit::Count, kMaxCount},
    {"net.throughput", Unit::Mbps, kMaxMbps},
    {"net.tcp.retransmissions", Unit::Count, kMaxCount},
    {"net.packets", Unit::Count, kMaxCount},
    {"net.bytes", Unit::Bytes, kMaxCount},
    {"net.tcp.packets", Unit::Count, kMaxCount},
    {"net.udp.packets", Unit::Count, kMaxCount},
    {"net.quic.packets", Unit::Count, kMaxCount},
    {"navigation.duration", Unit::Milliseconds, kMaxDurationMs},
    {"navigation.progress", Unit::Percent, 100.0},
};

}  // namespace

const MetricContract* FindMetricContract(const std::string& metric) {
  for (const MetricContract& contract : kContracts) {
    if (metric == contract.metric) return &contract;
  }
  return nullptr;
}

ValidationResult ValidateMeasurement(const telemetry::TelemetryEvent& event,
                                     const MetricContract& contract) {
  ValidationResult result;
  if (!event.value || event.availability != telemetry::Availability::Available) {
    result.issue = ValidationIssue::MissingValue;
    result.detail = "no value reported";
    return result;
  }
  const double raw = *event.value;
  if (!std::isfinite(raw)) {
    result.issue = ValidationIssue::NonFinite;
    result.detail = "value is not finite";
    return result;
  }
  const auto canonical = ToCanonical(raw, event.unit, contract.unit);
  if (!canonical) {
    result.issue = ValidationIssue::UnexpectedUnit;
    result.detail = "unit '" + event.unit + "' cannot be converted to " + ToString(contract.unit);
    return result;
  }
  if (*canonical < 0.0) {
    result.issue = ValidationIssue::Negative;
    result.detail = "value is negative";
    return result;
  }
  if (*canonical > contract.max_value) {
    result.issue = ValidationIssue::OutOfRange;
    result.detail = "value exceeds the sanity bound for " + std::string(contract.metric);
    return result;
  }
  if ((contract.unit == Unit::Count || contract.unit == Unit::Bytes) &&
      *canonical != std::floor(*canonical)) {
    result.issue = ValidationIssue::NotAnInteger;
    result.detail = "count has a fractional part";
    return result;
  }
  result.canonical_value = canonical;
  return result;
}

const char* ToString(ValidationIssue value) {
  switch (value) {
    case ValidationIssue::None: return "none";
    case ValidationIssue::MissingValue: return "missing_value";
    case ValidationIssue::NonFinite: return "non_finite";
    case ValidationIssue::Negative: return "negative";
    case ValidationIssue::UnexpectedUnit: return "unexpected_unit";
    case ValidationIssue::OutOfRange: return "out_of_range";
    case ValidationIssue::NotAnInteger: return "not_an_integer";
  }
  return "unknown";
}

}  // namespace aafr::netstate
