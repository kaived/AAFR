#include "algorithms/execution/implementation_verifier.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <functional>
#include <limits>

#include "routing/topology/scenario_catalog.h"

namespace aafr::algorithms {
namespace {

using forwarding::ForwardingGraph;
using forwarding::ForwardingProblem;
namespace fwd = forwarding;

constexpr double kTolerance = 1e-9;

struct Fixture {
  std::string name;
  ForwardingGraph graph;
  ForwardingProblem problem;
  bool reachable{true};
  std::optional<double> optimum;  // Brute force over all simple paths.
};

// Minimum J(P) over every simple usable path, by exhaustive DFS. Only used
// on small fixtures.
std::optional<double> BruteForceOptimum(const ForwardingGraph& graph, const ForwardingProblem& problem) {
  const fwd::ProblemView view = fwd::BuildView(graph, problem);
  std::optional<double> best;
  std::vector<char> on_path(view.node_ids.size(), 0);
  std::vector<int> path{view.source};
  on_path[static_cast<std::size_t>(view.source)] = 1;
  std::function<void(int, double)> visit = [&](int node, double cost) {
    if (node == view.destination) {
      const auto validation = fwd::ValidatePath(graph, problem, ToNames(view, path));
      if (validation.feasible && (!best || cost < *best)) best = cost;
      return;
    }
    for (const auto& edge : view.out[static_cast<std::size_t>(node)]) {
      if (on_path[static_cast<std::size_t>(edge.to)]) continue;
      on_path[static_cast<std::size_t>(edge.to)] = 1;
      path.push_back(edge.to);
      visit(edge.to, cost + edge.cost);
      path.pop_back();
      on_path[static_cast<std::size_t>(edge.to)] = 0;
    }
  };
  visit(view.source, 0.0);
  return best;
}

std::vector<Fixture> Fixtures() {
  std::vector<Fixture> fixtures;
  {
    Fixture f;
    f.name = "demo";
    f.graph = fwd::LoopbackDemoGraph();
    f.problem = fwd::LoopbackDemoProblem(f.graph);
    fixtures.push_back(std::move(f));
  }
  {
    fwd::GeneratorConfig config;
    config.relays = 8;
    config.edge_probability = 0.35;
    Fixture f;
    f.name = "brute_force_8";
    f.graph = fwd::GenerateGraph("verify_brute_force", config, 7);
    f.problem.source = "entry";
    f.problem.destination = "dest";
    f.problem.graph_version = f.graph.version();
    fixtures.push_back(std::move(f));
  }
  {
    fwd::GeneratorConfig config;
    config.relays = 8;
    config.edge_probability = 0.35;
    Fixture f;
    f.name = "latency_budget_8";
    f.graph = fwd::GenerateGraph("verify_budget", config, 11);
    f.problem.source = "entry";
    f.problem.destination = "dest";
    f.problem.constraints.latency_budget_ms = 40.0;
    f.problem.graph_version = f.graph.version();
    fixtures.push_back(std::move(f));
  }
  {
    fwd::GeneratorConfig config;
    config.relays = 40;
    config.edge_probability = 0.1;
    Fixture f;
    f.name = "scale_40";
    f.graph = fwd::GenerateGraph("verify_scale", config, 13);
    f.problem.source = "entry";
    f.problem.destination = "dest";
    f.problem.graph_version = f.graph.version();
    fixtures.push_back(std::move(f));
  }
  {
    Fixture f;
    f.name = "unreachable";
    f.graph = fwd::LoopbackDemoGraph();
    f.graph.SetEdgeAvailable("relay_a", "dest", false);
    f.graph.SetEdgeAvailable("relay_b", "dest", false);
    f.problem = fwd::LoopbackDemoProblem(f.graph);
    f.reachable = false;
    fixtures.push_back(std::move(f));
  }
  for (Fixture& f : fixtures) {
    if (f.reachable && f.graph.nodes().size() <= 12) f.optimum = BruteForceOptimum(f.graph, f.problem);
  }
  return fixtures;
}

// Demo graph with a negative policy cycle relay_a <-> relay_b.
Fixture NegativeCycleFixture() {
  Fixture f;
  f.name = "negative_cycle";
  f.graph = fwd::LoopbackDemoGraph();
  fwd::ForwardingEdge back;
  back.from = "relay_b";
  back.to = "relay_a";
  back.latency_ms = fwd::Measurement::Of(1.0, fwd::Provenance::Configured);
  back.loss_ratio = fwd::Measurement::Of(0.0, fwd::Provenance::Configured);
  back.policy_cost = -10.0;
  f.graph.AddEdge(back);
  f.problem = fwd::LoopbackDemoProblem(f.graph);
  return f;
}

AlgorithmRequest MakeRequest(const Fixture& f, std::uint64_t seed, const std::atomic<bool>* cancel) {
  AlgorithmRequest request;
  request.graph = &f.graph;
  request.problem = &f.problem;
  request.seed = seed;
  request.budget.max_iterations = 60;
  request.cancel = cancel;
  return request;
}

}  // namespace

const AlgorithmVerification* VerificationReport::Find(AlgorithmId id) const {
  for (const auto& entry : algorithms) {
    if (entry.algorithm == id) return &entry;
  }
  return nullptr;
}

std::size_t VerificationReport::CountImplementationsPassed() const {
  return static_cast<std::size_t>(std::count_if(algorithms.begin(), algorithms.end(),
                                                [](const auto& a) { return a.implementation_passed; }));
}

std::size_t VerificationReport::CountVerifiedExecutable() const {
  return static_cast<std::size_t>(std::count_if(algorithms.begin(), algorithms.end(), [this](const auto& a) {
    return a.implementation_passed && a.forwarding_passed && forwarding_real_network;
  }));
}

VerificationReport VerifyImplementations(fwd::IForwardingExecutor* executor, const std::atomic<bool>* cancel) {
  const auto started = std::chrono::steady_clock::now();
  VerificationReport report;
  if (executor) {
    report.forwarding_executor = executor->Capability().executor;
    report.forwarding_real_network = executor->Capability().real_network;
  }
  const std::vector<Fixture> fixtures = Fixtures();
  const Fixture negative = NegativeCycleFixture();
  int request_counter = 0;

  for (const AlgorithmId id : kAllAlgorithmIds) {
    if (cancel && cancel->load()) break;
    AlgorithmVerification v;
    v.algorithm = id;
    const auto algorithm = CreateAlgorithm(id);
    if (!algorithm) {
      v.failures.push_back("no implementation");
      report.algorithms.push_back(v);
      continue;
    }
    v.version = algorithm->version();
    const bool exact = algorithm->deterministic();
    const auto fail = [&v](const std::string& what) { v.failures.push_back(what); };
    fwd::Path demo_path;

    for (const Fixture& f : fixtures) {
      const AlgorithmResult r = algorithm->Run(MakeRequest(f, 1, cancel));
      v.worst_runtime_ms = std::max(v.worst_runtime_ms, r.duration_ms);
      v.peak_accounted_memory_bytes = std::max(v.peak_accounted_memory_bytes, r.accounted_memory_bytes);
      v.max_nodes = std::max<std::uint64_t>(v.max_nodes, f.graph.nodes().size());
      v.max_edges = std::max<std::uint64_t>(v.max_edges, f.graph.edges().size());
      if (!f.reachable) {
        if (r.status != AlgorithmStatus::NoPath) fail(f.name + ": expected no_path, got " + ToString(r.status));
        else v.passed_checks.push_back(f.name + ": unreachable destination reported");
        continue;
      }
      const bool path_limits = f.problem.constraints.latency_budget_ms || f.problem.constraints.max_hops;
      if (path_limits && r.status == AlgorithmStatus::NoFeasiblePath) {
        // Exact algorithms optimize J(P) and check path-level limits after;
        // reporting an infeasible optimum is the correct, honest outcome.
        // So is "none found" when no feasible path exists at all.
        const auto validation = fwd::ValidatePath(f.graph, f.problem, r.path);
        const bool honest = !f.optimum || (exact && validation.valid && !validation.feasible);
        if (honest) v.passed_checks.push_back(f.name + ": path-level limit violation reported");
        else fail(f.name + ": reported no feasible path although one exists");
        continue;
      }
      if (r.status != AlgorithmStatus::PathFound || !r.objective) {
        fail(f.name + ": expected a feasible path, got " + ToString(r.status) + " " + r.message);
        continue;
      }
      const auto validation = fwd::ValidatePath(f.graph, f.problem, r.path);
      if (!validation.valid || !validation.feasible || !validation.objective ||
          std::fabs(*validation.objective - *r.objective) > kTolerance) {
        fail(f.name + ": returned path does not validate with the reported score");
        continue;
      }
      if (r.iterations > 60 && !exact) fail(f.name + ": exceeded the iteration budget");
      if (f.optimum) {
        if (exact && std::fabs(*r.objective - *f.optimum) > kTolerance) {
          fail(f.name + ": objective differs from the brute-force optimum");
        } else if (!exact && *r.objective < *f.optimum - kTolerance) {
          fail(f.name + ": objective below the brute-force optimum (objective inconsistency)");
        } else if (!exact && f.name == "demo" && std::fabs(*r.objective - *f.optimum) > kTolerance) {
          fail("demo: did not reach the optimum of the 4-node graph");
        }
      }
      if (f.name == "demo") demo_path = r.path;
      v.passed_checks.push_back(f.name + ": valid feasible path" + (f.optimum && exact ? " at the optimum" : ""));
      if (!exact) {
        const AlgorithmResult again = algorithm->Run(MakeRequest(f, 1, cancel));
        if (again.path != r.path || again.objective != r.objective) fail(f.name + ": not reproducible with a fixed seed");
      }
    }

    // Negative costs.
    const AlgorithmResult n = algorithm->Run(MakeRequest(negative, 1, cancel));
    if (id == AlgorithmId::Dijkstra || id == AlgorithmId::AStar) {
      if (n.status != AlgorithmStatus::InvalidInput) fail("negative costs were not refused");
      else v.passed_checks.push_back("negative costs refused");
    } else if (id == AlgorithmId::BellmanFord) {
      if (n.status != AlgorithmStatus::NegativeCycle) fail("negative cycle not detected");
      else v.passed_checks.push_back("negative cycle detected");
    } else if (n.status == AlgorithmStatus::PathFound) {
      // Metaheuristics search simple paths only, so the cycle cannot be
      // exploited; the path must still validate.
      if (!fwd::ValidatePath(negative.graph, negative.problem, n.path).valid) fail("invalid path with negative costs");
      else v.passed_checks.push_back("simple path under negative policy costs");
    } else {
      fail(std::string("negative-cost fixture: ") + ToString(n.status));
    }
    v.implementation_passed = v.failures.empty();

    if (executor && executor->Ready() && v.implementation_passed && !demo_path.empty()) {
      fwd::ForwardingRequest request;
      request.request_id = std::string("verify-") + ToString(id) + "-" + std::to_string(++request_counter);
      request.path = demo_path;
      request.payload = "verification";
      request.timeout_ms = 3000.0;
      const fwd::ForwardingExecutionResult outcome = executor->Execute(request, {cancel, request.request_id, "verification"});
      if (outcome.status == fwd::ForwardingStatus::Delivered && outcome.path_confirmed) {
        v.forwarding_passed = true;
        v.passed_checks.push_back("forwarding confirmed over " + fwd::PathText(outcome.confirmed_path) + " via " +
                                  report.forwarding_executor);
      } else {
        fail("forwarding not confirmed: " + std::string(fwd::ToString(outcome.status)) + " " + outcome.message);
      }
    }
    report.algorithms.push_back(std::move(v));
  }
  report.total_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
  return report;
}

CapabilityRegistry ApplyVerification(const CapabilityRegistry& registry, const VerificationReport& report) {
  CapabilityRegistry updated = registry;
  for (const AlgorithmVerification& v : report.algorithms) {
    if (!updated.Find(v.algorithm)) continue;
    if (!v.implementation_passed) {
      updated.SetAvailability(v.algorithm, ImplementationAvailability::ImplementationUnavailable);
      continue;
    }
    ImplementationProfile profile;
    // Never zero: the registry requires a positive measured runtime.
    profile.worst_case_runtime_ms = std::max(v.worst_runtime_ms, 0.001);
    profile.peak_memory_bytes = std::max<std::uint64_t>(v.peak_accounted_memory_bytes, 1);
    profile.max_nodes = v.max_nodes;
    profile.max_edges = v.max_edges;
    profile.execution_environments = {"in_process_cpp"};
    // Only forwarding over real traffic counts as integration evidence.
    const bool integrated = v.forwarding_passed && report.forwarding_real_network;
    std::string note = "v" + v.version + ": " + std::to_string(v.passed_checks.size()) + " verification checks passed";
    if (v.forwarding_passed) {
      note += "; forwarding confirmed via " + report.forwarding_executor +
              (report.forwarding_real_network ? " (real loopback traffic)"
                                              : " (simulated, not counted as integration evidence)");
      if (integrated) profile.execution_environments.push_back(report.forwarding_executor);
    }
    note += "; peak memory is accounted container memory, not process memory";
    profile.verification_note = note;
    updated.SetImplementation(v.algorithm, profile);
    updated.SetAvailability(v.algorithm, integrated ? ImplementationAvailability::VerifiedExecutable
                                                    : ImplementationAvailability::ImplementationPresent);
  }
  return updated;
}

}  // namespace aafr::algorithms
