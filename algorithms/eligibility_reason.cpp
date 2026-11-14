#include "algorithms/eligibility_reason.h"

namespace aafr::algorithms {
namespace {

struct Entry {
  ReasonCode code;
  const char* text;
  const char* meaning;
};

const Entry kCatalog[] = {
    {ReasonCode::EligibilityConfirmed, "ELIGIBILITY_CONFIRMED",
     "Every eligibility check passed. The algorithm may be considered; this says nothing about how well it would perform."},
    {ReasonCode::CheckPassed, "CHECK_PASSED", "This check passed."},
    {ReasonCode::ImplementationUnavailable, "IMPLEMENTATION_UNAVAILABLE",
     "Only a capability profile exists; AAFR has no implementation of the algorithm."},
    {ReasonCode::ImplementationUnverified, "IMPLEMENTATION_UNVERIFIED",
     "An implementation exists but has not been verified together with the forwarding layer."},
    {ReasonCode::ImplementationUnsupported, "IMPLEMENTATION_UNSUPPORTED",
     "The algorithm is not offered in this build or environment."},
    {ReasonCode::RequiredFeatureMissing, "REQUIRED_FEATURE_MISSING",
     "A NetworkState feature the algorithm needs was not observed."},
    {ReasonCode::RequiredFeatureUnusable, "REQUIRED_FEATURE_UNUSABLE",
     "A required feature exists but is stale, invalid, unattributed or has too few samples."},
    {ReasonCode::TopologyUnavailable, "TOPOLOGY_UNAVAILABLE", "No forwarding topology exists."},
    {ReasonCode::TopologyUnknown, "TOPOLOGY_UNKNOWN",
     "It is not known whether a forwarding topology exists or how it is represented."},
    {ReasonCode::UnsupportedTopology, "UNSUPPORTED_TOPOLOGY",
     "The topology is not available in a representation the algorithm can use."},
    {ReasonCode::ProblemUndefined, "PROBLEM_UNDEFINED", "No forwarding problem has been defined."},
    {ReasonCode::UnsupportedProblemRepresentation, "UNSUPPORTED_PROBLEM_REPRESENTATION",
     "The algorithm does not solve the defined forwarding problem."},
    {ReasonCode::ObjectiveUndefined, "OBJECTIVE_UNDEFINED", "No optimization objective has been defined."},
    {ReasonCode::UnsupportedObjective, "UNSUPPORTED_OBJECTIVE",
     "The algorithm does not support the defined optimization objective."},
    {ReasonCode::UnsupportedEdgeWeights, "UNSUPPORTED_EDGE_WEIGHTS",
     "Edge weights violate the algorithm's requirement (negative or non-finite)."},
    {ReasonCode::EdgeWeightsUnknown, "EDGE_WEIGHTS_UNKNOWN",
     "Edge weights are not described well enough to check the algorithm's requirement."},
    {ReasonCode::NegativeCyclePresent, "NEGATIVE_CYCLE_PRESENT",
     "A reachable negative-weight cycle makes shortest paths undefined."},
    {ReasonCode::RequiredConfigurationMissing, "REQUIRED_CONFIGURATION_MISSING",
     "A configuration parameter the algorithm needs was not supplied."},
    {ReasonCode::HeuristicUnavailable, "HEURISTIC_UNAVAILABLE",
     "The algorithm needs a heuristic and none is available for the topology."},
    {ReasonCode::HeuristicUnknown, "HEURISTIC_UNKNOWN",
     "It is not known whether a heuristic is available for the topology."},
    {ReasonCode::ComputationBudgetExceeded, "COMPUTATION_BUDGET_EXCEEDED",
     "The implementation's measured worst-case runtime is longer than the computation deadline."},
    {ReasonCode::ComputationBudgetUnknown, "COMPUTATION_BUDGET_UNKNOWN",
     "No computation deadline is configured."},
    {ReasonCode::ComputationCostUnknown, "COMPUTATION_COST_UNKNOWN",
     "The implementation's runtime has not been measured for this topology scale."},
    {ReasonCode::MemoryBudgetExceeded, "MEMORY_BUDGET_EXCEEDED",
     "The implementation's measured peak memory is larger than the memory budget."},
    {ReasonCode::MemoryBudgetUnknown, "MEMORY_BUDGET_UNKNOWN", "No memory budget is configured."},
    {ReasonCode::MemoryCostUnknown, "MEMORY_COST_UNKNOWN",
     "The implementation's peak memory has not been measured."},
    {ReasonCode::ForwardingCapabilityUnavailable, "FORWARDING_CAPABILITY_UNAVAILABLE",
     "The environment cannot execute any forwarding operation the algorithm's result needs."},
    {ReasonCode::ForwardingCapabilityUnknown, "FORWARDING_CAPABILITY_UNKNOWN",
     "The forwarding operations the environment can execute are not known."},
    {ReasonCode::UnsupportedProtocol, "UNSUPPORTED_PROTOCOL",
     "A protocol the forwarding problem requires is not supported."},
    {ReasonCode::ProtocolSupportUnknown, "PROTOCOL_SUPPORT_UNKNOWN",
     "Protocols are required but the supported set is not known."},
    {ReasonCode::StaleNetworkState, "STALE_NETWORK_STATE",
     "The newest measurement in the NetworkState is older than the freshness limit."},
    {ReasonCode::NetworkStateUnusable, "NETWORK_STATE_UNUSABLE",
     "The NetworkState has no usable feature."},
    {ReasonCode::ConstraintInvalid, "CONSTRAINT_INVALID",
     "A constraint this check depends on contradicts itself or another constraint."},
};

const Entry* Find(ReasonCode code) {
  for (const Entry& entry : kCatalog) {
    if (entry.code == code) return &entry;
  }
  return nullptr;
}

}  // namespace

const char* ToString(ReasonCode code) {
  const Entry* entry = Find(code);
  return entry ? entry->text : "UNKNOWN_REASON";
}

const char* Describe(ReasonCode code) {
  const Entry* entry = Find(code);
  return entry ? entry->meaning : "";
}

const std::vector<ReasonCode>& AllReasonCodes() {
  static const std::vector<ReasonCode> codes = [] {
    std::vector<ReasonCode> all;
    for (const Entry& entry : kCatalog) all.push_back(entry.code);
    return all;
  }();
  return codes;
}

}  // namespace aafr::algorithms
