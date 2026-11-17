// V0.6 deterministic recommendation provider.

#include "runtime/adaptive/recommendation_provider.h"

#include <cmath>

#include "tests/support/runtime_test_support.h"
#include "tests/support/test_support.h"

using namespace aafr;
using namespace aafr::runtime;
using algorithms::AlgorithmId;
using aafr::test::kPathA;
using aafr::test::kPathB;

namespace {

CandidateEvaluation Candidate(AlgorithmId id, const forwarding::Path& path, double objective, double ms = 0.1) {
  CandidateEvaluation c;
  c.algorithm = id;
  c.eligibility = algorithms::EligibilityStatus::Eligible;
  c.considered = true;
  c.computed = true;
  c.computation_status = "path_found";
  c.path = path;
  c.path_valid = true;
  c.path_feasible = true;
  c.objective = objective;
  c.computation_ms = ms;
  return c;
}

RecommendationInput Input(std::vector<CandidateEvaluation> candidates) {
  static const auto graph = std::make_shared<const forwarding::ForwardingGraph>(aafr::test::TwoPathGraph());
  RecommendationInput in;
  in.graph = graph;
  in.problem = aafr::test::ProblemFor(*graph);
  in.candidates = std::move(candidates);
  return in;
}

void RanksByObjective() {
  auto in = Input({Candidate(AlgorithmId::Dijkstra, kPathB, 21.0), Candidate(AlgorithmId::AntColonyOptimization, kPathA, 3.0)});
  const Recommendation r = DeterministicRecommendationProvider().Recommend(in);
  CHECK(r.status == RecommendationStatus::Recommended);
  CHECK(r.recommended == AlgorithmId::AntColonyOptimization);
  CHECK(r.ranking.size() == 2);
  CHECK(r.ranking[0].path == kPathA);
  CHECK(r.evidence == EvidenceQuality::EstimateOnly);
  CHECK(r.estimated_objective && *r.estimated_objective == 3.0);
  CHECK(aafr::test::Contains(r.explanation, "not measured"));
  CHECK(!r.estimated_improvement);  // no active configuration
}

void TieBreakRules() {
  // Equal objectives: a candidate on the active path wins over AlgorithmId order.
  auto in = Input({Candidate(AlgorithmId::Dijkstra, kPathA, 3.0), Candidate(AlgorithmId::GeneticAlgorithm, kPathB, 3.0)});
  in.active = AlgorithmId::GeneticAlgorithm;
  in.active_path = kPathB;
  Recommendation r = DeterministicRecommendationProvider().Recommend(in);
  CHECK(r.recommended == AlgorithmId::GeneticAlgorithm);
  // Same path for both: AlgorithmId order (exact first), not the active label.
  in = Input({Candidate(AlgorithmId::BellmanFord, kPathA, 3.0, 0.01), Candidate(AlgorithmId::Dijkstra, kPathA, 3.0, 0.5)});
  in.active = AlgorithmId::BellmanFord;
  in.active_path = kPathB;
  r = DeterministicRecommendationProvider().Recommend(in);
  CHECK(r.recommended == AlgorithmId::Dijkstra);
  // Determinism: same input, same output.
  const Recommendation again = DeterministicRecommendationProvider().Recommend(in);
  CHECK(again.recommended == r.recommended && again.explanation == r.explanation);
}

void ExcludesIneligibleAndInvalid() {
  CandidateEvaluation ineligible = Candidate(AlgorithmId::Dijkstra, kPathA, 1.0);
  ineligible.considered = false;
  ineligible.eligibility = algorithms::EligibilityStatus::Ineligible;
  CandidateEvaluation invalid = Candidate(AlgorithmId::AStar, kPathA, 1.0);
  invalid.path_valid = false;
  CandidateEvaluation infeasible = Candidate(AlgorithmId::BellmanFord, kPathA, 1.0);
  infeasible.path_feasible = false;
  auto in = Input({ineligible, invalid, infeasible, Candidate(AlgorithmId::GeneticAlgorithm, kPathB, 21.0)});
  const Recommendation r = DeterministicRecommendationProvider().Recommend(in);
  CHECK(r.recommended == AlgorithmId::GeneticAlgorithm);
  CHECK(r.ranking.size() == 1);
}

void NoUsableCandidates() {
  CandidateEvaluation ineligible = Candidate(AlgorithmId::Dijkstra, kPathA, 1.0);
  ineligible.considered = false;
  Recommendation r = DeterministicRecommendationProvider().Recommend(Input({ineligible}));
  CHECK(r.status == RecommendationStatus::Unavailable);
  CHECK(!r.recommended);
  CandidateEvaluation failed = Candidate(AlgorithmId::Dijkstra, kPathA, 1.0);
  failed.objective.reset();
  failed.path_valid = false;
  r = DeterministicRecommendationProvider().Recommend(Input({failed}));
  CHECK(r.status == RecommendationStatus::InsufficientEvidence);
  RecommendationInput no_graph;
  r = DeterministicRecommendationProvider().Recommend(no_graph);
  CHECK(r.status == RecommendationStatus::Error);
}

void EstimatedImprovementAgainstActivePath() {
  auto in = Input({Candidate(AlgorithmId::Dijkstra, kPathA, 3.0), Candidate(AlgorithmId::AStar, kPathB, 21.0)});
  in.active = AlgorithmId::AStar;
  in.active_path = kPathB;
  Recommendation r = DeterministicRecommendationProvider().Recommend(in);
  CHECK(r.estimated_improvement && std::fabs(*r.estimated_improvement - 18.0) < 1e-9);
  CHECK(r.estimated_improvement_relative && std::fabs(*r.estimated_improvement_relative - 18.0 / 21.0) < 1e-9);
  // An active path that does not validate gives no estimate (never zero).
  in.active_path = forwarding::Path{"entry", "dest"};
  r = DeterministicRecommendationProvider().Recommend(in);
  CHECK(!r.estimated_improvement);
  CHECK(aafr::test::Contains(r.explanation, "does not validate"));
}

void BenchmarkEvidenceRanking() {
  // Real (small) V0.5 benchmark aggregated per algorithm.
  benchmark::BenchmarkConfig config;
  config.scenario_ids = {forwarding::BenchmarkScenarioIds().front()};
  config.algorithms = {AlgorithmId::Dijkstra, AlgorithmId::GeneticAlgorithm};
  config.seeds = {1, 2};
  const auto report = benchmark::RunBenchmark(config);
  const runtime::BenchmarkEvidence evidence = EvidenceFromBenchmark(report, "unit-test benchmark");
  CHECK(evidence.algorithms.count(AlgorithmId::Dijkstra) == 1);
  CHECK(evidence.algorithms.at(AlgorithmId::Dijkstra).runs >= 1);
  CHECK(evidence.algorithms.at(AlgorithmId::Dijkstra).FeasibleRate() == 1.0);
  // Synthetic evidence: an unreliable algorithm ranks after a reliable one
  // even with a lower objective.
  runtime::BenchmarkEvidence synthetic;
  synthetic.source = "synthetic";
  synthetic.algorithms[AlgorithmId::Dijkstra] = {10, 10, 10, 0.0, 0.1};
  synthetic.algorithms[AlgorithmId::WhaleOptimizationAlgorithm] = {10, 5, 2, 30.0, 1.0};
  auto in = Input({Candidate(AlgorithmId::WhaleOptimizationAlgorithm, kPathA, 3.0), Candidate(AlgorithmId::Dijkstra, kPathB, 21.0)});
  in.benchmark = &synthetic;
  const Recommendation r = DeterministicRecommendationProvider().Recommend(in);
  CHECK(r.evidence == EvidenceQuality::WithBenchmark);
  CHECK(r.recommended == AlgorithmId::Dijkstra);
  CHECK(r.ranking[1].note == "below benchmark feasible-rate threshold");
  // Evidence for another objective version is ignored.
  synthetic.objective_version = 99;
  CHECK(DeterministicRecommendationProvider().Recommend(in).recommended == AlgorithmId::WhaleOptimizationAlgorithm);
}

}  // namespace

int main() {
  return aafr::test::RunTests({
      {"ranks_by_objective", RanksByObjective},
      {"tie_break_rules", TieBreakRules},
      {"excludes_ineligible_and_invalid", ExcludesIneligibleAndInvalid},
      {"no_usable_candidates", NoUsableCandidates},
      {"estimated_improvement_against_active_path", EstimatedImprovementAgainstActivePath},
      {"benchmark_evidence_ranking", BenchmarkEvidenceRanking},
  });
}
