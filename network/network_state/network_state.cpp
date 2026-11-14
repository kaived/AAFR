#include "network/network_state/network_state.h"

#include "network/network_state/execution_environment.h"

namespace aafr::netstate {

const Feature* WindowSnapshot::Find(const std::string& feature_id) const {
  for (const Feature& feature : features) {
    if (feature.id == feature_id) return &feature;
  }
  return nullptr;
}

const WindowSnapshot* NetworkState::FindWindow(const std::string& name) const {
  for (const WindowSnapshot& window : windows) {
    if (window.name == name) return &window;
  }
  return nullptr;
}

const Feature* NetworkState::FindFeature(const std::string& feature_id) const {
  const WindowSnapshot* window = PrimaryWindow();
  return window ? window->Find(feature_id) : nullptr;
}

double NetworkState::FeatureCompleteness() const {
  const std::uint64_t total = quality.Total();
  return total == 0 ? 0.0 : static_cast<double>(quality.valid) / static_cast<double>(total);
}

const std::vector<std::string>& FeatureIds() {
  static const std::vector<std::string> ids = {
      "rtt.handshake.mean_ms",       "rtt.handshake.median_ms",
      "rtt.handshake.min_ms",        "rtt.handshake.max_ms",
      "rtt.handshake.variation_ms",  "ack_delay.mean_ms",
      "ack_delay.jitter_ms",         "dns.response_time.mean_ms",
      "dns.response_time.max_ms",    "dns.responses",
      "tcp.retransmissions",         "tcp.retransmission_ratio",
      "throughput.mean_mbps",        "throughput.peak_mbps",
      "traffic.packets",             "traffic.bytes",
      "tcp.packets",                 "udp.packets",
      "quic.packets",                "tcp.connection_observations",
      "nav.duration.median_ms",      "nav.duration.mean_ms",
      "nav.succeeded",               "nav.failed",
      "nav.failure_ratio",
  };
  return ids;
}

const char* ToString(FeatureQuality value) {
  switch (value) {
    case FeatureQuality::Valid: return "valid";
    case FeatureQuality::Missing: return "missing";
    case FeatureQuality::Stale: return "stale";
    case FeatureQuality::InsufficientSamples: return "insufficient_samples";
    case FeatureQuality::Invalid: return "invalid";
    case FeatureQuality::Unattributed: return "unattributed";
  }
  return "unknown";
}

const char* ToString(ValueKind value) {
  switch (value) {
    case ValueKind::Measured: return "measured";
    case ValueKind::Derived: return "derived";
    case ValueKind::Configured: return "configured";
    case ValueKind::Unknown: return "unknown";
  }
  return "unknown";
}

const char* ToString(Unit value) {
  switch (value) {
    case Unit::Milliseconds: return "ms";
    case Unit::Mbps: return "Mbps";
    case Unit::Ratio: return "ratio";
    case Unit::Count: return "count";
    case Unit::Bytes: return "bytes";
    case Unit::Percent: return "percent";
  }
  return "unknown";
}

const char* ToString(CaptureStatus value) {
  switch (value) {
    case CaptureStatus::NeverStarted: return "never_started";
    case CaptureStatus::Active: return "active";
    case CaptureStatus::Stopped: return "stopped";
    case CaptureStatus::Error: return "error";
  }
  return "unknown";
}

const char* ToString(NavigationOutcome value) {
  switch (value) {
    case NavigationOutcome::Succeeded: return "succeeded";
    case NavigationOutcome::Failed: return "failed";
    case NavigationOutcome::Stopped: return "stopped";
  }
  return "unknown";
}

const char* ToString(StateValidity value) {
  switch (value) {
    case StateValidity::Valid: return "valid";
    case StateValidity::Degraded: return "degraded";
    case StateValidity::NoData: return "no_data";
  }
  return "unknown";
}

ExecutionEnvironment BrowserDefaultEnvironment() {
  ExecutionEnvironment environment;
  // Known facts about the V0.4 browser, stated rather than left unknown:
  // it has no forwarding topology and no layer that could execute a choice.
  environment.topology = ExecutionEnvironment::Topology{};
  environment.forwarding_operations = std::set<ForwardingOperation>{};
  return environment;
}

const char* ToString(TopologyRepresentation value) {
  switch (value) {
    case TopologyRepresentation::WeightedDirectedGraph: return "weighted_directed_graph";
    case TopologyRepresentation::CandidatePathSet: return "candidate_path_set";
  }
  return "unknown";
}

const char* ToString(ProblemRepresentation value) {
  switch (value) {
    case ProblemRepresentation::SingleSourceShortestPath: return "single_source_shortest_path";
    case ProblemRepresentation::SinglePairShortestPath: return "single_pair_shortest_path";
    case ProblemRepresentation::CandidateSolutionOptimization: return "candidate_solution_optimization";
  }
  return "unknown";
}

const char* ToString(OptimizationObjective value) {
  switch (value) {
    case OptimizationObjective::MinimizeAdditivePathCost: return "minimize_additive_path_cost";
    case OptimizationObjective::MinimizeScalarObjective: return "minimize_scalar_objective";
  }
  return "unknown";
}

const char* ToString(ForwardingOperation value) {
  switch (value) {
    case ForwardingOperation::SelectNextHop: return "select_next_hop";
    case ForwardingOperation::SelectPath: return "select_path";
    case ForwardingOperation::SelectEndpoint: return "select_endpoint";
  }
  return "unknown";
}

const char* ToString(TransportProtocol value) {
  switch (value) {
    case TransportProtocol::Tcp: return "tcp";
    case TransportProtocol::Udp: return "udp";
    case TransportProtocol::Quic: return "quic";
  }
  return "unknown";
}

}  // namespace aafr::netstate
