#include "network/network_state/network_constraints.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace aafr::netstate {
namespace {

std::string NumberText(double value) {
  char buffer[48];
  if (value == std::floor(value) && std::fabs(value) < 1e15) {
    std::snprintf(buffer, sizeof(buffer), "%.0f", value);
  } else {
    std::snprintf(buffer, sizeof(buffer), "%.6g", value);
  }
  return buffer;
}

class Set {
 public:
  explicit Set(NetworkConstraints& out) : out_(out) {}

  void Known(const char* id, ConstraintCategory category, ConstraintValue value, const char* unit,
             ConstraintSource source, std::string explanation,
             std::optional<telemetry::WallClock::time_point> timestamp = std::nullopt) {
    Constraint constraint;
    constraint.id = id;
    constraint.category = category;
    constraint.type = std::holds_alternative<bool>(value)
                          ? ConstraintType::Boolean
                          : std::holds_alternative<double>(value)
                                ? ConstraintType::Number
                                : std::holds_alternative<std::string>(value) ? ConstraintType::Text
                                                                             : ConstraintType::List;
    constraint.value = std::move(value);
    constraint.unit = unit;
    constraint.source = source;
    constraint.timestamp = timestamp;
    constraint.validity = ConstraintValidity::Valid;
    constraint.explanation = std::move(explanation);
    out_.constraints.push_back(std::move(constraint));
  }

  void Unknown(const char* id, ConstraintCategory category, ConstraintType type, const char* unit,
               std::string explanation) {
    Constraint constraint;
    constraint.id = id;
    constraint.category = category;
    constraint.type = type;
    constraint.unit = unit;
    constraint.source = ConstraintSource::Unknown;
    constraint.validity = ConstraintValidity::Unknown;
    constraint.explanation = std::move(explanation);
    out_.constraints.push_back(std::move(constraint));
  }

  // A configured optional: known when set, unknown otherwise.
  template <typename T>
  void Configured(const char* id, ConstraintCategory category, ConstraintType type,
                  const std::optional<T>& value, const char* unit, const char* what) {
    if (value) {
      Known(id, category, ConstraintValue(Convert(*value)), unit, ConstraintSource::Configured,
            std::string(what) + " is configured");
    } else {
      Unknown(id, category, type, unit, std::string(what) + " is not configured");
    }
  }

 private:
  static bool Convert(bool value) { return value; }
  static double Convert(double value) { return value; }
  static double Convert(std::uint64_t value) { return static_cast<double>(value); }

