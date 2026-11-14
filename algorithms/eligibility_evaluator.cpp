#include "algorithms/eligibility_evaluator.h"

#include <algorithm>
#include <cstdio>
#include <initializer_list>

namespace aafr::algorithms {
namespace {

using netstate::Constraint;
using netstate::ConstraintValidity;
using netstate::NetworkConstraints;
using netstate::NetworkState;
namespace id = netstate::constraint_id;

std::string Number(double value) {
  char buffer[48];
  std::snprintf(buffer, sizeof(buffer), "%.6g", value);
  return buffer;
}

std::string Join(const std::vector<std::string>& items) {
  std::string text;
  for (const std::string& item : items) {
    if (!text.empty()) text += ", ";
    text += item;
  }
  return text;
}

CheckResult Passed(EligibilityCheck check, std::string explanation) {
  return {check, CheckOutcome::Passed, ReasonCode::CheckPassed, std::move(explanation), {}};
}

CheckResult Failed(EligibilityCheck check, ReasonCode reason, std::string explanation,
                   std::vector<std::string> missing = {}) {
  return {check, CheckOutcome::Failed, reason, std::move(explanation), std::move(missing)};
}

CheckResult Unknown(EligibilityCheck check, ReasonCode reason, std::string explanation,
                    std::vector<std::string> missing) {
  return {check, CheckOutcome::Unknown, reason, std::move(explanation), std::move(missing)};
}

// The first of `ids` that is Invalid. A check must not read a contradictory
// constraint as "unknown"; it fails with CONSTRAINT_INVALID instead.
const Constraint* InvalidAmong(const NetworkConstraints& constraints,
                               std::initializer_list<const char*> ids) {
  for (const char* constraint_id : ids) {
    const Constraint* constraint = constraints.Find(constraint_id);
    if (constraint && constraint->validity == ConstraintValidity::Invalid) return constraint;
  }
  return nullptr;
}

CheckResult InvalidConstraint(EligibilityCheck check, const Constraint& constraint) {
  return Failed(check, ReasonCode::ConstraintInvalid,
                "constraint " + constraint.id + " is invalid: " + constraint.explanation,
                {constraint.id});
}

CheckResult CheckImplementation(const AlgorithmCapability& capability) {
  const auto check = EligibilityCheck::ImplementationAvailability;
  const std::string state = ToString(capability.availability);
  switch (capability.availability) {
    case ImplementationAvailability::VerifiedExecutable:
      return Passed(check, "implementation is verified executable: " +
                               capability.implementation.verification_note);
    case ImplementationAvailability::ImplementationPresent:
      return Failed(check, ReasonCode::ImplementationUnverified,
                    "an implementation is present but not verified with the forwarding layer");
    case ImplementationAvailability::Unsupported:
      return Failed(check, ReasonCode::ImplementationUnsupported,
                    "the algorithm is marked unsupported in this build or environment");
    case ImplementationAvailability::ProfileDefined:
    case ImplementationAvailability::ImplementationUnavailable:
      break;
  }
  return Failed(check, ReasonCode::ImplementationUnavailable,
                "implementation availability is '" + state + "': only the capability profile exists");
}

CheckResult CheckFeatures(const NetworkState& state, const AlgorithmCapability& capability) {
  const auto check = EligibilityCheck::RequiredFeatures;
  const auto& required = capability.properties.required_features;
  if (required.empty()) return Passed(check, "the algorithm requires no NetworkState feature");
  std::vector<std::string> missing;
  std::vector<std::string> unusable;
  std::vector<std::string> details;
  for (const std::string& feature_id : required) {
    const netstate::Feature* feature = state.FindFeature(feature_id);
    if (!feature || feature->quality == netstate::FeatureQuality::Missing) {
      missing.push_back(feature_id);
      details.push_back(feature_id + " is missing" +
                        (feature && !feature->quality_reason.empty() ? " (" + feature->quality_reason + ")"
                                                                      : std::string()));
    } else if (!feature->Usable()) {
      unusable.push_back(feature_id);
      details.push_back(feature_id + " is " + netstate::ToString(feature->quality) + " (" +
                        feature->quality_reason + ")");
    }
  }
  if (missing.empty() && unusable.empty()) {
    return Passed(check, "required features are valid in the '" + state.primary_window +
                             "' window: " + Join(required));
  }
  std::vector<std::string> needed = missing;
  needed.insert(needed.end(), unusable.begin(), unusable.end());
  return Failed(check,
                missing.empty() ? ReasonCode::RequiredFeatureUnusable : ReasonCode::RequiredFeatureMissing,
                Join(details), std::move(needed));
}

CheckResult CheckTopology(const NetworkConstraints& constraints, const AlgorithmCapability& capability) {
  const auto check = EligibilityCheck::TopologyInformation;
  if (const Constraint* invalid =
          InvalidAmong(constraints, {id::kTopologyAvailable, id::kTopologyRepresentations,
                                     id::kTopologyNodeCount, id::kCandidatePathCount})) {
    return InvalidConstraint(check, *invalid);
  }
  const auto available = constraints.Bool(id::kTopologyAvailable);
  if (!available) {
    return Unknown(check, ReasonCode::TopologyUnknown,
                   "it is not known whether a forwarding topology exists", {id::kTopologyAvailable});
  }
  if (!*available) {
    return Failed(check, ReasonCode::TopologyUnavailable, "no forwarding topology exists",
                  {id::kTopologyAvailable});
  }
  const auto representations = constraints.List(id::kTopologyRepresentations);
  if (!representations) {
    return Unknown(check, ReasonCode::TopologyUnknown, "the topology representation is not known",
                   {id::kTopologyRepresentations});
  }
  std::vector<std::string> accepted;
  for (const auto representation : capability.properties.topologies) {
    accepted.push_back(netstate::ToString(representation));
  }
  for (const std::string& offered : *representations) {
    if (std::find(accepted.begin(), accepted.end(), offered) != accepted.end()) {
      return Passed(check, "topology is available as " + offered);
    }
  }
  return Failed(check, ReasonCode::UnsupportedTopology,
                "topology is available as [" + Join(*representations) + "]; the algorithm accepts [" +
                    Join(accepted) + "]");
}

CheckResult CheckProblem(const NetworkConstraints& constraints, const AlgorithmCapability& capability) {
  const auto check = EligibilityCheck::ProblemRepresentation;
  const auto problem = constraints.Text(id::kProblem);
  const auto objective = constraints.Text(id::kObjective);
  if (problem) {
    bool supported = false;
    for (const auto candidate : capability.properties.problems) {
      supported = supported || *problem == netstate::ToString(candidate);
    }
    if (!supported) {
      return Failed(check, ReasonCode::UnsupportedProblemRepresentation,
                    "the algorithm does not solve '" + *problem + "'");
    }
  }
  if (objective) {
    bool supported = false;
    for (const auto candidate : capability.properties.objectives) {
      supported = supported || *objective == netstate::ToString(candidate);
    }
    if (!supported) {
      return Failed(check, ReasonCode::UnsupportedObjective,
                    "the algorithm does not support the objective '" + *objective + "'");
    }
  }
  if (!problem) {
    return Unknown(check, ReasonCode::ProblemUndefined, "no forwarding problem is defined", {id::kProblem});
  }
  if (!objective) {
    return Unknown(check, ReasonCode::ObjectiveUndefined, "no optimization objective is defined",
                   {id::kObjective});
  }
  return Passed(check, "solves '" + *problem + "' with objective '" + *objective + "'");
}

CheckResult CheckEdgeWeights(const NetworkConstraints& constraints, const AlgorithmCapability& capability) {
  const auto check = EligibilityCheck::EdgeWeights;
  const EdgeWeightRequirement requirement = capability.properties.edge_weights;
  if (requirement == EdgeWeightRequirement::DefinedByObjective) {
    return Passed(check, "no edge-weight requirement: the objective function scores candidate solutions");
  }
  if (const Constraint* invalid = InvalidAmong(
          constraints, {id::kEdgeWeightsFinite, id::kEdgeWeightsNegative, id::kNegativeCycle})) {
    return InvalidConstraint(check, *invalid);
  }
  const auto finite = constraints.Bool(id::kEdgeWeightsFinite);
  const auto negative = constraints.Bool(id::kEdgeWeightsNegative);
  if (finite && !*finite) {
    return Failed(check, ReasonCode::UnsupportedEdgeWeights, "some edge weights are missing or not finite");
  }
  if (negative && *negative && requirement == EdgeWeightRequirement::NonNegative) {
    return Failed(check, ReasonCode::UnsupportedEdgeWeights,
                  "the topology has negative edge weights; the algorithm requires nonnegative weights");
  }
  if (!finite || !negative) {
    std::vector<std::string> missing;
    if (!finite) missing.push_back(id::kEdgeWeightsFinite);
    if (!negative) missing.push_back(id::kEdgeWeightsNegative);
    return Unknown(check, ReasonCode::EdgeWeightsUnknown, "edge weights are not described",
                   std::move(missing));
  }
  if (!*negative) return Passed(check, "edge weights are finite and nonnegative");
  // Negative weights with a NoNegativeCycle requirement.
  const auto cycle = constraints.Bool(id::kNegativeCycle);
  if (!cycle) {
    return Unknown(check, ReasonCode::EdgeWeightsUnknown,
                   "negative edge weights are present and no negative-cycle check result is available",
                   {id::kNegativeCycle});
  }
  if (*cycle) {
    return Failed(check, ReasonCode::NegativeCyclePresent,
                  "a reachable negative-weight cycle is reported");
  }
  return Passed(check, "negative edge weights are present without a negative cycle");
}

CheckResult CheckConfiguration(const NetworkConstraints& constraints,
                               const AlgorithmCapability& capability,
                               const netstate::ExecutionEnvironment& environment) {
  const auto check = EligibilityCheck::RequiredConfiguration;
  const auto& required = capability.properties.required_configuration;
  const auto supplied = environment.algorithm_configuration.find(ToString(capability.id));
  std::vector<std::string> missing;
  for (const std::string& key : required) {
    const bool present = supplied != environment.algorithm_configuration.end() &&
                         supplied->second.count(key) != 0 && !supplied->second.at(key).empty();
    if (!present) missing.push_back("config:" + key);
  }
  if (!missing.empty()) {
    return Failed(check, ReasonCode::RequiredConfigurationMissing,
                  "missing configuration: " + Join(missing), missing);
  }
  if (capability.properties.requires_heuristic) {
    if (const Constraint* invalid = InvalidAmong(constraints, {id::kHeuristicAvailable})) {
      return InvalidConstraint(check, *invalid);
    }
    const auto heuristic = constraints.Bool(id::kHeuristicAvailable);
    if (!heuristic) {
      return Unknown(check, ReasonCode::HeuristicUnknown,
                     "it is not known whether a heuristic is available for the topology",
                     {id::kHeuristicAvailable});
    }
    if (!*heuristic) {
      return Failed(check, ReasonCode::HeuristicUnavailable,
                    "no heuristic is available for the topology", {id::kHeuristicAvailable});
    }
  }
  return Passed(check, required.empty() ? "the algorithm requires no configuration"
                                        : "required configuration is supplied: " + Join(required));
}

CheckResult CheckComputation(const NetworkConstraints& constraints, const AlgorithmCapability& capability) {
  const auto check = EligibilityCheck::ComputationBudget;
  if (const Constraint* invalid =
          InvalidAmong(constraints, {id::kComputationDeadlineMs, id::kTopologyNodeCount})) {
    return InvalidConstraint(check, *invalid);
  }
  const auto deadline = constraints.Number(id::kComputationDeadlineMs);
  const auto& implementation = capability.implementation;
  if (!implementation.worst_case_runtime_ms) {
    return Unknown(check, ReasonCode::ComputationCostUnknown,
                   "the implementation's worst-case runtime has not been measured",
                   {"implementation.worst_case_runtime_ms"});
  }
  if (!deadline) {
    return Unknown(check, ReasonCode::ComputationBudgetUnknown, "no computation deadline is configured",
                   {id::kComputationDeadlineMs});
  }
  if (*implementation.worst_case_runtime_ms > *deadline) {
    return Failed(check, ReasonCode::ComputationBudgetExceeded,
                  "measured worst-case runtime " + Number(*implementation.worst_case_runtime_ms) +
                      " ms exceeds the deadline of " + Number(*deadline) + " ms");
  }
  // A runtime measured on small topologies says nothing about larger ones.
  const auto nodes = constraints.Number(id::kTopologyNodeCount);
  if (implementation.max_nodes && nodes && *nodes > static_cast<double>(*implementation.max_nodes)) {
    return Unknown(check, ReasonCode::ComputationCostUnknown,
                   "runtime was measured up to " + std::to_string(*implementation.max_nodes) +
                       " nodes; the topology has " + Number(*nodes),
                   {"implementation.worst_case_runtime_ms"});
  }
  return Passed(check, "measured worst-case runtime " + Number(*implementation.worst_case_runtime_ms) +
                           " ms is within the deadline of " + Number(*deadline) + " ms");
}

CheckResult CheckMemory(const NetworkConstraints& constraints, const AlgorithmCapability& capability) {
  const auto check = EligibilityCheck::MemoryBudget;
  if (const Constraint* invalid = InvalidAmong(constraints, {id::kMemoryBudgetBytes})) {
    return InvalidConstraint(check, *invalid);
  }
  const auto budget = constraints.Number(id::kMemoryBudgetBytes);
  const auto& implementation = capability.implementation;
  if (!implementation.peak_memory_bytes) {
    return Unknown(check, ReasonCode::MemoryCostUnknown,
                   "the implementation's peak memory has not been measured",
                   {"implementation.peak_memory_bytes"});
  }
  if (!budget) {
    return Unknown(check, ReasonCode::MemoryBudgetUnknown, "no memory budget is configured",
                   {id::kMemoryBudgetBytes});
  }
  const double peak = static_cast<double>(*implementation.peak_memory_bytes);
  if (peak > *budget) {
    return Failed(check, ReasonCode::MemoryBudgetExceeded,
                  "measured peak memory " + Number(peak) + " bytes exceeds the budget of " +
                      Number(*budget) + " bytes");
  }
  return Passed(check, "measured peak memory " + Number(peak) + " bytes is within the budget of " +
                           Number(*budget) + " bytes");
}

CheckResult CheckForwarding(const NetworkConstraints& constraints, const AlgorithmCapability& capability) {
  const auto check = EligibilityCheck::ForwardingExecution;
  if (const Constraint* invalid =
          InvalidAmong(constraints, {id::kForwardingOperations, id::kRequiredProtocolsSupported})) {
    return InvalidConstraint(check, *invalid);
  }
  const auto operations = constraints.List(id::kForwardingOperations);
  std::vector<std::string> usable;
  for (const auto operation : capability.properties.usable_forwarding_operations) {
    usable.push_back(netstate::ToString(operation));
  }
  std::string matched;
  if (operations) {
    for (const std::string& offered : *operations) {
      if (std::find(usable.begin(), usable.end(), offered) != usable.end()) {
        matched = offered;
        break;
      }
    }
    if (matched.empty()) {
      return Failed(check, ReasonCode::ForwardingCapabilityUnavailable,
                    "the environment can execute [" + Join(*operations) +
                        "]; the algorithm's result needs one of [" + Join(usable) + "]",
                    {id::kForwardingOperations});
    }
  }
  const auto protocols = constraints.Bool(id::kRequiredProtocolsSupported);
  if (protocols && !*protocols) {
    const Constraint* constraint = constraints.Find(id::kRequiredProtocolsSupported);
    return Failed(check, ReasonCode::UnsupportedProtocol, constraint->explanation);
  }
  if (!operations) {
    return Unknown(check, ReasonCode::ForwardingCapabilityUnknown,
                   "the forwarding operations the environment can execute are not known",
                   {id::kForwardingOperations});
  }
  if (!protocols) {
    return Unknown(check, ReasonCode::ProtocolSupportUnknown,
                   "protocols are required but the supported set is not known",
                   {id::kSupportedProtocols});
  }
  return Passed(check, "the environment can execute " + matched);
}

CheckResult CheckDataQuality(const NetworkState& state, const NetworkConstraints& constraints) {
  const auto check = EligibilityCheck::DataQuality;
  if (state.validity == netstate::StateValidity::NoData) {
    return Failed(check, ReasonCode::NetworkStateUnusable, state.validity_reason);
  }
  const Constraint* fresh = constraints.Find(id::kStateFresh);
  const auto is_fresh = constraints.Bool(id::kStateFresh);
  if (!is_fresh) {
    return Unknown(check, ReasonCode::StaleNetworkState, "state freshness could not be determined",
                   {id::kStateFresh});
  }
  if (!*is_fresh) return Failed(check, ReasonCode::StaleNetworkState, fresh->explanation);
  return Passed(check, std::string("state is ") + netstate::ToString(state.validity) + " and fresh: " +
                           fresh->explanation);
}

void AppendUnique(std::vector<ReasonCode>& codes, ReasonCode code) {
  if (std::find(codes.begin(), codes.end(), code) == codes.end()) codes.push_back(code);
}

void AppendUnique(std::vector<std::string>& items, const std::vector<std::string>& more) {
  for (const std::string& item : more) {
    if (std::find(items.begin(), items.end(), item) == items.end()) items.push_back(item);
  }
}

}  // namespace

std::vector<EligibilityCheck> EligibilityResult::ChecksWith(CheckOutcome outcome) const {
  std::vector<EligibilityCheck> matching;
  for (const CheckResult& result : checks) {
    if (result.outcome == outcome) matching.push_back(result.check);
  }
  return matching;
}

const CheckResult* EligibilityResult::Find(EligibilityCheck check) const {
  for (const CheckResult& result : checks) {
    if (result.check == check) return &result;
  }
  return nullptr;
}

bool EligibilityResult::HasReason(ReasonCode code) const {
  return std::find(reason_codes.begin(), reason_codes.end(), code) != reason_codes.end();
}

EligibilityResult AlgorithmEligibilityEvaluator::Evaluate(
    const NetworkState& state, const NetworkConstraints& constraints,
    const AlgorithmCapability& capability, const netstate::ExecutionEnvironment& environment) const {
  EligibilityResult result;
  result.algorithm = capability.id;
  result.network_state_id = state.state_id;
  result.checks = {
      CheckImplementation(capability),
      CheckFeatures(state, capability),
      CheckTopology(constraints, capability),
      CheckProblem(constraints, capability),
      CheckEdgeWeights(constraints, capability),
      CheckConfiguration(constraints, capability, environment),
      CheckComputation(constraints, capability),
      CheckMemory(constraints, capability),
      CheckForwarding(constraints, capability),
      CheckDataQuality(state, constraints),
  };

  bool failed = false;
  bool unknown = false;
  for (const CheckResult& check : result.checks) {
    if (check.outcome == CheckOutcome::Failed) {
      failed = true;
      AppendUnique(result.reason_codes, check.reason);
    }
  }
  for (const CheckResult& check : result.checks) {
    if (check.outcome == CheckOutcome::Unknown) {
      unknown = true;
      AppendUnique(result.reason_codes, check.reason);
    }
    if (check.outcome != CheckOutcome::Passed) {
      AppendUnique(result.missing_information, check.missing_information);
    }
  }

  const CheckResult& implementation = result.checks.front();
  if (implementation.reason == ReasonCode::ImplementationUnavailable) {
    result.status = EligibilityStatus::NotImplemented;
  } else if (failed || (unknown && policy_.unknown_is_ineligible)) {
    result.status = EligibilityStatus::Ineligible;
  } else if (unknown) {
    result.status = EligibilityStatus::Unknown;
  } else {
    result.status = EligibilityStatus::Eligible;
    result.reason_codes = {ReasonCode::EligibilityConfirmed};
  }

  // The explanation restates the check results; it adds no judgement.
  switch (result.status) {
    case EligibilityStatus::Eligible:
      result.explanation = "All " + std::to_string(result.checks.size()) +
                           " eligibility checks passed. Eligible does not mean best.";
      break;
    case EligibilityStatus::NotImplemented:
      result.explanation = "Not implemented: " + implementation.explanation + ".";
      break;
    case EligibilityStatus::Ineligible:
      result.explanation = failed ? "Ineligible." : "Ineligible: undecided checks count as failed under the configured policy.";
      break;
    case EligibilityStatus::Unknown:
      result.explanation = "Eligibility cannot be decided.";
      break;
  }
  for (const CheckOutcome outcome : {CheckOutcome::Failed, CheckOutcome::Unknown}) {
    std::string part;
    for (const CheckResult& check : result.checks) {
      if (check.outcome != outcome) continue;
      if (result.status == EligibilityStatus::NotImplemented && &check == &implementation) continue;
      if (!part.empty()) part += "; ";
      part += std::string(ToString(check.check)) + " [" + ToString(check.reason) + "]: " + check.explanation;
    }
    if (!part.empty()) {
      result.explanation += std::string(outcome == CheckOutcome::Failed ? " Failed: " : " Unknown: ") + part + ".";
    }
  }
  return result;
}

std::vector<EligibilityResult> AlgorithmEligibilityEvaluator::EvaluateAll(
    const NetworkState& state, const NetworkConstraints& constraints,
    const CapabilityRegistry& registry, const netstate::ExecutionEnvironment& environment) const {
  std::vector<EligibilityResult> results;
  for (const AlgorithmCapability& capability : registry.All()) {
    results.push_back(Evaluate(state, constraints, capability, environment));
  }
  return results;
}

const char* ToString(EligibilityStatus value) {
  switch (value) {
    case EligibilityStatus::Eligible: return "eligible";
    case EligibilityStatus::Ineligible: return "ineligible";
    case EligibilityStatus::Unknown: return "unknown";
    case EligibilityStatus::NotImplemented: return "not_implemented";
  }
  return "unknown";
}

const char* ToString(EligibilityCheck value) {
  switch (value) {
    case EligibilityCheck::ImplementationAvailability: return "implementation_availability";
    case EligibilityCheck::RequiredFeatures: return "required_features";
    case EligibilityCheck::TopologyInformation: return "topology_information";
    case EligibilityCheck::ProblemRepresentation: return "problem_representation";
    case EligibilityCheck::EdgeWeights: return "edge_weights";
    case EligibilityCheck::RequiredConfiguration: return "required_configuration";
    case EligibilityCheck::ComputationBudget: return "computation_budget";
    case EligibilityCheck::MemoryBudget: return "memory_budget";
    case EligibilityCheck::ForwardingExecution: return "forwarding_execution";
    case EligibilityCheck::DataQuality: return "data_quality";
  }
  return "unknown";
}

const char* ToString(CheckOutcome value) {
  switch (value) {
    case CheckOutcome::Passed: return "passed";
    case CheckOutcome::Failed: return "failed";
    case CheckOutcome::Unknown: return "unknown";
  }
  return "unknown";
}

std::string ReasonCodesText(const std::vector<ReasonCode>& codes) {
  std::string text;
  for (const ReasonCode code : codes) {
    if (!text.empty()) text += ", ";
    text += ToString(code);
  }
  return text;
}

}  // namespace aafr::algorithms
