#pragma once

// Deterministic algorithm eligibility evaluation. Plain C++17: no clock, no
// randomness, no I/O, so the same inputs always give the same result.
//
// Eligibility answers one question: may this algorithm be considered for
// this NetworkState and these constraints? It is not a performance
// prediction, it ranks nothing, and it selects and switches nothing.
//
// Policy for unknown information: a check whose inputs are unknown has the
// outcome Unknown. It is never counted as passed. EligibilityPolicy decides
// whether an algorithm with unknown checks is reported as Unknown (default)
// or as Ineligible.

#include <cstdint>
#include <string>
#include <vector>

#include "algorithms/algorithm_capability.h"
#include "algorithms/capability_registry.h"
#include "algorithms/eligibility_reason.h"
#include "network/network_state/execution_environment.h"
#include "network/network_state/network_constraints.h"
#include "network/network_state/network_state.h"

namespace aafr::algorithms {

enum class EligibilityStatus {
  Eligible,        // Every check passed.
  Ineligible,      // At least one check failed.
  Unknown,         // Nothing failed, but at least one check could not be decided.
  NotImplemented,  // AAFR has no implementation; other checks are informational.
};

// The ten rules, in evaluation order.
enum class EligibilityCheck {
  ImplementationAvailability,
  RequiredFeatures,
  TopologyInformation,
  ProblemRepresentation,
  EdgeWeights,
  RequiredConfiguration,
  ComputationBudget,
  MemoryBudget,
  ForwardingExecution,
  DataQuality,
};

enum class CheckOutcome { Passed, Failed, Unknown };

struct CheckResult {
  EligibilityCheck check{EligibilityCheck::ImplementationAvailability};
  CheckOutcome outcome{CheckOutcome::Unknown};
  ReasonCode reason{ReasonCode::CheckPassed};
  // Built from the values that were compared.
  std::string explanation;
  // Constraint ids, feature ids or "config:<name>" entries that would be
  // needed to decide or to pass this check.
  std::vector<std::string> missing_information;
};

struct EligibilityResult {
  AlgorithmId algorithm{AlgorithmId::Dijkstra};
  std::uint64_t network_state_id{0};
  EligibilityStatus status{EligibilityStatus::Unknown};
  // All ten checks, in EligibilityCheck order.
  std::vector<CheckResult> checks;
  // Reasons for the status: failed checks first, then unknown ones, without
  // repeats; exactly {EligibilityConfirmed} when eligible.
  std::vector<ReasonCode> reason_codes;
  std::string explanation;
  std::vector<std::string> missing_information;

  std::vector<EligibilityCheck> ChecksWith(CheckOutcome outcome) const;
  const CheckResult* Find(EligibilityCheck check) const;
  bool HasReason(ReasonCode code) const;
};

struct EligibilityPolicy {
  // When true, an algorithm with undecidable checks is Ineligible instead of
  // Unknown. The checks themselves still report Unknown.
  bool unknown_is_ineligible{false};
};

class AlgorithmEligibilityEvaluator {
 public:
  explicit AlgorithmEligibilityEvaluator(EligibilityPolicy policy = {}) : policy_(policy) {}

  EligibilityResult Evaluate(const netstate::NetworkState& state,
                             const netstate::NetworkConstraints& constraints,
                             const AlgorithmCapability& capability,
                             const netstate::ExecutionEnvironment& environment) const;

  // One result per registered algorithm, in AlgorithmId order.
  std::vector<EligibilityResult> EvaluateAll(const netstate::NetworkState& state,
                                             const netstate::NetworkConstraints& constraints,
                                             const CapabilityRegistry& registry,
                                             const netstate::ExecutionEnvironment& environment) const;

 private:
  EligibilityPolicy policy_;
};

const char* ToString(EligibilityStatus value);
const char* ToString(EligibilityCheck value);
const char* ToString(CheckOutcome value);
// Comma-separated reason-code texts.
std::string ReasonCodesText(const std::vector<ReasonCode>& codes);

}  // namespace aafr::algorithms