  NetworkConstraints& out_;
};

Constraint* FindMutable(NetworkConstraints& constraints, const char* id) {
  for (Constraint& constraint : constraints.constraints) {
    if (constraint.id == id) return &constraint;
  }
  return nullptr;
}

void Invalidate(NetworkConstraints& constraints, std::initializer_list<const char*> ids,
                const std::string& conflict) {
  constraints.conflicts.push_back(conflict);
  for (const char* id : ids) {
    if (Constraint* constraint = FindMutable(constraints, id)) {
      constraint->validity = ConstraintValidity::Invalid;
      constraint->explanation += " Conflict: " + conflict + ".";
    }
  }
}

// Contradictions make the affected constraints Invalid so that no consumer
// can read them as values.
void Validate(NetworkConstraints& c) {
  const auto positive = [&c](const char* id, const char* what) {
    const auto value = c.Number(id);
    if (value && (!std::isfinite(*value) || *value <= 0.0)) {
      Invalidate(c, {id}, std::string(what) + " must be a positive finite number");
    }
  };
  positive(constraint_id::kComputationDeadlineMs, "the computation deadline");
  positive(constraint_id::kLatencyBudgetMs, "the latency budget");
  positive(constraint_id::kMemoryBudgetBytes, "the memory budget");
  positive(constraint_id::kCpuBudgetPercent, "the CPU budget");
  if (const auto cpu = c.Number(constraint_id::kCpuBudgetPercent); cpu && *cpu > 100.0) {
    Invalidate(c, {constraint_id::kCpuBudgetPercent}, "the CPU budget exceeds 100 percent");
  }
  if (const auto ratio = c.Number(constraint_id::kMaxRetransmissionRatio);
      ratio && (!std::isfinite(*ratio) || *ratio < 0.0 || *ratio > 1.0)) {
    Invalidate(c, {constraint_id::kMaxRetransmissionRatio, constraint_id::kReliabilityWithinLimit},
               "the retransmission-ratio limit is outside 0..1");
  }
  const auto deadline = c.Number(constraint_id::kComputationDeadlineMs);
  const auto latency = c.Number(constraint_id::kLatencyBudgetMs);
  if (deadline && latency && *deadline > *latency) {
    Invalidate(c, {constraint_id::kComputationDeadlineMs, constraint_id::kLatencyBudgetMs},
               "the computation deadline is longer than the whole latency budget");
  }

  const auto available = c.Bool(constraint_id::kTopologyAvailable);
  const auto representations = c.List(constraint_id::kTopologyRepresentations);
  const auto nodes = c.Number(constraint_id::kTopologyNodeCount);
  const auto paths = c.Number(constraint_id::kCandidatePathCount);
  const auto has = [&representations](TopologyRepresentation wanted) {
    return representations &&
           std::find(representations->begin(), representations->end(), ToString(wanted)) !=
               representations->end();
  };
  if (available && *available) {
    if (representations && representations->empty()) {
      Invalidate(c, {constraint_id::kTopologyAvailable, constraint_id::kTopologyRepresentations},
                 "a topology is declared available but has no representation");
    }
    if (has(TopologyRepresentation::WeightedDirectedGraph) && nodes && *nodes == 0.0) {
      Invalidate(c, {constraint_id::kTopologyRepresentations, constraint_id::kTopologyNodeCount},
                 "a graph topology is declared but it has no nodes");
    }
    if (has(TopologyRepresentation::CandidatePathSet) && paths && *paths == 0.0) {
      Invalidate(c, {constraint_id::kTopologyRepresentations, constraint_id::kCandidatePathCount},
                 "a candidate path set is declared but it is empty");
    }
  }
  const auto negative = c.Bool(constraint_id::kEdgeWeightsNegative);
  const auto cycle = c.Bool(constraint_id::kNegativeCycle);
  if (negative && cycle && *cycle && !*negative) {
    Invalidate(c, {constraint_id::kEdgeWeightsNegative, constraint_id::kNegativeCycle},
               "a negative cycle is reported although no edge weight is negative");
  }
  const auto reachable = c.Bool(constraint_id::kReachable);
  if (reachable && !*reachable && paths && *paths > 0.0) {
    Invalidate(c, {constraint_id::kReachable, constraint_id::kCandidatePathCount},
               "candidate paths are listed although the destination is declared unreachable");
  }
}

}  // namespace

const Constraint* NetworkConstraints::Find(const std::string& id) const {
  for (const Constraint& constraint : constraints) {
    if (constraint.id == id) return &constraint;
  }
  return nullptr;
}

std::optional<bool> NetworkConstraints::Bool(const std::string& id) const {
  const Constraint* constraint = Find(id);
  if (!constraint || !constraint->Known()) return std::nullopt;
  if (const bool* value = std::get_if<bool>(&constraint->value)) return *value;
  return std::nullopt;
}

std::optional<double> NetworkConstraints::Number(const std::string& id) const {
  const Constraint* constraint = Find(id);
  if (!constraint || !constraint->Known()) return std::nullopt;
  if (const double* value = std::get_if<double>(&constraint->value)) return *value;
  return std::nullopt;
}

std::optional<std::string> NetworkConstraints::Text(const std::string& id) const {
  const Constraint* constraint = Find(id);
  if (!constraint || !constraint->Known()) return std::nullopt;
  if (const std::string* value = std::get_if<std::string>(&constraint->value)) return *value;
  return std::nullopt;
}

std::optional<std::vector<std::string>> NetworkConstraints::List(const std::string& id) const {
  const Constraint* constraint = Find(id);
  if (!constraint || !constraint->Known()) return std::nullopt;
  if (const auto* value = std::get_if<std::vector<std::string>>(&constraint->value)) return *value;
  return std::nullopt;
}

