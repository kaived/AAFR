#pragma once

// What AAFR knows about the environment a forwarding algorithm would run in:
// the forwarding topology, the operations the forwarding layer can execute and
// the budgets an algorithm must fit. Plain C++17.
//
// Every field that describes something AAFR may not know is optional. An
// empty optional means "unknown" and is never read as "supported" or
// "unlimited". The browser's own environment (BrowserDefaultEnvironment) says
// truthfully that no forwarding topology and no forwarding operation exists.

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace aafr::netstate {

enum class TopologyRepresentation {
  WeightedDirectedGraph,  // Nodes and directed edges with numeric weights.
  CandidatePathSet,       // An explicit list of alternative paths or endpoints.
};

// The question an algorithm is asked to answer.
enum class ProblemRepresentation {
  SingleSourceShortestPath,
  SinglePairShortestPath,
  // Search over encoded candidate solutions scored by an objective function.
  CandidateSolutionOptimization,
};

enum class OptimizationObjective {
  MinimizeAdditivePathCost,  // Sum of edge weights along a path.
  MinimizeScalarObjective,   // Any objective function returning one number.
};

enum class ForwardingOperation {
  SelectNextHop,
  SelectPath,
  SelectEndpoint,
};

enum class TransportProtocol { Tcp, Udp, Quic };

struct EdgeWeightInfo {
  // False when a weight is missing, NaN or infinite.
  bool all_finite{false};
  bool has_negative{false};
  // Whether a negative-weight cycle is reachable. Unknown unless checked.
  std::optional<bool> has_negative_cycle;
};

struct TopologyInfo {
  std::set<TopologyRepresentation> representations;
  std::uint64_t node_count{0};
  std::uint64_t edge_count{0};
  std::uint64_t candidate_path_count{0};
  // Whether a destination is reachable from the source at all.
  std::optional<bool> reachable;
  // Whether the topology stayed unchanged over the observation window.
  std::optional<bool> stable;
  std::optional<EdgeWeightInfo> edge_weights;
  // Whether an admissible heuristic (for A*) is available for this topology.
  std::optional<bool> heuristic_available;
};

struct ExecutionEnvironment {
  // nullopt: not known whether a topology exists. A value with
  // `available == false` states that none exists.
  struct Topology {
    bool available{false};
    TopologyInfo info;
  };
  std::optional<Topology> topology;

  // nullopt: unknown. An empty set states that nothing can be executed.
  std::optional<std::set<ForwardingOperation>> forwarding_operations;
  std::optional<std::set<TransportProtocol>> supported_protocols;
  // Protocols the forwarding problem needs; checked against the set above.
  std::set<TransportProtocol> required_protocols;
  std::vector<std::string> forwarding_restrictions;

  std::optional<ProblemRepresentation> problem;
  std::optional<OptimizationObjective> objective;

  // Budgets for one algorithm execution.
  std::optional<double> computation_deadline_ms;
  std::optional<double> latency_budget_ms;
  std::optional<std::uint64_t> memory_budget_bytes;
  std::optional<double> cpu_budget_percent;
  // Highest acceptable TCP retransmission ratio (0..1), when one is required.
  std::optional<double> max_retransmission_ratio;

  // Configuration supplied per algorithm id: parameter name -> value.
  std::map<std::string, std::map<std::string, std::string>> algorithm_configuration;
};

// The environment of the V0.4 browser: no forwarding topology, no forwarding
// layer, no problem definition and no configured budgets.
ExecutionEnvironment BrowserDefaultEnvironment();

const char* ToString(TopologyRepresentation value);
const char* ToString(ProblemRepresentation value);
const char* ToString(OptimizationObjective value);
const char* ToString(ForwardingOperation value);
const char* ToString(TransportProtocol value);

}  // namespace aafr::netstate
