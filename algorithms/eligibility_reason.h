#pragma once

// Reason codes for algorithm eligibility results. Plain C++17.
//
// The single catalog: every code an eligibility check can emit is defined
// here with its stable text and its meaning. The text never changes once
// released; new codes are added at the end of their group.

#include <vector>

namespace aafr::algorithms {

enum class ReasonCode {
  // Outcome of a whole evaluation.
  EligibilityConfirmed,
  // One check passed.
  CheckPassed,

  ImplementationUnavailable,
  ImplementationUnverified,
  ImplementationUnsupported,

  RequiredFeatureMissing,
  RequiredFeatureUnusable,

  TopologyUnavailable,
  TopologyUnknown,
  UnsupportedTopology,

  ProblemUndefined,
  UnsupportedProblemRepresentation,
  ObjectiveUndefined,
  UnsupportedObjective,

  UnsupportedEdgeWeights,
  EdgeWeightsUnknown,
  NegativeCyclePresent,

  RequiredConfigurationMissing,
  HeuristicUnavailable,
  HeuristicUnknown,

  ComputationBudgetExceeded,
  ComputationBudgetUnknown,
  ComputationCostUnknown,

  MemoryBudgetExceeded,
  MemoryBudgetUnknown,
  MemoryCostUnknown,

  ForwardingCapabilityUnavailable,
  ForwardingCapabilityUnknown,
  UnsupportedProtocol,
  ProtocolSupportUnknown,

  StaleNetworkState,
  NetworkStateUnusable,
  ConstraintInvalid,
};

// Stable machine-readable text, e.g. "REQUIRED_FEATURE_MISSING".
const char* ToString(ReasonCode code);
// What the code means, independent of any one evaluation.
const char* Describe(ReasonCode code);
// Every code, in declaration order.
const std::vector<ReasonCode>& AllReasonCodes();

}  // namespace aafr::algorithms
