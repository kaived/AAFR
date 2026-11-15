#include "routing/topology/forwarding_problem.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace aafr::forwarding {
namespace {

std::string Number(double value) {
  char buffer[48];
  std::snprintf(buffer, sizeof(buffer), "%.6g", value);
  return buffer;
}

void Add(PathValidation& v, PathIssue issue, std::string detail) {
  if (!v.Has(issue)) v.issues.push_back(issue);
  v.details.push_back(std::move(detail));
}

// Why an edge cannot be used for this problem; empty when it can.
std::string EdgeExclusion(const ForwardingGraph& graph, const ForwardingProblem& problem,
                          const ForwardingEdge& edge) {
  const ForwardingConstraints& c = problem.constraints;
  if (!edge.available) return "edge unavailable";
  const ForwardingNode* to = graph.FindNode(edge.to);
  const ForwardingNode* from = graph.FindNode(edge.from);
  if (!to || !from) return "unknown node";
  if (!to->available || !from->available) return "node unavailable";
  if (c.forbidden_nodes.count(edge.to) || c.forbidden_nodes.count(edge.from)) return "node forbidden";
  if (edge.protocols.count(c.protocol) == 0) return "protocol unsupported";
  if (c.min_bandwidth_mbps) {
    if (!edge.bandwidth_mbps.value) return "bandwidth unknown";
    if (*edge.bandwidth_mbps.value < *c.min_bandwidth_mbps) return "bandwidth below minimum";
  }
  if (c.latency_budget_ms && !edge.latency_ms.value) return "latency unknown";
  // Only relays may sit between source and destination.
  if (edge.to != problem.destination) {
    if (to->role != NodeRole::Relay) return "next node is not a relay";
    if (c.allowed_relays && c.allowed_relays->count(edge.to) == 0) return "relay not allowed";
  }
  if (!EdgeCost(edge, problem.objective)) return "a weighted attribute is unknown";
  return {};
}

}  // namespace

std::vector<std::string> ForwardingObjective::Validate() const {
  std::vector<std::string> issues;
  const auto check = [&issues](double value, const char* name, bool allow_negative) {
    if (!std::isfinite(value)) issues.push_back(std::string(name) + " is not finite");
    else if (!allow_negative && value < 0.0) issues.push_back(std::string(name) + " is negative");
  };
  check(latency_weight, "latency_weight", false);
  check(loss_weight, "loss_weight", false);
  check(hop_weight, "hop_weight", false);
  check(policy_weight, "policy_weight", true);
  if (latency_weight <= 0.0 && loss_weight <= 0.0 && hop_weight <= 0.0) {
    issues.push_back("at least one of latency_weight, loss_weight, hop_weight must be positive");
  }
  return issues;
}

std::string ForwardingObjective::Describe() const {
  return "J(P)=sum_e[" + Number(latency_weight) + "*latency_ms + " + Number(loss_weight) +
         "*-ln(1-loss) + " + Number(hop_weight) + " + " + Number(policy_weight) + "*policy_cost] (v" +
         std::to_string(kVersion) + ")";
}

bool PathValidation::Has(PathIssue issue) const {
  return std::find(issues.begin(), issues.end(), issue) != issues.end();
}

std::optional<double> EdgeCost(const ForwardingEdge& edge, const ForwardingObjective& objective) {
  double cost = objective.hop_weight + objective.policy_weight * edge.policy_cost;
  if (objective.latency_weight != 0.0) {
    if (!edge.latency_ms.value) return std::nullopt;
    cost += objective.latency_weight * *edge.latency_ms.value;
  }
  if (objective.loss_weight != 0.0) {
    if (!edge.loss_ratio.value) return std::nullopt;
    cost += objective.loss_weight * -std::log1p(-*edge.loss_ratio.value);
  }
  return cost;
}

