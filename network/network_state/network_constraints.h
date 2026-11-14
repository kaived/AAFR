#pragma once

// NetworkConstraints: the facts and limits an algorithm must satisfy to be
// considered, each with its source, validity and an explanation. Plain C++17.
//
// An unknown constraint is recorded as unknown. It is never turned into a
// satisfied one: consumers decide what unknown means through an explicit
// policy (see algorithms/eligibility_evaluator.h).

#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "network/network_state/execution_environment.h"
#include "network/network_state/network_state.h"

namespace aafr::netstate {

enum class ConstraintCategory { Network, Performance, Execution, Operational };

enum class ConstraintSource {
  Measured,    // Read from a NetworkState feature.
  Configured,  // Supplied by the ExecutionEnvironment.
  Derived,     // Computed from other constraints or features.
  Unknown,     // Nobody supplied it.
};

enum class ConstraintValidity {
  Valid,
  Invalid,  // Contradicts itself or another constraint; must not be relied on.
  Unknown,  // No value.
};

enum class ConstraintType { Boolean, Number, Text, List };

using ConstraintValue = std::variant<std::monostate, bool, double, std::string, std::vector<std::string>>;

struct Constraint {
  std::string id;
  ConstraintCategory category{ConstraintCategory::Network};
  ConstraintType type{ConstraintType::Boolean};
  ConstraintValue value;  // monostate when unknown.
  std::string unit;       // Empty when the value has no unit.
  ConstraintSource source{ConstraintSource::Unknown};
  // When the value was observed; set for measured and derived constraints.
  std::optional<telemetry::WallClock::time_point> timestamp;
  ConstraintValidity validity{ConstraintValidity::Unknown};
  std::string explanation;

  bool Known() const { return validity == ConstraintValidity::Valid; }
};

// Constraint ids. Kept in one place so producers and consumers cannot drift.
namespace constraint_id {
inline constexpr const char* kTopologyAvailable = "network.topology.available";
inline constexpr const char* kTopologyRepresentations = "network.topology.representations";
inline constexpr const char* kTopologyNodeCount = "network.topology.node_count";
inline constexpr const char* kTopologyEdgeCount = "network.topology.edge_count";
inline constexpr const char* kCandidatePathCount = "network.topology.candidate_path_count";
inline constexpr const char* kReachable = "network.topology.reachable";
inline constexpr const char* kTopologyStable = "network.topology.stable";
inline constexpr const char* kEdgeWeightsFinite = "network.edge_weights.all_finite";
inline constexpr const char* kEdgeWeightsNegative = "network.edge_weights.has_negative";
inline constexpr const char* kNegativeCycle = "network.edge_weights.has_negative_cycle";
inline constexpr const char* kHeuristicAvailable = "network.topology.heuristic_available";
inline constexpr const char* kSupportedProtocols = "network.protocols.supported";
inline constexpr const char* kRequiredProtocolsSupported = "network.protocols.required_supported";
inline constexpr const char* kForwardingRestrictions = "network.forwarding.restrictions";
inline constexpr const char* kAvailableMeasurements = "network.measurements.available";
inline constexpr const char* kComputationDeadlineMs = "performance.computation_deadline_ms";
inline constexpr const char* kLatencyBudgetMs = "performance.latency_budget_ms";
inline constexpr const char* kMaxRetransmissionRatio = "performance.max_retransmission_ratio";
inline constexpr const char* kReliabilityWithinLimit = "performance.reliability_within_limit";
inline constexpr const char* kMemoryBudgetBytes = "execution.memory_budget_bytes";
inline constexpr const char* kCpuBudgetPercent = "execution.cpu_budget_percent";
inline constexpr const char* kForwardingOperations = "execution.forwarding_operations";
inline constexpr const char* kProblem = "execution.problem_representation";
inline constexpr const char* kObjective = "execution.optimization_objective";
inline constexpr const char* kMonitoringAvailable = "operational.monitoring_available";
inline constexpr const char* kCaptureInterrupted = "operational.capture_interrupted";
inline constexpr const char* kStateFresh = "operational.state_fresh";
inline constexpr const char* kStateValidity = "operational.state_validity";
}  // namespace constraint_id

struct NetworkConstraints {
  std::uint64_t network_state_id{0};
  std::vector<Constraint> constraints;
  // Contradictions found between constraints; the affected ones are Invalid.
  std::vector<std::string> conflicts;

  const Constraint* Find(const std::string& id) const;
  // Typed reads. nullopt unless the constraint exists, is Valid and holds
  // that type, so an unknown or invalid constraint can never read as a value.
  std::optional<bool> Bool(const std::string& id) const;
  std::optional<double> Number(const std::string& id) const;
  std::optional<std::string> Text(const std::string& id) const;
  std::optional<std::vector<std::string>> List(const std::string& id) const;

  std::uint64_t CountBySource(ConstraintSource source) const;
  std::uint64_t CountByValidity(ConstraintValidity validity) const;
};

struct ConstraintPolicy {
  // The state is fresh when its newest measurement is at most this old.
  double max_state_age_ms{10'000.0};
};

// Builds the constraint set for one NetworkState and environment, then
// checks it for contradictions. Deterministic: no clock, no I/O.
class ConstraintEvaluator {
 public:
  explicit ConstraintEvaluator(ConstraintPolicy policy = {}) : policy_(policy) {}

  NetworkConstraints Evaluate(const NetworkState& state, const ExecutionEnvironment& environment) const;

 private:
  ConstraintPolicy policy_;
};

const char* ToString(ConstraintCategory value);
const char* ToString(ConstraintSource value);
const char* ToString(ConstraintValidity value);
const char* ToString(ConstraintType value);
// The value as display text; "unknown" for monostate.
std::string ValueText(const Constraint& constraint);

}  // namespace aafr::netstate
