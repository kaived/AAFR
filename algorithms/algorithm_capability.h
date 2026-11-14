#pragma once

// AlgorithmCapability: what an algorithm can do and what it needs. Plain C++17.
//
// The contract keeps two things apart:
//  - AlgorithmProperties: inherent to the algorithm as published. True of
//    any correct implementation.
//  - ImplementationProfile: facts about AAFR's own implementation. Empty until
//    one exists, and measured rather than assumed once it does.
//
// A capability describes applicability only. It makes no claim about which
// algorithm performs better under any network condition.

#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "algorithms/algorithm_id.h"
#include "network/network_state/execution_environment.h"

namespace aafr::algorithms {

// Bump when a capability field changes meaning.
inline constexpr int kCapabilitySchemaVersion = 1;

enum class AlgorithmFamily {
  ClassicalShortestPath,
  Metaheuristic,
};

enum class ImplementationAvailability {
  ProfileDefined,             // Only this profile exists.
  ImplementationUnavailable,  // Looked for; AAFR has no implementation.
  ImplementationPresent,      // Code exists but is not verified end to end.
  VerifiedExecutable,         // Implementation and forwarding integration verified.
  Unsupported,                // Will not be offered in this build or environment.
};

enum class EdgeWeightRequirement {
  NonNegative,           // Negative weights break the algorithm's guarantee.
  NoNegativeCycle,       // Negative weights allowed; a reachable negative cycle is not.
  DefinedByObjective,    // No edge-weight rule; the objective function scores solutions.
};

enum class OptimalityGuarantee {
  Exact,                   // Returns an optimal answer when its preconditions hold.
  ConditionalOnHeuristic,  // Optimal only with an admissible heuristic.
  None,                    // Returns the best solution found within its budget.
};

struct AlgorithmProperties {
  AlgorithmFamily family{AlgorithmFamily::ClassicalShortestPath};
  std::set<netstate::ProblemRepresentation> problems;
  std::set<netstate::TopologyRepresentation> topologies;
  std::set<netstate::OptimizationObjective> objectives;
  EdgeWeightRequirement edge_weights{EdgeWeightRequirement::NonNegative};
  OptimalityGuarantee optimality{OptimalityGuarantee::None};
  bool requires_heuristic{false};
  bool deterministic{true};
  bool detects_negative_cycles{false};
  // Textbook bounds. For metaheuristics these depend on configured budgets.
  std::string time_complexity;
  std::string space_complexity;
  // NetworkState feature ids that must be usable in the primary window.
  std::vector<std::string> required_features;
  // Configuration parameter names that must be supplied.
  std::vector<std::string> required_configuration;
  // Forwarding operations the result needs in order to take effect; any one
  // of them is enough.
  std::set<netstate::ForwardingOperation> usable_forwarding_operations;
  std::vector<std::string> limitations;
};

// Facts about AAFR's implementation. Every field is empty until measured.
struct ImplementationProfile {
  // Worst execution time measured for the supported topology scale.
  std::optional<double> worst_case_runtime_ms;
  std::optional<std::uint64_t> peak_memory_bytes;
  // Largest topology the measurements above cover.
  std::optional<std::uint64_t> max_nodes;
  std::optional<std::uint64_t> max_edges;
  // Where it runs, e.g. "in_process_cpp". Empty: nowhere.
  std::vector<std::string> execution_environments;
  // What the verification covered, for VerifiedExecutable.
  std::string verification_note;
};

struct AlgorithmCapability {
  AlgorithmId id{AlgorithmId::Dijkstra};
  std::string display_name;
  int schema_version{kCapabilitySchemaVersion};
  ImplementationAvailability availability{ImplementationAvailability::ProfileDefined};
  AlgorithmProperties properties;
  ImplementationProfile implementation;
};

const char* ToString(AlgorithmFamily value);
const char* ToString(ImplementationAvailability value);
const char* ToString(EdgeWeightRequirement value);
const char* ToString(OptimalityGuarantee value);

}  // namespace aafr::algorithms
