#pragma once

// AAFRRuntime: the V0.6 adaptive runtime and central orchestrator.
// Plain C++17, no Qt.
//
//   NetworkState / forwarding graph -> NetworkChangeDetector -> scheduler
//     -> eligibility (V0.4/V0.5) -> algorithm computation -> path validation
//     -> IRecommendationProvider -> incumbent vs challenger
//     -> ControlledProbeEngine + StatisticalArbitrationEngine (when possible)
//     -> SwitchingStabilityController -> RETAIN | SWITCH
//     -> activation through the V0.5 forwarding executor -> confirmation
//     -> commit (or recovery) -> DecisionRecord -> outcome observation
//
// Ownership: the runtime is the only owner of the active forwarding
// configuration. The provider recommends, arbitration supplies evidence and
// the executor forwards; none of them changes the active configuration. A
// configuration becomes active only after the executor confirmed the path
// for its activation request (path_confirmed), and only if the forwarding
// graph has not changed since the evaluation (otherwise the evaluation is
// discarded as stale).
//
// Threading: Step() and Initialize() are serialized by one mutex, so two
// evaluations or switches can never overlap. Start() runs Step() on a
// worker thread every tick (never the Qt GUI thread); Status(), Decisions(),
// Events() and ForwardActive() are safe from any thread. Stop() cancels
// probing and activation, joins the worker and ends in Stopped.
//
// No ML: the runtime runs the deterministic provider unless another
// IRecommendationProvider is injected.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "algorithms/execution/algorithm_execution_engine.h"
#include "network/network_state/network_state_service.h"
#include "network/telemetry/telemetry_pipeline.h"
#include "routing/forwarding/forwarding_executor.h"
#include "runtime/adaptive/controlled_probe_engine.h"
#include "runtime/adaptive/decision_record.h"
#include "runtime/adaptive/network_change_detector.h"
#include "runtime/adaptive/outcome_observer.h"
#include "runtime/adaptive/performance_evaluator.h"
#include "runtime/adaptive/recommendation_provider.h"
#include "runtime/adaptive/runtime_clock.h"
#include "runtime/adaptive/stability_controller.h"
#include "runtime/adaptive/statistical_arbitration.h"
#include "runtime/observability/runtime_contracts.h"

