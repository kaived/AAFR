#include "runtime/adaptive/recommendation_provider.h"

#include <algorithm>
#include <cmath>
#include <sstream>

namespace aafr::runtime {

BenchmarkEvidence EvidenceFromBenchmark(const benchmark::BenchmarkReport& report, const std::string& source) {
  BenchmarkEvidence evidence;
  evidence.source = source;
  std::map<algorithms::AlgorithmId, std::vector<double>> gaps, times;
  for (const benchmark::BenchmarkSummary& s : report.summaries) {
    AlgorithmBenchmarkEvidence& e = evidence.algorithms[s.algorithm];
    e.runs += s.runs;
    e.feasible += s.feasible;
    e.at_reference += s.at_reference;
  }
  for (const benchmark::BenchmarkRun& run : report.runs) {
    times[run.algorithm].push_back(run.result.duration_ms);
    if (run.gap_percent) gaps[run.algorithm].push_back(*run.gap_percent);
  }
  for (auto& entry : evidence.algorithms) {
    const auto& g = gaps[entry.first];
    if (!g.empty()) {
      double sum = 0.0;
      for (double v : g) sum += v;
      entry.second.mean_gap_percent = sum / static_cast<double>(g.size());
    }
    entry.second.median_computation_ms = benchmark::Summarize(times[entry.first]).median;
  }
  return evidence;
}

namespace {
std::string Fixed(double v, int precision = 3) {
  std::ostringstream out;
  out.setf(std::ios::fixed);
  out.precision(precision);
  out << v;
  return out.str();
}
}  // namespace

// Ranking rules, in order:
//  1. Only candidates that are Usable(): considered by eligibility, computed,
//     valid and feasible on this graph version, with a J(P).
//  2. With benchmark evidence (same objective version), candidates meeting
//     min_benchmark_feasible_rate rank before those that do not.
//  3. Lower J(P) first; objectives within tie_tolerance are tied.
//  4. Ties: a candidate that forwards over the active path first (staying
//     costs nothing), then AlgorithmId order (exact algorithms before
//     metaheuristics), then lower computation time.
// The active algorithm gets no tie preference of its own: when a tie forces
// a path change anyway, the established exact algorithm is chosen, so an
// algorithm left for a safety reason is chosen again once it is eligible.
Recommendation DeterministicRecommendationProvider::Recommend(const RecommendationInput& input) const {
  Recommendation r;
  r.provider_id = Id() + "/v" + Version();
  r.source = "forwarding_objective_v" + std::to_string(forwarding::ForwardingObjective::kVersion) +
             " on graph version " + std::to_string(input.graph ? input.graph->version() : 0);
  if (!input.graph) {
    r.status = RecommendationStatus::Error;
    r.error = "no forwarding graph";
    r.explanation = r.error;
    return r;
  }
  const bool benchmark_usable =
      input.benchmark && input.benchmark->objective_version == forwarding::ForwardingObjective::kVersion &&
      !input.benchmark->algorithms.empty();

  std::vector<const CandidateEvaluation*> usable;
  std::size_t considered = 0;
  for (const CandidateEvaluation& c : input.candidates) {
    if (c.considered) ++considered;
    if (c.Usable()) usable.push_back(&c);
  }
  if (usable.empty()) {
    r.status = considered == 0 ? RecommendationStatus::Unavailable : RecommendationStatus::InsufficientEvidence;
    r.explanation = considered == 0 ? "no algorithm passed eligibility"
                                    : std::to_string(considered) +
                                          " eligible algorithm(s), none produced a valid feasible path";
    return r;
  }
  const auto meets_benchmark = [&](algorithms::AlgorithmId id) {
    if (!benchmark_usable) return true;
    const auto found = input.benchmark->algorithms.find(id);
    return found != input.benchmark->algorithms.end() &&
           found->second.FeasibleRate() >= config_.min_benchmark_feasible_rate;
  };
  const auto on_active_path = [&](const CandidateEvaluation* c) {
    return input.active_path && c->path == *input.active_path;
  };
  std::stable_sort(usable.begin(), usable.end(), [&](const CandidateEvaluation* a, const CandidateEvaluation* b) {
    const bool ma = meets_benchmark(a->algorithm), mb = meets_benchmark(b->algorithm);
    if (ma != mb) return ma;
    if (std::fabs(*a->objective - *b->objective) > config_.tie_tolerance) return *a->objective < *b->objective;
    const bool pa = on_active_path(a), pb = on_active_path(b);
    if (pa != pb) return pa;
    if (a->algorithm != b->algorithm) return static_cast<int>(a->algorithm) < static_cast<int>(b->algorithm);
    return a->computation_ms < b->computation_ms;
  });
  for (const CandidateEvaluation* c : usable) {
    RankedCandidate rc;
    rc.algorithm = c->algorithm;
    rc.path = c->path;
    rc.objective = c->objective;
    rc.computation_ms = c->computation_ms;
    if (benchmark_usable && !meets_benchmark(c->algorithm)) rc.note = "below benchmark feasible-rate threshold";
    r.ranking.push_back(rc);
  }
  const RankedCandidate& best = r.ranking.front();
  r.status = RecommendationStatus::Recommended;
  r.recommended = best.algorithm;
  r.estimated_objective = best.objective;
  r.evidence = benchmark_usable ? EvidenceQuality::WithBenchmark : EvidenceQuality::EstimateOnly;
  // The improvement estimate compares the active configuration's path with
  // the recommended path on this same graph version and objective.
  std::optional<double> active_objective;
  if (input.active_path) {
    const forwarding::PathValidation v = forwarding::ValidatePath(*input.graph, input.problem, *input.active_path);
    if (v.valid && v.objective) active_objective = v.objective;
  }
  if (active_objective && best.objective) {
    r.estimated_improvement = *active_objective - *best.objective;
    if (std::fabs(*active_objective) > 1e-12) {
      r.estimated_improvement_relative = *r.estimated_improvement / std::fabs(*active_objective);
    }
  }
  std::ostringstream text;
  text << algorithms::ToString(best.algorithm) << " ranks first of " << r.ranking.size() << " usable candidate(s) with J(P)="
       << Fixed(*best.objective) << " on path " << forwarding::PathText(best.path);
  if (active_objective) {
    text << "; active path J(P)=" << Fixed(*active_objective) << " (estimated improvement "
         << Fixed(*r.estimated_improvement) << ")";
  } else if (input.active_path) {
    text << "; the active path does not validate on this graph version";
  }
  if (benchmark_usable) text << "; benchmark evidence from " << input.benchmark->source;
  text << ". J(P) is computed from configured/simulated edge values, not measured.";
  r.explanation = text.str();
  return r;
}

const char* ToString(RecommendationStatus value) {
  switch (value) {
    case RecommendationStatus::Recommended: return "recommended";
    case RecommendationStatus::InsufficientEvidence: return "insufficient_evidence";
    case RecommendationStatus::Unavailable: return "unavailable";
    case RecommendationStatus::Error: return "error";
  }
  return "unknown";
}

const char* ToString(EvidenceQuality value) {
  switch (value) {
    case EvidenceQuality::None: return "none";
    case EvidenceQuality::EstimateOnly: return "estimate_only";
    case EvidenceQuality::WithBenchmark: return "estimate_with_benchmark";
  }
  return "unknown";
}

}  // namespace aafr::runtime
