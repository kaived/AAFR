#pragma once

// The controlled forwarding graph G = (V, E). Plain C++17.
//
// V holds application-layer endpoints and relays that AAFR itself controls.
// E holds the forwarding transitions those nodes are configured to allow. The
// graph describes nothing else: it is not the Internet's routing topology and
// it is never inferred from packet capture.
//
// Every edge attribute carries its provenance, so a configured or simulated
// number can never be read as a measurement, and an absent number stays
// "unknown" instead of becoming zero.

#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "network/network_state/execution_environment.h"

namespace aafr::forwarding {

// Bump when a graph field changes meaning.
inline constexpr int kGraphSchemaVersion = 1;

enum class Provenance {
  Measured,    // Observed on the forwarding layer itself.
  Configured,  // Set by whoever built the controlled environment.
  Estimated,   // Derived from other values.
  Simulated,   // Produced by the deterministic test environment.
  Unknown,     // Nobody supplied it.
};

enum class NodeRole {
  Entry,        // Where a request enters the controlled forwarding layer.
  Relay,        // Forwards to one of its configured neighbours.
  Destination,  // A controlled endpoint that answers the request.
};

struct Measurement {
  std::optional<double> value;
  Provenance provenance{Provenance::Unknown};

  static Measurement Of(double value, Provenance provenance) { return {value, provenance}; }
};

struct ForwardingNode {
  std::string id;
  NodeRole role{NodeRole::Relay};
  bool available{true};
};

struct ForwardingEdge {
  std::string from;
  std::string to;
  Measurement latency_ms;      // One-way forwarding delay of this transition.
  Measurement loss_ratio;      // Probability (0..1) that a request is lost here.
  Measurement bandwidth_mbps;  // Capacity of the transition.
  // A signed, operator-configured preference added to the edge cost (for
  // example a credit for a preferred relay). It is never a measurement: AAFR
  // does not invent negative latency.
  double policy_cost{0.0};
  std::set<netstate::TransportProtocol> protocols{netstate::TransportProtocol::Tcp};
  bool available{true};
};

class ForwardingGraph {
 public:
  ForwardingGraph() = default;
  explicit ForwardingGraph(std::string id) : id_(std::move(id)) {}

  // Each mutation bumps the version, so a path computed on one version can be
  // recognised as stale on the next.
  void AddNode(ForwardingNode node);
  void AddEdge(ForwardingEdge edge);
  // False when the node or edge does not exist.
  bool SetNodeAvailable(const std::string& id, bool available);
  bool SetEdgeAvailable(const std::string& from, const std::string& to, bool available);
  bool SetEdgeLatency(const std::string& from, const std::string& to, Measurement latency);
  // V0.6: an operator preference change (see ForwardingEdge::policy_cost).
  bool SetEdgePolicyCost(const std::string& from, const std::string& to, double policy_cost);

  const std::string& id() const { return id_; }
  int schema_version() const { return kGraphSchemaVersion; }
  std::uint64_t version() const { return version_; }
  const std::vector<ForwardingNode>& nodes() const { return nodes_; }
  const std::vector<ForwardingEdge>& edges() const { return edges_; }

  const ForwardingNode* FindNode(const std::string& id) const;
  const ForwardingEdge* FindEdge(const std::string& from, const std::string& to) const;
  // Index into nodes(), or -1.
  int IndexOf(const std::string& id) const;

  // Problems with the graph; empty when it is well-formed. Checks ids,
  // duplicates, dangling and self-referencing edges, and attribute ranges
  // (finite; latency, loss and bandwidth nonnegative; loss below 1).
  std::vector<std::string> Validate() const;

 private:
  ForwardingEdge* MutableEdge(const std::string& from, const std::string& to);

  std::string id_{"graph"};
  std::uint64_t version_{1};
  std::vector<ForwardingNode> nodes_;
  std::vector<ForwardingEdge> edges_;
};

const char* ToString(Provenance value);
const char* ToString(NodeRole value);

}  // namespace aafr::forwarding