std::uint64_t NetworkConstraints::CountBySource(ConstraintSource source) const {
  return static_cast<std::uint64_t>(
      std::count_if(constraints.begin(), constraints.end(),
                    [source](const Constraint& c) { return c.source == source; }));
}

std::uint64_t NetworkConstraints::CountByValidity(ConstraintValidity validity) const {
  return static_cast<std::uint64_t>(
      std::count_if(constraints.begin(), constraints.end(),
                    [validity](const Constraint& c) { return c.validity == validity; }));
}

NetworkConstraints ConstraintEvaluator::Evaluate(const NetworkState& state,
                                                 const ExecutionEnvironment& environment) const {
  using C = ConstraintCategory;
  using T = ConstraintType;
  using S = ConstraintSource;
  namespace id = constraint_id;

  NetworkConstraints out;
  out.network_state_id = state.state_id;
  Set set(out);

  // ---- Network: forwarding topology ----
  const char* const kNoTopologyIds[] = {id::kTopologyRepresentations, id::kTopologyNodeCount,
                                        id::kTopologyEdgeCount,       id::kCandidatePathCount,
                                        id::kReachable,               id::kTopologyStable,
                                        id::kEdgeWeightsFinite,       id::kEdgeWeightsNegative,
                                        id::kNegativeCycle,           id::kHeuristicAvailable};
  const auto type_of = [](const char* constraint) {
    const std::string name = constraint;
    if (name == id::kTopologyRepresentations) return T::List;
    if (name == id::kTopologyNodeCount || name == id::kTopologyEdgeCount ||
        name == id::kCandidatePathCount) {
      return T::Number;
    }
    return T::Boolean;
  };
  if (!environment.topology) {
    set.Unknown(id::kTopologyAvailable, C::Network, T::Boolean, "",
                "the environment does not say whether a forwarding topology exists");
    for (const char* constraint : kNoTopologyIds) {
      set.Unknown(constraint, C::Network, type_of(constraint), "", "no topology information");
    }
  } else if (!environment.topology->available) {
    set.Known(id::kTopologyAvailable, C::Network, false, "", S::Configured,
              "the environment declares that no forwarding topology exists");
    for (const char* constraint : kNoTopologyIds) {
      set.Unknown(constraint, C::Network, type_of(constraint), "",
                  "not applicable: no forwarding topology exists");
    }
  } else {
    const TopologyInfo& info = environment.topology->info;
    set.Known(id::kTopologyAvailable, C::Network, true, "", S::Configured,
              "the environment supplies a forwarding topology");
    std::vector<std::string> representations;
    for (const TopologyRepresentation representation : info.representations) {
      representations.push_back(ToString(representation));
    }
    set.Known(id::kTopologyRepresentations, C::Network, representations, "", S::Configured,
              "representations the topology is available in");
    set.Known(id::kTopologyNodeCount, C::Network, static_cast<double>(info.node_count), "nodes",
              S::Configured, "nodes in the topology");
    set.Known(id::kTopologyEdgeCount, C::Network, static_cast<double>(info.edge_count), "edges",
              S::Configured, "edges in the topology");
    set.Known(id::kCandidatePathCount, C::Network, static_cast<double>(info.candidate_path_count),
              "paths", S::Configured, "candidate paths or endpoints");
    set.Configured(id::kReachable, C::Network, T::Boolean, info.reachable, "", "destination reachability");
    set.Configured(id::kTopologyStable, C::Network, T::Boolean, info.stable, "", "topology stability");
    if (info.edge_weights) {
      set.Known(id::kEdgeWeightsFinite, C::Network, info.edge_weights->all_finite, "", S::Configured,
                "whether every edge weight is a finite number");
      set.Known(id::kEdgeWeightsNegative, C::Network, info.edge_weights->has_negative, "",
                S::Configured, "whether any edge weight is negative");
      set.Configured(id::kNegativeCycle, C::Network, T::Boolean,
                     info.edge_weights->has_negative_cycle, "", "negative-cycle check result");
    } else {
      for (const char* constraint :
           {id::kEdgeWeightsFinite, id::kEdgeWeightsNegative, id::kNegativeCycle}) {
        set.Unknown(constraint, C::Network, T::Boolean, "", "edge weights are not described");
      }
    }
    set.Configured(id::kHeuristicAvailable, C::Network, T::Boolean, info.heuristic_available, "",
                   "admissible heuristic availability");
  }

  // ---- Network: protocols, restrictions, measurements ----
  if (environment.supported_protocols) {
    std::vector<std::string> protocols;
    for (const TransportProtocol protocol : *environment.supported_protocols) {
      protocols.push_back(ToString(protocol));
    }
    set.Known(id::kSupportedProtocols, C::Network, protocols, "", S::Configured,
              "protocols the forwarding layer supports");
  } else {
    set.Unknown(id::kSupportedProtocols, C::Network, T::List, "",
                "supported protocols are not configured");
  }
  if (environment.required_protocols.empty()) {
    set.Known(id::kRequiredProtocolsSupported, C::Network, true, "", S::Derived,
              "the forwarding problem names no required protocol");
  } else if (!environment.supported_protocols) {
    set.Unknown(id::kRequiredProtocolsSupported, C::Network, T::Boolean, "",
                "protocols are required but the supported set is unknown");
  } else {
    std::string missing;
    for (const TransportProtocol protocol : environment.required_protocols) {
      if (environment.supported_protocols->count(protocol) == 0) {
        if (!missing.empty()) missing += ", ";
        missing += ToString(protocol);
      }
    }
    set.Known(id::kRequiredProtocolsSupported, C::Network, missing.empty(), "", S::Derived,
              missing.empty() ? "every required protocol is supported"
                              : "unsupported required protocols: " + missing);
  }
  set.Known(id::kForwardingRestrictions, C::Network, environment.forwarding_restrictions, "",
            S::Configured, "configured forwarding restrictions");
  {
    std::vector<std::string> usable;
    if (const WindowSnapshot* primary = state.PrimaryWindow()) {
      for (const Feature& feature : primary->features) {
        if (feature.Usable()) usable.push_back(feature.id);
      }
    }
    set.Known(id::kAvailableMeasurements, C::Network, usable, "", S::Measured,
              "features usable in the primary window", state.observed_at);
  }

  // ---- Performance ----
  set.Configured(id::kComputationDeadlineMs, C::Performance, T::Number,
                 environment.computation_deadline_ms, "ms", "the computation deadline");
  set.Configured(id::kLatencyBudgetMs, C::Performance, T::Number, environment.latency_budget_ms, "ms",
                 "the latency budget");
  set.Configured(id::kMaxRetransmissionRatio, C::Performance, T::Number,
                 environment.max_retransmission_ratio, "ratio", "the retransmission-ratio limit");
  {
    const Feature* ratio = state.FindFeature("tcp.retransmission_ratio");
    if (!environment.max_retransmission_ratio) {
      set.Unknown(id::kReliabilityWithinLimit, C::Performance, T::Boolean, "",
                  "no retransmission-ratio limit is configured");
    } else if (!ratio || !ratio->Usable()) {
      set.Unknown(id::kReliabilityWithinLimit, C::Performance, T::Boolean, "",
                  std::string("tcp.retransmission_ratio is ") +
                      (ratio ? ToString(ratio->quality) : "not part of the state"));
    } else {
      const bool within = *ratio->value <= *environment.max_retransmission_ratio;
      set.Known(id::kReliabilityWithinLimit, C::Performance, within, "", S::Derived,
                "measured retransmission ratio " + NumberText(*ratio->value) + " against limit " +
                    NumberText(*environment.max_retransmission_ratio) +
                    " (a retransmission indicator, not a packet-loss rate)",
                state.observed_at);
    }
  }

  // ---- Execution ----
  set.Configured(id::kMemoryBudgetBytes, C::Execution, T::Number, environment.memory_budget_bytes,
                 "bytes", "the memory budget");
  set.Configured(id::kCpuBudgetPercent, C::Execution, T::Number, environment.cpu_budget_percent,
                 "percent", "the CPU budget");
  if (environment.forwarding_operations) {
    std::vector<std::string> operations;
    for (const ForwardingOperation operation : *environment.forwarding_operations) {
      operations.push_back(ToString(operation));
    }
    set.Known(id::kForwardingOperations, C::Execution, operations, "", S::Configured,
              operations.empty() ? "the environment declares that no forwarding operation can be executed"
                                 : "forwarding operations the environment can execute");
  } else {
    set.Unknown(id::kForwardingOperations, C::Execution, T::List, "",
                "supported forwarding operations are not configured");
  }
  if (environment.problem) {
    set.Known(id::kProblem, C::Execution, std::string(ToString(*environment.problem)), "",
              S::Configured, "the forwarding problem to solve");
  } else {
    set.Unknown(id::kProblem, C::Execution, T::Text, "", "no forwarding problem is defined");
  }
  if (environment.objective) {
    set.Known(id::kObjective, C::Execution, std::string(ToString(*environment.objective)), "",
              S::Configured, "the optimization objective");
  } else {
    set.Unknown(id::kObjective, C::Execution, T::Text, "", "no optimization objective is defined");
  }

  // ---- Operational ----
  {
    const bool capture = state.sources.capture == CaptureStatus::Active;
    const bool any = capture || state.sources.browser_events;
    set.Known(id::kMonitoringAvailable, C::Operational, any, "", S::Measured,
              std::string("packet capture is ") + ToString(state.sources.capture) + "; browser events " +
                  (state.sources.browser_events ? "observed" : "not observed"),
              state.observed_at);
    const bool interrupted = state.sources.capture == CaptureStatus::Stopped ||
                             state.sources.capture == CaptureStatus::Error;
    set.Known(id::kCaptureInterrupted, C::Operational, interrupted, "", S::Measured,
              std::string("packet capture is ") + ToString(state.sources.capture), state.observed_at);
    if (state.freshness_ms) {
      set.Known(id::kStateFresh, C::Operational, *state.freshness_ms <= policy_.max_state_age_ms, "",
                S::Derived,
                "newest measurement is " + NumberText(*state.freshness_ms) + " ms old (limit " +
                    NumberText(policy_.max_state_age_ms) + " ms)",
                state.observed_at);
    } else {
      set.Known(id::kStateFresh, C::Operational, false, "", S::Derived,
                "no measurement has been accepted, so the state cannot be fresh", state.observed_at);
    }
    set.Known(id::kStateValidity, C::Operational, std::string(ToString(state.validity)), "",
              S::Derived, state.validity_reason, state.observed_at);
  }

  Validate(out);
  return out;
}

