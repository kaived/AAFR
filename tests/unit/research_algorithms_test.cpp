// Research alignment, Phase 3: the four-algorithm pool (research/
// path_algorithms). Dijkstra on start-up delays and A* on explicit predicted
// link weights are checked on the six-switch testbed, against brute force,
// and against the unchanged production Dijkstra / A* (algorithms/) on seeded
// random graphs. ACO and GA are declared and must report NotImplemented.
// J_eng uses declared test statistics, not training statistics. No XGBoost.
// Deterministic; no network access.

#include <cmath>
#include <functional>
#include <limits>
#include <set>
#include <string>

#include "algorithms/common/path_algorithm.h"
#include "research/path_algorithms.h"
#include "tests/fixtures/research/reference_values.h"
#include "tests/support/test_support.h"

using namespace aafr::research;
namespace rf = aafr::research_fixture;
namespace fwd = aafr::forwarding;
namespace alg = aafr::algorithms;

namespace {

using Path = std::vector<std::string>;

bool Near(double a, double b) { return std::fabs(a - b) <= 1e-12 * std::max(1.0, std::fabs(b)); }

// Bidirectional six-switch testbed; start-up delays and live delays are the
// configured one-way delays, no loss, no utilization.
ResearchGraph Testbed() {
  ResearchGraph g;
  g.nodes = {"s1", "s2", "s3", "s4", "s5", "s6"};
  for (const auto& l : rf::kTestbedLinks) {
    for (const auto& [from, to] : {std::pair{l.a, l.b}, std::pair{l.b, l.a}}) {
      ResearchLink link;
      link.observation = LinkObservation{from, to, l.delay_ms, 0.0, l.capacity_mbps, 0.0};
      link.startup_delay_ms = l.delay_ms;
      g.links.push_back(link);
    }
  }
  return g;
}

ResearchLink* Find(ResearchGraph& g, const std::string& from, const std::string& to) {
  for (auto& l : g.links) {
    if (l.observation.from == from && l.observation.to == to) return &l;
  }
  return nullptr;
}

void SetBoth(ResearchGraph& g, const std::string& a, const std::string& b, const std::function<void(ResearchLink&)>& f) {
  f(*Find(g, a, b));
  f(*Find(g, b, a));
}

void SetWeights(ResearchGraph& g, double default_weight) {
  for (auto& l : g.links) l.predicted_weight = default_weight;
}

// Declared test statistics for J_eng (not training statistics).
NormalizationStatistics TestStatistics() {
  NormalizationStatistics s;
  s.source = "declared test fixture";
  s.features[NormalizedFeature::Delay] = {100, 10.0, 5.0, 1};
  s.features[NormalizedFeature::Loss] = {100, 0.02, 0.02, 1};
  s.features[NormalizedFeature::InverseBandwidth] = {100, 0.05, 0.05, 1};
  s.features[NormalizedFeature::Hops] = {100, 3.0, 1.0, 1};
  return s;
}

// Every simple path from s to t over available links.
std::vector<Path> AllSimplePaths(const ResearchGraph& g, const std::string& s, const std::string& t) {
  std::vector<Path> out;
  Path path{s};
  std::set<std::string> seen{s};
  std::function<void(const std::string&)> visit = [&](const std::string& u) {
    if (u == t) {
      out.push_back(path);
      return;
    }
    for (const auto& l : g.links) {
      if (!l.available || l.observation.from != u || seen.count(l.observation.to)) continue;
      seen.insert(l.observation.to);
      path.push_back(l.observation.to);
      visit(l.observation.to);
      path.pop_back();
      seen.erase(l.observation.to);
    }
  };
  visit(s);
  return out;
}

double SumCost(const ResearchGraph& g, const Path& p, bool weights) {
  double sum = 0.0;
  for (std::size_t i = 0; i + 1 < p.size(); ++i) {
    for (const auto& l : g.links) {
      if (l.observation.from == p[i] && l.observation.to == p[i + 1]) {
        sum += weights ? *l.predicted_weight : *l.startup_delay_ms;
      }
    }
  }
  return sum;
}

// ---- Dijkstra ----

void DijkstraUsesStartupDelays() {
  ResearchGraph g = Testbed();
  CHECK(ValidateGraph(g).empty());
  Candidate c = StaticDelayDijkstra(g, "s1", "s6");
  CHECK(c.status == CandidateStatus::PathFound);
  CHECK((c.path == Path{"s1", "s2", "s6"}));
  CHECK(c.cost_kind == CostKind::StartupDelayMs);
  CHECK(c.optimization_cost && *c.optimization_cost == rf::kDefaultPathDelayMs);
  // Congestion raises the live delay of s1-s2; the static baseline does not
  // react, because it routes on the start-up snapshot.
  SetBoth(g, "s1", "s2", [](ResearchLink& l) {
    l.observation.delay_ms = 50.0;
    l.observation.utilization = 0.98;
  });
  c = StaticDelayDijkstra(g, "s1", "s6");
  CHECK((c.path == Path{"s1", "s2", "s6"}));
  CHECK(*c.optimization_cost == rf::kDefaultPathDelayMs);
  // Its optimization cost is not the path's quality: J on live values now
  // ranks the detour better.
  const auto stats = TestStatistics();
  const PathEvaluation chosen = EvaluatePath(g, c.path, stats);
  const PathEvaluation detour = EvaluatePath(g, {"s1", "s3", "s6"}, stats);
  CHECK(chosen.quality.value && detour.quality.value && *detour.quality.value < *chosen.quality.value);
}

void DijkstraMatchesBruteForce() {
  const ResearchGraph g = Testbed();
  double best = std::numeric_limits<double>::infinity();
  for (const Path& p : AllSimplePaths(g, "s1", "s6")) best = std::min(best, SumCost(g, p, false));
  const Candidate c = StaticDelayDijkstra(g, "s1", "s6");
  CHECK(Near(*c.optimization_cost, best));
}

void UnavailableLinksAndMissingCosts() {
  ResearchGraph g = Testbed();
  SetBoth(g, "s1", "s2", [](ResearchLink& l) { l.available = false; });
  SetBoth(g, "s1", "s3", [](ResearchLink& l) { l.available = false; });
  Candidate c = StaticDelayDijkstra(g, "s1", "s6");
  CHECK((c.path == Path{"s1", "s4", "s5", "s6"}) && *c.optimization_cost == 21.0);
  SetBoth(g, "s1", "s4", [](ResearchLink& l) { l.available = false; });
  CHECK(StaticDelayDijkstra(g, "s1", "s6").status == CandidateStatus::NoPath);

  ResearchGraph missing = Testbed();
  Find(missing, "s3", "s5")->startup_delay_ms.reset();
  c = StaticDelayDijkstra(missing, "s1", "s6");
  CHECK(c.status == CandidateStatus::MissingCost && aafr::test::Contains(c.message, "s3->s5") && c.path.empty());
  // An unavailable link needs no cost.
  Find(missing, "s3", "s5")->available = false;
  CHECK(StaticDelayDijkstra(missing, "s1", "s6").status == CandidateStatus::PathFound);

  ResearchGraph negative = Testbed();
  Find(negative, "s2", "s3")->startup_delay_ms = -1.0;
  CHECK(StaticDelayDijkstra(negative, "s1", "s6").status == CandidateStatus::InvalidCost);
}

void GraphValidation() {
  ResearchGraph g = Testbed();
  CHECK(StaticDelayDijkstra(g, "s1", "s9").status == CandidateStatus::InvalidInput);
  CHECK(StaticDelayDijkstra(g, "s1", "s1").status == CandidateStatus::InvalidInput);
  ResearchGraph dup = g;
  dup.links.push_back(dup.links.front());
  CHECK(!ValidateGraph(dup).empty());
  ResearchGraph loop = g;
  loop.links.push_back(ResearchLink{LinkObservation{"s2", "s2", 1.0, 0.0, 1.0, 0.0}, 1.0, 1.0, true});
  CHECK(!ValidateGraph(loop).empty());
  ResearchGraph unknown = g;
  unknown.links.push_back(ResearchLink{LinkObservation{"s2", "x", 1.0, 0.0, 1.0, 0.0}, 1.0, 1.0, true});
  CHECK(!ValidateGraph(unknown).empty());
  CHECK(StaticDelayDijkstra(unknown, "s1", "s6").status == CandidateStatus::InvalidInput);
}

// ---- A* on explicit predicted link weights ----

void AStarUsesPredictedWeights() {
  ResearchGraph g = Testbed();
  SetWeights(g, 1.0);
  SetBoth(g, "s1", "s2", [](ResearchLink& l) { l.predicted_weight = 40.0; });  // Bad link per the model.
  Candidate c = PredictedWeightAStar(g, "s1", "s6");
  CHECK(c.status == CandidateStatus::PathFound && c.cost_kind == CostKind::PredictedWeight);
  CHECK((c.path == Path{"s1", "s3", "s6"}) && *c.optimization_cost == 2.0);
  double best = std::numeric_limits<double>::infinity();
  for (const Path& p : AllSimplePaths(g, "s1", "s6")) best = std::min(best, SumCost(g, p, true));
  CHECK(*c.optimization_cost == best);
  // A* never reads the start-up delays and Dijkstra never reads W_link.
  for (auto& l : g.links) l.startup_delay_ms = 1.0;
  CHECK(PredictedWeightAStar(g, "s1", "s6").path == c.path);
}

void AStarRefusesMissingOrInvalidWeights() {
  ResearchGraph g = Testbed();
  SetWeights(g, 1.0);
  Find(g, "s4", "s5")->predicted_weight.reset();  // The model did not run for this link.
  Candidate c = PredictedWeightAStar(g, "s1", "s6");
  CHECK(c.status == CandidateStatus::MissingCost && aafr::test::Contains(c.message, "s4->s5"));
  Find(g, "s4", "s5")->predicted_weight = -0.5;  // Unfloored prediction.
  CHECK(PredictedWeightAStar(g, "s1", "s6").status == CandidateStatus::InvalidCost);
  Find(g, "s4", "s5")->predicted_weight = std::numeric_limits<double>::quiet_NaN();
  CHECK(PredictedWeightAStar(g, "s1", "s6").status == CandidateStatus::InvalidCost);
  // Without any weights A* cannot run; Dijkstra still can.
  ResearchGraph none = Testbed();
  CHECK(PredictedWeightAStar(none, "s1", "s6").status == CandidateStatus::MissingCost);
  CHECK(StaticDelayDijkstra(none, "s1", "s6").status == CandidateStatus::PathFound);
}

void AStarWithAllZeroWeights() {
  // All predicted weights 0 (the known limitation for links under ~10 ms):
  // min W = 0 makes the heuristic 0 and every path costs 0, so the result is
  // decided only by tie-breaking, which the specification leaves open.
  ResearchGraph g = Testbed();
  SetWeights(g, 0.0);
  const Candidate a = PredictedWeightAStar(g, "s1", "s6");
  const Candidate b = PredictedWeightAStar(g, "s1", "s6");
  CHECK(a.status == CandidateStatus::PathFound && *a.optimization_cost == 0.0);
  CHECK(a.path == b.path);  // Deterministic.
  CHECK(a.path.front() == "s1" && a.path.back() == "s6" && PathLinks(g, a.path));
}

// ---- Cross-check against the production implementations ----

struct Random {
  ResearchGraph graph;
  fwd::ForwardingGraph forwarding;
};

// Seeded random digraph with continuous costs (no ties). The same cost is
// the research graph's start-up delay and W_link, and the production
// graph's latency (objective latency-only), so all must agree exactly.
Random MakeRandom(std::uint64_t seed, std::size_t n, double p) {
  alg::Rng rng(seed);
  Random r;
  r.forwarding = fwd::ForwardingGraph("random");
  for (std::size_t i = 0; i < n; ++i) {
    const std::string id = "n" + std::to_string(i);
    r.graph.nodes.push_back(id);
    fwd::ForwardingNode node;
    node.id = id;
    node.role = i == 0 ? fwd::NodeRole::Entry : i + 1 == n ? fwd::NodeRole::Destination : fwd::NodeRole::Relay;
    r.forwarding.AddNode(node);
  }
  for (std::size_t u = 0; u < n; ++u) {
    for (std::size_t v = 0; v < n; ++v) {
      if (u == v || v == 0 || u + 1 == n || rng.Uniform() >= p) continue;
      const double cost = rng.Uniform(0.5, 10.0);
      const std::string from = r.graph.nodes[u], to = r.graph.nodes[v];
      r.graph.links.push_back(ResearchLink{LinkObservation{from, to, cost, 0.0, 100.0, 0.0}, cost, cost, true});
      fwd::ForwardingEdge e;
      e.from = from;
      e.to = to;
      e.latency_ms = fwd::Measurement::Of(cost, fwd::Provenance::Configured);
      e.loss_ratio = fwd::Measurement::Of(0.0, fwd::Provenance::Configured);
      e.bandwidth_mbps = fwd::Measurement::Of(100.0, fwd::Provenance::Configured);
      r.forwarding.AddEdge(e);
    }
  }
  return r;
}

alg::AlgorithmResult Production(alg::AlgorithmId id, const Random& r, const std::string& t) {
  fwd::ForwardingProblem problem;
  problem.source = "n0";
  problem.destination = t;
  problem.objective.latency_weight = 1.0;
  problem.objective.loss_weight = 0.0;
  problem.objective.hop_weight = 0.0;
  problem.objective.policy_weight = 0.0;
  problem.graph_version = r.forwarding.version();
  alg::AlgorithmRequest request;
  request.graph = &r.forwarding;
  request.problem = &problem;
  if (id == alg::AlgorithmId::AStar) request.parameters = {{"heuristic", "hop_lower_bound"}, {"heuristic_weight", "1"}};
  return alg::CreateAlgorithm(id)->Run(request);
}

void MatchesProductionOnRandomGraphs() {
  std::size_t compared = 0;
  for (std::uint64_t seed = 1; seed <= 40; ++seed) {
    const Random r = MakeRandom(seed, 12, 0.25);
    CHECK(ValidateGraph(r.graph).empty());
    const std::string t = "n11";
    const Candidate d = StaticDelayDijkstra(r.graph, "n0", t);
    const Candidate a = PredictedWeightAStar(r.graph, "n0", t);
    const auto pd = Production(alg::AlgorithmId::Dijkstra, r, t);
    const auto pa = Production(alg::AlgorithmId::AStar, r, t);
    if (d.status == CandidateStatus::NoPath) {
      CHECK(pd.status == alg::AlgorithmStatus::NoPath && a.status == CandidateStatus::NoPath);
      continue;
    }
    ++compared;
    CHECK(d.status == CandidateStatus::PathFound && pd.status == alg::AlgorithmStatus::PathFound);
    CHECK(d.path == pd.path && a.path == pa.path);
    CHECK(Near(*d.optimization_cost, *pd.objective) && Near(*a.optimization_cost, *pa.objective));
    // Admissible heuristic: A* finds the same optimum as Dijkstra on W_link,
    // expanding no more nodes.
    CHECK(Near(*a.optimization_cost, *d.optimization_cost) && a.path == d.path);
    CHECK(a.expansions <= d.expansions);
    double best = std::numeric_limits<double>::infinity();
    for (const Path& p : AllSimplePaths(r.graph, "n0", t)) best = std::min(best, SumCost(r.graph, p, true));
    CHECK(Near(*a.optimization_cost, best));
  }
  CHECK(compared >= 20);
}

// ---- ACO and GA ----

void MetaheuristicsAreDeclaredNotGuessed() {
  CHECK(AntColonyParameters{}.ants == rf::kAcoAnts && AntColonyParameters{}.iterations == rf::kAcoIterations);
  CHECK(GeneticParameters{}.population == rf::kGaPopulation && GeneticParameters{}.generations == rf::kGaGenerations);
  const ResearchGraph g = Testbed();
  const auto stats = TestStatistics();
  const Candidate aco = AntColonyOptimization(g, "s1", "s6", stats);
  const Candidate ga = GeneticAlgorithm(g, "s1", "s6", stats);
  CHECK(aco.status == CandidateStatus::NotImplemented && aco.cost_kind == CostKind::EngineCost && aco.path.empty());
  CHECK(ga.status == CandidateStatus::NotImplemented && ga.cost_kind == CostKind::EngineCost && ga.path.empty());
  CHECK(aafr::test::Contains(aco.message, "not defined") && aafr::test::Contains(ga.message, "not defined"));
}

void EngineCostOptimumIsAReferenceForLaterPhases() {
  // Brute-force J_eng optimum on the congested testbed under the declared
  // test statistics: the reference a future ACO / GA must reach. It differs
  // from the static Dijkstra path, so the objectives really differ.
  ResearchGraph g = Testbed();
  SetBoth(g, "s1", "s2", [](ResearchLink& l) { l.observation.utilization = 0.95; });
  const auto stats = TestStatistics();
  Path best;
  double best_cost = std::numeric_limits<double>::infinity();
  std::size_t evaluated = 0;
  for (const Path& p : AllSimplePaths(g, "s1", "s6")) {
    const PathEvaluation e = EvaluatePath(g, p, stats);
    CHECK(e.on_graph && e.engine_cost.value);
    ++evaluated;
    if (*e.engine_cost.value < best_cost) {
      best_cost = *e.engine_cost.value;
      best = p;
    }
  }
  CHECK(evaluated > 5);
  CHECK(best != StaticDelayDijkstra(g, "s1", "s6").path);
  CHECK(best.front() == "s1" && best.back() == "s6");
  // J_eng and J are separate measures of the same path.
  const PathEvaluation e = EvaluatePath(g, best, stats);
  CHECK(e.quality.value && *e.quality.value != *e.engine_cost.value);
  // Without statistics J_eng is unavailable; J (raw) is still defined.
  const PathEvaluation raw = EvaluatePath(g, best, NormalizationStatistics{});
  CHECK(!raw.engine_cost.value && raw.quality.value);
}

// ---- Candidates and arms ----

void SamePathFromDifferentAlgorithmsIsOneArm() {
  ResearchGraph g = Testbed();
  // Unit weights, s1-s3 heavier: A*'s unique optimum is s1-s2-s6 (W = 2),
  // the same path static Dijkstra takes (4 ms).
  SetWeights(g, 1.0);
  SetBoth(g, "s1", "s3", [](ResearchLink& l) { l.predicted_weight = 5.0; });
  const auto candidates = RunPool(g, "s1", "s6", TestStatistics());
  CHECK(candidates.size() == 4);
  CHECK(candidates[0].algorithm == PoolAlgorithm::Dijkstra && candidates[1].algorithm == PoolAlgorithm::AStar);
  CHECK(candidates[0].path == candidates[1].path);
  CHECK(candidates[0].optimization_cost != candidates[1].optimization_cost);  // Same path, different costs.

  CandidateArms arms = DistinctChallengers(candidates, {"s1", "s3", "s6"});
  CHECK(arms.challengers.size() == 1 && arms.installed_algorithms.empty());
  CHECK((arms.challengers[0].path == Path{"s1", "s2", "s6"}));
  CHECK((arms.challengers[0].algorithms == std::vector<PoolAlgorithm>{PoolAlgorithm::Dijkstra, PoolAlgorithm::AStar}));
  // The installed path is never an arm.
  arms = DistinctChallengers(candidates, {"s1", "s2", "s6"});
  CHECK(arms.challengers.empty());
  CHECK((arms.installed_algorithms == std::vector<PoolAlgorithm>{PoolAlgorithm::Dijkstra, PoolAlgorithm::AStar}));
  // Distinct paths become distinct arms: A* now takes s1-s4-s5-s6 (W = 3),
  // Dijkstra still s1-s2-s6.
  SetBoth(g, "s1", "s2", [](ResearchLink& l) { l.predicted_weight = 40.0; });
  const auto split = RunPool(g, "s1", "s6", TestStatistics());
  CHECK((split[1].path == Path{"s1", "s4", "s5", "s6"}));
  arms = DistinctChallengers(split, {"s1", "s3", "s6"});
  CHECK(arms.challengers.size() == 2);
  CHECK(arms.challengers[0].algorithms.size() == 1 && arms.challengers[1].algorithms.size() == 1);
}

}  // namespace

int main() {
  return aafr::test::RunTests({
      {"dijkstra_uses_startup_delays", DijkstraUsesStartupDelays},
      {"dijkstra_matches_brute_force", DijkstraMatchesBruteForce},
      {"unavailable_links_and_missing_costs", UnavailableLinksAndMissingCosts},
      {"graph_validation", GraphValidation},
      {"astar_uses_predicted_weights", AStarUsesPredictedWeights},
      {"astar_refuses_missing_or_invalid_weights", AStarRefusesMissingOrInvalidWeights},
      {"astar_with_all_zero_weights", AStarWithAllZeroWeights},
      {"matches_production_on_random_graphs", MatchesProductionOnRandomGraphs},
      {"metaheuristics_are_declared_not_guessed", MetaheuristicsAreDeclaredNotGuessed},
      {"engine_cost_optimum_is_a_reference_for_later_phases", EngineCostOptimumIsAReferenceForLaterPhases},
      {"same_path_from_different_algorithms_is_one_arm", SamePathFromDifferentAlgorithmsIsOneArm},
  });
}
