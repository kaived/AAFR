#pragma once

// The V0.6 explainable DecisionRecord. Plain C++17.
//
// One record per significant evaluation (initial activation, retain,
// switch, rejected switch, recovery, discarded evaluation). A record is
// written once and never edited: outcome observations that arrive later are
// kept as numbered OutcomeRevisions linked by decision_id
// (RuntimeOutcomeObserver), so the history of what was decided stays intact.
// Missing data is represented as empty optionals or explicit "missing"
// entries, never as zero.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "algorithms/algorithm_id.h"
#include "network/telemetry/telemetry_event.h"
#include "routing/topology/forwarding_problem.h"
#include "runtime/adaptive/performance_evaluator.h"
#include "runtime/adaptive/reason_codes.h"
#include "runtime/adaptive/recommendation_provider.h"
#include "runtime/adaptive/runtime_types.h"
#include "runtime/adaptive/stability_controller.h"
#include "runtime/observability/runtime_contracts.h"

namespace aafr::runtime {

inline constexpr const char* kDecisionRecordSchema = "aafr.decision_record.v1";

enum class DecisionType {
  InitialActivation,
  Retain,
  Switch,            // Ordinary switch, gates passed.
  SwitchRejected,    // A better candidate existed but a gate blocked it.
  SafetySwitch,      // Switch under a safety override.
  Recovery,          // Configuration re-established after a failure.
  Unavailable,       // No configuration could be selected or confirmed.
  Discarded,         // Evaluation became stale before activation.
};

enum class ActivationStatus { NotRequested, Pending, Confirmed, Failed, TimedOut, Cancelled };
enum class RecoveryStatus { NotNeeded, PreviousRetained, Recovered, Failed };

struct DecisionRecord {
  // Identification
  std::string schema{kDecisionRecordSchema};
  std::string decision_id;
  std::string session_id;
  std::int64_t monotonic_ns{0};
  telemetry::WallClock::time_point timestamp{};
  std::string evaluation_id;

  // Network context
  std::uint64_t network_state_id{0};  // 0: no NetworkState was available.
  std::string network_validity;       // "valid", "degraded", "no_data", or "unavailable".
  std::string measurement_quality;    // e.g. "valid=6 missing=19 stale=0 ...".
  std::string graph_id;
  std::uint64_t graph_version{0};
  std::string constraint_version;     // Hash of the constraint signature; empty when none.
  EvaluationTrigger trigger{EvaluationTrigger::Manual};
  std::vector<std::string> coalesced_triggers;
  std::string trigger_detail;

  // Recommendation
  std::string provider_id;
  std::string recommendation_status;
  std::optional<algorithms::AlgorithmId> recommended_algorithm;
  std::vector<RankedCandidate> ranking;
  std::vector<std::string> eligible_algorithms;
  std::vector<std::string> overridden_checks;  // Host-state checks not enforced, per algorithm.
  std::string evidence_source;
  std::optional<double> estimated_improvement;
  std::optional<double> estimated_improvement_relative;
  std::string recommendation_explanation;
  // V0.7.2 (additive): where the ranking came from and, when a model-backed
  // provider ran, what the model predicted and what became of it. The
  // prediction (model->predicted_algorithm) is kept apart from the candidate
  // the runtime selected (candidate_algorithm) and from the confirmed active
  // algorithm (confirmed_algorithm).
  std::string recommendation_mode{"deterministic"};
  std::string recommendation_source{"deterministic"};
  std::optional<ModelRecommendationTrace> model;

  // Statistical evidence
  bool probing_attempted{false};
  std::string probing_unavailable_reason;
  std::string probe_scenario_id;
  std::size_t incumbent_samples{0};
  std::size_t challenger_samples{0};
  std::size_t paired_samples{0};
  std::string statistical_method;
  std::optional<double> latency_ratio;
  std::optional<double> ratio_ci_low;
  std::optional<double> ratio_ci_high;
  double ci_level{0.0};
  std::optional<double> p_holm;
  std::string assumption_validation;
  std::string evidence_validity;  // ArbitrationVerdict text, or "not_run".
  std::optional<double> incumbent_failure_rate;
  std::optional<double> challenger_failure_rate;
  bool simulated_delay{false};    // Probe paths carried injected (configured) delays.
  std::string evidence_explanation;

  // Stability
  std::optional<StabilityAssessment> stability;
  bool safety_override{false};
  std::optional<RuntimeReason> safety_reason;

  // Decision
  std::optional<algorithms::AlgorithmId> previous_algorithm;
  forwarding::Path previous_path;
  std::optional<algorithms::AlgorithmId> candidate_algorithm;
  forwarding::Path candidate_path;
  DecisionType type{DecisionType::Retain};
  std::vector<RuntimeReason> reasons;
  std::string explanation;

  // Execution
  std::string activation_request_id;
  ActivationStatus activation{ActivationStatus::NotRequested};
  std::optional<algorithms::AlgorithmId> confirmed_algorithm;  // Active after this decision, if confirmed.
  forwarding::Path confirmed_path;
  std::uint64_t config_version{0};
  RecoveryStatus recovery{RecoveryStatus::NotNeeded};
  std::string execution_detail;
  std::optional<double> activation_latency_ms;

  // Overheads (kept apart: never summed into forwarding performance)
  double algorithm_computation_ms{0.0};  // Sum over the algorithms run.
  double decision_ms{0.0};               // Evaluation wall time without probing and activation.
  double probe_overhead_ms{0.0};
  std::size_t probe_requests{0};

  // Outcome (filled by later OutcomeRevisions; here only the window)
  double outcome_window_ms{0.0};
  std::string outcome_status{"pending"};

  bool HasReason(RuntimeReason reason) const;
};

std::string ToJson(const DecisionRecord& record);
// The V0.3 observability contract view of the record.
observability::DecisionRecord ToObservability(const DecisionRecord& record);

const char* ToString(DecisionType value);
const char* ToString(ActivationStatus value);
const char* ToString(RecoveryStatus value);

// A later, versioned summary of what happened after a decision.
struct OutcomeRevision {
  std::string decision_id;
  std::uint64_t revision{0};  // Increases with every observation added.
  std::uint64_t config_version{0};
  std::optional<algorithms::AlgorithmId> algorithm;
  std::uint64_t network_state_id{0};
  std::int64_t first_observation_ns{0};
  std::int64_t last_observation_ns{0};
  double window_ms{0.0};
  bool window_complete{false};
  std::size_t health_checks{0};
  std::size_t application_requests{0};
  // V0.7.3: browser connections forwarded under this decision's configuration.
  std::size_t browser_streams{0};
  std::size_t browser_stream_failures{0};
  ForwardingPerformance performance;
};

std::string ToJson(const OutcomeRevision& revision);

}  // namespace aafr::runtime