std::vector<std::string> ValidateProblem(const ForwardingGraph& graph, const ForwardingProblem& problem) {
  std::vector<std::string> issues = graph.Validate();
  for (const std::string& issue : problem.objective.Validate()) issues.push_back("objective: " + issue);
  const ForwardingNode* source = graph.FindNode(problem.source);
  const ForwardingNode* destination = graph.FindNode(problem.destination);
  if (!source) issues.push_back("source '" + problem.source + "' is not in the graph");
  else if (source->role != NodeRole::Entry) issues.push_back("source '" + problem.source + "' is not an entry node");
  if (!destination) {
    issues.push_back("destination '" + problem.destination + "' is not in the graph");
  } else if (destination->role != NodeRole::Destination) {
    issues.push_back("destination '" + problem.destination + "' is not a destination node");
  }
  if (problem.source == problem.destination) issues.push_back("source and destination are the same node");
  if (problem.graph_version != 0 && problem.graph_version != graph.version()) {
    issues.push_back("problem was posed against graph version " + std::to_string(problem.graph_version) +
                     "; the graph is at version " + std::to_string(graph.version()));
  }
  const ForwardingConstraints& c = problem.constraints;
  const auto positive = [&issues](const std::optional<double>& value, const char* name) {
    if (value && (!std::isfinite(*value) || *value <= 0.0)) issues.push_back(std::string(name) + " must be positive");
  };
  positive(c.latency_budget_ms, "latency_budget_ms");
  positive(c.min_bandwidth_mbps, "min_bandwidth_mbps");
  positive(c.computation_deadline_ms, "computation_deadline_ms");
  if (c.max_hops && *c.max_hops == 0) issues.push_back("max_hops must be positive");
  if (c.max_objective_evaluations && *c.max_objective_evaluations == 0) {
    issues.push_back("max_objective_evaluations must be positive");
  }
  if (c.forbidden_nodes.count(problem.source) || c.forbidden_nodes.count(problem.destination)) {
    issues.push_back("source or destination is forbidden");
  }
  return issues;
}

ProblemView BuildView(const ForwardingGraph& graph, const ForwardingProblem& problem) {
  ProblemView view;
  for (const ForwardingNode& node : graph.nodes()) view.node_ids.push_back(node.id);
  view.out.resize(view.node_ids.size());
  view.source = graph.IndexOf(problem.source);
  view.destination = graph.IndexOf(problem.destination);
  bool first = true;
  for (const ForwardingEdge& edge : graph.edges()) {
    const std::string reason = EdgeExclusion(graph, problem, edge);
    if (!reason.empty()) {
      view.excluded_edges.push_back(edge.from + "->" + edge.to + ": " + reason);
      continue;
    }
    const int from = graph.IndexOf(edge.from);
    UsableEdge usable;
    usable.to = graph.IndexOf(edge.to);
    usable.cost = *EdgeCost(edge, problem.objective);
    usable.latency_ms = edge.latency_ms.value.value_or(0.0);
    // Paths never pass through the destination or re-enter the source.
    if (edge.from == problem.destination || edge.to == problem.source) {
      view.excluded_edges.push_back(edge.from + "->" + edge.to + ": leaves the destination or enters the source");
      continue;
    }
    view.has_negative_cost = view.has_negative_cost || usable.cost < 0.0;
    view.min_edge_cost = first ? usable.cost : std::min(view.min_edge_cost, usable.cost);
    first = false;
    view.out[static_cast<std::size_t>(from)].push_back(usable);
    ++view.usable_edge_count;
  }
  // Deterministic neighbour order, independent of insertion order.
  for (auto& edges : view.out) {
    std::sort(edges.begin(), edges.end(), [](const UsableEdge& a, const UsableEdge& b) { return a.to < b.to; });
  }
  return view;
}

