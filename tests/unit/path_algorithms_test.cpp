// V0.5: the ten path algorithms. Exact algorithms against known answers and
// brute force; metaheuristics for validity, reproducibility, budgets,
// constraints and infeasibility. Deterministic; no network access.

#include <cmath>
#include <functional>

#include "algorithms/common/path_algorithm.h"
#include "algorithms/execution/implementation_verifier.h"
#include "routing/topology/scenario_catalog.h"
#include "tests/support/test_support.h"

using namespace aafr::algorithms;
namespace fwd = aafr::forwarding;

namespace {

const AlgorithmId kExact[] = {AlgorithmId::Dijkstra, AlgorithmId::AStar, AlgorithmId::BellmanFord};
const AlgorithmId kMeta[] = {AlgorithmId::AntColonyOptimization, AlgorithmId::ParticleSwarmOptimization,
                             AlgorithmId::GeneticAlgorithm,      AlgorithmId::ArtificialBeeColony,
                             AlgorithmId::GreyWolfOptimizer,     AlgorithmId::FireflyAlgorithm,
                             AlgorithmId::WhaleOptimizationAlgorithm};

struct Case {
  fwd::ForwardingGraph graph;
  fwd::ForwardingProblem problem;
};

Case Demo() {
  Case c{fwd::LoopbackDemoGraph(), {}};
  c.problem = fwd::LoopbackDemoProblem(c.graph);
  return c;
}

Case Generated(std::size_t relays, double p, std::uint64_t seed) {
  fwd::GeneratorConfig config;
  config.relays = relays;
  config.edge_probability = p;
  Case c{fwd::GenerateGraph("t", config, seed), {}};
  c.problem.source = "entry";
  c.problem.destination = "dest";
  c.problem.graph_version = c.graph.version();
  return c;
}

AlgorithmResult Run(AlgorithmId id, const Case& c, std::uint64_t seed = 1, std::uint64_t iterations = 60,
                    AlgorithmParameters parameters = {}, const std::atomic<bool>* cancel = nullptr) {
  AlgorithmRequest request;
  request.graph = &c.graph;
  request.problem = &c.problem;
  request.seed = seed;
  request.budget.max_iterations = iterations;
  request.parameters = std::move(parameters);
  request.cancel = cancel;
  return CreateAlgorithm(id)->Run(request);
}

std::optional<double> BruteForce(const Case& c) {
  const auto view = fwd::BuildView(c.graph, c.problem);
  std::optional<double> best;
  std::vector<char> seen(view.node_ids.size(), 0);
  std::vector<int> path{view.source};
  seen[static_cast<std::size_t>(view.source)] = 1;
  std::function<void(int, double)> visit = [&](int node, double cost) {
    if (node == view.destination) {
      if (fwd::ValidatePath(c.graph, c.problem, ToNames(view, path)).feasible && (!best || cost < *best)) best = cost;
      return;
    }
    for (const auto& edge : view.out[static_cast<std::size_t>(node)]) {
      if (seen[static_cast<std::size_t>(edge.to)]) continue;
      seen[static_cast<std::size_t>(edge.to)] = 1;
      path.push_back(edge.to);
      visit(edge.to, cost + edge.cost);
      path.pop_back();
      seen[static_cast<std::size_t>(edge.to)] = 0;
    }
  };
  visit(view.source, 0.0);
  return best;
}

void TestKnownShortestPaths() {
  const Case c = Demo();
  for (const AlgorithmId id : kExact) {
    const AlgorithmResult r = Run(id, c);
    CHECK(r.status == AlgorithmStatus::PathFound);
    CHECK((r.path == fwd::Path{"entry", "relay_a", "dest"}));
    CHECK(r.objective && std::fabs(*r.objective - 5.0) < 1e-12);
    CHECK(r.optimality_claimed);
    CHECK(r.termination == Termination::Completed);
    CHECK(r.algorithm == id && !r.algorithm_version.empty());
  }
  // Raising relay_a's cost moves every exact algorithm to relay_b.
  Case shifted = Demo();
  shifted.graph.SetEdgeLatency("entry", "relay_a", fwd::Measurement::Of(10.0, fwd::Provenance::Configured));
  shifted.problem.graph_version = shifted.graph.version();
  for (const AlgorithmId id : kExact) CHECK((Run(id, shifted).path == fwd::Path{"entry", "relay_b", "dest"}));
}

void TestExactAgainstBruteForce() {
  for (std::uint64_t seed = 1; seed <= 12; ++seed) {
    const Case c = Generated(9, 0.3, seed);
    const auto optimum = BruteForce(c);
    for (const AlgorithmId id : kExact) {
      const AlgorithmResult r = Run(id, c);
      if (!optimum) {
        CHECK(r.status == AlgorithmStatus::NoPath);
        continue;
      }
      CHECK(r.status == AlgorithmStatus::PathFound);
      CHECK(r.objective && std::fabs(*r.objective - *optimum) < 1e-9);
      // Reconstruction: the reported path really scores the reported value.
      const auto validation = fwd::ValidatePath(c.graph, c.problem, r.path);
      CHECK(validation.valid && std::fabs(*validation.objective - *r.objective) < 1e-12);
    }
  }
}

void TestAStarHeuristics() {
  const Case c = Generated(30, 0.12, 4);
  const AlgorithmResult dijkstra = Run(AlgorithmId::Dijkstra, c);
  const AlgorithmResult zero = Run(AlgorithmId::AStar, c, 1, 60, {{"heuristic", "zero"}});
  const AlgorithmResult hops = Run(AlgorithmId::AStar, c, 1, 60, {{"heuristic", "hop_lower_bound"}});
  CHECK(*zero.objective == *dijkstra.objective && *hops.objective == *dijkstra.objective);
  CHECK(zero.optimality_claimed && hops.optimality_claimed);
  CHECK(hops.iterations <= zero.iterations);  // An informed heuristic never expands more.
  const AlgorithmResult weighted = Run(AlgorithmId::AStar, c, 1, 60, {{"heuristic_weight", "3"}});
  CHECK(weighted.status == AlgorithmStatus::PathFound);
  CHECK(!weighted.optimality_claimed);
  CHECK(aafr::test::Contains(weighted.optimality_note, "inadmissible"));
  CHECK(Run(AlgorithmId::AStar, c, 1, 60, {{"heuristic", "magic"}}).status == AlgorithmStatus::InvalidInput);
}

void TestInvalidInputs() {
  const Case c = Demo();
  for (const AlgorithmId id : kAllAlgorithmIds) {
    CHECK(Run(id, c, 1, 60, {{"no_such_parameter", "1"}}).status == AlgorithmStatus::InvalidInput);
    Case wrong = Demo();
    wrong.problem.source = "relay_a";
    CHECK(Run(id, wrong).status == AlgorithmStatus::InvalidInput);
    AlgorithmRequest empty;
    CHECK(CreateAlgorithm(id)->Run(empty).status == AlgorithmStatus::InvalidInput);
    CHECK(Run(id, c, 1, 0).status == AlgorithmStatus::InvalidInput);  // Zero iteration budget.
  }
  CHECK(Run(AlgorithmId::ParticleSwarmOptimization, c, 1, 60, {{"swarm_size", "abc"}}).status ==
        AlgorithmStatus::InvalidInput);
  CHECK(Run(AlgorithmId::GeneticAlgorithm, c, 1, 60, {{"selection_method", "lottery"}}).status ==
        AlgorithmStatus::InvalidInput);
}

Case NegativeEdges(bool cycle) {
  Case c = Demo();
  // A preference credit on relay_b makes the longer path cheaper.
  fwd::ForwardingGraph graph("neg");
  for (const auto& node : c.graph.nodes()) graph.AddNode(node);
  for (auto edge : c.graph.edges()) {
    if (edge.from == "relay_a" && edge.to == "relay_b") edge.policy_cost = -3.0;
    graph.AddEdge(edge);
  }
  if (cycle) {
    fwd::ForwardingEdge back;
    back.from = "relay_b";
    back.to = "relay_a";
    back.latency_ms = fwd::Measurement::Of(1.0, fwd::Provenance::Configured);
    back.loss_ratio = fwd::Measurement::Of(0.0, fwd::Provenance::Configured);
    back.policy_cost = -2.0;  // Cycle cost: -1.5 + (1 + 0.5 - 2) = -2.
    graph.AddEdge(back);
  }
  c.graph = graph;
  c.problem.graph_version = graph.version();
  return c;
}

void TestNegativeEdges() {
  const Case c = NegativeEdges(false);
  CHECK(Run(AlgorithmId::Dijkstra, c).status == AlgorithmStatus::InvalidInput);
  CHECK(Run(AlgorithmId::AStar, c).status == AlgorithmStatus::InvalidInput);
  const AlgorithmResult bf = Run(AlgorithmId::BellmanFord, c);
  CHECK(bf.status == AlgorithmStatus::PathFound);
  CHECK((bf.path == fwd::Path{"entry", "relay_a", "relay_b", "dest"}));
  CHECK(std::fabs(*bf.objective - (5.5 - 3.0)) < 1e-12);
  CHECK(*bf.objective == *BruteForce(c));

  const Case cyclic = NegativeEdges(true);
  const AlgorithmResult detected = Run(AlgorithmId::BellmanFord, cyclic);
  CHECK(detected.status == AlgorithmStatus::NegativeCycle);
  CHECK(aafr::test::Contains(detected.message, "relay_a") && aafr::test::Contains(detected.message, "relay_b"));
  CHECK(detected.path.empty());
  // Metaheuristics search simple paths only and stay well-defined.
  for (const AlgorithmId id : kMeta) {
    const AlgorithmResult r = Run(id, cyclic);
    CHECK(r.status == AlgorithmStatus::PathFound);
    CHECK(fwd::ValidatePath(cyclic.graph, cyclic.problem, r.path).valid);
  }
}

void TestUnreachable() {
  Case c = Demo();
  c.graph.SetEdgeAvailable("relay_a", "dest", false);
  c.graph.SetEdgeAvailable("relay_b", "dest", false);
  c.problem.graph_version = c.graph.version();
  for (const AlgorithmId id : kAllAlgorithmIds) {
    const AlgorithmResult r = Run(id, c);
    CHECK(r.status == AlgorithmStatus::NoPath);
    CHECK(r.path.empty() && !r.objective);
    CHECK(r.evaluations == 0 || id == AlgorithmId::Dijkstra || id == AlgorithmId::AStar ||
          id == AlgorithmId::BellmanFord);
  }
}

void TestMetaheuristicValidityAndReproducibility() {
  const Case demo = Demo();
  const Case medium = Generated(40, 0.1, 21);
  const double optimum = *Run(AlgorithmId::Dijkstra, medium).objective;
  for (const AlgorithmId id : kMeta) {
    const AlgorithmResult r = Run(id, demo, 7);
    CHECK(r.status == AlgorithmStatus::PathFound && r.feasible);
    CHECK(std::fabs(*r.objective - 5.0) < 1e-12);  // Reaches the optimum on 4 nodes.
    CHECK(!r.optimality_claimed);
    for (const std::uint64_t seed : {1ull, 2ull, 3ull}) {
      const AlgorithmResult a = Run(id, medium, seed);
      const AlgorithmResult b = Run(id, medium, seed);
      CHECK(a.status == AlgorithmStatus::PathFound);
      CHECK(a.path == b.path && a.objective == b.objective && a.evaluations == b.evaluations);
      // Objective consistency: the shared validator agrees, and no
      // metaheuristic beats the exact optimum.
      const auto validation = fwd::ValidatePath(medium.graph, medium.problem, a.path);
      CHECK(validation.valid && std::fabs(*validation.objective - *a.objective) < 1e-12);
      CHECK(*a.objective >= optimum - 1e-9);
      CHECK(a.seed == seed && a.parameters.size() >= 1);
    }
  }
}

void TestBudgetsAndTermination() {
  const Case c = Generated(40, 0.1, 22);
  for (const AlgorithmId id : kMeta) {
    const AlgorithmResult r = Run(id, c, 1, 5);
    CHECK(r.iterations <= 5);
    CHECK(r.termination == Termination::IterationBudget || r.termination == Termination::EvaluationBudget);

    AlgorithmRequest request;
    request.graph = &c.graph;
    request.problem = &c.problem;
    request.budget.max_iterations = 100000;
    request.budget.max_evaluations = 50;
    const AlgorithmResult limited = CreateAlgorithm(id)->Run(request);
    CHECK(limited.evaluations <= 50);
    CHECK(limited.termination == Termination::EvaluationBudget);

    request.budget.max_evaluations.reset();
    request.budget.max_iterations = 100000000;
    request.budget.deadline_ms = 30.0;
    const AlgorithmResult timed = CreateAlgorithm(id)->Run(request);
    CHECK(timed.termination == Termination::Deadline);
    CHECK(timed.duration_ms < 2000.0);

    std::atomic<bool> cancel{true};
    request.cancel = &cancel;
    const AlgorithmResult cancelled = CreateAlgorithm(id)->Run(request);
    CHECK(cancelled.status == AlgorithmStatus::Cancelled && cancelled.path.empty());
  }
  // The problem's own evaluation limit tightens the budget.
  Case limited = c;
  limited.problem.constraints.max_objective_evaluations = 20;
  CHECK(Run(AlgorithmId::GeneticAlgorithm, limited, 1, 1000).evaluations <= 20);
}

void TestConstraintHandling() {
  // Latency budget: every reported feasible path really meets it, and an
  // impossible budget is reported as no feasible path.
  Case c = Generated(30, 0.15, 23);
  c.problem.constraints.latency_budget_ms = 30.0;
  for (const AlgorithmId id : kAllAlgorithmIds) {
    const AlgorithmResult r = Run(id, c, 3);
    if (r.status == AlgorithmStatus::PathFound) {
      CHECK(r.feasible);
      CHECK(fwd::ValidatePath(c.graph, c.problem, r.path).latency_ms <= 30.0);
    } else {
      CHECK(r.status == AlgorithmStatus::NoFeasiblePath);
    }
  }
  Case impossible = Demo();
  impossible.problem.constraints.latency_budget_ms = 1.0;
  for (const AlgorithmId id : kAllAlgorithmIds) {
    const AlgorithmResult r = Run(id, impossible);
    CHECK(r.status == AlgorithmStatus::NoFeasiblePath);
    CHECK(!r.feasible);
  }
  // Allowed relays restrict every algorithm.
  Case restricted = Demo();
  restricted.problem.constraints.allowed_relays = std::set<std::string>{"relay_b"};
  for (const AlgorithmId id : kAllAlgorithmIds) {
    CHECK((Run(id, restricted).path == fwd::Path{"entry", "relay_b", "dest"}));
  }
}

void TestVerifier() {
  const VerificationReport report = VerifyImplementations(nullptr);
  CHECK(report.algorithms.size() == 10);
  for (const auto& v : report.algorithms) {
    if (!v.implementation_passed) {
      for (const auto& failure : v.failures) std::cerr << ToString(v.algorithm) << ": " << failure << "\n";
    }
    CHECK(v.implementation_passed);
    CHECK(!v.forwarding_passed);
    CHECK(v.worst_runtime_ms > 0.0 && v.max_nodes >= 42);
  }
  // Without forwarding evidence nothing becomes executable.
  const CapabilityRegistry registry = ApplyVerification(CapabilityRegistry::BuiltIn(), report);
  for (const auto& capability : registry.All()) {
    CHECK(capability.availability == ImplementationAvailability::ImplementationPresent);
    CHECK(capability.implementation.worst_case_runtime_ms.has_value());
  }
  CHECK(registry.ExecutableAlgorithms().empty());

  // Simulated forwarding is not integration evidence either.
  fwd::ForwardingGraph demo = fwd::LoopbackDemoGraph();
  fwd::SimulatedForwardingExecutor simulated(demo);
  const VerificationReport sim = VerifyImplementations(&simulated);
  CHECK(sim.CountVerifiedExecutable() == 0);
  CHECK(ApplyVerification(CapabilityRegistry::BuiltIn(), sim).ExecutableAlgorithms().empty());

  // A failed verification keeps the algorithm unavailable.
  VerificationReport failed = report;
  failed.algorithms[0].implementation_passed = false;
  CHECK(ApplyVerification(CapabilityRegistry::BuiltIn(), failed).Find(AlgorithmId::Dijkstra)->availability ==
        ImplementationAvailability::ImplementationUnavailable);
}

void TestDistinctImplementations() {
  // Ten separate implementations, not wrappers around one solver: each
  // reports its own id and parameter set, and the encodings differ by family.
  std::set<std::string> encodings;
  std::set<std::string> parameter_sets;
  for (const AlgorithmId id : kAllAlgorithmIds) {
    const auto algorithm = CreateAlgorithm(id);
    CHECK(algorithm->id() == id);
    encodings.insert(algorithm->encoding());
    std::string names;
    for (const auto& entry : algorithm->DefaultParameters()) names += entry.first + ";";
    parameter_sets.insert(names);
  }
  CHECK(encodings.size() == 4);  // graph_search, random_key_priority, node_sequence_path, edge_pheromone.
  // Dijkstra and Bellman-Ford take no parameters; the other eight differ.
  CHECK(parameter_sets.size() == 9);
  // With the same seed the five random-key algorithms still search
  // differently: their evaluation traces are not all identical.
  const Case c = Generated(40, 0.1, 24);
  std::set<std::string> outcomes;
  for (const AlgorithmId id : {AlgorithmId::ParticleSwarmOptimization, AlgorithmId::ArtificialBeeColony,
                               AlgorithmId::GreyWolfOptimizer, AlgorithmId::FireflyAlgorithm,
                               AlgorithmId::WhaleOptimizationAlgorithm}) {
    const AlgorithmResult r = Run(id, c, 1, 3);
    outcomes.insert(std::to_string(r.evaluations) + "|" + fwd::PathText(r.path));
  }
  CHECK(outcomes.size() >= 2);
}

}  // namespace

int main() {
  return aafr::test::RunTests({
      {"known_shortest_paths", TestKnownShortestPaths},
      {"exact_against_brute_force", TestExactAgainstBruteForce},
      {"astar_heuristics", TestAStarHeuristics},
      {"invalid_inputs", TestInvalidInputs},
      {"negative_edges_and_cycles", TestNegativeEdges},
      {"unreachable_destination", TestUnreachable},
      {"metaheuristic_validity_reproducibility", TestMetaheuristicValidityAndReproducibility},
      {"budgets_and_termination", TestBudgetsAndTermination},
      {"constraint_handling", TestConstraintHandling},
      {"implementation_verifier", TestVerifier},
      {"distinct_implementations", TestDistinctImplementations},
  });
}
