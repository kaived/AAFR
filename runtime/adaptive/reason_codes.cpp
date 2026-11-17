#include "runtime/adaptive/reason_codes.h"

namespace aafr::runtime {

namespace {
struct Entry {
  RuntimeReason reason;
  const char* text;
  const char* meaning;
};

const Entry kEntries[] = {
    {RuntimeReason::InitialActivation, "INITIAL_ACTIVATION", "first confirmed forwarding configuration of this runtime session"},
    {RuntimeReason::NetworkChangeMeaningful, "NETWORK_CHANGE_MEANINGFUL", "a tracked measurement moved beyond its absolute and relative thresholds for the required number of observations"},
    {RuntimeReason::NetworkChangeIgnored, "NETWORK_CHANGE_IGNORED", "observed changes stayed within thresholds, were not sustained, or concerned unusable measurements"},
    {RuntimeReason::ConstraintChanged, "CONSTRAINT_CHANGED", "the set or validity of network constraints changed"},
    {RuntimeReason::TopologyChanged, "TOPOLOGY_CHANGED", "forwarding graph availability or edge attributes changed"},
    {RuntimeReason::CandidateIneligible, "CANDIDATE_INELIGIBLE", "the candidate algorithm did not pass eligibility evaluation"},
    {RuntimeReason::RecommendationUnavailable, "RECOMMENDATION_UNAVAILABLE", "the recommendation provider returned no usable recommendation"},
    {RuntimeReason::CandidateSameAsActive, "CANDIDATE_SAME_AS_ACTIVE", "the best candidate is the active algorithm or forwards over the same path"},
    {RuntimeReason::ProbingUnavailable, "PROBING_UNAVAILABLE", "controlled incumbent/challenger probing could not run"},
    {RuntimeReason::ProbeTimeout, "PROBE_TIMEOUT", "at least one controlled probe reached its deadline"},
    {RuntimeReason::InsufficientSamples, "INSUFFICIENT_SAMPLES", "fewer valid paired observations than the comparison requires"},
    {RuntimeReason::StatisticalEvidenceInvalid, "STATISTICAL_EVIDENCE_INVALID", "the assumptions of the statistical comparison did not hold"},
    {RuntimeReason::StatisticalEvidenceInsufficient, "STATISTICAL_EVIDENCE_INSUFFICIENT", "the evidence did not separate the challenger from the incumbent"},
    {RuntimeReason::ImprovementConfirmed, "IMPROVEMENT_CONFIRMED", "valid evidence shows a practically meaningful improvement"},
    {RuntimeReason::ImprovementMarginNotMet, "IMPROVEMENT_MARGIN_NOT_MET", "the improvement is smaller than the configured margin"},
    {RuntimeReason::MinimumDwellNotMet, "MINIMUM_DWELL_NOT_MET", "the active algorithm has been active for less than the minimum dwell time"},
    {RuntimeReason::CooldownActive, "COOLDOWN_ACTIVE", "a recent switch or failed activation blocks ordinary switching"},
    {RuntimeReason::HysteresisNotSatisfied, "HYSTERESIS_NOT_SATISFIED", "the challenger has not been preferred for enough consecutive evaluations"},
    {RuntimeReason::ActivePathFailed, "ACTIVE_PATH_FAILED", "forwarding over the confirmed active path failed"},
    {RuntimeReason::SafetyOverride, "SAFETY_OVERRIDE", "ordinary stability gates were bypassed for a documented safety reason"},
    {RuntimeReason::SwitchRequested, "SWITCH_REQUESTED", "activation of a validated candidate was requested"},
    {RuntimeReason::SwitchCommitted, "SWITCH_COMMITTED", "activation was confirmed and the new configuration committed"},
    {RuntimeReason::SwitchFailed, "SWITCH_FAILED", "activation of the candidate could not be confirmed"},
    {RuntimeReason::RecoveryRequested, "RECOVERY_REQUESTED", "a recovery activation was attempted"},
    {RuntimeReason::RecoverySucceeded, "RECOVERY_SUCCEEDED", "recovery confirmed a working forwarding configuration"},
    {RuntimeReason::RecoveryFailed, "RECOVERY_FAILED", "recovery could not confirm any forwarding configuration"},
    {RuntimeReason::ActiveConfigurationUnknown, "ACTIVE_CONFIGURATION_UNKNOWN", "the runtime cannot verify which forwarding configuration is active"},
    {RuntimeReason::ActiveAlgorithmIneligible, "ACTIVE_ALGORITHM_INELIGIBLE", "the active algorithm no longer passes eligibility evaluation"},
    {RuntimeReason::ActiveConfigurationInvalid, "ACTIVE_CONFIGURATION_INVALID", "the active path no longer validates on the current forwarding graph"},
    {RuntimeReason::ChallengerNotBetter, "CHALLENGER_NOT_BETTER", "the evidence shows the challenger is equal to or worse than the incumbent"},
    {RuntimeReason::NoEligibleAlgorithm, "NO_ELIGIBLE_ALGORITHM", "no algorithm passed eligibility and produced a valid, feasible path"},
    {RuntimeReason::NetworkStateUnavailable, "NETWORK_STATE_UNAVAILABLE", "no NetworkState snapshot was available"},
    {RuntimeReason::EvaluationStale, "EVALUATION_STALE", "the forwarding graph changed during evaluation, so its result was discarded"},
    {RuntimeReason::ManualEvaluation, "MANUAL_EVALUATION", "an operator requested a diagnostic evaluation"},
    {RuntimeReason::ConstraintViolated, "CONSTRAINT_VIOLATED", "a hard forwarding constraint is violated on the active path"},
    {RuntimeReason::ImprovementEstimateOnly, "IMPROVEMENT_ESTIMATE_ONLY", "only the configured-graph objective estimate supports the change; nothing was measured"},
    {RuntimeReason::PreviousConfigurationRetained, "PREVIOUS_CONFIGURATION_RETAINED", "after a failed switch the previous configuration was re-confirmed and kept"},
    {RuntimeReason::CandidateBackoff, "CANDIDATE_BACKOFF", "the candidate failed activation recently and is in its retry backoff"},
    {RuntimeReason::ActivationTimeout, "ACTIVATION_TIMEOUT", "the activation confirmation request timed out"},
    {RuntimeReason::MeasurementReview, "MEASUREMENT_REVIEW",
     "measurement-led review: the incumbent was probed against the best alternative path"},
    {RuntimeReason::ModelRecommendationUsed, "MODEL_RECOMMENDATION_USED",
     "the expert model's eligible, valid recommendation was ranked first; evidence checks and gates still applied"},
    {RuntimeReason::ModelFallbackDeterministic, "MODEL_FALLBACK_DETERMINISTIC",
     "AI-preferred mode, but the model result was unusable, so the deterministic ranking was used"},
    {RuntimeReason::ModelShadowOnly, "MODEL_SHADOW_ONLY",
     "shadow mode: the expert model was consulted and recorded but did not influence the ranking"},
};

const Entry* Find(RuntimeReason reason) {
  for (const Entry& entry : kEntries) {
    if (entry.reason == reason) return &entry;
  }
  return nullptr;
}
}  // namespace

const char* ToString(RuntimeReason reason) {
  const Entry* entry = Find(reason);
  return entry ? entry->text : "UNKNOWN";
}

const char* Meaning(RuntimeReason reason) {
  const Entry* entry = Find(reason);
  return entry ? entry->meaning : "unknown reason";
}

std::string ReasonsText(const std::vector<RuntimeReason>& reasons) {
  std::string text;
  for (RuntimeReason reason : reasons) text += (text.empty() ? "" : ",") + std::string(ToString(reason));
  return text;
}

const std::vector<RuntimeReason>& AllRuntimeReasons() {
  static const std::vector<RuntimeReason> all = [] {
    std::vector<RuntimeReason> v;
    for (const Entry& entry : kEntries) v.push_back(entry.reason);
    return v;
  }();
  return all;
}

}  // namespace aafr::runtime