PathValidation ValidatePath(const ForwardingGraph& graph, const ForwardingProblem& problem, const Path& path) {
  PathValidation v;
  const ForwardingConstraints& c = problem.constraints;
  if (problem.graph_version != 0 && problem.graph_version != graph.version()) {
    Add(v, PathIssue::GraphVersionMismatch, "the graph changed after the problem was posed");
  }
  if (path.empty()) {
    Add(v, PathIssue::Empty, "the path is empty");
    return v;
  }
  if (path.front() != problem.source) Add(v, PathIssue::WrongSource, "path starts at '" + path.front() + "'");
  if (path.back() != problem.destination) Add(v, PathIssue::WrongDestination, "path ends at '" + path.back() + "'");
  std::set<std::string> seen;
  for (std::size_t i = 0; i < path.size(); ++i) {
    const std::string& id = path[i];
    if (!seen.insert(id).second) Add(v, PathIssue::RepeatedNode, "node '" + id + "' appears twice (loop)");
    const ForwardingNode* node = graph.FindNode(id);
    if (!node) {
      Add(v, PathIssue::UnknownNode, "node '" + id + "' is not in the graph");
      continue;
    }
    if (!node->available) Add(v, PathIssue::NodeUnavailable, "node '" + id + "' is unavailable");
    if (c.forbidden_nodes.count(id)) Add(v, PathIssue::NodeForbidden, "node '" + id + "' is forbidden");
    const bool intermediate = i > 0 && i + 1 < path.size();
    if (intermediate) {
      if (node->role != NodeRole::Relay) Add(v, PathIssue::NotARelay, "node '" + id + "' is not a relay");
      if (c.allowed_relays && c.allowed_relays->count(id) == 0) {
        Add(v, PathIssue::RelayNotAllowed, "relay '" + id + "' is not allowed");
      }
    }
  }
  double objective = 0.0;
  bool cost_known = true;
  for (std::size_t i = 0; i + 1 < path.size(); ++i) {
    const std::string name = path[i] + "->" + path[i + 1];
    const ForwardingEdge* edge = graph.FindEdge(path[i], path[i + 1]);
    if (!edge) {
      Add(v, PathIssue::MissingEdge, "edge " + name + " does not exist");
      cost_known = false;
      continue;
    }
    if (!edge->available) Add(v, PathIssue::EdgeUnavailable, "edge " + name + " is unavailable");
    if (edge->protocols.count(c.protocol) == 0) {
      Add(v, PathIssue::ProtocolUnsupported, "edge " + name + " does not carry " + netstate::ToString(c.protocol));
    }
    if (c.min_bandwidth_mbps &&
        (!edge->bandwidth_mbps.value || *edge->bandwidth_mbps.value < *c.min_bandwidth_mbps)) {
      Add(v, edge->bandwidth_mbps.value ? PathIssue::BandwidthBelowMinimum : PathIssue::AttributeUnknown,
          "edge " + name + " bandwidth is below " + Number(*c.min_bandwidth_mbps) + " Mbps or unknown");
    }
    const auto cost = EdgeCost(*edge, problem.objective);
    if (!cost || (c.latency_budget_ms && !edge->latency_ms.value)) {
      Add(v, PathIssue::AttributeUnknown, "edge " + name + " has an unknown attribute the objective needs");
      cost_known = false;
      continue;
    }
    objective += *cost;
    v.latency_ms += edge->latency_ms.value.value_or(0.0);
    v.delivery_probability *= 1.0 - edge->loss_ratio.value.value_or(0.0);
  }
  v.hops = path.size() - 1;
  v.valid = v.issues.empty();
  if (v.valid && cost_known) v.objective = objective;
  if (v.valid) {
    if (c.latency_budget_ms && v.latency_ms > *c.latency_budget_ms) {
      Add(v, PathIssue::LatencyBudgetExceeded,
          "path latency " + Number(v.latency_ms) + " ms exceeds the budget of " + Number(*c.latency_budget_ms) + " ms");
    }
    if (c.max_hops && v.hops > *c.max_hops) {
      Add(v, PathIssue::HopLimitExceeded,
          "path has " + std::to_string(v.hops) + " hops; the limit is " + std::to_string(*c.max_hops));
    }
    v.feasible = !v.Has(PathIssue::LatencyBudgetExceeded) && !v.Has(PathIssue::HopLimitExceeded);
  }
  return v;
}

const char* ToString(PathIssue issue) {
  switch (issue) {
    case PathIssue::Empty: return "EMPTY_PATH";
    case PathIssue::WrongSource: return "WRONG_SOURCE";
    case PathIssue::WrongDestination: return "WRONG_DESTINATION";
    case PathIssue::UnknownNode: return "UNKNOWN_NODE";
    case PathIssue::NodeUnavailable: return "NODE_UNAVAILABLE";
    case PathIssue::NodeForbidden: return "NODE_FORBIDDEN";
    case PathIssue::RelayNotAllowed: return "RELAY_NOT_ALLOWED";
    case PathIssue::NotARelay: return "NOT_A_RELAY";
    case PathIssue::MissingEdge: return "MISSING_EDGE";
    case PathIssue::EdgeUnavailable: return "EDGE_UNAVAILABLE";
    case PathIssue::ProtocolUnsupported: return "PROTOCOL_UNSUPPORTED";
    case PathIssue::BandwidthBelowMinimum: return "BANDWIDTH_BELOW_MINIMUM";
    case PathIssue::AttributeUnknown: return "ATTRIBUTE_UNKNOWN";
    case PathIssue::RepeatedNode: return "REPEATED_NODE";
    case PathIssue::GraphVersionMismatch: return "GRAPH_VERSION_MISMATCH";
    case PathIssue::LatencyBudgetExceeded: return "LATENCY_BUDGET_EXCEEDED";
    case PathIssue::HopLimitExceeded: return "HOP_LIMIT_EXCEEDED";
  }
  return "UNKNOWN";
}

std::string PathText(const Path& path) {
  std::string text;
  for (const std::string& node : path) {
    if (!text.empty()) text += " -> ";
    text += node;
  }
  return text.empty() ? "(none)" : text;
}

}  // namespace aafr::forwarding
