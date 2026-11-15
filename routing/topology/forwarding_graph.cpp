#include "routing/topology/forwarding_graph.h"

#include <cmath>
#include <map>
#include <utility>

namespace aafr::forwarding {
namespace {

void CheckMeasurement(const Measurement& measurement, const std::string& what, bool below_one,
                      std::vector<std::string>& issues) {
  if (!measurement.value) return;  // Unknown is allowed; it is handled when the edge is scored.
  const double value = *measurement.value;
  if (!std::isfinite(value)) {
    issues.push_back(what + " is not finite");
  } else if (value < 0.0) {
    issues.push_back(what + " is negative");
  } else if (below_one && value >= 1.0) {
    issues.push_back(what + " must be below 1");
  }
  if (measurement.provenance == Provenance::Unknown) {
    issues.push_back(what + " has a value but no provenance");
  }
}

}  // namespace

void ForwardingGraph::AddNode(ForwardingNode node) {
  nodes_.push_back(std::move(node));
  ++version_;
}

void ForwardingGraph::AddEdge(ForwardingEdge edge) {
  edges_.push_back(std::move(edge));
  ++version_;
}

bool ForwardingGraph::SetNodeAvailable(const std::string& id, bool available) {
  for (ForwardingNode& node : nodes_) {
    if (node.id != id) continue;
    if (node.available != available) {
      node.available = available;
      ++version_;
    }
    return true;
  }
  return false;
}

ForwardingEdge* ForwardingGraph::MutableEdge(const std::string& from, const std::string& to) {
  for (ForwardingEdge& edge : edges_) {
    if (edge.from == from && edge.to == to) return &edge;
  }
  return nullptr;
}

bool ForwardingGraph::SetEdgeAvailable(const std::string& from, const std::string& to, bool available) {
  ForwardingEdge* edge = MutableEdge(from, to);
  if (!edge) return false;
  if (edge->available != available) {
    edge->available = available;
    ++version_;
  }
  return true;
}

bool ForwardingGraph::SetEdgeLatency(const std::string& from, const std::string& to, Measurement latency) {
  ForwardingEdge* edge = MutableEdge(from, to);
  if (!edge) return false;
  edge->latency_ms = latency;
  ++version_;
  return true;
}

const ForwardingNode* ForwardingGraph::FindNode(const std::string& id) const {
  const int index = IndexOf(id);
  return index < 0 ? nullptr : &nodes_[static_cast<std::size_t>(index)];
}

const ForwardingEdge* ForwardingGraph::FindEdge(const std::string& from, const std::string& to) const {
  for (const ForwardingEdge& edge : edges_) {
    if (edge.from == from && edge.to == to) return &edge;
  }
  return nullptr;
}

int ForwardingGraph::IndexOf(const std::string& id) const {
  for (std::size_t i = 0; i < nodes_.size(); ++i) {
    if (nodes_[i].id == id) return static_cast<int>(i);
  }
  return -1;
}

std::vector<std::string> ForwardingGraph::Validate() const {
  std::vector<std::string> issues;
  if (nodes_.empty()) issues.push_back("the graph has no nodes");
  std::map<std::string, int> seen;
  for (const ForwardingNode& node : nodes_) {
    if (node.id.empty()) issues.push_back("a node has an empty id");
    // Node ids travel in forwarding headers, so they must be plain tokens.
    for (const char c : node.id) {
      const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                      c == '_' || c == '-' || c == '.';
      if (!ok) {
        issues.push_back("node id '" + node.id + "' has characters other than letters, digits, '_', '-', '.'");
        break;
      }
    }
    if (++seen[node.id] == 2) issues.push_back("node id '" + node.id + "' is used more than once");
  }
  std::map<std::pair<std::string, std::string>, int> edge_seen;
  for (const ForwardingEdge& edge : edges_) {
    const std::string name = "edge " + edge.from + "->" + edge.to;
    if (seen.count(edge.from) == 0) issues.push_back(name + " starts at an unknown node");
    if (seen.count(edge.to) == 0) issues.push_back(name + " ends at an unknown node");
    if (edge.from == edge.to) issues.push_back(name + " is a self-loop");
    if (++edge_seen[{edge.from, edge.to}] == 2) issues.push_back(name + " is defined more than once");
    CheckMeasurement(edge.latency_ms, name + " latency", false, issues);
    CheckMeasurement(edge.loss_ratio, name + " loss ratio", true, issues);
    CheckMeasurement(edge.bandwidth_mbps, name + " bandwidth", false, issues);
    if (edge.bandwidth_mbps.value && *edge.bandwidth_mbps.value == 0.0) {
      issues.push_back(name + " bandwidth is zero");
    }
    if (!std::isfinite(edge.policy_cost)) issues.push_back(name + " policy cost is not finite");
    if (edge.protocols.empty()) issues.push_back(name + " supports no protocol");
  }
  return issues;
}

const char* ToString(Provenance value) {
  switch (value) {
    case Provenance::Measured: return "measured";
    case Provenance::Configured: return "configured";
    case Provenance::Estimated: return "estimated";
    case Provenance::Simulated: return "simulated";
    case Provenance::Unknown: return "unknown";
  }
  return "unknown";
}

const char* ToString(NodeRole value) {
  switch (value) {
    case NodeRole::Entry: return "entry";
    case NodeRole::Relay: return "relay";
    case NodeRole::Destination: return "destination";
  }
  return "unknown";
}

}  // namespace aafr::forwarding
