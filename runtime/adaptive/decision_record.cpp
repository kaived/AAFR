#include "runtime/adaptive/decision_record.h"

#include <algorithm>
#include <chrono>
#include <sstream>

#include "experiments/benchmark/benchmark_runner.h"

namespace aafr::runtime {

bool DecisionRecord::HasReason(RuntimeReason reason) const {
  return std::find(reasons.begin(), reasons.end(), reason) != reasons.end();
}

namespace {

class JsonObject {
 public:
  JsonObject& Str(const char* key, const std::string& value) {
    Key(key);
    out_ << '"' << benchmark::JsonEscape(value) << '"';
    return *this;
  }
  JsonObject& Num(const char* key, double value) {
    Key(key);
    out_.precision(9);
    out_ << value;
    return *this;
  }
  JsonObject& Opt(const char* key, const std::optional<double>& value) {
    Key(key);
    if (value) {
      out_.precision(9);
      out_ << *value;
    } else {
      out_ << "null";
    }
    return *this;
  }
  JsonObject& UInt(const char* key, std::uint64_t value) {
    Key(key);
    out_ << value;
    return *this;
  }
  JsonObject& Int(const char* key, std::int64_t value) {
    Key(key);
    out_ << value;
    return *this;
  }
  JsonObject& Bool(const char* key, bool value) {
    Key(key);
    out_ << (value ? "true" : "false");
    return *this;
  }
  JsonObject& Raw(const char* key, const std::string& json) {
    Key(key);
    out_ << json;
    return *this;
  }
  JsonObject& StrList(const char* key, const std::vector<std::string>& values) {
    Key(key);
    out_ << '[';
    for (std::size_t i = 0; i < values.size(); ++i) out_ << (i ? "," : "") << '"' << benchmark::JsonEscape(values[i]) << '"';
    out_ << ']';
    return *this;
  }
  std::string Done() { return out_.str() + "}"; }

