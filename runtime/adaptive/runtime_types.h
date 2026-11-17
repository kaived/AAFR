#pragma once

// Shared V0.6 runtime contracts: lifecycle, evaluation triggers, the
// confirmed forwarding configuration, per-algorithm candidate evaluations and
// runtime events. Plain C++17.

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "algorithms/algorithm_id.h"
#include "algorithms/eligibility_evaluator.h"
#include "routing/topology/forwarding_graph.h"
#include "routing/topology/forwarding_problem.h"

namespace aafr::runtime {

// Bump when a runtime record field changes meaning.
inline constexpr int kRuntimeSchemaVersion = 1;

enum class RuntimeLifecycle {
  Uninitialized,
  Initializing,  // Verifying forwarding and selecting the initial configuration.
  Ready,         // A confirmed configuration is active; waiting for triggers.
  Evaluating,    // Comparing the active algorithm against challengers.
  Switching,     // Activating and confirming a new configuration.
  Degraded,      // No confirmed configuration, or it failed; recovery evaluations continue.
  Stopping,
  Stopped,
  Failed,        // Unrecoverable setup error (for example invalid configuration).
};

// Whether `from` -> `to` is a permitted lifecycle transition. Same-state
// "transitions" are not transitions and return false.
bool IsValidTransition(RuntimeLifecycle from, RuntimeLifecycle to);
const char* ToString(RuntimeLifecycle value);

enum class EvaluationTrigger {
  Startup,
  MeaningfulChange,
  ConstraintChange,
  TopologyChange,
  FailureRecovery,
  Manual,
  Periodic,  // Re-check while Degraded, or a re-check of a time-blocked challenger.
  // V0.7.1 measurement-led review: probe the incumbent against the best
  // alternative path even when J(P) on the graph prefers the incumbent,
  // because the graph may not show what measurements show (and its
  // recovery). Added at the end; earlier values keep their meaning.
  Review,
};

const char* ToString(EvaluationTrigger value);
// Higher wins when triggers are coalesced into one evaluation.
int Priority(EvaluationTrigger value);
// Safety triggers skip the minimum spacing between evaluations.
bool IsSafetyTrigger(EvaluationTrigger value);

// The one configuration the runtime treats as active. It exists only after
// the forwarding executor confirmed the path (path_confirmed) for this
// configuration's activation request.
struct ActiveConfiguration {
  std::uint64_t config_version{0};  // Increases by one per committed configuration.
  algorithms::AlgorithmId algorithm{algorithms::AlgorithmId::Dijkstra};
  forwarding::Path path;
  std::string graph_id;
  std::uint64_t graph_version{0};
  std::optional<double> objective;  // J(P) at activation, ForwardingObjective v<objective_version>.
  int objective_version{forwarding::ForwardingObjective::kVersion};
  std::int64_t activated_monotonic_ns{0};
  std::string activation_request_id;
  std::string decision_id;
  std::string confirmation_evidence;
  double activation_latency_ms{0.0};  // Request to confirmation.
};

// One algorithm's standing in one evaluation.
struct CandidateEvaluation {
  algorithms::AlgorithmId algorithm{algorithms::AlgorithmId::Dijkstra};
  algorithms::EligibilityStatus eligibility{algorithms::EligibilityStatus::Unknown};
  std::string eligibility_reasons;  // V0.4 reason codes, comma-separated.
  // Host-state checks (RequiredFeatures, DataQuality) that failed but were
  // not enforced because the controlled graph does not use host features.
  std::vector<std::string> overridden_checks;
  bool considered{false};  // Eligible (or eligible after the documented override).
  bool computed{false};
  std::string computation_status;  // AlgorithmStatus text.
  forwarding::Path path;
  bool path_valid{false};
  bool path_feasible{false};
  std::optional<double> objective;
  double computation_ms{0.0};
  std::string note;

  bool Usable() const { return considered && computed && path_valid && path_feasible && objective.has_value(); }
};

enum class RuntimeEventType {
  RuntimeInitialized,
  LifecycleChanged,
  NetworkChangeDetected,
  EvaluationStarted,
  RecommendationGenerated,
  ProbingStarted,
  ProbingCompleted,
  StatisticalEvaluationCompleted,
  AlgorithmRetained,
  SwitchRequested,
  SwitchCommitted,
  SwitchFailed,
  RecoveryStarted,
  RecoveryCompleted,
  EvaluationDiscarded,
};

const char* ToString(RuntimeEventType value);

struct RuntimeEvent {
  std::uint64_t sequence{0};
  std::int64_t monotonic_ns{0};
  RuntimeEventType type{RuntimeEventType::LifecycleChanged};
  std::string decision_id;  // Empty when the event belongs to no decision.
  std::string detail;
};

}  // namespace aafr::runtime
