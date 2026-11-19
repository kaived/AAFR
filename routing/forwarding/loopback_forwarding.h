#pragma once

// Real application-layer forwarding over loopback TCP. Plain C++17 with the
// platform socket API (Winsock on Windows); no Qt.
//
// Every node of the controlled graph runs a small TCP server bound to
// 127.0.0.1 on an ephemeral port. A request carries its remaining route and
// the trace of nodes that handled it so far:
//
//   AAFR1 REQ <request_id> <budget_ms> <route|-> <trace|->\n<payload>\n
//   AAFR1 RSP <request_id> <status> <trace>\n<body>\n
//
// A node appends itself to the trace, refuses loops and over-long traces,
// forwards only to a configured, available neighbour in the same network
// (never to an arbitrary host), and relays the response back. The
// destination answers. The trace is the execution evidence; each node also
// keeps a bounded log of the request ids it handled, which the executor
// cross-checks before it reports a path as confirmed.
//
// Safety: loopback only; neighbours resolve only to nodes of this network;
// header and payload sizes are bounded; at most kMaxConnections concurrent
// connections per node; every wait has a deadline; Stop() closes all
// sockets and joins all threads.
//
// V0.7.3 byte streams. The same nodes also carry opaque TCP tunnels:
//
//   AAFR1 TUN <conn_id> <budget_ms> <route|-> <trace|-> <host:port>

//   AAFR1 TUNRSP <conn_id> <status> <trace|-> <egress|->

//
// Each node checks the next hop exactly as for a request, appends itself to
// the trace, records the connection id in its log and opens the next hop.
// The Destination node is the egress: it checks the EgressPolicy, resolves
// the target, checks every resolved address against the policy and connects.
// Once TUNRSP OK has travelled back, every node copies bytes in both
// directions without reading them (TLS stays end to end between the browser
// and the real server). Each node keeps a bounded StreamTraversal record per
// tunnel (open/close times, bytes each way, how it ended), which is the
// relay-level evidence that a stream really crossed it. At most kMaxStreams
// tunnels per node; idle tunnels close after stream_idle_timeout_ms.

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "routing/forwarding/egress_policy.h"
#include "routing/forwarding/forwarding_executor.h"
#include "routing/topology/forwarding_graph.h"

namespace aafr::forwarding {

struct LoopbackOptions {
  // Sleep each edge's configured latency inside the relay before forwarding.
  // The delay is then Simulated even though the traffic is real.
  bool inject_edge_latency{false};
  double max_injected_delay_ms{500.0};
  // V0.7.3 tunnels: close after this long without a byte either way, and
  // bound every blocking send.
  double stream_idle_timeout_ms{120'000.0};
  int stream_send_timeout_ms{10'000};
};

// Relay-level evidence for one tunnel at one node.
struct StreamTraversal {
  std::string connection_id;
  std::string node;
  NodeRole role{NodeRole::Relay};
  std::string next_hop;     // Empty at the Destination (the egress).
  std::string status;       // TUNRSP status this node returned or relayed.
  bool open{false};         // Still copying bytes.
  std::int64_t opened_ns{0};  // steady_clock, since its epoch.
  std::int64_t closed_ns{0};
  std::uint64_t bytes_toward_destination{0};
  std::uint64_t bytes_toward_client{0};
  std::string end;          // closed | idle_timeout | interrupted | error | not_established
};

class LoopbackNode;
class LoopbackDirectory;

class LoopbackForwardingNetwork {
 public:
  static constexpr std::size_t kMaxHops = 16;
  static constexpr std::size_t kMaxConnections = 32;
  // V0.7.3: concurrent tunnels per node, in addition to kMaxConnections.
  static constexpr std::size_t kMaxStreams = 64;
  static constexpr std::size_t kStreamLogCapacity = 512;

  // Copies the graph's nodes and edges; later graph changes are applied with
  // SetNodeOnline / SetEdgeEnabled or by rebuilding the network.
  explicit LoopbackForwardingNetwork(const ForwardingGraph& graph, LoopbackOptions options = {});
  ~LoopbackForwardingNetwork();
  LoopbackForwardingNetwork(const LoopbackForwardingNetwork&) = delete;
  LoopbackForwardingNetwork& operator=(const LoopbackForwardingNetwork&) = delete;

  // Starts a server for every available node. False (with a reason) when a
  // socket cannot be opened; nothing is left running then.
  bool Start(std::string* error = nullptr);
  void Stop();
  bool Running() const { return running_; }

  // Takes a node's server down (connections to it then fail for real) or
  // brings it back on a new port.
  bool SetNodeOnline(const std::string& id, bool online);
  bool SetEdgeEnabled(const std::string& from, const std::string& to, bool enabled);
  // V0.6: adopts a newer version of the same graph (same nodes and edges):
  // every edge's availability and configured latency, and the graph version
  // requests are checked against. Node availability is left to
  // SetNodeOnline. False (with a reason) when the graph's node or edge set
  // differs from the one the network was built from; nothing changes then.
  bool SyncWith(const ForwardingGraph& graph, std::string* error = nullptr);

  // 0 when the node is not running.
  std::uint16_t PortOf(const std::string& id) const;
  // Requests handled by a node, and whether its log holds a request id.
  std::uint64_t HandledCount(const std::string& id) const;
  bool NodeLogged(const std::string& id, const std::string& request_id) const;
  std::size_t ActiveConnections() const;
  std::uint64_t graph_version() const { return graph_version_.load(); }
  bool InjectsLatency() const;
  // V0.7.4 controlled-experiment fault injection: the relay at `from` sleeps
  // an extra `delay_ms` before forwarding over from->to, whether or not
  // configured edge latency is injected. The forwarding graph is not told
  // (a hidden impairment that only measurements can reveal); 0 removes it.
  // False when the edge does not exist or the delay is invalid.
  bool SetHiddenEdgeDelay(const std::string& from, const std::string& to, double delay_ms);

  // V0.7.3 tunnels.
  void SetEgressPolicy(const EgressPolicy& policy);
  EgressPolicy egress_policy() const;
  // Every node's record of a connection (empty when no node carried it).
  std::vector<StreamTraversal> StreamTraversals(const std::string& connection_id) const;
  std::size_t ActiveStreams() const;
  std::uint64_t StreamsHandled(const std::string& id) const;

 private:
  std::shared_ptr<LoopbackDirectory> directory_;
  std::map<std::string, std::unique_ptr<LoopbackNode>> nodes_;
  std::map<std::string, bool> offline_initially_;
  std::atomic<std::uint64_t> graph_version_{0};
  bool running_{false};
};

class LoopbackForwardingExecutor final : public IForwardingExecutor, public IStreamForwardingExecutor {
 public:
  // The network must outlive the executor.
  explicit LoopbackForwardingExecutor(LoopbackForwardingNetwork& network) : network_(network) {}
  ForwardingCapability Capability() const override;
  bool Ready() const override { return network_.Running(); }
  ForwardingExecutionResult Execute(const ForwardingRequest& request,
                                    const ForwardingExecutionContext& context) override;
  bool StreamsReady() const override { return network_.Running(); }
  StreamOpenResult OpenStream(const StreamOpenRequest& request, const ForwardingExecutionContext& context) override;
  const LoopbackForwardingNetwork& network() const { return network_; }

 private:
  LoopbackForwardingNetwork& network_;
};

}  // namespace aafr::forwarding
