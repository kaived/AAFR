// V0.5: forwarding graph, objective, path validation and the simulated
// executor. Deterministic; no network access.

#include <cmath>

#include "routing/forwarding/forwarding_executor.h"
#include "routing/topology/scenario_catalog.h"
#include "tests/support/test_support.h"

using namespace aafr::forwarding;

namespace {

ForwardingEdge Edge(const std::string& from, const std::string& to, double latency, double loss = 0.0) {
  ForwardingEdge edge;
  edge.from = from;
  edge.to = to;
  edge.latency_ms = Measurement::Of(latency, Provenance::Configured);
  edge.loss_ratio = Measurement::Of(loss, Provenance::Configured);
  edge.bandwidth_mbps = Measurement::Of(100.0, Provenance::Configured);
  return edge;
}

void TestValidGraph() {
  const ForwardingGraph graph = LoopbackDemoGraph();
  CHECK(graph.Validate().empty());
  CHECK(graph.nodes().size() == 4);
  CHECK(graph.edges().size() == 5);
  CHECK(graph.FindEdge("entry", "relay_a") != nullptr);
  CHECK(graph.FindEdge("relay_a", "entry") == nullptr);  // Missing edge.
  const ForwardingProblem problem = LoopbackDemoProblem(graph);
  CHECK(ValidateProblem(graph, problem).empty());
}

void TestInvalidGraph() {
  ForwardingGraph graph("bad");
  graph.AddNode({"entry", NodeRole::Entry, true});
  graph.AddNode({"entry", NodeRole::Relay, true});        // Duplicate id.
  graph.AddNode({"bad id", NodeRole::Relay, true});       // Illegal characters.
  graph.AddEdge(Edge("entry", "ghost", 1.0));             // Dangling.
  graph.AddEdge(Edge("entry", "entry", 1.0));             // Self-loop.
  ForwardingEdge negative = Edge("entry", "bad id", -3.0);  // Negative latency.
  graph.AddEdge(negative);
  ForwardingEdge lossy = Edge("bad id", "entry", 1.0, 1.0);  // Loss must be < 1.
  graph.AddEdge(lossy);
  ForwardingEdge no_provenance = Edge("bad id", "ghost", 1.0);
  no_provenance.latency_ms.provenance = Provenance::Unknown;
  graph.AddEdge(no_provenance);
  const auto issues = graph.Validate();
  const auto has = [&issues](const std::string& text) {
    for (const auto& issue : issues) {
      if (aafr::test::Contains(issue, text)) return true;
    }
    return false;
  };
  CHECK(has("used more than once"));
  CHECK(has("characters other than"));
  CHECK(has("unknown node"));
  CHECK(has("self-loop"));
  CHECK(has("latency is negative"));
  CHECK(has("must be below 1"));
  CHECK(has("no provenance"));
  CHECK(ForwardingGraph().Validate().size() == 1);  // No nodes.
}

void TestProblemValidation() {
  const ForwardingGraph graph = LoopbackDemoGraph();
  ForwardingProblem problem = LoopbackDemoProblem(graph);
  problem.source = "relay_a";
  CHECK(!ValidateProblem(graph, problem).empty());  // Source must be an entry.
  problem = LoopbackDemoProblem(graph);
  problem.destination = "nowhere";
  CHECK(!ValidateProblem(graph, problem).empty());
  problem = LoopbackDemoProblem(graph);
  problem.objective.latency_weight = -1.0;
  CHECK(!ValidateProblem(graph, problem).empty());
  problem = LoopbackDemoProblem(graph);
  problem.constraints.latency_budget_ms = 0.0;
  CHECK(!ValidateProblem(graph, problem).empty());
  problem = LoopbackDemoProblem(graph);
  problem.graph_version = graph.version() + 1;
  CHECK(!ValidateProblem(graph, problem).empty());
}

void TestObjective() {
  const ForwardingGraph graph = LoopbackDemoGraph();
  const ForwardingProblem problem = LoopbackDemoProblem(graph);
  // J = sum(1 * latency + 100 * -ln(1 - 0) + 0.5 + 1 * 0).
  auto a = ValidatePath(graph, problem, {"entry", "relay_a", "dest"});
  CHECK(a.valid && a.feasible);
  CHECK_NEAR(*a.objective, 2.0 + 2.0 + 1.0, 1e-12);
  CHECK_NEAR(a.latency_ms, 4.0, 1e-12);
  CHECK(a.hops == 2);
  auto b = ValidatePath(graph, problem, {"entry", "relay_b", "dest"});
  CHECK_NEAR(*b.objective, 4.0 + 1.0 + 1.0, 1e-12);
  auto ab = ValidatePath(graph, problem, {"entry", "relay_a", "relay_b", "dest"});
  CHECK_NEAR(*ab.objective, 2.0 + 1.0 + 1.0 + 1.5, 1e-12);

  // Loss enters as -ln(1 - p): additive over edges.
  ForwardingEdge lossy = Edge("x", "y", 0.0, 0.1);
  ForwardingObjective objective;
  CHECK_NEAR(*EdgeCost(lossy, objective), 100.0 * -std::log(0.9) + 0.5, 1e-9);
  // Unknown weighted attribute: no cost (never zero).
  lossy.loss_ratio = {};
  CHECK(!EdgeCost(lossy, objective).has_value());
  objective.loss_weight = 0.0;
  CHECK(EdgeCost(lossy, objective).has_value());
}

void TestPathFeasibility() {
  ForwardingGraph graph = LoopbackDemoGraph();
  ForwardingProblem problem = LoopbackDemoProblem(graph);
  CHECK(ValidatePath(graph, problem, {}).Has(PathIssue::Empty));
  CHECK(ValidatePath(graph, problem, {"relay_a", "dest"}).Has(PathIssue::WrongSource));
  CHECK(ValidatePath(graph, problem, {"entry", "relay_a"}).Has(PathIssue::WrongDestination));
  CHECK(ValidatePath(graph, problem, {"entry", "dest"}).Has(PathIssue::MissingEdge));
  CHECK(ValidatePath(graph, problem, {"entry", "relay_a", "relay_a", "dest"}).Has(PathIssue::RepeatedNode));
  CHECK(ValidatePath(graph, problem, {"entry", "ghost", "dest"}).Has(PathIssue::UnknownNode));

  problem.constraints.latency_budget_ms = 3.0;
  auto over = ValidatePath(graph, problem, {"entry", "relay_a", "dest"});
  CHECK(over.valid && !over.feasible && over.Has(PathIssue::LatencyBudgetExceeded));
  CHECK(over.objective.has_value());  // Score still reported.
  problem = LoopbackDemoProblem(graph);
  problem.constraints.max_hops = 2;
  CHECK(ValidatePath(graph, problem, {"entry", "relay_a", "relay_b", "dest"}).Has(PathIssue::HopLimitExceeded));
  problem = LoopbackDemoProblem(graph);
  problem.constraints.allowed_relays = std::set<std::string>{"relay_b"};
  CHECK(ValidatePath(graph, problem, {"entry", "relay_a", "dest"}).Has(PathIssue::RelayNotAllowed));
  problem = LoopbackDemoProblem(graph);
  problem.constraints.min_bandwidth_mbps = 5000.0;
  CHECK(ValidatePath(graph, problem, {"entry", "relay_a", "dest"}).Has(PathIssue::BandwidthBelowMinimum));
  problem = LoopbackDemoProblem(graph);
  problem.constraints.protocol = aafr::netstate::TransportProtocol::Quic;
  CHECK(ValidatePath(graph, problem, {"entry", "relay_a", "dest"}).Has(PathIssue::ProtocolUnsupported));

  // Availability changes bump the version; a pinned problem notices.
  problem = LoopbackDemoProblem(graph);
  const auto before = graph.version();
  CHECK(graph.SetEdgeAvailable("entry", "relay_a", false));
  CHECK(graph.version() == before + 1);
  CHECK(!graph.SetEdgeAvailable("entry", "nowhere", false));
  auto stale = ValidatePath(graph, problem, {"entry", "relay_a", "dest"});
  CHECK(stale.Has(PathIssue::GraphVersionMismatch) && stale.Has(PathIssue::EdgeUnavailable));
  problem.graph_version = graph.version();
  CHECK(ValidatePath(graph, problem, {"entry", "relay_b", "dest"}).feasible);
  graph.SetNodeAvailable("relay_b", false);
  problem.graph_version = graph.version();
  CHECK(ValidatePath(graph, problem, {"entry", "relay_b", "dest"}).Has(PathIssue::NodeUnavailable));
}

void TestProblemView() {
  ForwardingGraph graph = LoopbackDemoGraph();
  ForwardingProblem problem = LoopbackDemoProblem(graph);
  ProblemView view = BuildView(graph, problem);
  CHECK(view.usable_edge_count == 5);
  CHECK(!view.has_negative_cost);
  CHECK_NEAR(view.min_edge_cost, 1.5, 1e-12);  // relay_a -> relay_b and relay_b -> dest.
  // Unreachable destination: every edge into dest down.
  graph.SetEdgeAvailable("relay_a", "dest", false);
  graph.SetEdgeAvailable("relay_b", "dest", false);
  problem.graph_version = graph.version();
  view = BuildView(graph, problem);
  CHECK(view.usable_edge_count == 3);
  CHECK(view.excluded_edges.size() == 2);
}

void TestSimulatedExecutor() {
  ForwardingGraph graph = LoopbackDemoGraph();
  SimulatedForwardingExecutor executor(graph, 9);
  ForwardingRequest request;
  request.request_id = "sim-1";
  request.path = {"entry", "relay_a", "dest"};
  auto result = executor.Execute(request, {});
  CHECK(result.status == ForwardingStatus::Delivered);
  CHECK(result.path_confirmed);
  CHECK(result.provenance == Provenance::Simulated);
  CHECK_NEAR(result.end_to_end_ms, 4.0, 1e-12);

  request.path = {"entry", "relay_a", "entry", "dest"};
  CHECK(executor.Execute(request, {}).status == ForwardingStatus::RequestRejected);  // Loop.
  request.path = {"entry", "relay_b", "dest"};
  graph.SetNodeAvailable("relay_b", false);
  result = executor.Execute(request, {});
  CHECK(result.status == ForwardingStatus::RelayUnavailable && result.failed_at == "relay_b");
  CHECK(!result.path_confirmed);

  // Seeded loss: the same request id always has the same outcome.
  ForwardingGraph lossy("lossy");
  lossy.AddNode({"entry", NodeRole::Entry, true});
  lossy.AddNode({"dest", NodeRole::Destination, true});
  lossy.AddEdge(Edge("entry", "dest", 1.0, 0.5));
  SimulatedForwardingExecutor sim(lossy, 3);
  int lost = 0;
  for (int i = 0; i < 200; ++i) {
    ForwardingRequest r;
    r.request_id = "loss-" + std::to_string(i);
    r.path = {"entry", "dest"};
    const auto first = sim.Execute(r, {});
    CHECK(first.status == sim.Execute(r, {}).status);
    lost += first.status == ForwardingStatus::Lost;
  }
  CHECK(lost > 60 && lost < 140);
  std::atomic<bool> cancel{true};
  ForwardingRequest r;
  r.request_id = "c";
  r.path = {"entry", "dest"};
  CHECK(sim.Execute(r, {&cancel, "", ""}).status == ForwardingStatus::Cancelled);
}

void TestScenarioCatalog() {
  const auto ids = BenchmarkScenarioIds();
  CHECK(ids.size() == 12);
  for (const auto& scenario : BenchmarkScenarios()) {
    CHECK(scenario.graph.Validate().empty());
    CHECK(ValidateProblem(scenario.graph, scenario.problem).empty());
  }
  GeneratorConfig config;
  const ForwardingGraph a = GenerateGraph("g", config, 5);
  const ForwardingGraph b = GenerateGraph("g", config, 5);
  CHECK(a.edges().size() == b.edges().size());
  bool same = a.edges().size() == b.edges().size();
  for (std::size_t i = 0; same && i < a.edges().size(); ++i) {
    same = a.edges()[i].from == b.edges()[i].from && a.edges()[i].to == b.edges()[i].to &&
           *a.edges()[i].latency_ms.value == *b.edges()[i].latency_ms.value;
  }
  CHECK(same);
  for (const auto& edge : a.edges()) CHECK(edge.latency_ms.provenance == Provenance::Simulated);
  CHECK(FindScenario("dynamic")->epochs.size() == 3);
  CHECK(!FindScenario("nope").has_value());
}

}  // namespace

int main() {
  return aafr::test::RunTests({
      {"valid_graph", TestValidGraph},
      {"invalid_graph", TestInvalidGraph},
      {"problem_validation", TestProblemValidation},
      {"objective", TestObjective},
      {"path_feasibility", TestPathFeasibility},
      {"problem_view", TestProblemView},
      {"simulated_executor", TestSimulatedExecutor},
      {"scenario_catalog", TestScenarioCatalog},
  });
}