namespace aafr::runtime {

struct RuntimeConfig {
  std::string session_id;  // Letters and digits; generated when empty.
  double tick_ms{500.0};
  ChangeDetectorConfig detector;
  SchedulerConfig scheduler;
  StabilityConfig stability;
  ArbitrationConfig arbitration;
  ProbeConfig probe;
  // Controlled probing + statistical arbitration. When false, or when
  // probing is unavailable, the documented estimate-only policy applies.
  bool statistical_arbitration{true};
  std::size_t max_challengers{2};
  // RequiredFeatures and DataQuality judge host traffic, which the
  // controlled graph does not use. When false they may fail without
  // excluding an algorithm; the record lists them as overridden.
  bool enforce_host_state_checks{false};
  std::uint64_t algorithm_seed{1};
  std::uint64_t max_iterations{100};
  double computation_deadline_ms{1000.0};
  double activation_timeout_ms{2000.0};
  int activation_attempts{2};
  // Requests over the active path to detect failure; 0 disables.
  double health_check_interval_ms{2000.0};
  int failures_for_safety_override{2};
  double degraded_retry_ms{2000.0};
  // V0.7.1 measurement-led review while Ready; 0 disables. Every interval
  // the incumbent is probed against the best alternative path (a detour
  // around its relays when every algorithm agrees on it), so degradation
  // and recovery that only measurements show are still acted on. Reviews
  // pass the same stability gates; without probing they never switch.
  double review_interval_ms{30'000.0};
  double outcome_window_ms{30'000.0};
  std::size_t decision_history{256};
  std::size_t event_history{512};
  std::size_t error_history{16};
  // Appends one JSON line per DecisionRecord when set.
  std::string decision_log_path;

  std::vector<std::string> Validate() const;
};

struct RuntimeDependencies {
  // Required. A new immutable snapshot whenever the topology changes.
  std::function<std::shared_ptr<const forwarding::ForwardingGraph>()> graph_source;
  // Required. The problem posed against a graph version.
  std::function<forwarding::ForwardingProblem(const forwarding::ForwardingGraph&)> problem_for;
  // Optional. The latest V0.4 snapshot (NetworkState + constraints).
  std::function<std::shared_ptr<const netstate::StateSnapshot>()> state_source;
  // Required. Executes activation, probe, health and application requests.
  forwarding::IForwardingExecutor* executor{nullptr};
  // Required. Provides the V0.5 eligibility evaluation (registry, environment).
  const algorithms::AlgorithmExecutionEngine* eligibility{nullptr};
  // Optional; the DeterministicRecommendationProvider when null.
  const IRecommendationProvider* provider{nullptr};
  // Optional; a steady clock when null.
  const IMonotonicClock* clock{nullptr};
  telemetry::TelemetryPipeline* pipeline{nullptr};
  const BenchmarkEvidence* benchmark{nullptr};
  // The executor sleeps configured edge delays (recorded as provenance).
  bool executor_injects_delay{false};
};

struct RuntimeCounters {
  std::uint64_t evaluations{0};
  std::uint64_t retained{0};
  std::uint64_t switches{0};
  std::uint64_t rejected{0};
  std::uint64_t failed_switches{0};
  std::uint64_t recoveries{0};
  std::uint64_t discarded{0};
  std::uint64_t safety_overrides{0};
  std::uint64_t meaningful_changes{0};
  std::uint64_t ignored_changes{0};
  std::uint64_t coalesced_requests{0};
  std::uint64_t probe_campaigns{0};
  std::uint64_t reviews{0};
  std::uint64_t probe_requests{0};
  double probe_ms{0.0};
  double decision_ms{0.0};
  double computation_ms{0.0};
};

struct RuntimeStatus {
  RuntimeLifecycle lifecycle{RuntimeLifecycle::Uninitialized};
  std::string session_id;
  std::optional<ActiveConfiguration> active;
  bool active_verified{false};
  std::string provider_id;
  std::vector<std::string> eligible_algorithms;
  std::optional<algorithms::AlgorithmId> candidate;
  forwarding::Path candidate_path;
  std::optional<EvaluationTrigger> last_trigger;
  std::optional<std::int64_t> last_evaluation_ns;
  std::optional<std::chrono::system_clock::time_point> last_evaluation_at;
  std::string last_evaluation;
  std::string last_switch;
  std::string last_change;
  std::optional<StabilityAssessment> last_gates;
  double dwell_remaining_ms{0.0};
  double cooldown_remaining_ms{0.0};
  // Measurement-led review: time until the next one (nullopt: disabled or
  // not Ready) and the outcome of the latest one.
  std::optional<double> review_due_ms;
  std::string last_review;
  std::string activation_status;
  std::string health;
  int consecutive_failures{0};
  bool statistical_arbitration{false};
  bool pending_evaluation{false};
  std::vector<std::string> errors;
  RuntimeCounters counters;
};

class AAFRRuntime final : public observability::IRuntimeObservabilityProvider {
 public:
  AAFRRuntime(RuntimeDependencies dependencies, RuntimeConfig config = {});
  ~AAFRRuntime() override;
  AAFRRuntime(const AAFRRuntime&) = delete;
  AAFRRuntime& operator=(const AAFRRuntime&) = delete;

  // Initial selection on the caller's thread: validates configuration and
  // dependencies, evaluates, activates and confirms the first configuration.
  // True when a configuration is confirmed (Ready); false when Degraded or
  // Failed. Safe to call once; later calls return the current outcome.
  bool Initialize();
  // Initialize() if needed, then Step() on a worker thread every tick.
  void Start();
  // Cancels probing and activation, joins the worker, ends in Stopped.
  void Stop();
  // One iteration on the caller's thread: health check, change detection,
  // scheduling, at most one evaluation. For tests and tools.
  void Step();

  void RequestEvaluation(EvaluationTrigger trigger = EvaluationTrigger::Manual, const std::string& reason = "");
  // Forwards one application request over the confirmed active path.
  forwarding::ForwardingExecutionResult ForwardActive(const std::string& request_id, const std::string& payload,
                                                      double timeout_ms);

  RuntimeStatus Status() const;
  std::vector<DecisionRecord> Decisions(std::size_t max_records) const;
  std::vector<RuntimeEvent> Events(std::size_t max_events) const;
  std::optional<OutcomeRevision> Outcome(const std::string& decision_id) const;
  OutcomeCounters OutcomeTotals() const { return outcomes_.Counters(); }
  std::string DecisionsJsonl() const;
  const RuntimeConfig& config() const { return config_; }

  // IRuntimeObservabilityProvider (the V0.3 viewer contract).
  observability::RuntimeSnapshot Snapshot() const override;
  std::vector<observability::DecisionRecord> RecentDecisions(std::size_t max_records) const override;

 private:
  struct EvaluationInputs;
  struct Attempt;

  void Evaluate(const PendingEvaluation& pending);
  std::vector<CandidateEvaluation> ComputeCandidates(const EvaluationInputs& in, double* total_ms);
  // Recommendation over candidates recomputed with the relays of a path
  // that failed in measurement forbidden (the graph may not show the
  // failure, so every algorithm would otherwise return the same path).
  Recommendation DetourRecommendation(const EvaluationInputs& in, const forwarding::Path& failed, double* total_ms);
  bool Activate(const RankedCandidate& candidate, DecisionRecord& record, const EvaluationInputs& in,
                bool initial, const Recommendation& recommendation);
  Attempt TryActivate(const forwarding::Path& path, const forwarding::ForwardingGraph& graph,
                      const std::string& decision_id, algorithms::AlgorithmId algorithm, std::uint64_t state_id);
  bool Recover(const forwarding::Path& failed_path, DecisionRecord& record, const EvaluationInputs& in,
               const Recommendation& recommendation, bool initial);
  void Commit(const RankedCandidate& candidate, DecisionRecord& record, const EvaluationInputs& in,
              const Attempt& attempt, bool initial);
  void HealthCheck(std::int64_t now_ns);
  void ObserveSources(std::int64_t now_ns);
  bool SetLifecycle(RuntimeLifecycle to);
  void FinishRecord(DecisionRecord record, std::int64_t started_ns);
  void Emit(RuntimeEventType type, const std::string& decision_id, const std::string& detail);
  void AddError(const std::string& error);
  void RefreshStatusTimers(std::int64_t now_ns);
  std::string NextId(const char* prefix);

  RuntimeDependencies deps_;
  const RuntimeConfig config_;
  std::unique_ptr<IRecommendationProvider> owned_provider_;
  SteadyClock steady_clock_;
  const IRecommendationProvider* provider_;
  const IMonotonicClock* clock_;

  // Step-thread state (guarded by step_mutex_).
  std::mutex step_mutex_;
  NetworkChangeDetector detector_;
  SwitchingStabilityController stability_;
  StatisticalArbitrationEngine arbitration_;
  ControlledProbeEngine probes_;
  std::int64_t next_health_ns_{0};
  std::int64_t next_degraded_retry_ns_{0};
  std::int64_t next_review_ns_{0};
  // The time-blocked challenger was found by a review (measurement, not
  // J(P)), so its follow-up must be a review too or it is never re-probed.
  bool followup_is_review_{false};
  // Re-check a preferred challenger that only time-based gates blocked
  // (dwell, cooldown, hysteresis, backoff); 0 when none is due.
  std::int64_t followup_due_ns_{0};
  std::uint64_t config_seq_{0};
  bool initialized_{false};
  bool ever_committed_{false};

  // Shared counters.
  std::atomic<std::uint64_t> id_seq_{0};
  std::atomic<int> consecutive_failures_{0};
  std::atomic<bool> cancel_{false};
  std::atomic<bool> stop_requested_{false};

  mutable std::mutex sched_mutex_;
  EvaluationScheduler scheduler_;

  // Published state (guarded by state_mutex_).
  mutable std::mutex state_mutex_;
  RuntimeStatus status_;
  std::int64_t dwell_until_ns_{0};
  std::int64_t cooldown_until_ns_{0};
  std::deque<DecisionRecord> decisions_;
  std::deque<RuntimeEvent> events_;
  std::uint64_t event_seq_{0};

  RuntimeOutcomeObserver outcomes_;

  std::mutex worker_mutex_;
  std::condition_variable wake_;
  std::thread worker_;
};

}  // namespace aafr::runtime
