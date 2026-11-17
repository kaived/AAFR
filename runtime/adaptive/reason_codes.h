#pragma once

// Stable reason codes for V0.6 runtime decisions. Plain C++17.
//
// A code names one fact the runtime established; its text (ToString) and
// meaning (Meaning) never change once released. A DecisionRecord carries the
// codes that applied plus an explanation built from the values that were
// actually compared; the code alone never stands in for that evidence.
// Add new codes at the end; never reuse or rename one.

#include <string>
#include <vector>

namespace aafr::runtime {

enum class RuntimeReason {
  InitialActivation,                // First confirmed configuration of this runtime session.
  NetworkChangeMeaningful,          // A tracked measurement moved beyond its thresholds, sustained.
  NetworkChangeIgnored,             // Changes stayed within thresholds or were not sustained.
  ConstraintChanged,                // The NetworkConstraints set changed.
  TopologyChanged,                  // Forwarding graph availability or attributes changed.
  CandidateIneligible,              // The candidate failed V0.4 eligibility.
  RecommendationUnavailable,        // The provider returned no usable recommendation.
  CandidateSameAsActive,            // The best candidate is the active algorithm or its path.
  ProbingUnavailable,               // Controlled probing could not run for this comparison.
  ProbeTimeout,                     // At least one probe hit its deadline.
  InsufficientSamples,              // Too few valid paired probe observations.
  StatisticalEvidenceInvalid,       // Assumptions of the comparison did not hold.
  StatisticalEvidenceInsufficient,  // Evidence did not separate incumbent and challenger.
  ImprovementConfirmed,             // Evidence shows a practically meaningful improvement.
  ImprovementMarginNotMet,          // Improvement smaller than the configured margin.
  MinimumDwellNotMet,               // The active algorithm has not been active long enough.
  CooldownActive,                   // A recent switch or failed activation blocks switching.
  HysteresisNotSatisfied,           // The challenger has not won for enough consecutive evaluations.
  ActivePathFailed,                 // Forwarding over the confirmed active path failed.
  SafetyOverride,                   // Ordinary gates were bypassed for a safety reason.
  SwitchRequested,                  // Activation of a validated candidate was requested.
  SwitchCommitted,                  // Activation was confirmed and the new state committed.
  SwitchFailed,                     // Activation could not be confirmed.
  RecoveryRequested,                // A recovery activation was attempted.
  RecoverySucceeded,                // Recovery confirmed a working configuration.
  RecoveryFailed,                   // No configuration could be confirmed during recovery.
  ActiveConfigurationUnknown,       // The runtime cannot verify which configuration is active.
  ActiveAlgorithmIneligible,        // The active algorithm no longer passes eligibility.
  ActiveConfigurationInvalid,       // The active path no longer validates on the current graph.
  ChallengerNotBetter,              // Evidence shows the challenger is not better (equal or worse).
  NoEligibleAlgorithm,              // No algorithm passed eligibility and produced a valid path.
  NetworkStateUnavailable,          // No usable NetworkState snapshot was available.
  EvaluationStale,                  // The graph changed during evaluation; the result was discarded.
  ManualEvaluation,                 // An operator requested a diagnostic evaluation.
  ConstraintViolated,               // A hard forwarding constraint is violated on the active path.
  ImprovementEstimateOnly,          // Only a model estimate (J(P)) supports the change; no measurement.
  PreviousConfigurationRetained,    // After a failed switch, the previous configuration was re-confirmed.
  CandidateBackoff,                 // The candidate failed activation recently and is backing off.
  ActivationTimeout,                // The activation request timed out.
  // V0.7.1
  MeasurementReview,                // Measurement-led review: the incumbent was probed against an alternative.
};

// Stable machine-readable text, e.g. "SWITCH_COMMITTED".
const char* ToString(RuntimeReason reason);
// The fixed meaning of the code, for explanations and documentation.
const char* Meaning(RuntimeReason reason);
std::string ReasonsText(const std::vector<RuntimeReason>& reasons);
// Every code, in declaration order (for documentation and tests).
const std::vector<RuntimeReason>& AllRuntimeReasons();

}  // namespace aafr::runtime
