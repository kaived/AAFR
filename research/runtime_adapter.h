#pragma once

// Phase 5B.1: the research pipeline attached to the AAFR runtime through a
// controlled, non-actuating orchestration layer. Plain C++17, thread-safe.
//
// What it does: on an explicit request (never on its own), it runs the
// research pipeline (PlanTournament, DecideTournament) on a supplied
// telemetry snapshot and, when a tournament is needed, supplied probe
// observations, and records the result as a research recommendation:
// keep, switch or undetermined.
//
// What it cannot do: it holds no forwarding executor, no recommendation
// provider and no reference to the runtime's control interface, so it
// cannot activate a path, send a probe, change the runtime's active
// configuration or influence the runtime's own (production) decisions. A
// ResearchRecommendation is a separate type from the runtime's
// DecisionRecord and is marked applied = false; nothing in AAFR applies it.
//
// Reading the runtime: ReadRuntime() uses only AAFRRuntime::Status() to learn
// the installed (active) path and how long ago the runtime last applied a
// switch. The research dwell rule (30 s) is enforced by the research library
// from that applied-switch time; research recommendations never reset it,
// because they change nothing. No other stability rule is added here.
//
// Lifecycle (ResearchRuntimeState):
//   Unavailable  the model or its statistics could not be loaded; every
//                evaluation is refused (reason recorded)
//   Idle         ready, nothing evaluated yet
//   Evaluating   an evaluation is running (evaluations are serialized)
//   Decided      the last evaluation produced a recommendation
//   Error        the last request violated the input contract (no
//                recommendation was produced; the reasons are recorded)
// From Decided or Error a new request goes back through Evaluating.

#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "research/research_pipeline.h"
#include "routing/topology/forwarding_graph.h"
#include "runtime/adaptive/aafr_runtime.h"

namespace aafr::research {

enum class ResearchRuntimeState { Unavailable, Idle, Evaluating, Decided, Error };

struct ResearchRecommendation {
  std::uint64_t sequence{0};
  ArbitrationVerdict verdict{ArbitrationVerdict::Undetermined};
  std::vector<std::string> installed_path;
  std::vector<std::string> recommended_path;  // Set only for a switch recommendation.
  // A research recommendation is never a forwarding change.
  static constexpr bool kApplied = false;
  bool applied() const { return kApplied; }
  std::string kind{"research_recommendation"};
  DataOrigin telemetry_origin{DataOrigin::Controlled};
  std::optional<DataOrigin> probe_origin;
  std::string model_id, model_source_commit;
  ResearchDecisionRecord record;
  std::string record_json;  // ToJson(record), without timing.
};

struct ResearchRuntimeCounters {
  std::uint64_t requests{0};
  std::uint64_t recommendations{0};
  std::uint64_t keep{0};
  std::uint64_t switch_recommended{0};
  std::uint64_t undetermined{0};
  std::uint64_t contract_errors{0};
  std::uint64_t refused_unavailable{0};
};

struct ResearchRuntimeStatus {
  ResearchRuntimeState state{ResearchRuntimeState::Unavailable};
  std::string state_reason;
  std::string model_id, model_source_commit, statistics_source;
  std::vector<std::string> last_errors;  // Input-contract violations of the last request.
  std::optional<ResearchRecommendation> last;
  ResearchRuntimeCounters counters;
  // The adapter has no actuation path; stated in every status.
  static constexpr bool kActuationEnabled = false;
};

struct ResearchEvaluation {
  bool produced{false};  // A recommendation was recorded.
  std::optional<ResearchRecommendation> recommendation;
  std::vector<std::string> errors;
  ResearchRuntimeState state{ResearchRuntimeState::Unavailable};
};

class ResearchRuntimeAdapter {
 public:
  struct Options {
    std::string model_dir;
    std::string xgboost_library;
    PipelineConfig pipeline;
    std::size_t history{64};
  };

  explicit ResearchRuntimeAdapter(Options options);
  ResearchRuntimeAdapter(const ResearchRuntimeAdapter&) = delete;
  ResearchRuntimeAdapter& operator=(const ResearchRuntimeAdapter&) = delete;

  ResearchRuntimeStatus Status() const;
  // Newest last, at most max_records.
  std::vector<ResearchRecommendation> History(std::size_t max_records) const;

  // Stage 1 only, for a caller that must know the arms before supplying
  // probe observations. Changes no state; nullopt (with errors) when
  // unavailable or the snapshot is invalid.
  std::optional<TournamentPlan> Plan(const TelemetrySnapshot& telemetry, std::vector<std::string>* errors = nullptr) const;

  // Both stages on the caller's thread; serialized with other evaluations.
  ResearchEvaluation Evaluate(const TelemetrySnapshot& telemetry, const std::optional<ProbeObservations>& observations);

 private:
  void SetState(ResearchRuntimeState state, std::string reason);

  Options options_;
  std::unique_ptr<LinkWeightModel> model_;
  std::optional<NormalizationStatistics> statistics_;
  std::mutex evaluation_mutex_;    // One evaluation at a time.
  mutable std::mutex status_mutex_;  // Guards everything below.
  ResearchRuntimeStatus status_;
  std::deque<ResearchRecommendation> history_;
  std::uint64_t sequence_{0};
};

// ---- Read-only view of the runtime ----

struct RuntimeView {
  bool has_active{false};
  std::vector<std::string> installed_path;   // The runtime's active path.
  std::uint64_t config_version{0};           // 1: initial activation, > 1: an applied switch happened.
  // Seconds since the runtime last applied a switch; nullopt when it has
  // applied none (only the initial activation) or has no active configuration.
  std::optional<double> seconds_since_applied_switch;
};

// Reads AAFRRuntime::Status() only. now_ns must come from the runtime's clock.
RuntimeView ReadRuntime(const runtime::AAFRRuntime& runtime, std::int64_t now_ns);

// Delays of the runtime's forwarding graph at engine start, keyed by
// "from->to": the static costs of the research Dijkstra.
using StartupDelays = std::map<std::string, double>;
StartupDelays CaptureStartupDelays(const forwarding::ForwardingGraph& graph);

// A controlled telemetry snapshot from the runtime's forwarding graph and
// view. Per link: delay, loss and capacity from the graph (absent when the
// graph value is unknown), utilization absent (the forwarding graph does not
// carry it; never defaulted), the start-up delay from `startup`, available
// when edge and both nodes are available. The description records the value
// provenance. The source and destination are the ends of the installed path.
TelemetrySnapshot SnapshotFromRuntimeGraph(const forwarding::ForwardingGraph& graph, const StartupDelays& startup,
                                           const RuntimeView& view, const std::string& description);

const char* ToString(ResearchRuntimeState state);

}  // namespace aafr::research