 private:
  void Key(const char* key) {
    out_ << (first_ ? "{" : ",") << '"' << key << "\":";
    first_ = false;
  }
  std::ostringstream out_;
  bool first_{true};
};

std::string AlgorithmText(const std::optional<algorithms::AlgorithmId>& id) {
  return id ? algorithms::ToString(*id) : "";
}

std::vector<std::string> ReasonTexts(const std::vector<RuntimeReason>& reasons) {
  std::vector<std::string> out;
  for (RuntimeReason r : reasons) out.push_back(ToString(r));
  return out;
}

std::string GateJson(const GateResult& g) {
  return JsonObject()
      .Str("gate", g.gate)
      .Bool("applicable", g.applicable)
      .Bool("passed", g.passed)
      .Opt("remaining_ms", g.remaining_ms)
      .Str("detail", g.detail)
      .Done();
}

}  // namespace

std::string ToJson(const DecisionRecord& r) {
  std::string ranking = "[";
  for (std::size_t i = 0; i < r.ranking.size(); ++i) {
    ranking += (i ? "," : "") + JsonObject()
                                    .Str("algorithm", algorithms::ToString(r.ranking[i].algorithm))
                                    .Str("path", forwarding::PathText(r.ranking[i].path))
                                    .Opt("objective", r.ranking[i].objective)
                                    .Num("computation_ms", r.ranking[i].computation_ms)
                                    .Str("note", r.ranking[i].note)
                                    .Done();
  }
  ranking += "]";
  std::string stability = "null";
  if (r.stability) {
    std::string gates = "[";
    const auto list = r.stability->Gates();
    for (std::size_t i = 0; i < list.size(); ++i) gates += (i ? "," : "") + GateJson(list[i]);
    gates += "]";
    stability = JsonObject()
                    .Bool("allowed", r.stability->allowed)
                    .Bool("safety_override", r.stability->safety_override)
                    .Raw("gates", gates)
                    .StrList("blocking", ReasonTexts(r.stability->blocking))
                    .Done();
  }
  const auto wall_ms = std::chrono::duration_cast<std::chrono::milliseconds>(r.timestamp.time_since_epoch()).count();
  return JsonObject()
      .Str("schema", r.schema)
      .Str("decision_id", r.decision_id)
      .Str("session_id", r.session_id)
      .Int("monotonic_ns", r.monotonic_ns)
      .Int("timestamp_unix_ms", static_cast<std::int64_t>(wall_ms))
      .Str("evaluation_id", r.evaluation_id)
      .UInt("network_state_id", r.network_state_id)
      .Str("network_validity", r.network_validity)
      .Str("measurement_quality", r.measurement_quality)
      .Str("graph_id", r.graph_id)
      .UInt("graph_version", r.graph_version)
      .Str("constraint_version", r.constraint_version)
      .Str("trigger", ToString(r.trigger))
      .StrList("coalesced_triggers", r.coalesced_triggers)
      .Str("trigger_detail", r.trigger_detail)
      .Str("provider_id", r.provider_id)
      .Str("recommendation_status", r.recommendation_status)
      .Str("recommended_algorithm", AlgorithmText(r.recommended_algorithm))
      .Raw("ranking", ranking)
      .StrList("eligible_algorithms", r.eligible_algorithms)
      .StrList("overridden_checks", r.overridden_checks)
      .Str("evidence_source", r.evidence_source)
      .Opt("estimated_improvement", r.estimated_improvement)
      .Opt("estimated_improvement_relative", r.estimated_improvement_relative)
      .Str("recommendation_explanation", r.recommendation_explanation)
      .Str("recommendation_mode", r.recommendation_mode)
      .Str("recommendation_source", r.recommendation_source)
      .Raw("model", r.model ? ToJson(*r.model) : std::string("null"))
      .Bool("probing_attempted", r.probing_attempted)
      .Str("probing_unavailable_reason", r.probing_unavailable_reason)
      .Str("probe_scenario_id", r.probe_scenario_id)
      .UInt("incumbent_samples", r.incumbent_samples)
      .UInt("challenger_samples", r.challenger_samples)
      .UInt("paired_samples", r.paired_samples)
      .Str("statistical_method", r.statistical_method)
      .Opt("latency_ratio", r.latency_ratio)
      .Opt("ratio_ci_low", r.ratio_ci_low)
      .Opt("ratio_ci_high", r.ratio_ci_high)
      .Num("ci_level", r.ci_level)
      .Opt("p_holm", r.p_holm)
      .Str("assumption_validation", r.assumption_validation)
      .Str("evidence_validity", r.evidence_validity)
      .Opt("incumbent_failure_rate", r.incumbent_failure_rate)
      .Opt("challenger_failure_rate", r.challenger_failure_rate)
      .Bool("simulated_delay", r.simulated_delay)
      .Str("evidence_explanation", r.evidence_explanation)
      .Raw("stability", stability)
      .Bool("safety_override", r.safety_override)
      .Str("safety_reason", r.safety_reason ? ToString(*r.safety_reason) : "")
      .Str("previous_algorithm", AlgorithmText(r.previous_algorithm))
      .Str("previous_path", forwarding::PathText(r.previous_path))
      .Str("candidate_algorithm", AlgorithmText(r.candidate_algorithm))
      .Str("candidate_path", forwarding::PathText(r.candidate_path))
      .Str("decision_type", ToString(r.type))
      .StrList("reason_codes", ReasonTexts(r.reasons))
      .Str("explanation", r.explanation)
      .Str("activation_request_id", r.activation_request_id)
      .Str("activation_status", ToString(r.activation))
      .Str("confirmed_algorithm", AlgorithmText(r.confirmed_algorithm))
      .Str("confirmed_path", forwarding::PathText(r.confirmed_path))
      .UInt("config_version", r.config_version)
      .Str("recovery_status", ToString(r.recovery))
      .Str("execution_detail", r.execution_detail)
      .Opt("activation_latency_ms", r.activation_latency_ms)
      .Num("algorithm_computation_ms", r.algorithm_computation_ms)
      .Num("decision_ms", r.decision_ms)
      .Num("probe_overhead_ms", r.probe_overhead_ms)
      .UInt("probe_requests", r.probe_requests)
      .Num("outcome_window_ms", r.outcome_window_ms)
      .Str("outcome_status", r.outcome_status)
      .Done();
}

std::string ToJson(const OutcomeRevision& o) {
  const ForwardingPerformance& p = o.performance;
  return JsonObject()
      .Str("schema", "aafr.outcome_revision.v1")
      .Str("decision_id", o.decision_id)
      .UInt("revision", o.revision)
      .UInt("config_version", o.config_version)
      .Str("algorithm", AlgorithmText(o.algorithm))
      .UInt("network_state_id", o.network_state_id)
      .Num("window_ms", o.window_ms)
      .Bool("window_complete", o.window_complete)
      .UInt("health_checks", o.health_checks)
      .UInt("application_requests", o.application_requests)
      .UInt("browser_streams", o.browser_streams)
      .UInt("browser_stream_failures", o.browser_stream_failures)
      .Str("metrics_version", p.metrics_version)
      .UInt("requests", p.requests)
      .UInt("successes", p.successes)
      .UInt("failures", p.failures)
      .UInt("timeouts", p.timeouts)
      .Opt("latency_median_ms", p.latency_median_ms)
      .Opt("latency_p90_ms", p.latency_p90_ms)
      .Opt("jitter_ms", p.jitter_ms)
      .Opt("failure_rate", p.failure_rate)
      .Opt("goodput_bytes_per_s", p.goodput_bytes_per_s)
      .StrList("missing", p.missing)
      .Done();
}

observability::DecisionRecord ToObservability(const DecisionRecord& r) {
  observability::DecisionRecord o;
  o.decision_id = r.decision_id;
  o.timestamp = r.timestamp;
  o.network_state_ref = r.network_state_id ? "state#" + std::to_string(r.network_state_id) : "unavailable";
  o.previous_algorithm = AlgorithmText(r.previous_algorithm);
  o.candidate_algorithm = AlgorithmText(r.candidate_algorithm);
  if (r.stability) {
    for (const GateResult& g : r.stability->Gates()) o.stability_gates.push_back({g.gate, g.passed, g.detail});
  }
  if (!r.probing_unavailable_reason.empty()) o.constraints.push_back({"probing", false, r.probing_unavailable_reason});
  switch (r.type) {
    case DecisionType::Retain:
    case DecisionType::Discarded:
      o.decision_type = observability::DecisionType::Retain;
      break;
    case DecisionType::SwitchRejected:
    case DecisionType::Unavailable:
      o.decision_type = observability::DecisionType::SwitchRejected;
      break;
    default:
      o.decision_type = observability::DecisionType::SwitchApproved;
      break;
  }
  o.reason_code = r.reasons.empty() ? "" : ToString(r.reasons.front());
  o.explanation = r.explanation;
  switch (r.activation) {
    case ActivationStatus::NotRequested: o.execution = observability::ExecutionConfirmation::NotApplicable; break;
    case ActivationStatus::Pending: o.execution = observability::ExecutionConfirmation::Pending; break;
    case ActivationStatus::Confirmed: o.execution = observability::ExecutionConfirmation::Committed; break;
    default:
      o.execution = r.recovery == RecoveryStatus::PreviousRetained ? observability::ExecutionConfirmation::RolledBack
                                                                    : observability::ExecutionConfirmation::Failed;
      break;
  }
  return o;
}

const char* ToString(DecisionType value) {
  switch (value) {
    case DecisionType::InitialActivation: return "initial_activation";
    case DecisionType::Retain: return "retain";
    case DecisionType::Switch: return "switch";
    case DecisionType::SwitchRejected: return "switch_rejected";
    case DecisionType::SafetySwitch: return "safety_switch";
    case DecisionType::Recovery: return "recovery";
    case DecisionType::Unavailable: return "unavailable";
    case DecisionType::Discarded: return "discarded";
  }
  return "unknown";
}

const char* ToString(ActivationStatus value) {
  switch (value) {
    case ActivationStatus::NotRequested: return "not_requested";
    case ActivationStatus::Pending: return "pending";
    case ActivationStatus::Confirmed: return "confirmed";
    case ActivationStatus::Failed: return "failed";
    case ActivationStatus::TimedOut: return "timed_out";
    case ActivationStatus::Cancelled: return "cancelled";
  }
  return "unknown";
}

const char* ToString(RecoveryStatus value) {
  switch (value) {
    case RecoveryStatus::NotNeeded: return "not_needed";
    case RecoveryStatus::PreviousRetained: return "previous_retained";
    case RecoveryStatus::Recovered: return "recovered";
    case RecoveryStatus::Failed: return "failed";
  }
  return "unknown";
}

}  // namespace aafr::runtime
