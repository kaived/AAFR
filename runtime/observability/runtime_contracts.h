#pragma once

// Read-only observability contracts between a future AAFR runtime and any
// viewer (the Developer Tools panel, experiment tooling, JSONL exports).
//
// V0.3 defines these types only. No runtime, switching mechanism or model
// produces them yet, and the browser ships no provider: viewers must show
// "not available" rather than placeholder values.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace aafr::observability {

using Timestamp = std::chrono::system_clock::time_point;

enum class ExecutionState {
  Unknown,
  Initializing,
  Active,
  Draining,
  Inactive,
  Failed,
};

struct AlgorithmStatus {
  std::string algorithm_id;
  std::optional<Timestamp> activated_at;
  // The DecisionRecord that activated this algorithm, if any.
  std::string activation_decision_id;
  ExecutionState execution_state{ExecutionState::Unknown};
  // When the runtime last confirmed the state above (not when it was read).
  std::optional<Timestamp> last_confirmed_at;
};

enum class ImplementationAvailability {
  NotImplemented,
  Experimental,
  Available,
};

struct ResourceRequirements {
  std::string time_complexity;  // For example "O(E log V)".
  std::optional<double> typical_runtime_ms;
  std::optional<std::uint64_t> memory_bytes;
};

struct AlgorithmCapability {
  std::string algorithm_id;
  // Problem/topology characteristics it supports, e.g. "non_negative_weights".
  std::vector<std::string> supported_characteristics;
  // NetworkState inputs it needs, e.g. "link_latency".
  std::vector<std::string> required_inputs;
  std::vector<std::string> operational_restrictions;
  ImplementationAvailability availability{ImplementationAvailability::NotImplemented};
  ResourceRequirements resources;
};

struct ConstraintResult {
  std::string constraint;
  bool satisfied{false};
  std::string detail;
};

struct StabilityGateResult {
  std::string gate;  // For example "min_dwell", "confidence", "meaningful_change".
  bool passed{false};
  std::string detail;
};

enum class DecisionType {
  Retain,          // Keep the active algorithm.
  SwitchApproved,  // Gates passed; a switch was requested.
  SwitchRejected,  // A different candidate was proposed but a gate or constraint failed.
};

// Whether an approved switch actually took effect. An approved switch is not
// a committed switch until the routing controller confirms it.
enum class ExecutionConfirmation {
  NotApplicable,  // Retain and rejected decisions.
  Pending,
  Committed,
  Failed,
  RolledBack,
};

struct DecisionRecord {
  std::string decision_id;
  Timestamp timestamp{};
  std::string network_state_ref;
  std::string previous_algorithm;
  std::string candidate_algorithm;
  std::vector<ConstraintResult> constraints;
  std::vector<StabilityGateResult> stability_gates;
  DecisionType decision_type{DecisionType::Retain};
  std::string reason_code;
  std::string explanation;
  ExecutionConfirmation execution{ExecutionConfirmation::NotApplicable};
};

enum class RuntimeState {
  NotIntegrated,
  Starting,
  Running,
  Degraded,
  Stopped,
};

struct RuntimeSnapshot {
  RuntimeState state{RuntimeState::NotIntegrated};
  std::optional<AlgorithmStatus> active_algorithm;
  std::string activation_reason;
  std::optional<Timestamp> last_evaluation_at;
};

// Implemented by the future runtime. Viewers hold a nullable pointer; null
// means the runtime is not integrated.
class IRuntimeObservabilityProvider {
 public:
  virtual ~IRuntimeObservabilityProvider() = default;
  virtual RuntimeSnapshot Snapshot() const = 0;
  // Newest last, at most max_records.
  virtual std::vector<DecisionRecord> RecentDecisions(std::size_t max_records) const = 0;
};

const char* ToString(ExecutionState value);
const char* ToString(ImplementationAvailability value);
const char* ToString(DecisionType value);
const char* ToString(ExecutionConfirmation value);
const char* ToString(RuntimeState value);

// Human-readable outcome that keeps "approved" and "committed" distinct,
// e.g. "Switch approved, awaiting commit" vs "Switch committed".
std::string SwitchOutcome(const DecisionRecord& record);

// Names of failed constraints and gates, comma-separated; empty if none.
std::string FailedChecks(const DecisionRecord& record);

}  // namespace aafr::observability
