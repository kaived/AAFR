#pragma once

// The forwarding execution contract. Plain C++17.
//
// An executor takes a path that already passed forwarding::ValidatePath and
// sends one request along it. It reports the path it can prove was used
// (confirmed_path) separately from the path it was asked to use
// (requested_path). A path counts as active only when path_confirmed is true.
//
// Two executors exist:
//  - SimulatedForwardingExecutor: deterministic, in-process, for tests and
//    benchmarks. Every number it reports is labelled Simulated.
//  - LoopbackForwardingExecutor (loopback_forwarding.h): real TCP requests
//    through relays bound to 127.0.0.1.

#include <atomic>
#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "network/network_state/execution_environment.h"
#include "routing/topology/forwarding_graph.h"
#include "routing/topology/forwarding_problem.h"

namespace aafr::forwarding {

struct ForwardingRequest {
  // Letters, digits, '-' and '_', at most 64 characters.
  std::string request_id;
  Path path;
  std::string payload;  // At most kMaxPayloadBytes, no newline.
  double timeout_ms{2000.0};
  std::uint64_t graph_version{0};

  static constexpr std::size_t kMaxPayloadBytes = 1024;
};

struct ForwardingExecutionContext {
  const std::atomic<bool>* cancel{nullptr};
  std::string execution_id;
  std::string scenario_id;
};

enum class ForwardingStatus {
  Delivered,         // The destination answered and the response came back.
  RequestRejected,   // The executor refused the request before sending it.
  RelayUnavailable,  // A node on the path could not be reached.
  Lost,              // Simulated loss on an edge.
  Timeout,
  Cancelled,
  Failed,            // Any other forwarding error, with a response status.
};

struct ForwardingExecutionResult {
  std::string request_id;
  ForwardingStatus status{ForwardingStatus::Failed};
  Path requested_path;
  // Nodes that demonstrably handled the request, in order.
  Path confirmed_path;
  // True only when the response arrived, the evidence trace equals the
  // requested path and every node's own log records the request.
  bool path_confirmed{false};
  std::string response_status;  // e.g. "OK", "NEXT_HOP_UNAVAILABLE".
  std::string response_body;
  std::string failed_at;        // Node where forwarding stopped, if known.
  double end_to_end_ms{0.0};
  Provenance provenance{Provenance::Unknown};
  std::string evidence;          // How confirmation was established.
  std::string message;
};

struct ForwardingCapability {
  std::string executor;  // e.g. "loopback_tcp", "simulated".
  std::set<netstate::ForwardingOperation> operations;
  std::set<netstate::TransportProtocol> protocols;
  bool real_network{false};
  bool loopback_only{true};
  std::size_t max_hops{16};
};

class IForwardingExecutor {
 public:
  virtual ~IForwardingExecutor() = default;
  virtual ForwardingCapability Capability() const = 0;
  // Whether the executor can run requests now (e.g. relays started).
  virtual bool Ready() const = 0;
  // Blocking; bounded by request.timeout_ms. Safe to call from any thread.
  virtual ForwardingExecutionResult Execute(const ForwardingRequest& request,
                                            const ForwardingExecutionContext& context) = 0;
};

// Checks the request fields every executor relies on; empty when acceptable.
std::vector<std::string> ValidateRequest(const ForwardingRequest& request, std::size_t max_hops);

// Deterministic in-process forwarding over the graph's current state. Edge
// latency is summed (not slept), node and edge availability are honoured,
// and loss is drawn from a generator seeded by (seed, request id).
class SimulatedForwardingExecutor final : public IForwardingExecutor {
 public:
  // The graph must outlive the executor.
  SimulatedForwardingExecutor(const ForwardingGraph& graph, std::uint64_t seed = 1) : graph_(graph), seed_(seed) {}
  ForwardingCapability Capability() const override;
  bool Ready() const override { return true; }
  ForwardingExecutionResult Execute(const ForwardingRequest& request,
                                    const ForwardingExecutionContext& context) override;

 private:
  const ForwardingGraph& graph_;
  std::uint64_t seed_;
};

const char* ToString(ForwardingStatus value);

}  // namespace aafr::forwarding
