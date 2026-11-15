#pragma once

// The forwarding problem, its objective and candidate-path validation.
// Plain C++17, deterministic, no I/O.
//
// Problem: given the controlled graph G = (V, E), a source s (the entry) and
// a destination t, find a simple path P = (s = v0, v1, ..., vk = t) that
// satisfies the hard constraints and minimizes the objective J(P).
//
// Objective (additive, so the exact algorithms solve it exactly and every
// metaheuristic is scored by the same function):
//
//   c(e) = w_latency * latency_ms(e)
//        + w_loss    * -ln(1 - loss_ratio(e))
//        + w_hop
//        + w_policy  * policy_cost(e)
//   J(P) = sum of c(e) over the edges of P
//
// -ln(1 - p) is the additive form of end-to-end delivery probability:
// sum(-ln(1 - p_i)) = -ln(prod(1 - p_i)). Bandwidth is a hard per-edge
// constraint (min_bandwidth_mbps) rather than a cost term because the
// bottleneck is not additive.
//
// Hard constraints are never mixed into J(P). Per-edge constraints (edge and
// node availability, relays allowed, protocol, minimum bandwidth, known
// attributes for weighted terms) decide which edges are usable at all.
// Path-level constraints (latency budget, hop limit) are checked on the
// whole path: a path that violates them is reported infeasible together with
// its score, not silently penalized.

#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "network/network_state/execution_environment.h"
#include "routing/topology/forwarding_graph.h"

namespace aafr::forwarding {

using Path = std::vector<std::string>;

struct ForwardingObjective {
  // Bump when the cost formula changes; recorded with every result.
  static constexpr int kVersion = 1;
  double latency_weight{1.0};    // per ms
  double loss_weight{100.0};     // per unit of -ln(1 - p)
  double hop_weight{0.5};        // per edge
  double policy_weight{1.0};     // per unit of configured policy cost

  // Problems with the weights; empty when valid (finite, nonnegative except
  // policy_weight, at least one positive cost weight).
  std::vector<std::string> Validate() const;
  std::string Describe() const;
};

struct ForwardingConstraints {
  std::optional<double> latency_budget_ms;  // Path-level: sum of edge latency.
  std::optional<std::size_t> max_hops;      // Path-level: number of edges.
  std::optional<double> min_bandwidth_mbps; // Per edge.
  // Relays a path may cross. nullopt: every relay is allowed.
  std::optional<std::set<std::string>> allowed_relays;
  std::set<std::string> forbidden_nodes;
  netstate::TransportProtocol protocol{netstate::TransportProtocol::Tcp};
  // Budgets for computing a path (not for forwarding).
  std::optional<double> computation_deadline_ms;
  std::optional<std::uint64_t> max_objective_evaluations;
  std::optional<std::uint64_t> memory_budget_bytes;
};

struct ForwardingProblem {
  std::string id{"problem"};
  std::string source;
  std::string destination;
  ForwardingObjective objective;
  ForwardingConstraints constraints;
  // Graph version the problem was posed against; 0: not pinned.
  std::uint64_t graph_version{0};
};

// One edge usable under the per-edge hard constraints, with its cost.
struct UsableEdge {
  int to{-1};
  double cost{0.0};
  double latency_ms{0.0};
};

// The graph reduced to what an algorithm may use for this problem: node
// indices, usable outgoing edges with their objective cost. Every algorithm
// works on this view, so all ten see the same costs.
struct ProblemView {
  std::vector<std::string> node_ids;
  std::vector<std::vector<UsableEdge>> out;
  int source{-1};
  int destination{-1};
  std::size_t usable_edge_count{0};
  bool has_negative_cost{false};
  double min_edge_cost{0.0};
  // Edges left out and why, e.g. "relay_a->dest: unavailable".
  std::vector<std::string> excluded_edges;
};

enum class PathIssue {
  Empty,
  WrongSource,
  WrongDestination,
  UnknownNode,
  NodeUnavailable,
  NodeForbidden,
  RelayNotAllowed,
  NotARelay,
  MissingEdge,
  EdgeUnavailable,
  ProtocolUnsupported,
  BandwidthBelowMinimum,
  AttributeUnknown,
  RepeatedNode,
  GraphVersionMismatch,
  LatencyBudgetExceeded,
  HopLimitExceeded,
};

struct PathValidation {
  // Structurally executable: exists, connects, loop-free, every node and edge
  // usable. A path can be valid but infeasible (it breaks a path-level limit).
  bool valid{false};
  bool feasible{false};
  std::vector<PathIssue> issues;
  std::vector<std::string> details;
  std::optional<double> objective;  // J(P); set whenever valid.
  double latency_ms{0.0};
  double delivery_probability{1.0};
  std::size_t hops{0};

  bool Has(PathIssue issue) const;
};

// Problems with the problem itself; empty when it can be solved.
std::vector<std::string> ValidateProblem(const ForwardingGraph& graph, const ForwardingProblem& problem);

ProblemView BuildView(const ForwardingGraph& graph, const ForwardingProblem& problem);

// Cost of one edge under the objective; nullopt when a weighted attribute is
// unknown (the edge is then unusable, never zero-cost).
std::optional<double> EdgeCost(const ForwardingEdge& edge, const ForwardingObjective& objective);

// The single validator used before any path is executed.
PathValidation ValidatePath(const ForwardingGraph& graph, const ForwardingProblem& problem, const Path& path);

const char* ToString(PathIssue issue);
std::string PathText(const Path& path);

}  // namespace aafr::forwarding
