#include "routing/forwarding/forwarding_executor.h"

#include <cstdio>
#include <set>

namespace aafr::forwarding {
namespace {

std::uint64_t Mix(std::uint64_t x) {
  x += 0x9E3779B97F4A7C15ull;
  x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
  x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
  return x ^ (x >> 31);
}

bool TokenChar(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
}

}  // namespace

std::vector<std::string> ValidateRequest(const ForwardingRequest& request, std::size_t max_hops) {
  std::vector<std::string> issues;
  if (request.request_id.empty() || request.request_id.size() > 64) issues.push_back("request id must have 1-64 characters");
  for (const char c : request.request_id) {
    if (!TokenChar(c)) {
      issues.push_back("request id may contain only letters, digits, '-' and '_'");
      break;
    }
  }
  if (request.path.size() < 2) issues.push_back("the path needs at least an entry and a destination");
  if (request.path.size() > max_hops + 1) issues.push_back("the path is longer than the executor's hop limit");
  std::set<std::string> seen;
  for (const std::string& node : request.path) {
    if (!seen.insert(node).second) issues.push_back("node '" + node + "' repeats: forwarding loops are refused");
  }
  if (request.payload.size() > ForwardingRequest::kMaxPayloadBytes) issues.push_back("payload too large");
  if (request.payload.find('\n') != std::string::npos || request.payload.find('\r') != std::string::npos) {
    issues.push_back("payload may not contain line breaks");
  }
  if (!(request.timeout_ms > 0.0) || request.timeout_ms > 60000.0) issues.push_back("timeout must be in (0, 60000] ms");
  return issues;
}

ForwardingCapability SimulatedForwardingExecutor::Capability() const {
  ForwardingCapability capability;
  capability.executor = "simulated";
  capability.operations = {netstate::ForwardingOperation::SelectPath};
  capability.protocols = {netstate::TransportProtocol::Tcp, netstate::TransportProtocol::Udp,
                          netstate::TransportProtocol::Quic};
  capability.real_network = false;
  return capability;
}

ForwardingExecutionResult SimulatedForwardingExecutor::Execute(const ForwardingRequest& request,
                                                               const ForwardingExecutionContext& context) {
  ForwardingExecutionResult result;
  result.request_id = request.request_id;
  result.requested_path = request.path;
  result.provenance = Provenance::Simulated;
  const auto issues = ValidateRequest(request, Capability().max_hops);
  if (!issues.empty()) {
    result.status = ForwardingStatus::RequestRejected;
    result.response_status = "REJECTED";
    result.message = issues.front();
    return result;
  }
  if (context.cancel && context.cancel->load()) {
    result.status = ForwardingStatus::Cancelled;
    result.response_status = "CANCELLED";
    return result;
  }
  if (request.graph_version != 0 && request.graph_version != graph_.version()) {
    result.status = ForwardingStatus::RequestRejected;
    result.response_status = "STALE_GRAPH";
    result.message = "the graph changed after the path was computed";
    return result;
  }
  std::uint64_t state = seed_;
  for (const char c : request.request_id) state = Mix(state ^ static_cast<unsigned char>(c));
  const auto uniform = [&state] {
    state = Mix(state);
    return static_cast<double>(state >> 11) * 0x1.0p-53;
  };
  for (std::size_t i = 0; i < request.path.size(); ++i) {
    const std::string& id = request.path[i];
    const ForwardingNode* node = graph_.FindNode(id);
    if (!node || !node->available) {
      result.status = ForwardingStatus::RelayUnavailable;
      result.response_status = "NODE_UNAVAILABLE";
      result.failed_at = id;
      result.message = "simulated: node '" + id + "' is unavailable";
      return result;
    }
    result.confirmed_path.push_back(id);
    if (i + 1 == request.path.size()) break;
    const ForwardingEdge* edge = graph_.FindEdge(id, request.path[i + 1]);
    if (!edge || !edge->available) {
      result.status = ForwardingStatus::Failed;
      result.response_status = "LINK_DOWN";
      result.failed_at = id;
      result.message = "simulated: edge " + id + "->" + request.path[i + 1] + " is unavailable";
      return result;
    }
    result.end_to_end_ms += edge->latency_ms.value.value_or(0.0);
    if (result.end_to_end_ms > request.timeout_ms) {
      result.status = ForwardingStatus::Timeout;
      result.response_status = "TIMEOUT";
      result.failed_at = id;
      return result;
    }
    if (edge->loss_ratio.value && uniform() < *edge->loss_ratio.value) {
      result.status = ForwardingStatus::Lost;
      result.response_status = "LOST";
      result.failed_at = id;
      result.message = "simulated loss on " + id + "->" + request.path[i + 1];
      return result;
    }
  }
  result.status = ForwardingStatus::Delivered;
  result.response_status = "OK";
  result.response_body = "simulated;served-by=" + request.path.back();
  result.path_confirmed = result.confirmed_path == request.path;
  result.evidence = "simulated traversal of the graph (no network traffic)";
  return result;
}

const char* ToString(ForwardingStatus value) {
  switch (value) {
    case ForwardingStatus::Delivered: return "delivered";
    case ForwardingStatus::RequestRejected: return "request_rejected";
    case ForwardingStatus::RelayUnavailable: return "relay_unavailable";
    case ForwardingStatus::Lost: return "lost";
    case ForwardingStatus::Timeout: return "timeout";
    case ForwardingStatus::Cancelled: return "cancelled";
    case ForwardingStatus::Failed: return "failed";
  }
  return "unknown";
}

}  // namespace aafr::forwarding
