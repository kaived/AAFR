#include "algorithms/capability_registry.h"

#include <algorithm>
#include <cmath>

#include "network/network_state/network_state.h"

namespace aafr::algorithms {
namespace {

using netstate::ForwardingOperation;
using netstate::OptimizationObjective;
using netstate::ProblemRepresentation;
using netstate::TopologyRepresentation;

// V0.4 has no forwarding-problem definition yet, so the only input every
// profile can honestly name is latency evidence for weighting or scoring
// paths. V0.5 replaces this with per-path requirements.
const char* const kLatencyEvidence = "rtt.handshake.mean_ms";
const char* const kProvisionalInputNote =
    "Required NetworkState inputs are provisional until V0.5 defines the forwarding problem; "
    "host-wide latency evidence is not a per-path edge weight.";

AlgorithmCapability Classical(AlgorithmId id, const char* name) {
  AlgorithmCapability capability;
  capability.id = id;
  capability.display_name = name;
  capability.availability = ImplementationAvailability::ImplementationUnavailable;
  AlgorithmProperties& p = capability.properties;
  p.family = AlgorithmFamily::ClassicalShortestPath;
  p.topologies = {TopologyRepresentation::WeightedDirectedGraph};
  p.objectives = {OptimizationObjective::MinimizeAdditivePathCost};
  p.deterministic = true;
  p.required_features = {kLatencyEvidence};
  p.usable_forwarding_operations = {ForwardingOperation::SelectNextHop, ForwardingOperation::SelectPath};
  p.limitations = {kProvisionalInputNote};
  return capability;
}

AlgorithmCapability Metaheuristic(AlgorithmId id, const char* name,
                                  std::set<TopologyRepresentation> topologies,
                                  std::vector<std::string> configuration, const char* time,
                                  const char* space, std::vector<std::string> limitations) {
  AlgorithmCapability capability;
  capability.id = id;
  capability.display_name = name;
  capability.availability = ImplementationAvailability::ImplementationUnavailable;
  AlgorithmProperties& p = capability.properties;
  p.family = AlgorithmFamily::Metaheuristic;
  p.problems = {ProblemRepresentation::CandidateSolutionOptimization};
  p.topologies = std::move(topologies);
  p.objectives = {OptimizationObjective::MinimizeScalarObjective,
                  OptimizationObjective::MinimizeAdditivePathCost};
  p.edge_weights = EdgeWeightRequirement::DefinedByObjective;
  p.optimality = OptimalityGuarantee::None;
  p.deterministic = false;
  p.time_complexity = time;
  p.space_complexity = space;
  p.required_features = {kLatencyEvidence};
  // Every metaheuristic needs a solution encoding, an objective function,
  // an iteration budget and a seed for reproducible runs.
  p.required_configuration = {"solution_encoding", "objective_function", "max_iterations", "random_seed"};
  p.required_configuration.insert(p.required_configuration.end(), configuration.begin(),
                                  configuration.end());
  p.usable_forwarding_operations = {ForwardingOperation::SelectNextHop, ForwardingOperation::SelectPath,
                                    ForwardingOperation::SelectEndpoint};
  p.limitations = {
      "Stochastic: results vary between runs unless the random seed is fixed.",
      "No optimality guarantee; solution quality depends on the configured budget.",
      "Applicability depends on the forwarding problem and on the implementation; no claim is "
      "made about performance under latency, loss or congestion.",
      kProvisionalInputNote,
  };
  p.limitations.insert(p.limitations.end(), limitations.begin(), limitations.end());
  return capability;
}

const char* const kContinuousOrigin =
    "Formulated for continuous search spaces; path selection needs a discrete encoding of "
    "candidate paths.";

std::vector<std::string> MissingVerificationEvidence(const AlgorithmCapability& capability) {
  std::vector<std::string> missing;
  const ImplementationProfile& implementation = capability.implementation;
  if (!implementation.worst_case_runtime_ms) missing.push_back("measured worst-case runtime");
  if (!implementation.peak_memory_bytes) missing.push_back("measured peak memory");
  if (implementation.execution_environments.empty()) missing.push_back("an execution environment");
  if (implementation.verification_note.empty()) missing.push_back("a verification note");
  return missing;
}

bool Fail(std::vector<std::string>* errors, std::vector<std::string> reasons) {
  if (errors) *errors = std::move(reasons);
  return false;
}

}  // namespace

std::vector<std::string> ValidateProfile(const AlgorithmCapability& capability) {
  std::vector<std::string> issues;
  const AlgorithmProperties& p = capability.properties;
  if (capability.display_name.empty()) issues.push_back("display name is empty");
  if (capability.schema_version != kCapabilitySchemaVersion) {
    issues.push_back("capability schema version is not " + std::to_string(kCapabilitySchemaVersion));
  }
  if (p.problems.empty()) issues.push_back("no supported problem representation");
  if (p.topologies.empty()) issues.push_back("no supported topology representation");
  if (p.objectives.empty()) issues.push_back("no supported optimization objective");
  if (p.usable_forwarding_operations.empty()) issues.push_back("no usable forwarding operation");
  if (p.time_complexity.empty()) issues.push_back("time complexity is not stated");
  if (p.space_complexity.empty()) issues.push_back("space complexity is not stated");
  const auto needs = [&p](const char* key) {
    return std::find(p.required_configuration.begin(), p.required_configuration.end(), key) !=
           p.required_configuration.end();
  };
  if (p.requires_heuristic && !needs("heuristic")) {
    issues.push_back("a heuristic is required but not listed as required configuration");
  }
  if (p.optimality == OptimalityGuarantee::ConditionalOnHeuristic && !p.requires_heuristic) {
    issues.push_back("optimality depends on a heuristic that is not required");
  }
  if (p.family == AlgorithmFamily::Metaheuristic && p.optimality != OptimalityGuarantee::None) {
    issues.push_back("a metaheuristic cannot claim an optimality guarantee");
  }
  const auto& known_features = netstate::FeatureIds();
  for (const std::string& feature : p.required_features) {
    if (std::find(known_features.begin(), known_features.end(), feature) == known_features.end()) {
      issues.push_back("required feature '" + feature + "' is not a NetworkState feature");
    }
  }
  const ImplementationProfile& implementation = capability.implementation;
  if (implementation.worst_case_runtime_ms &&
      (!std::isfinite(*implementation.worst_case_runtime_ms) ||
       *implementation.worst_case_runtime_ms <= 0.0)) {
    issues.push_back("measured runtime must be a positive finite number");
  }
  if (implementation.peak_memory_bytes && *implementation.peak_memory_bytes == 0) {
    issues.push_back("measured peak memory must be positive");
  }
  if (capability.availability == ImplementationAvailability::VerifiedExecutable) {
    for (const std::string& missing : MissingVerificationEvidence(capability)) {
      issues.push_back("verified-executable claim lacks " + missing);
    }
  }
  return issues;
}

CapabilityRegistry CapabilityRegistry::BuiltIn() {
  using T = TopologyRepresentation;
  CapabilityRegistry registry;

  {
    AlgorithmCapability c = Classical(AlgorithmId::Dijkstra, "Dijkstra");
    c.properties.problems = {ProblemRepresentation::SingleSourceShortestPath,
                             ProblemRepresentation::SinglePairShortestPath};
    c.properties.edge_weights = EdgeWeightRequirement::NonNegative;
    c.properties.optimality = OptimalityGuarantee::Exact;
    c.properties.time_complexity = "O((V + E) log V) with a binary heap";
    c.properties.space_complexity = "O(V)";
    c.properties.limitations.push_back(
        "The standard formulation is incorrect with negative edge weights.");
    registry.Register(std::move(c));
  }
  {
    AlgorithmCapability c = Classical(AlgorithmId::AStar, "A*");
    c.properties.problems = {ProblemRepresentation::SinglePairShortestPath};
    c.properties.edge_weights = EdgeWeightRequirement::NonNegative;
    c.properties.optimality = OptimalityGuarantee::ConditionalOnHeuristic;
    c.properties.requires_heuristic = true;
    c.properties.required_configuration = {"heuristic"};
    c.properties.time_complexity =
        "O((V + E) log V) worst case with a consistent heuristic; fewer expansions with a better one";
    c.properties.space_complexity = "O(V)";
    c.properties.limitations.push_back(
        "Returns an optimal path only if the heuristic never overestimates the remaining cost.");
    c.properties.limitations.push_back("Needs a destination: it does not compute all shortest paths.");
    registry.Register(std::move(c));
  }
  {
    AlgorithmCapability c = Classical(AlgorithmId::BellmanFord, "Bellman-Ford");
    c.properties.problems = {ProblemRepresentation::SingleSourceShortestPath,
                             ProblemRepresentation::SinglePairShortestPath};
    c.properties.edge_weights = EdgeWeightRequirement::NoNegativeCycle;
    c.properties.optimality = OptimalityGuarantee::Exact;
    c.properties.detects_negative_cycles = true;
    c.properties.time_complexity = "O(V * E)";
    c.properties.space_complexity = "O(V)";
    c.properties.limitations.push_back(
        "Shortest paths are undefined when a negative-weight cycle is reachable; it reports the cycle.");
    c.properties.limitations.push_back("Slower than Dijkstra on graphs without negative weights.");
    registry.Register(std::move(c));
  }
  registry.Register(Metaheuristic(
      AlgorithmId::AntColonyOptimization, "Ant Colony Optimization (ACO)",
      {T::WeightedDirectedGraph, T::CandidatePathSet},
      {"ant_count", "evaporation_rate", "pheromone_weight", "heuristic_weight"},
      "O(iterations * ants * path-construction cost)", "O(E) pheromone values plus O(ants * path length)",
      {"Constructs paths on a graph; convergence depends on pheromone parameters."}));
  registry.Register(Metaheuristic(
      AlgorithmId::ParticleSwarmOptimization, "Particle Swarm Optimization (PSO)", {T::CandidatePathSet},
      {"swarm_size", "inertia_weight", "cognitive_coefficient", "social_coefficient"},
      "O(iterations * particles * objective-evaluation cost)", "O(particles * dimensions)",
      {kContinuousOrigin}));
  registry.Register(Metaheuristic(
      AlgorithmId::GeneticAlgorithm, "Genetic Algorithm (GA)",
      {T::WeightedDirectedGraph, T::CandidatePathSet},
      {"population_size", "crossover_rate", "mutation_rate", "selection_method"},
      "O(generations * population * objective-evaluation cost)", "O(population * chromosome length)",
      {"Crossover and mutation must keep encoded paths valid, or a repair step is needed."}));
  registry.Register(Metaheuristic(
      AlgorithmId::ArtificialBeeColony, "Artificial Bee Colony (ABC)", {T::CandidatePathSet},
      {"colony_size", "abandonment_limit"},
      "O(iterations * colony size * objective-evaluation cost)", "O(colony size * dimensions)",
      {kContinuousOrigin}));
  registry.Register(Metaheuristic(
      AlgorithmId::GreyWolfOptimizer, "Grey Wolf Optimizer (GWO)", {T::CandidatePathSet},
      {"pack_size"}, "O(iterations * wolves * objective-evaluation cost)", "O(wolves * dimensions)",
      {kContinuousOrigin}));
  registry.Register(Metaheuristic(
      AlgorithmId::FireflyAlgorithm, "Firefly Algorithm", {T::CandidatePathSet},
      {"firefly_count", "attractiveness", "absorption_coefficient", "randomization"},
      "O(iterations * fireflies^2 * objective-evaluation cost)", "O(fireflies * dimensions)",
      {kContinuousOrigin, "The standard form compares every pair of fireflies each iteration."}));
  registry.Register(Metaheuristic(
      AlgorithmId::WhaleOptimizationAlgorithm, "Whale Optimization Algorithm (WOA)",
      {T::CandidatePathSet}, {"whale_count", "spiral_constant"},
      "O(iterations * whales * objective-evaluation cost)", "O(whales * dimensions)",
      {kContinuousOrigin}));
  return registry;
}

bool CapabilityRegistry::Register(AlgorithmCapability capability, std::vector<std::string>* errors) {
  std::vector<std::string> issues = ValidateProfile(capability);
  if (profiles_.count(capability.id) != 0) {
    issues.push_back(std::string("algorithm '") + ToString(capability.id) + "' is already registered");
  }
  if (!issues.empty()) return Fail(errors, std::move(issues));
  const AlgorithmId id = capability.id;
  profiles_.emplace(id, std::move(capability));
  return true;
}

const AlgorithmCapability* CapabilityRegistry::Find(AlgorithmId id) const {
  const auto found = profiles_.find(id);
  return found == profiles_.end() ? nullptr : &found->second;
}

const AlgorithmCapability* CapabilityRegistry::Find(const std::string& id) const {
  const auto parsed = ParseAlgorithmId(id);
  return parsed ? Find(*parsed) : nullptr;
}

std::vector<AlgorithmCapability> CapabilityRegistry::All() const {
  std::vector<AlgorithmCapability> all;
  all.reserve(profiles_.size());
  for (const auto& entry : profiles_) all.push_back(entry.second);
  return all;
}

bool CapabilityRegistry::SetAvailability(AlgorithmId id, ImplementationAvailability availability,
                                         std::vector<std::string>* errors) {
  const auto found = profiles_.find(id);
  if (found == profiles_.end()) return Fail(errors, {"algorithm is not registered"});
  AlgorithmCapability candidate = found->second;
  candidate.availability = availability;
  std::vector<std::string> issues = ValidateProfile(candidate);
  if (!issues.empty()) return Fail(errors, std::move(issues));
  found->second = std::move(candidate);
  return true;
}

bool CapabilityRegistry::SetImplementation(AlgorithmId id, ImplementationProfile implementation,
                                           std::vector<std::string>* errors) {
  const auto found = profiles_.find(id);
  if (found == profiles_.end()) return Fail(errors, {"algorithm is not registered"});
  AlgorithmCapability candidate = found->second;
  candidate.implementation = std::move(implementation);
  std::vector<std::string> issues = ValidateProfile(candidate);
  if (!issues.empty()) return Fail(errors, std::move(issues));
  found->second = std::move(candidate);
  return true;
}

std::vector<AlgorithmId> CapabilityRegistry::ExecutableAlgorithms() const {
  std::vector<AlgorithmId> executable;
  for (const auto& entry : profiles_) {
    if (entry.second.availability == ImplementationAvailability::VerifiedExecutable) {
      executable.push_back(entry.first);
    }
  }
  return executable;
}

observability::AlgorithmCapability ToObservabilityCapability(const AlgorithmCapability& capability) {
  observability::AlgorithmCapability out;
  out.algorithm_id = ToString(capability.id);
  const AlgorithmProperties& p = capability.properties;
  for (const auto problem : p.problems) out.supported_characteristics.push_back(netstate::ToString(problem));
  for (const auto topology : p.topologies) out.supported_characteristics.push_back(netstate::ToString(topology));
  out.supported_characteristics.push_back(std::string("edge_weights:") + ToString(p.edge_weights));
  out.required_inputs = p.required_features;
  out.operational_restrictions = p.limitations;
  switch (capability.availability) {
    case ImplementationAvailability::VerifiedExecutable:
      out.availability = observability::ImplementationAvailability::Available;
      break;
    case ImplementationAvailability::ImplementationPresent:
      out.availability = observability::ImplementationAvailability::Experimental;
      break;
    default:
      out.availability = observability::ImplementationAvailability::NotImplemented;
      break;
  }
  out.resources.time_complexity = p.time_complexity;
  out.resources.memory_bytes = capability.implementation.peak_memory_bytes;
  return out;
}

const char* ToString(AlgorithmId id) {
  switch (id) {
    case AlgorithmId::Dijkstra: return "dijkstra";
    case AlgorithmId::AStar: return "astar";
    case AlgorithmId::BellmanFord: return "bellman_ford";
    case AlgorithmId::AntColonyOptimization: return "aco";
    case AlgorithmId::ParticleSwarmOptimization: return "pso";
    case AlgorithmId::GeneticAlgorithm: return "ga";
    case AlgorithmId::ArtificialBeeColony: return "abc";
    case AlgorithmId::GreyWolfOptimizer: return "gwo";
    case AlgorithmId::FireflyAlgorithm: return "firefly";
    case AlgorithmId::WhaleOptimizationAlgorithm: return "woa";
  }
  return "unknown";
}

std::optional<AlgorithmId> ParseAlgorithmId(const std::string& text) {
  for (const AlgorithmId id : kAllAlgorithmIds) {
    if (text == ToString(id)) return id;
  }
  return std::nullopt;
}

const char* ToString(AlgorithmFamily value) {
  switch (value) {
    case AlgorithmFamily::ClassicalShortestPath: return "classical_shortest_path";
    case AlgorithmFamily::Metaheuristic: return "metaheuristic";
  }
  return "unknown";
}

const char* ToString(ImplementationAvailability value) {
  switch (value) {
    case ImplementationAvailability::ProfileDefined: return "profile_defined";
    case ImplementationAvailability::ImplementationUnavailable: return "implementation_unavailable";
    case ImplementationAvailability::ImplementationPresent: return "implementation_present";
    case ImplementationAvailability::VerifiedExecutable: return "verified_executable";
    case ImplementationAvailability::Unsupported: return "unsupported";
  }
  return "unknown";
}

const char* ToString(EdgeWeightRequirement value) {
  switch (value) {
    case EdgeWeightRequirement::NonNegative: return "non_negative";
    case EdgeWeightRequirement::NoNegativeCycle: return "no_negative_cycle";
    case EdgeWeightRequirement::DefinedByObjective: return "defined_by_objective";
  }
  return "unknown";
}

const char* ToString(OptimalityGuarantee value) {
  switch (value) {
    case OptimalityGuarantee::Exact: return "exact";
    case OptimalityGuarantee::ConditionalOnHeuristic: return "conditional_on_heuristic";
    case OptimalityGuarantee::None: return "none";
  }
  return "unknown";
}

}  // namespace aafr::algorithms
