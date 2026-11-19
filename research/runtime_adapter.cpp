#include "research/runtime_adapter.h"

#include <algorithm>
#include <map>
#include <sstream>

namespace aafr::research {

ResearchRuntimeAdapter::ResearchRuntimeAdapter(Options options) : options_(std::move(options)) {
  auto loaded = LinkWeightModel::Load(options_.model_dir, options_.xgboost_library);
  std::string stats_error;
  if (loaded.model) statistics_ = PathObjectiveStatistics(options_.model_dir, &stats_error);
  std::lock_guard<std::mutex> lock(status_mutex_);
  if (!loaded.model) {
    status_.state = ResearchRuntimeState::Unavailable;
    status_.state_reason = "link-weight model unavailable: " + loaded.error;
    return;
  }
  if (!statistics_) {
    status_.state = ResearchRuntimeState::Unavailable;
    status_.state_reason = "path-objective statistics unavailable: " + stats_error;
    return;
  }
  model_ = std::move(loaded.model);
  status_.model_id = model_->info().model_id;
  status_.model_source_commit = model_->info().source_commit;
  status_.statistics_source = statistics_->source;
  status_.state = ResearchRuntimeState::Idle;
  status_.state_reason = "ready; no evaluation yet";
}

void ResearchRuntimeAdapter::SetState(ResearchRuntimeState state, std::string reason) {
  std::lock_guard<std::mutex> lock(status_mutex_);
  status_.state = state;
  status_.state_reason = std::move(reason);
}

ResearchRuntimeStatus ResearchRuntimeAdapter::Status() const {
  std::lock_guard<std::mutex> lock(status_mutex_);
  return status_;
}

std::vector<ResearchRecommendation> ResearchRuntimeAdapter::History(std::size_t max_records) const {
  std::lock_guard<std::mutex> lock(status_mutex_);
  const std::size_t n = std::min(max_records, history_.size());
  return std::vector<ResearchRecommendation>(history_.end() - static_cast<long>(n), history_.end());
}

std::optional<TournamentPlan> ResearchRuntimeAdapter::Plan(const TelemetrySnapshot& telemetry,
                                                           std::vector<std::string>* errors) const {
  if (!model_ || !statistics_) {
    if (errors) *errors = {"research pipeline unavailable"};
    return std::nullopt;
  }
  const auto issues = ValidateTelemetry(telemetry);
  if (!issues.empty()) {
    if (errors) *errors = issues;
    return std::nullopt;
  }
  return PlanTournament(telemetry, *model_, *statistics_, options_.pipeline);
}

ResearchEvaluation ResearchRuntimeAdapter::Evaluate(const TelemetrySnapshot& telemetry,
                                                    const std::optional<ProbeObservations>& observations) {
  std::lock_guard<std::mutex> serial(evaluation_mutex_);
  ResearchEvaluation out;
  {
    std::lock_guard<std::mutex> lock(status_mutex_);
    ++status_.counters.requests;
    if (status_.state == ResearchRuntimeState::Unavailable) {
      ++status_.counters.refused_unavailable;
      out.errors = {status_.state_reason};
      out.state = status_.state;
      return out;
    }
    status_.state = ResearchRuntimeState::Evaluating;
    status_.state_reason = "evaluating: " + telemetry.description;
  }
  // Input contract: a described, valid snapshot; observations, when given,
  // must be described (DecideTournament checks them against the plan).
  std::vector<std::string> issues = ValidateTelemetry(telemetry);
  if (observations && observations->description.empty()) issues.push_back("probe observations have no description");
  if (!issues.empty()) {
    std::lock_guard<std::mutex> lock(status_mutex_);
    ++status_.counters.contract_errors;
    status_.last_errors = issues;
    status_.state = ResearchRuntimeState::Error;
    status_.state_reason = "input contract violated: " + issues.front();
    out.errors = issues;
    out.state = status_.state;
    return out;
  }
  const TournamentPlan plan = PlanTournament(telemetry, *model_, *statistics_, options_.pipeline);
  ResearchDecisionRecord record = DecideTournament(telemetry, plan, observations, *model_, *statistics_, options_.pipeline);

  ResearchRecommendation rec;
  rec.verdict = record.verdict;
  rec.installed_path = telemetry.installed_path;
  if (record.verdict == ArbitrationVerdict::Switch) rec.recommended_path = record.selected_path;
  rec.telemetry_origin = record.telemetry_origin;
  rec.probe_origin = record.probe_origin;
  rec.model_id = record.model_id;
  rec.model_source_commit = record.model_source_commit;
  rec.record_json = ToJson(record);
  rec.record = std::move(record);

  std::lock_guard<std::mutex> lock(status_mutex_);
  rec.sequence = ++sequence_;
  ++status_.counters.recommendations;
  switch (rec.verdict) {
    case ArbitrationVerdict::Keep: ++status_.counters.keep; break;
    case ArbitrationVerdict::Switch: ++status_.counters.switch_recommended; break;
    case ArbitrationVerdict::Undetermined: ++status_.counters.undetermined; break;
  }
  status_.last_errors.clear();
  status_.last = rec;
  status_.state = ResearchRuntimeState::Decided;
  status_.state_reason = std::string("research recommendation #") + std::to_string(rec.sequence) + ": " +
                         ToString(rec.verdict) + " (not applied)";
  history_.push_back(rec);
  while (history_.size() > options_.history) history_.pop_front();
  out.produced = true;
  out.recommendation = std::move(rec);
  out.state = status_.state;
  return out;
}

RuntimeView ReadRuntime(const runtime::AAFRRuntime& rt, std::int64_t now_ns) {
  RuntimeView v;
  const runtime::RuntimeStatus s = rt.Status();
  if (!s.active) return v;
  v.has_active = true;
  v.installed_path = s.active->path;
  v.config_version = s.active->config_version;
  if (s.active->config_version > 1 && now_ns >= s.active->activated_monotonic_ns) {
    v.seconds_since_applied_switch = static_cast<double>(now_ns - s.active->activated_monotonic_ns) / 1e9;
  }
  return v;
}

StartupDelays CaptureStartupDelays(const forwarding::ForwardingGraph& graph) {
  StartupDelays delays;
  for (const auto& e : graph.edges()) {
    if (e.latency_ms.value && e.latency_ms.provenance != forwarding::Provenance::Unknown) {
      delays[e.from + "->" + e.to] = *e.latency_ms.value;
    }
  }
  return delays;
}

TelemetrySnapshot SnapshotFromRuntimeGraph(const forwarding::ForwardingGraph& graph, const StartupDelays& startup,
                                           const RuntimeView& view, const std::string& description) {
  TelemetrySnapshot t;
  t.origin = DataOrigin::Controlled;
  std::map<std::string, int> provenance;
  const auto value = [&provenance](const forwarding::Measurement& m) -> std::optional<double> {
    ++provenance[forwarding::ToString(m.provenance)];
    if (!m.value || m.provenance == forwarding::Provenance::Unknown) return std::nullopt;
    return m.value;
  };
  for (const auto& n : graph.nodes()) t.graph.nodes.push_back(n.id);
  for (const auto& e : graph.edges()) {
    ResearchLink l;
    l.observation.from = e.from;
    l.observation.to = e.to;
    l.observation.delay_ms = value(e.latency_ms);
    l.observation.loss_ratio = value(e.loss_ratio);
    l.observation.capacity_mbps = value(e.bandwidth_mbps);
    // Utilization: not carried by the forwarding graph; left absent.
    const auto it = startup.find(e.from + "->" + e.to);
    if (it != startup.end()) l.startup_delay_ms = it->second;
    const auto* from = graph.FindNode(e.from);
    const auto* to = graph.FindNode(e.to);
    l.available = e.available && from && to && from->available && to->available;
    t.graph.links.push_back(l);
  }
  t.installed_path = view.installed_path;
  if (!view.installed_path.empty()) {
    t.source = view.installed_path.front();
    t.destination = view.installed_path.back();
  }
  t.seconds_since_last_switch = view.seconds_since_applied_switch;
  std::ostringstream d;
  d << "controlled: " << description << "; AAFR runtime forwarding graph '" << graph.id() << "' v" << graph.version()
    << ", value provenance";
  for (const auto& [k, n] : provenance) d << " " << k << "=" << n;
  d << "; utilization not measured";
  t.description = d.str();
  return t;
}

const char* ToString(ResearchRuntimeState state) {
  switch (state) {
    case ResearchRuntimeState::Unavailable: return "unavailable";
    case ResearchRuntimeState::Idle: return "idle";
    case ResearchRuntimeState::Evaluating: return "evaluating";
    case ResearchRuntimeState::Decided: return "decided";
    case ResearchRuntimeState::Error: return "error";
  }
  return "unknown";
}

}  // namespace aafr::research
