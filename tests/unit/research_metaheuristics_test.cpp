// Research alignment, Phase 3: ACO and GA on the engine cost J_eng
// (research/path_algorithms). Statistics are declared test fixtures, not
// training statistics. Stochastic results are judged by success rates over
// fixed seeds against brute-force optima, never by requiring the optimum in
// every run; the rates are printed as RESULT lines. Deterministic (seeded);
// no network access.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <functional>
#include <iostream>
#include <limits>
#include <set>
#include <string>

#include "algorithms/common/path_algorithm.h"
#include "research/path_algorithms.h"
#include "tests/fixtures/research/reference_values.h"
#include "tests/support/test_support.h"

using namespace aafr::research;
namespace rf = aafr::research_fixture;
namespace alg = aafr::algorithms;

namespace {

using Path = std::vector<std::string>;
constexpr double kEps = 1e-9;

ResearchGraph Testbed() {
  ResearchGraph g;
  g.nodes = {"s1", "s2", "s3", "s4", "s5", "s6"};
  for (const auto& l : rf::kTestbedLinks) {
    for (const auto& [from, to] : {std::pair{l.a, l.b}, std::pair{l.b, l.a}}) {
      ResearchLink link;
      link.observation = LinkObservation{from, to, l.delay_ms, 0.0, l.capacity_mbps, 0.0};
      link.startup_delay_ms = l.delay_ms;
      link.predicted_weight = 0.0;
      g.links.push_back(link);
    }
  }
  return g;
}

ResearchGraph Congested() {
  ResearchGraph g = Testbed();
  for (auto& l : g.links) {
    const bool s1s2 = (l.observation.from == "s1" && l.observation.to == "s2") ||
                      (l.observation.from == "s2" && l.observation.to == "s1");
    if (s1s2) l.observation.utilization = 0.95;
  }
  return g;
}

ResearchLink* Find(ResearchGraph& g, const std::string& from, const std::string& to) {
  for (auto& l : g.links) {
    if (l.observation.from == from && l.observation.to == to) return &l;
  }
  return nullptr;
}

// Declared test statistics. delay_sd is a power of two so a shift of the
// delay mean by a multiple of it shifts every z(D) by an exact constant.
NormalizationStatistics Statistics(double delay_mean = 10.0) {
  NormalizationStatistics s;
  s.source = "declared test fixture";
  s.features[NormalizedFeature::Delay] = {100, delay_mean, 4.0, 1};
  s.features[NormalizedFeature::Loss] = {100, 0.02, 0.02, 1};
  s.features[NormalizedFeature::InverseBandwidth] = {100, 0.05, 0.05, 1};
  s.features[NormalizedFeature::Hops] = {100, 3.0, 1.0, 1};
  return s;
}

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

struct Optimum {
  std::optional<double> cost;
  Path path;
  std::size_t paths{0};
  double min_cost{std::numeric_limits<double>::infinity()};
  double max_cost{-std::numeric_limits<double>::infinity()};
};

Optimum BruteForce(const ResearchGraph& g, const std::string& s, const std::string& t, const NormalizationStatistics& st) {
  Optimum o;
  for (const Path& p : AllSimplePaths(g, s, t)) {
    ++o.paths;
    const auto e = EvaluatePath(g, p, st);
    if (!e.engine_cost.value) continue;
    const double c = *e.engine_cost.value;
    o.min_cost = std::min(o.min_cost, c);
    o.max_cost = std::max(o.max_cost, c);
    if (!o.cost || c < *o.cost) {
      o.cost = c;
      o.path = p;
    }
  }
  return o;
}

// Seeded random digraph with random link measurements (all J_eng defined).
ResearchGraph RandomGraph(std::uint64_t seed, std::size_t n, double p) {
  alg::Rng rng(seed);
  ResearchGraph g;
  for (std::size_t i = 0; i < n; ++i) g.nodes.push_back("n" + std::to_string(i));
  const double capacities[] = {10.0, 50.0, 100.0};
  for (std::size_t u = 0; u < n; ++u) {
    for (std::size_t v = 0; v < n; ++v) {
      if (u == v || v == 0 || u + 1 == n || rng.Uniform() >= p) continue;
      ResearchLink l;
      l.observation = LinkObservation{g.nodes[u], g.nodes[v], rng.Uniform(1.0, 20.0), rng.Uniform(0.0, 0.05),
                                      capacities[rng.Index(3)], rng.Uniform(0.0, 0.9)};
      g.links.push_back(l);
    }
  }
  return g;
}

bool ValidOnGraph(const ResearchGraph& g, const Path& p, const std::string& s, const std::string& t) {
  if (p.size() < 2 || p.front() != s || p.back() != t) return false;
  std::set<std::string> seen(p.begin(), p.end());
  return seen.size() == p.size() && PathLinks(g, p).has_value();
}

using Runner = std::function<Candidate(const ResearchGraph&, const NormalizationStatistics&, std::uint64_t)>;
const Runner kAco = [](const ResearchGraph& g, const NormalizationStatistics& s, std::uint64_t seed) {
  AntColonyParameters p;
  p.seed = seed;
  return AntColonyOptimization(g, "s1", "s6", s, p);
};
const Runner kGa = [](const ResearchGraph& g, const NormalizationStatistics& s, std::uint64_t seed) {
  GeneticParameters p;
  p.seed = seed;
  return GeneticAlgorithm(g, "s1", "s6", s, p);
};

// ---- Inputs ----

void InvalidInputsAreRefused() {
  const auto stats = Statistics();
  for (const auto& [name, run] : {std::pair{"aco", kAco}, std::pair{"ga", kGa}}) {
    aafr::test::current_test = std::string("invalid_inputs_") + name;
    ResearchGraph dup = Testbed();
    dup.links.push_back(dup.links.front());
    CHECK(run(dup, stats, 1).status == CandidateStatus::InvalidInput);
    NormalizationStatistics partial = stats;
    partial.features.erase(NormalizedFeature::Hops);
    Candidate c = run(Testbed(), partial, 1);
    CHECK(c.status == CandidateStatus::InvalidInput && aafr::test::Contains(c.message, "hops"));
    NormalizationStatistics no_source = stats;
    no_source.source.clear();
    CHECK(run(Testbed(), no_source, 1).status == CandidateStatus::InvalidInput);
    CHECK(run(Testbed(), NormalizationStatistics{}, 1).status == CandidateStatus::InvalidInput);

    ResearchGraph missing = Testbed();
    Find(missing, "s3", "s5")->observation.utilization.reset();
    c = run(missing, stats, 1);
    CHECK(c.status == CandidateStatus::MissingCost && aafr::test::Contains(c.message, "s3->s5") && c.path.empty());
    Find(missing, "s3", "s5")->available = false;  // An unavailable link needs no measurement.
    CHECK(run(missing, stats, 1).status == CandidateStatus::PathFound);
    ResearchGraph invalid = Testbed();
    Find(invalid, "s2", "s3")->observation.loss_ratio = 1.5;
    CHECK(run(invalid, stats, 1).status == CandidateStatus::InvalidCost);

    ResearchGraph cut = Testbed();
    for (auto& l : cut.links) {
      if (l.observation.from == "s1") l.available = false;
    }
    CHECK(run(cut, stats, 1).status == CandidateStatus::NoPath);
  }
  aafr::test::current_test = "invalid_inputs_are_refused";
  // Unknown endpoints and equal endpoints.
  CHECK(AntColonyOptimization(Testbed(), "s1", "x", stats).status == CandidateStatus::InvalidInput);
  CHECK(GeneticAlgorithm(Testbed(), "s1", "s1", stats).status == CandidateStatus::InvalidInput);
  AntColonyParameters bad_aco;
  bad_aco.rho = 0.0;
  CHECK(AntColonyOptimization(Testbed(), "s1", "s6", stats, bad_aco).status == CandidateStatus::InvalidInput);
  GeneticParameters bad_ga;
  bad_ga.population = 1;
  CHECK(GeneticAlgorithm(Testbed(), "s1", "s6", stats, bad_ga).status == CandidateStatus::InvalidInput);
}

void UndefinedEngineCostPathsAreRejected() {
  // s1-s2 fully utilized: B = 0 on every path through it, so J_eng is
  // undefined there; those paths are rejected, never scored as 0 or infinite.
  ResearchGraph g = Testbed();
  Find(g, "s1", "s2")->observation.utilization = 1.0;
  const auto stats = Statistics();
  for (const auto& run : {kAco, kGa}) {
    for (std::uint64_t seed = 1; seed <= 5; ++seed) {
      const Candidate c = run(g, stats, seed);
      CHECK(c.status == CandidateStatus::PathFound);
      CHECK(c.path.size() >= 2 && c.path[1] != "s2");
    }
  }
  // Every path undefined: no scored path at all.
  ResearchGraph dead = Testbed();
  for (auto& l : dead.links) {
    if (l.observation.from == "s1") l.observation.utilization = 1.0;
  }
  for (const auto& run : {kAco, kGa}) {
    const Candidate c = run(dead, stats, 1);
    CHECK(c.status == CandidateStatus::NoScoredPath && c.path.empty() && !c.optimization_cost);
    CHECK(c.rejected_paths > 0);
  }
}

// ---- Objective ----

void ResultCostIsTheJengOfTheReturnedPath() {
  const ResearchGraph g = Congested();
  const auto stats = Statistics();
  const Optimum opt = BruteForce(g, "s1", "s6", stats);
  for (const auto& run : {kAco, kGa}) {
    for (std::uint64_t seed = 1; seed <= 10; ++seed) {
      const Candidate c = run(g, stats, seed);
      CHECK(c.status == CandidateStatus::PathFound && c.cost_kind == CostKind::EngineCost);
      CHECK(ValidOnGraph(g, c.path, "s1", "s6"));
      const PathEvaluation e = EvaluatePath(g, c.path, stats);
      CHECK(e.engine_cost.value && *c.optimization_cost == *e.engine_cost.value);
      CHECK(*c.optimization_cost >= *opt.cost - kEps);  // Never better than the true optimum.
      CHECK(e.quality.value && *e.quality.value != *c.optimization_cost);  // J is not J_eng.
    }
  }
}

void NegativeObjectiveKeepsTheRanking() {
  // Shift every J_eng by exactly -256 (delay mean + 256 * sd): all values
  // become negative, the ranking is unchanged, so with the same seed ACO and
  // GA must make the same choices and return the same path.
  const ResearchGraph g = Congested();
  const auto base = Statistics(10.0);
  const auto shifted = Statistics(10.0 + 256.0 * 4.0);
  const Optimum o_base = BruteForce(g, "s1", "s6", base);
  const Optimum o_shift = BruteForce(g, "s1", "s6", shifted);
  CHECK(o_base.min_cost < 0.0 && o_base.max_cost > 0.0);  // Mixed signs.
  CHECK(o_shift.max_cost < 0.0);                           // All negative.
  CHECK(o_base.path == o_shift.path);
  CHECK(std::fabs((*o_base.cost - *o_shift.cost) - 256.0) < 1e-9);
  for (const auto& run : {kAco, kGa}) {
    for (std::uint64_t seed = 1; seed <= 10; ++seed) {
      const Candidate a = run(g, base, seed);
      const Candidate b = run(g, shifted, seed);
      CHECK(a.path == b.path);
      CHECK(a.evaluations == b.evaluations && a.rejected_paths == b.rejected_paths);
      CHECK(std::fabs((*a.optimization_cost - *b.optimization_cost) - 256.0) < 1e-9);
    }
  }
}

// ---- Reproducibility, validity and budgets ----

void FixedSeedsReproduce() {
  const ResearchGraph g = RandomGraph(7, 10, 0.35);
  const auto stats = Statistics();
  for (const bool aco : {true, false}) {
    const auto run = [&](std::uint64_t seed) {
      if (aco) {
        AntColonyParameters p;
        p.seed = seed;
        return AntColonyOptimization(g, "n0", "n9", stats, p);
      }
      GeneticParameters p;
      p.seed = seed;
      return GeneticAlgorithm(g, "n0", "n9", stats, p);
    };
    for (std::uint64_t seed = 1; seed <= 5; ++seed) {
      const Candidate a = run(seed), b = run(seed);
      CHECK(a.status == CandidateStatus::PathFound && a.seed == seed);
      CHECK(a.path == b.path && a.optimization_cost == b.optimization_cost);
      CHECK(a.evaluations == b.evaluations && a.rejected_paths == b.rejected_paths);
    }
  }
}

void BudgetsAndPathValidity() {
  // On graphs where every path has a defined J_eng, GA's only rejections
  // would be invalid crossover / mutation children: there must be none.
  // ACO's rejections are ants that hit a dead end.
  const auto stats = Statistics();
  double aco_ms = 0.0, ga_ms = 0.0;
  std::size_t runs = 0, ga_invalid = 0, aco_dead_ends = 0;
  for (std::uint64_t g_seed = 1; g_seed <= 10; ++g_seed) {
    const ResearchGraph g = RandomGraph(g_seed, 10, 0.35);
    for (std::uint64_t seed = 1; seed <= 3; ++seed) {
      AntColonyParameters ap;
      ap.seed = seed;
      GeneticParameters gp;
      gp.seed = seed;
      const Candidate a = AntColonyOptimization(g, "n0", "n9", stats, ap);
      const Candidate b = GeneticAlgorithm(g, "n0", "n9", stats, gp);
      if (a.status == CandidateStatus::NoPath) {
        CHECK(b.status == CandidateStatus::NoPath);
        continue;
      }
      ++runs;
      CHECK(a.iterations == rf::kAcoIterations && b.iterations == rf::kGaGenerations);
      CHECK(a.evaluations + a.rejected_paths == rf::kAcoAnts * rf::kAcoIterations);
      CHECK(b.evaluations == rf::kGaPopulation + rf::kGaGenerations * (rf::kGaPopulation - 1));
      CHECK(ValidOnGraph(g, a.path, "n0", "n9") && ValidOnGraph(g, b.path, "n0", "n9"));
      ga_invalid += b.rejected_paths;
      aco_dead_ends += a.rejected_paths;
      aco_ms += a.duration_ms;
      ga_ms += b.duration_ms;
      CHECK(a.duration_ms < 5000.0 && b.duration_ms < 5000.0);
    }
  }
  CHECK(runs >= 15);
  CHECK(ga_invalid == 0);
  std::cout << "RESULT budgets: runs=" << runs << " aco_mean_ms=" << aco_ms / runs << " ga_mean_ms=" << ga_ms / runs
            << " aco_dead_end_ants=" << aco_dead_ends << " ga_invalid_children=" << ga_invalid << "\n";
}

// ---- Candidates ----

void IdenticalPathsFormOneArm() {
  const ResearchGraph g = Congested();
  const auto stats = Statistics();
  const auto candidates = RunPool(g, "s1", "s6", stats);
  CHECK(candidates.size() == 4);
  CHECK(candidates[2].status == CandidateStatus::PathFound && candidates[3].status == CandidateStatus::PathFound);
  // Installed: Dijkstra's path. Every other distinct path is exactly one arm,
  // labelled with every algorithm that returned it.
  const Path installed = candidates[0].path;
  const auto check_arms = [&](const CandidateArms& arms, const Path& inst) {
    std::set<Path> expected;
    for (const Candidate& c : candidates) {
      if (c.path != inst) expected.insert(c.path);
    }
    CHECK(arms.challengers.size() == expected.size());
    for (const Arm& arm : arms.challengers) {
      CHECK(arm.path != inst && expected.count(arm.path) == 1);
      for (const Candidate& c : candidates) {
        const bool listed = std::count(arm.algorithms.begin(), arm.algorithms.end(), c.algorithm) == 1;
        CHECK(listed == (c.path == arm.path));
      }
    }
    for (const Candidate& c : candidates) {
      const bool listed = std::count(arms.installed_algorithms.begin(), arms.installed_algorithms.end(), c.algorithm) == 1;
      CHECK(listed == (c.path == inst));
    }
  };
  check_arms(DistinctChallengers(candidates, installed), installed);
  check_arms(DistinctChallengers(candidates, candidates[2].path), candidates[2].path);  // Installed: ACO's.
  // ACO and GA returning one path give one arm with both labels.
  if (candidates[2].path == candidates[3].path && candidates[2].path != installed) {
    const auto arms = DistinctChallengers(candidates, installed);
    std::size_t shared = 0;
    for (const Arm& arm : arms.challengers) {
      shared += std::count(arm.algorithms.begin(), arm.algorithms.end(), PoolAlgorithm::AntColony) == 1 &&
                std::count(arm.algorithms.begin(), arm.algorithms.end(), PoolAlgorithm::Genetic) == 1;
    }
    CHECK(shared == 1);
    std::cout << "RESULT shared_arm: aco and ga returned " << aafr::forwarding::PathText(candidates[2].path) << "\n";
  }
}

// ---- Comparative results ----

void SuccessRatesAgainstBruteForce() {
  const auto stats = Statistics();
  struct Tally {
    std::size_t runs{0}, hits{0};
    double gap{0.0};
  };
  const auto study = [&](const char* label, const std::vector<std::pair<ResearchGraph, std::pair<std::string, std::string>>>& graphs) {
    Tally aco, ga;
    std::size_t instances = 0, paths = 0;
    for (const auto& [g, ends] : graphs) {
      const Optimum opt = BruteForce(g, ends.first, ends.second, stats);
      if (!opt.cost) continue;
      ++instances;
      paths += opt.paths;
      for (std::uint64_t seed = 1; seed <= 20; ++seed) {
        AntColonyParameters ap;
        ap.seed = seed;
        GeneticParameters gp;
        gp.seed = seed;
        for (auto [tally, c] : {std::pair{&aco, AntColonyOptimization(g, ends.first, ends.second, stats, ap)},
                                std::pair{&ga, GeneticAlgorithm(g, ends.first, ends.second, stats, gp)}}) {
          CHECK(c.status == CandidateStatus::PathFound);
          CHECK(*c.optimization_cost >= *opt.cost - kEps);
          ++tally->runs;
          const double gap = *c.optimization_cost - *opt.cost;
          tally->hits += gap <= kEps;
          tally->gap += gap;
        }
      }
    }
    std::cout << "RESULT " << label << ": instances=" << instances << " mean_simple_paths=" << paths / std::max<std::size_t>(instances, 1)
              << " aco_optimum_rate=" << static_cast<double>(aco.hits) / aco.runs
              << " aco_mean_gap=" << aco.gap / aco.runs << " ga_optimum_rate=" << static_cast<double>(ga.hits) / ga.runs
              << " ga_mean_gap=" << ga.gap / ga.runs << " runs_each=" << aco.runs << "\n";
    return std::pair{aco, ga};
  };
  const auto testbed = study("testbed_congested", {{Congested(), {"s1", "s6"}}});
  CHECK(testbed.first.hits > 0 && testbed.second.hits > 0);
  std::vector<std::pair<ResearchGraph, std::pair<std::string, std::string>>> random;
  for (std::uint64_t seed = 101; seed <= 130; ++seed) random.push_back({RandomGraph(seed, 10, 0.35), {"n0", "n9"}});
  const auto rnd = study("random_10_node", random);
  CHECK(rnd.first.runs > 0 && rnd.second.runs > 0);
}

void ComparativeTestbedResults() {
  // One row per algorithm on the congested testbed: its own optimization
  // cost, and the independent J and J_eng of the path it returned. A*
  // uses explicit predicted weights (no model): 10 on s1-s2, 1 elsewhere.
  ResearchGraph g = Congested();
  for (auto& l : g.links) l.predicted_weight = 1.0;
  Find(g, "s1", "s2")->predicted_weight = 10.0;
  Find(g, "s2", "s1")->predicted_weight = 10.0;
  const auto stats = Statistics();
  for (const Candidate& c : RunPool(g, "s1", "s6", stats)) {
    CHECK(c.status == CandidateStatus::PathFound);
    const PathEvaluation e = EvaluatePath(g, c.path, stats);
    CHECK(e.quality.value && e.engine_cost.value);
    std::cout << "RESULT testbed " << ToString(c.algorithm) << ": path=" << aafr::forwarding::PathText(c.path)
              << " cost[" << ToString(c.cost_kind) << "]=" << *c.optimization_cost << " J=" << *e.quality.value
              << " J_eng=" << *e.engine_cost.value << " ms=" << c.duration_ms << "\n";
  }
}

}  // namespace

int main() {
  return aafr::test::RunTests({
      {"invalid_inputs_are_refused", InvalidInputsAreRefused},
      {"undefined_engine_cost_paths_are_rejected", UndefinedEngineCostPathsAreRejected},
      {"result_cost_is_the_jeng_of_the_returned_path", ResultCostIsTheJengOfTheReturnedPath},
      {"negative_objective_keeps_the_ranking", NegativeObjectiveKeepsTheRanking},
      {"fixed_seeds_reproduce", FixedSeedsReproduce},
      {"budgets_and_path_validity", BudgetsAndPathValidity},
      {"identical_paths_form_one_arm", IdenticalPathsFormOneArm},
      {"success_rates_against_brute_force", SuccessRatesAgainstBruteForce},
      {"comparative_testbed_results", ComparativeTestbedResults},
  });
}
