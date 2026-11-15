#include "routing/topology/scenario_catalog.h"

#include <cstdio>

namespace aafr::forwarding {
namespace {

// Same generator as the algorithms (xoshiro-style mixing), kept local so the
// topology library does not depend on the algorithm library.
class Generator {
 public:
  explicit Generator(std::uint64_t seed) : state_(seed) {}
  std::uint64_t Next() {
    std::uint64_t z = (state_ += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
  }
  double Uniform() { return static_cast<double>(Next() >> 11) * 0x1.0p-53; }
  double Uniform(double low, double high) { return low + (high - low) * Uniform(); }
  std::size_t Index(std::size_t size) { return size == 0 ? 0 : static_cast<std::size_t>(Next() % size); }

 private:
  std::uint64_t state_;
};

std::string Relay(std::size_t i) { return "r" + std::to_string(i); }

std::string Text(double value) {
  char buffer[32];
  std::snprintf(buffer, sizeof(buffer), "%g", value);
  return buffer;
}

ForwardingEdge MakeEdge(Generator& g, const GeneratorConfig& c, const std::string& from, const std::string& to) {
  ForwardingEdge edge;
  edge.from = from;
  edge.to = to;
  edge.latency_ms = Measurement::Of(g.Uniform(c.min_latency_ms, c.max_latency_ms), Provenance::Simulated);
  edge.loss_ratio = Measurement::Of(g.Uniform(0.0, c.max_loss_ratio), Provenance::Simulated);
  edge.bandwidth_mbps = Measurement::Of(g.Uniform(c.min_bandwidth_mbps, c.max_bandwidth_mbps), Provenance::Simulated);
  return edge;
}

ForwardingProblem DefaultProblem(const ForwardingGraph& graph, const std::string& id) {
  ForwardingProblem problem;
  problem.id = id;
  problem.source = "entry";
  problem.destination = "dest";
  problem.graph_version = graph.version();
  return problem;
}

Scenario Generated(const std::string& id, const std::string& description, GeneratorConfig config,
                   std::uint64_t seed) {
  Scenario s;
  s.id = id;
  s.description = description;
  s.generator_seed = seed;
  s.graph = GenerateGraph(id, config, seed);
  s.problem = DefaultProblem(s.graph, id);
  s.configuration = {{"relays", std::to_string(config.relays)},
                     {"edge_probability", Text(config.edge_probability)},
                     {"latency_ms_range", Text(config.min_latency_ms) + "-" + Text(config.max_latency_ms)},
                     {"max_loss_ratio", Text(config.max_loss_ratio)},
                     {"bandwidth_mbps_range", Text(config.min_bandwidth_mbps) + "-" + Text(config.max_bandwidth_mbps)},
                     {"guarantee_path", config.guarantee_path ? "true" : "false"},
                     {"provenance", "simulated"}};
  return s;
}

void Repin(Scenario& s) { s.problem.graph_version = s.graph.version(); }

}  // namespace

ForwardingGraph GenerateGraph(const std::string& id, const GeneratorConfig& c, std::uint64_t seed) {
  Generator g(seed);
  ForwardingGraph graph(id);
  graph.AddNode({"entry", NodeRole::Entry, true});
  for (std::size_t i = 0; i < c.relays; ++i) graph.AddNode({Relay(i), NodeRole::Relay, true});
  graph.AddNode({"dest", NodeRole::Destination, true});
  const auto maybe = [&](const std::string& from, const std::string& to) {
    if (g.Uniform() < c.edge_probability) graph.AddEdge(MakeEdge(g, c, from, to));
  };
  for (std::size_t i = 0; i < c.relays; ++i) maybe("entry", Relay(i));
  for (std::size_t i = 0; i < c.relays; ++i) {
    for (std::size_t j = 0; j < c.relays; ++j) {
      if (i != j) maybe(Relay(i), Relay(j));
    }
  }
  for (std::size_t i = 0; i < c.relays; ++i) maybe(Relay(i), "dest");
  if (c.guarantee_path && c.relays >= 2) {
    const std::size_t a = g.Index(c.relays);
    std::size_t b = g.Index(c.relays - 1);
    if (b >= a) ++b;
    const std::vector<std::pair<std::string, std::string>> backbone = {
        {"entry", Relay(a)}, {Relay(a), Relay(b)}, {Relay(b), "dest"}};
    for (const auto& [from, to] : backbone) {
      if (!graph.FindEdge(from, to)) graph.AddEdge(MakeEdge(g, c, from, to));
    }
  }
  return graph;
}

void ApplyChanges(ForwardingGraph& graph, const std::vector<TopologyChange>& changes) {
  for (const TopologyChange& change : changes) {
    switch (change.kind) {
      case TopologyChange::Kind::EdgeLatency:
        graph.SetEdgeLatency(change.from, change.to, Measurement::Of(change.latency_ms, Provenance::Simulated));
        break;
      case TopologyChange::Kind::EdgeAvailable:
        graph.SetEdgeAvailable(change.from, change.to, change.available);
        break;
      case TopologyChange::Kind::NodeAvailable:
        graph.SetNodeAvailable(change.from, change.available);
        break;
    }
  }
}

std::vector<Scenario> BenchmarkScenarios() {
  std::vector<Scenario> all;
  GeneratorConfig base;

  GeneratorConfig small = base;
  small.relays = 6;
  small.edge_probability = 0.4;
  all.push_back(Generated("small", "6 relays, edge probability 0.4", small, 101));

  GeneratorConfig medium = base;
  medium.relays = 40;
  medium.edge_probability = 0.1;
  all.push_back(Generated("medium", "40 relays, edge probability 0.1", medium, 102));

  GeneratorConfig large = base;
  large.relays = 150;
  large.edge_probability = 0.03;
  all.push_back(Generated("large", "150 relays, edge probability 0.03", large, 103));

  GeneratorConfig sparse = base;
  sparse.relays = 40;
  sparse.edge_probability = 0.04;
  all.push_back(Generated("sparse", "40 relays, edge probability 0.04", sparse, 104));

  GeneratorConfig dense = base;
  dense.relays = 25;
  dense.edge_probability = 0.6;
  all.push_back(Generated("dense", "25 relays, edge probability 0.6", dense, 105));

  {
    // Eight parallel two-relay chains with near-equal cost plus cross links:
    // many candidate paths whose objectives differ by small margins.
    Scenario s;
    s.id = "multiple_candidates";
    s.description = "8 parallel two-relay chains with near-equal costs and cross links";
    s.generator_seed = 106;
    Generator g(106);
    GeneratorConfig c = base;
    s.graph = ForwardingGraph(s.id);
    s.graph.AddNode({"entry", NodeRole::Entry, true});
    for (std::size_t i = 0; i < 16; ++i) s.graph.AddNode({Relay(i), NodeRole::Relay, true});
    s.graph.AddNode({"dest", NodeRole::Destination, true});
    c.min_latency_ms = 9.0;
    c.max_latency_ms = 11.0;
    for (std::size_t k = 0; k < 8; ++k) {
      s.graph.AddEdge(MakeEdge(g, c, "entry", Relay(2 * k)));
      s.graph.AddEdge(MakeEdge(g, c, Relay(2 * k), Relay(2 * k + 1)));
      s.graph.AddEdge(MakeEdge(g, c, Relay(2 * k + 1), "dest"));
      if (k + 1 < 8) s.graph.AddEdge(MakeEdge(g, c, Relay(2 * k), Relay(2 * k + 3)));
    }
    s.problem = DefaultProblem(s.graph, s.id);
    s.configuration = {{"chains", "8"}, {"latency_ms_range", "9-11"}, {"provenance", "simulated"}};
    all.push_back(std::move(s));
  }
  {
    Scenario s = Generated("disconnected", "40 relays; every edge into the destination removed", medium, 107);
    for (const ForwardingEdge& edge : std::vector<ForwardingEdge>(s.graph.edges())) {
      if (edge.to == "dest") s.graph.SetEdgeAvailable(edge.from, edge.to, false);
    }
    s.configuration["modification"] = "all edges into dest unavailable";
    Repin(s);
    all.push_back(std::move(s));
  }
  {
    Scenario s = Generated("high_latency", "40 relays; 30% of edges 100-400 ms; latency budget 150 ms", medium, 108);
    Generator g(1108);
    for (const ForwardingEdge& edge : std::vector<ForwardingEdge>(s.graph.edges())) {
      if (g.Uniform() < 0.3) {
        s.graph.SetEdgeLatency(edge.from, edge.to, Measurement::Of(g.Uniform(100.0, 400.0), Provenance::Simulated));
      }
    }
    s.problem.constraints.latency_budget_ms = 150.0;
    s.configuration["modification"] = "30% of edges set to 100-400 ms";
    s.configuration["latency_budget_ms"] = "150";
    Repin(s);
    all.push_back(std::move(s));
  }
  {
    GeneratorConfig c = medium;
    c.max_bandwidth_mbps = 1000.0;
    Scenario s = Generated("constrained_bandwidth", "40 relays; minimum bandwidth 600 Mbps per edge", c, 109);
    s.problem.constraints.min_bandwidth_mbps = 600.0;
    s.configuration["min_bandwidth_mbps"] = "600";
    all.push_back(std::move(s));
  }
  {
    Scenario s = Generated("link_failure", "40 relays; 20% of edges failed", medium, 110);
    Generator g(1110);
    for (const ForwardingEdge& edge : std::vector<ForwardingEdge>(s.graph.edges())) {
      if (g.Uniform() < 0.2) s.graph.SetEdgeAvailable(edge.from, edge.to, false);
    }
    s.configuration["modification"] = "20% of edges unavailable";
    Repin(s);
    all.push_back(std::move(s));
  }
  {
    Scenario s = Generated("limited_budget", "40 relays; 3 iterations and 30 objective evaluations", medium, 111);
    s.max_iterations = 3;
    s.max_evaluations = 30;
    s.configuration["max_iterations"] = "3";
    s.configuration["max_evaluations"] = "30";
    all.push_back(std::move(s));
  }
  {
    Scenario s = Generated("dynamic", "40 relays; 4 epochs of latency drift and one failure per epoch", medium, 112);
    Generator g(1112);
    const std::vector<ForwardingEdge> edges = s.graph.edges();
    for (int epoch = 1; epoch <= 3; ++epoch) {
      std::vector<TopologyChange> changes;
      for (const ForwardingEdge& edge : edges) {
        if (g.Uniform() < 0.25) {
          TopologyChange change;
          change.kind = TopologyChange::Kind::EdgeLatency;
          change.from = edge.from;
          change.to = edge.to;
          change.latency_ms = g.Uniform(1.0, 60.0);
          changes.push_back(change);
        }
      }
      const ForwardingEdge& failed = edges[g.Index(edges.size())];
      TopologyChange failure;
      failure.kind = TopologyChange::Kind::EdgeAvailable;
      failure.from = failed.from;
      failure.to = failed.to;
      failure.available = false;
      changes.push_back(failure);
      s.epochs.push_back(std::move(changes));
    }
    s.configuration["epochs"] = "4";
    s.configuration["drift"] = "25% of edges re-drawn in 1-60 ms per epoch, one edge failure per epoch";
    all.push_back(std::move(s));
  }
  return all;
}

std::vector<std::string> BenchmarkScenarioIds() {
  std::vector<std::string> ids;
  for (const Scenario& s : BenchmarkScenarios()) ids.push_back(s.id);
  return ids;
}

std::optional<Scenario> FindScenario(const std::string& id) {
  for (Scenario& s : BenchmarkScenarios()) {
    if (s.id == id) return std::move(s);
  }
  return std::nullopt;
}

ForwardingGraph LoopbackDemoGraph() {
  ForwardingGraph graph("loopback_demo");
  graph.AddNode({"entry", NodeRole::Entry, true});
  graph.AddNode({"relay_a", NodeRole::Relay, true});
  graph.AddNode({"relay_b", NodeRole::Relay, true});
  graph.AddNode({"dest", NodeRole::Destination, true});
  const auto edge = [&graph](const char* from, const char* to, double latency) {
    ForwardingEdge e;
    e.from = from;
    e.to = to;
    e.latency_ms = Measurement::Of(latency, Provenance::Configured);
    e.loss_ratio = Measurement::Of(0.0, Provenance::Configured);
    e.bandwidth_mbps = Measurement::Of(1000.0, Provenance::Configured);
    graph.AddEdge(e);
  };
  edge("entry", "relay_a", 2.0);
  edge("entry", "relay_b", 4.0);
  edge("relay_a", "dest", 2.0);
  edge("relay_b", "dest", 1.0);
  edge("relay_a", "relay_b", 1.0);
  return graph;
}

ForwardingProblem LoopbackDemoProblem(const ForwardingGraph& graph) {
  ForwardingProblem problem = DefaultProblem(graph, "loopback_demo");
  problem.constraints.max_hops = 4;
  problem.constraints.computation_deadline_ms = 1000.0;
  return problem;
}

}  // namespace aafr::forwarding