const char* ToString(ConstraintCategory value) {
  switch (value) {
    case ConstraintCategory::Network: return "network";
    case ConstraintCategory::Performance: return "performance";
    case ConstraintCategory::Execution: return "execution";
    case ConstraintCategory::Operational: return "operational";
  }
  return "unknown";
}

const char* ToString(ConstraintSource value) {
  switch (value) {
    case ConstraintSource::Measured: return "measured";
    case ConstraintSource::Configured: return "configured";
    case ConstraintSource::Derived: return "derived";
    case ConstraintSource::Unknown: return "unknown";
  }
  return "unknown";
}

const char* ToString(ConstraintValidity value) {
  switch (value) {
    case ConstraintValidity::Valid: return "valid";
    case ConstraintValidity::Invalid: return "invalid";
    case ConstraintValidity::Unknown: return "unknown";
  }
  return "unknown";
}

const char* ToString(ConstraintType value) {
  switch (value) {
    case ConstraintType::Boolean: return "boolean";
    case ConstraintType::Number: return "number";
    case ConstraintType::Text: return "text";
    case ConstraintType::List: return "list";
  }
  return "unknown";
}

std::string ValueText(const Constraint& constraint) {
  struct Visitor {
    std::string operator()(std::monostate) const { return "unknown"; }
    std::string operator()(bool value) const { return value ? "true" : "false"; }
    std::string operator()(double value) const { return NumberText(value); }
    std::string operator()(const std::string& value) const { return value; }
    std::string operator()(const std::vector<std::string>& value) const {
      if (value.empty()) return "(none)";
      std::string text;
      for (const std::string& item : value) {
        if (!text.empty()) text += ", ";
        text += item;
      }
      return text;
    }
  };
  return std::visit(Visitor{}, constraint.value);
}

}  // namespace aafr::netstate
