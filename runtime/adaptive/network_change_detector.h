#pragma once

// Meaningful-change detection and evaluation scheduling. Plain C++17,
// deterministic: time comes from the observation, never from a clock.
//
// The detector compares each new observation with a per-measurement
// baseline and reports whether anything changed enough to justify an
// evaluation. It never decides to switch.
//
// NetworkState features (host traffic from the V0.4 pipeline):
//   change = |current - baseline|
//   threshold = max(absolute, relative * |baseline|)
//   A feature counts only when it is Usable() (valid quality, value present)
//   in both the baseline and the current observation and no older than
//   max_feature_age_ms. A change >= threshold must persist for
//   sustain_observations consecutive new NetworkState snapshots before it is
//   meaningful; the baseline then moves to the current value. The absolute
//   floor keeps a zero or near-zero baseline from turning noise into an
//   infinite relative change.
//   Missing, stale, insufficient or invalid measurements never trigger an
//   evaluation; they reset the feature's streak and are reported.
//
// Forwarding graph (the controlled topology the algorithms route on):
//   node or edge availability changes, and edge latency changes beyond
//   max(edge_latency_absolute_ms, edge_latency_relative * baseline), are
//   topology changes, and so is any change of an edge's operator policy
//   cost (it changes J(P) and can change eligibility). Graph values are
//   configured or injected, not noisy samples, so they need no sustain.
//
// Constraints: any change of the constraint signature (ids, validity,
// values) is a constraint change. A capture-interface change rebaselines
// every feature (a new interface is a new measurement population) and is a
// meaningful change.

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "network/network_state/network_constraints.h"
#include "network/network_state/network_state.h"
#include "routing/topology/forwarding_graph.h"
#include "runtime/adaptive/reason_codes.h"
#include "runtime/adaptive/runtime_types.h"

namespace aafr::runtime {

struct FeatureThreshold {
  std::string feature_id;
  double absolute{0.0};  // In the feature's unit.
  double relative{0.0};  // Fraction of |baseline|.
};

struct ChangeDetectorConfig {
  std::vector<FeatureThreshold> features = DefaultFeatureThresholds();
  int sustain_observations{2};
  double max_feature_age_ms{10'000.0};
  double edge_latency_absolute_ms{2.0};
  double edge_latency_relative{0.25};

  static std::vector<FeatureThreshold> DefaultFeatureThresholds();
  // Problems with the configuration; empty when valid.
  std::vector<std::string> Validate() const;
};

struct ObservedFeature {
  std::optional<double> value;
  netstate::FeatureQuality quality{netstate::FeatureQuality::Missing};
  std::optional<double> age_ms;
  std::uint64_t samples{0};
};

struct ObservedEdge {
  bool available{true};
  std::optional<double> latency_ms;
  double policy_cost{0.0};  // Operator preference; any change alters J(P).
};

// What the detector looks at, extracted from immutable snapshots.
struct ChangeObservation {
  std::int64_t monotonic_ns{0};
  std::uint64_t network_state_id{0};  // 0: no NetworkState.
  std::map<std::string, ObservedFeature> features;
  std::string capture_interface;
  std::string constraint_signature;
  bool has_graph{false};
  std::uint64_t graph_version{0};
  std::map<std::string, bool> node_available;
  std::map<std::string, ObservedEdge> edges;  // Key "from->to".
};

// Builds an observation. Any pointer may be null.
ChangeObservation MakeObservation(std::int64_t monotonic_ns, const netstate::NetworkState* state,
                                  const netstate::NetworkConstraints* constraints,
                                  const forwarding::ForwardingGraph* graph, const ChangeDetectorConfig& config);
std::string ConstraintSignature(const netstate::NetworkConstraints& constraints);

struct FeatureChange {
  std::string feature_id;
  std::optional<double> baseline;
  std::optional<double> current;
  double delta{0.0};
  double threshold{0.0};
  int streak{0};
  // "meaningful", "pending_sustain", "within_threshold", "baseline_set",
  // "unusable:<quality>", "stale_age".
  std::string status;
};

struct ChangeAssessment {
  bool meaningful{false};  // An evaluation is justified.
  bool duplicate{false};   // Same NetworkState and graph version as last time.
  std::vector<EvaluationTrigger> triggers;
  std::vector<RuntimeReason> reasons;
  std::vector<FeatureChange> features;
  std::vector<std::string> topology_changes;
  bool constraint_changed{false};
  bool interface_changed{false};
  std::string explanation;
};

class NetworkChangeDetector {
 public:
  explicit NetworkChangeDetector(ChangeDetectorConfig config = {});
  ChangeAssessment Observe(const ChangeObservation& observation);
  // Forget every baseline (the next observation becomes the baseline).
  void Reset();
  const ChangeDetectorConfig& config() const { return config_; }

 private:
  struct FeatureBaseline {
    std::optional<double> value;
    int streak{0};
  };

  ChangeDetectorConfig config_;
  bool has_previous_{false};
  std::uint64_t last_state_id_{0};
  std::uint64_t last_graph_version_{0};
  std::string capture_interface_;
  std::string constraint_signature_;
  std::map<std::string, FeatureBaseline> baselines_;
  bool has_graph_baseline_{false};
  std::map<std::string, bool> node_baseline_;
  std::map<std::string, ObservedEdge> edge_baseline_;
};

// ---- Scheduling ----

struct SchedulerConfig {
  // Minimum spacing between evaluations; safety triggers ignore it.
  double min_interval_ms{1000.0};
};

struct PendingEvaluation {
  EvaluationTrigger trigger{EvaluationTrigger::Manual};
  std::set<EvaluationTrigger> coalesced;  // Every trigger folded into this one.
  std::size_t requests{0};
  std::int64_t first_requested_ns{0};
  std::string reason;
};

// Coalesces evaluation requests: at most one evaluation is pending, it keeps
// the highest-priority trigger, and it runs no sooner than min_interval_ms
// after the previous one unless a safety trigger is pending.
class EvaluationScheduler {
 public:
  explicit EvaluationScheduler(SchedulerConfig config = {}) : config_(config) {}
  void Request(EvaluationTrigger trigger, std::int64_t now_ns, const std::string& reason);
  // The evaluation to run now, if any; marks it started.
  std::optional<PendingEvaluation> Take(std::int64_t now_ns);
  bool HasPending() const { return pending_.has_value(); }
  std::uint64_t coalesced_requests() const { return coalesced_total_; }
  std::uint64_t started() const { return started_total_; }

 private:
  SchedulerConfig config_;
  std::optional<PendingEvaluation> pending_;
  std::optional<std::int64_t> last_started_ns_;
  std::uint64_t coalesced_total_{0};
  std::uint64_t started_total_{0};
};

}  // namespace aafr::runtime
