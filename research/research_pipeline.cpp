#include "research/research_pipeline.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <set>
#include <sstream>

namespace aafr::research {
namespace {

bool SimplePathOnGraph(const ResearchGraph& g, const std::vector<std::string>& p) {
  if (p.size() < 2) return false;
  std::set<std::string> seen(p.begin(), p.end());
  return seen.size() == p.size() && PathLinks(g, p).has_value();
}

// ---- JSON writing ----

class Json {
 public:
  std::string str() const { return out_.str(); }
  Json& Begin(char c) { Comma(); out_ << c; first_ = true; return *this; }
  Json& End(char c) { out_ << c; first_ = false; return *this; }
  Json& Key(const std::string& k) { Comma(); Quote(k); out_ << ':'; first_ = true; return *this; }
  Json& Str(const std::string& v) { Comma(); Quote(v); return *this; }
  Json& Num(double v) {
    Comma();
    if (!std::isfinite(v)) out_ << "null";
    else {
      char buffer[32];
      std::snprintf(buffer, sizeof(buffer), "%.17g", v);
      out_ << buffer;
    }
    return *this;
  }
  Json& Int(long long v) { Comma(); out_ << v; return *this; }
  Json& Bool(bool v) { Comma(); out_ << (v ? "true" : "false"); return *this; }
  Json& Null() { Comma(); out_ << "null"; return *this; }
  Json& Opt(const std::optional<double>& v) { return v ? Num(*v) : Null(); }
  Json& Path(const std::vector<std::string>& p) {
    Begin('[');
    for (const auto& n : p) Str(n);
    return End(']');
  }
  Json& Algorithms(const std::vector<PoolAlgorithm>& a) {
    Begin('[');
    for (const auto x : a) Str(ToString(x));
    return End(']');
  }
  Json& ScoreValue(const Score& s) {
    Begin('{');
    Key("value").Opt(s.value);
    if (!s.value) Key("unavailable_reason").Str(s.unavailable_reason);
    return End('}');
  }

