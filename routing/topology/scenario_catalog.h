#pragma once

// Deterministic controlled-forwarding scenarios. Plain C++17.
//
// Every value produced here is Simulated (generated graphs) or Configured
// (the loopback demo topology). None of it is a network observation. The
// same scenario id and seed always produce the same graph.

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "routing/topology/forwarding_graph.h"
#include "routing/topology/forwarding_problem.h"

namespace aafr::forwarding {

struct TopologyChange {
  enum class Kind { EdgeLatency, EdgeAvailable, NodeAvailable };
  Kind kind{Kind::EdgeLatency};
  std::string from;  // Node id for NodeAvailable.
  std::string to;
  double latency_ms{0.0};
  bool available{true};
};

struct Scenario {
  std::string id;
  std::string description;
  std::uint64_t generator_seed{0};
  ForwardingGraph graph;
  ForwardingProblem problem;
  // Dynamic topology: change sets applied one after another; the problem is
  // solved once per epoch (epoch 0 is the unchanged graph).
  std::vector<std::vector<TopologyChange>> epochs;
  // Budget overrides for the limited-budget scenario.
  std::optional<std::uint64_t> max_iterations;
  std::optional<std::uint64_t> max_evaluations;
  // Generator settings, recorded with exported results.
  std::map<std::string, std::string> configuration;
};

struct GeneratorConfig {
  std::size_t relays{10};
  double edge_probability{0.2};
  double min_latency_ms{1.0};
  double max_latency_ms{20.0};
  double max_loss_ratio{0.02};
  double min_bandwidth_mbps{10.0};
  double max_bandwidth_mbps{1000.0};
  // Guarantee one entry -> relay -> relay -> destination path.
  bool guarantee_path{true};
};

// Entry "entry", destination "dest", relays "r0".."r<n-1>", every attribute
// Simulated, drawn from a generator seeded with `seed`.
ForwardingGraph GenerateGraph(const std::string& id, const GeneratorConfig& config, std::uint64_t seed);

void ApplyChanges(ForwardingGraph& graph, const std::vector<TopologyChange>& changes);

// The twelve benchmark scenarios, in a fixed order.
std::vector<Scenario> BenchmarkScenarios();
std::vector<std::string> BenchmarkScenarioIds();
std::optional<Scenario> FindScenario(const std::string& id);

// The topology the browser's loopback proof of concept runs on:
//   entry -> relay_a -> dest, entry -> relay_b -> dest, relay_a -> relay_b.
// Latencies are Configured values, not measurements.
ForwardingGraph LoopbackDemoGraph();
ForwardingProblem LoopbackDemoProblem(const ForwardingGraph& graph);

}  // namespace aafr::forwarding