 private:
  void Comma() {
    if (!first_) out_ << ',';
    first_ = false;
  }
  void Quote(const std::string& s) {
    out_ << '"';
    for (const char c : s) {
      switch (c) {
        case '"': out_ << "\\\""; break;
        case '\\': out_ << "\\\\"; break;
        case '\n': out_ << "\\n"; break;
        case '\r': out_ << "\\r"; break;
        case '\t': out_ << "\\t"; break;
        default:
          if (static_cast<unsigned char>(c) < 0x20) {
            char buffer[8];
            std::snprintf(buffer, sizeof(buffer), "\\u%04x", static_cast<unsigned>(static_cast<unsigned char>(c)));
            out_ << buffer;
          } else {
            out_ << c;
          }
      }
    }
    out_ << '"';
  }
  std::ostringstream out_;
  bool first_{true};
};

}  // namespace

std::vector<std::string> ValidateTelemetry(const TelemetrySnapshot& t) {
  std::vector<std::string> issues;
  if (t.description.empty()) issues.push_back("telemetry snapshot has no description");
  for (const auto& i : ValidateGraph(t.graph)) issues.push_back("graph: " + i);
  const std::set<std::string> nodes(t.graph.nodes.begin(), t.graph.nodes.end());
  if (!nodes.count(t.source) || !nodes.count(t.destination)) issues.push_back("unknown source or destination");
  if (t.source == t.destination) issues.push_back("source equals destination");
  if (t.installed_path.empty() || t.installed_path.front() != t.source || t.installed_path.back() != t.destination ||
      !SimplePathOnGraph(t.graph, t.installed_path)) {
    issues.push_back("installed path is not a simple source-destination path over available links");
  }
  if (t.seconds_since_last_switch && !(std::isfinite(*t.seconds_since_last_switch) && *t.seconds_since_last_switch >= 0.0)) {
    issues.push_back("seconds since last switch must be finite and >= 0");
  }
  return issues;
}

TournamentPlan PlanTournament(const TelemetrySnapshot& telemetry, const LinkWeightModel& model,
                              const NormalizationStatistics& statistics, const PipelineConfig& config) {
  TournamentPlan plan;
  plan.issues = ValidateTelemetry(telemetry);
  for (const auto& i : statistics.Validate()) plan.issues.push_back("statistics: " + i);
  if (!plan.issues.empty()) return plan;

  plan.predicted_graph = telemetry.graph;
  plan.predictions = AssignPredictedWeights(model, plan.predicted_graph);
  const ResearchGraph& g = plan.predicted_graph;
  const std::string& s = telemetry.source;
  const std::string& d = telemetry.destination;
  std::vector<Candidate> candidates{StaticDelayDijkstra(g, s, d), PredictedWeightAStar(g, s, d),
                                    AntColonyOptimization(g, s, d, statistics, config.aco),
                                    GeneticAlgorithm(g, s, d, statistics, config.ga)};
  for (const Candidate& c : candidates) {
    CandidateRecord r;
    r.candidate = c;
    if (c.status == CandidateStatus::PathFound) r.evaluation = EvaluatePath(g, c.path, statistics);
    plan.candidates.push_back(std::move(r));
  }
  const CandidateArms arms = DistinctChallengers(candidates, telemetry.installed_path);
  plan.arms.push_back(PlannedArm{"incumbent", true, telemetry.installed_path, arms.installed_algorithms});
  for (std::size_t i = 0; i < arms.challengers.size(); ++i) {
    plan.arms.push_back(PlannedArm{"challenger-" + std::to_string(i + 1), false, arms.challengers[i].path,
                                   arms.challengers[i].algorithms});
  }
  plan.tournament_needed = plan.arms.size() > 1;
  return plan;
}

ResearchDecisionRecord DecideTournament(const TelemetrySnapshot& telemetry, const TournamentPlan& plan,
                                        const std::optional<ProbeObservations>& observations,
                                        const LinkWeightModel& model, const NormalizationStatistics& statistics,
                                        const PipelineConfig& config) {
  ResearchDecisionRecord r;
  r.telemetry_origin = telemetry.origin;
  r.telemetry_description = telemetry.description;
  r.model_id = model.info().model_id;
  r.model_source_commit = model.info().source_commit;
  r.statistics_source = statistics.source;
  r.source = telemetry.source;
  r.destination = telemetry.destination;
  r.installed_path = telemetry.installed_path;
  r.seconds_since_last_switch = telemetry.seconds_since_last_switch;
  r.plan = plan;
  if (!plan.issues.empty()) {
    r.reason = "invalid plan: " + plan.issues.front();
    return r;
  }
  if (!plan.tournament_needed) {
    r.verdict = ArbitrationVerdict::Keep;
    r.reason = "no algorithm proposed a path other than the installed one; no tournament";
    return r;
  }
  if (!observations) {
    r.reason = "a tournament is needed but no probe observations were supplied";
    return r;
  }
  r.probe_origin = observations->origin;
  r.probe_description = observations->description;
  const ProbeTournament& t = observations->tournament;
  if (observations->description.empty()) r.observation_issues.push_back("probe observations have no description");
  if (t.arms.size() != plan.arms.size()) r.observation_issues.push_back("observed arms differ in number from the planned arms");
  for (const PlannedArm& a : plan.arms) {
    const auto it = std::find_if(t.arms.begin(), t.arms.end(), [&](const ProbeArm& o) { return o.arm_id == a.arm_id; });
    if (it == t.arms.end()) r.observation_issues.push_back("no observations for planned arm " + a.arm_id);
    else if (it->path != a.path || it->incumbent != a.incumbent) {
      r.observation_issues.push_back("observations for " + a.arm_id + " are for a different path or role");
    }
  }
  for (const auto& i : ValidateTournament(t)) r.observation_issues.push_back(i);
  if (!r.observation_issues.empty()) {
    r.reason = "probe observations do not match the plan: " + r.observation_issues.front();
    return r;
  }

  // Measured outcome and goodness of every arm.
  std::map<std::string, Score> goodness;
  for (const PlannedArm& a : plan.arms) {
    const ProbeArm& o = *std::find_if(t.arms.begin(), t.arms.end(), [&](const ProbeArm& x) { return x.arm_id == a.arm_id; });
    const ArmSummary s = SummarizeArm(o);
    ArmRecord ar;
    ar.arm_id = a.arm_id;
    ar.incumbent = a.incumbent;
    ar.path = a.path;
    ar.algorithms = a.algorithms;
    ar.sent = s.sent;
    ar.returned = s.returned;
    ar.mean_rtt_ms = s.mean_rtt_ms;
    ar.delivery_ratio = s.delivery_ratio;
    ar.complete_frames = static_cast<std::size_t>(
        std::count_if(s.frames.begin(), s.frames.end(), [](const FrameSummary& f) { return f.complete; }));
    ar.valid_frames = s.valid_frames;
    if (const auto links = PathLinks(telemetry.graph, a.path)) ar.bandwidth_mbps = ComputePathMetrics(*links).bandwidth_mbps;
    ArmOutcome outcome;
    outcome.hops = a.path.size() - 1;
    outcome.bandwidth_mbps = ar.bandwidth_mbps;
    outcome.mean_rtt_ms = s.mean_rtt_ms;
    outcome.delivery_ratio = s.delivery_ratio;
    ar.goodness = Goodness(outcome, statistics);
    goodness[a.arm_id] = ar.goodness;
    r.arms.push_back(std::move(ar));
  }
  std::map<std::string, std::optional<double>> delta;
  std::map<std::string, Score> delta_score;
  for (const PlannedArm& a : plan.arms) {
    if (a.incumbent) continue;
    delta_score[a.arm_id] = GoodnessDifference(goodness[a.arm_id], goodness["incumbent"]);
    delta[a.arm_id] = delta_score[a.arm_id].value;
  }
  const ArbitrationOutcome outcome = ArbitrateTournament(t, delta, telemetry.seconds_since_last_switch, config.rule,
                                                         config.budget, config.weights);
  for (const ChallengerEvaluation& e : outcome.challengers) r.challengers.push_back(ChallengerRecord{e.arm_id, delta_score[e.arm_id], e});
  r.verdict = outcome.verdict;
  r.selected_arm = outcome.selected_arm;
  r.qualifying_arms = outcome.qualifying_arms;
  r.tied_arms = outcome.tied_arms;
  if (r.selected_arm) {
    for (const PlannedArm& a : plan.arms) {
      if (a.arm_id == *r.selected_arm) r.selected_path = a.path;
    }
  }
  r.reason = outcome.reason;
  return r;
}

std::string ToJson(const ResearchDecisionRecord& r, bool include_timing) {
  Json j;
  j.Begin('{');
  j.Key("schema").Str(r.schema);
  j.Key("telemetry").Begin('{').Key("origin").Str(ToString(r.telemetry_origin)).Key("description").Str(r.telemetry_description).End('}');
  j.Key("probes").Begin('{');
  if (r.probe_origin) j.Key("origin").Str(ToString(*r.probe_origin)).Key("description").Str(r.probe_description);
  else j.Key("origin").Null();
  j.End('}');
  j.Key("model").Begin('{').Key("id").Str(r.model_id).Key("source_commit").Str(r.model_source_commit).End('}');
  j.Key("statistics_source").Str(r.statistics_source);
  j.Key("source").Str(r.source).Key("destination").Str(r.destination).Key("installed_path").Path(r.installed_path);
  j.Key("seconds_since_last_switch").Opt(r.seconds_since_last_switch);
  j.Key("plan_issues").Begin('[');
  for (const auto& i : r.plan.issues) j.Str(i);
  j.End(']');
  j.Key("link_weights").Begin('[');
  for (std::size_t i = 0; i < r.plan.predictions.size() && i < r.plan.predicted_graph.links.size(); ++i) {
    const auto& p = r.plan.predictions[i];
    const auto& l = r.plan.predicted_graph.links[i].observation;
    j.Begin('{').Key("link").Str(l.from + "->" + l.to).Key("status").Str(ToString(p.status));
    j.Key("raw").Opt(p.raw ? std::optional<double>(static_cast<double>(*p.raw)) : std::nullopt).Key("w_link").Opt(p.weight);
    if (!p.message.empty()) j.Key("message").Str(p.message);
    j.End('}');
  }
  j.End(']');
  j.Key("candidates").Begin('[');
  for (const auto& c : r.plan.candidates) {
    const Candidate& k = c.candidate;
    j.Begin('{').Key("algorithm").Str(ToString(k.algorithm)).Key("status").Str(ToString(k.status));
    j.Key("path").Path(k.path).Key("cost_kind").Str(ToString(k.cost_kind)).Key("optimization_cost").Opt(k.optimization_cost);
    if (k.algorithm == PoolAlgorithm::AntColony || k.algorithm == PoolAlgorithm::Genetic) {
      j.Key("seed").Int(static_cast<long long>(k.seed)).Key("iterations").Int(static_cast<long long>(k.iterations));
      j.Key("evaluations").Int(static_cast<long long>(k.evaluations)).Key("rejected_paths").Int(static_cast<long long>(k.rejected_paths));
    }
    if (k.status == CandidateStatus::PathFound) {
      j.Key("quality_j").ScoreValue(c.evaluation.quality).Key("engine_cost_j_eng").ScoreValue(c.evaluation.engine_cost);
    }
    if (!k.message.empty()) j.Key("message").Str(k.message);
    if (include_timing) j.Key("duration_ms").Num(k.duration_ms);
    j.End('}');
  }
  j.End(']');
  j.Key("arms").Begin('[');
  for (const auto& a : r.plan.arms) {
    j.Begin('{').Key("arm_id").Str(a.arm_id).Key("incumbent").Bool(a.incumbent).Key("path").Path(a.path);
    j.Key("algorithms").Algorithms(a.algorithms);
    const auto it = std::find_if(r.arms.begin(), r.arms.end(), [&](const ArmRecord& x) { return x.arm_id == a.arm_id; });
    if (it != r.arms.end()) {
      j.Key("sent").Int(static_cast<long long>(it->sent)).Key("returned").Int(static_cast<long long>(it->returned));
      j.Key("mean_rtt_ms").Opt(it->mean_rtt_ms).Key("delivery_ratio").Num(it->delivery_ratio);
      j.Key("complete_frames").Int(static_cast<long long>(it->complete_frames));
      j.Key("valid_frames").Int(static_cast<long long>(it->valid_frames)).Key("bandwidth_mbps").Opt(it->bandwidth_mbps);
      j.Key("goodness").ScoreValue(it->goodness);
    }
    j.End('}');
  }
  j.End(']');
  j.Key("observation_issues").Begin('[');
  for (const auto& i : r.observation_issues) j.Str(i);
  j.End(']');
  j.Key("challengers").Begin('[');
  for (const auto& c : r.challengers) {
    const ChallengerEvaluation& e = c.evaluation;
    j.Begin('{').Key("arm_id").Str(c.arm_id).Key("delta_goodness").ScoreValue(c.delta_goodness);
    j.Key("paired_rounds").Int(static_cast<long long>(e.paired_rounds));
    j.Key("anova").Begin('{').Key("computed").Bool(e.anova.computed);
    if (e.anova.computed) {
      j.Key("f").Num(e.anova.f).Key("df_effect").Num(e.anova.df_effect).Key("df_error").Num(e.anova.df_error);
      j.Key("p").Num(e.anova.p_value).Key("partial_eta_squared").Num(e.anova.partial_eta_squared);
      j.Key("mean_difference_log10").Num(e.anova.mean_difference_log10);
      j.Key("ci_log10").Begin('[').Num(e.anova.ci_low_log10).Num(e.anova.ci_high_log10).End(']');
      j.Key("rtt_ratio").Num(e.anova.rtt_ratio).Key("ratio_ci").Begin('[').Num(e.anova.ratio_low).Num(e.anova.ratio_high).End(']');
    } else {
      j.Key("unavailable_reason").Str(e.anova.unavailable_reason);
    }
    j.End('}');
    j.Key("p_holm").Opt(e.p_holm);
    j.Key("feasibility").Begin('{').Key("loss_wilson").Begin('[').Num(e.feasibility.loss_low).Num(e.feasibility.loss_high).End(']');
    j.Key("infeasible").Bool(e.feasibility.infeasible).Key("within_budget").Bool(e.feasibility.within_budget).End('}');
    j.Key("confidence").Begin('{').Key("computed").Bool(e.confidence.computed);
    if (e.confidence.computed) {
      j.Key("s_eta").Num(e.confidence.s_eta).Key("s_ci").Num(e.confidence.s_ci).Key("s_g").Num(e.confidence.s_g);
      j.Key("c").Num(e.confidence.confidence).Key("repair").Bool(e.confidence.repair);
      j.Key("challenger_infeasible").Bool(e.confidence.challenger_infeasible);
    } else {
      j.Key("unavailable_reason").Str(e.confidence.unavailable_reason);
    }
    j.End('}');
    j.Key("decision").Begin('{').Key("confidence_ok").Bool(e.decision.confidence_ok).Key("significance_ok").Bool(e.decision.significance_ok);
    j.Key("dwell_ok").Bool(e.decision.dwell_ok).Key("qualifies").Bool(e.decision.qualifies).End('}');
    j.End('}');
  }
  j.End(']');
  j.Key("verdict").Str(ToString(r.verdict));
  if (r.selected_arm) j.Key("selected_arm").Str(*r.selected_arm).Key("selected_path").Path(r.selected_path);
  else j.Key("selected_arm").Null();
  j.Key("qualifying_arms").Path(r.qualifying_arms).Key("tied_arms").Path(r.tied_arms);
  j.Key("reason").Str(r.reason).Key("action").Str(r.action);
  j.End('}');
  return j.str();
}

const char* ToString(DataOrigin origin) {
  switch (origin) {
    case DataOrigin::Controlled: return "controlled";
    case DataOrigin::Recorded: return "recorded";
    case DataOrigin::Synthetic: return "synthetic";
  }
  return "unknown";
}

}  // namespace aafr::research
