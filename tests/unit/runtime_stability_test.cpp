// V0.6 stability gates, reason codes and lifecycle transitions.

#include "runtime/adaptive/stability_controller.h"

#include <set>

#include "runtime/adaptive/reason_codes.h"
#include "runtime/adaptive/runtime_types.h"
#include "tests/support/runtime_test_support.h"
#include "tests/support/test_support.h"

using namespace aafr;
using namespace aafr::runtime;
using aafr::test::kPathA;
using aafr::test::kPathB;

namespace {

constexpr std::int64_t kMs = 1'000'000;

StabilityConfig Config() {
  StabilityConfig c;
  c.min_dwell_ms = 1000;
  c.cooldown_ms = 2000;
  c.failed_activation_cooldown_ms = 500;
  c.candidate_backoff_initial_ms = 1000;
  c.candidate_backoff_max_ms = 4000;
  c.improvement_margin_relative = 0.10;
  c.min_improvement_ms = 0.5;
  c.estimate_margin_relative = 0.20;
  c.sustain_evaluations = 2;
  return c;
}

SwitchProposal Measured(const forwarding::Path& path, double improvement, double lower, double ms) {
  SwitchProposal p;
  p.candidate = algorithms::AlgorithmId::Dijkstra;
  p.path = path;
  p.evidence = EvidenceKind::Measured;
  p.evidence_valid = true;
  p.measured_improvement_relative = improvement;
  p.measured_improvement_lower = lower;
  p.measured_improvement_ms = ms;
  return p;
}

bool Blocks(const StabilityAssessment& a, RuntimeReason r) {
  return std::find(a.blocking.begin(), a.blocking.end(), r) != a.blocking.end();
}

void DwellBlockedThenSatisfied() {
  SwitchingStabilityController s(Config());
  s.OnInitialActivation(0);
  auto a = s.Assess(Measured(kPathB, 0.3, 0.2, 5.0), 400 * kMs);
  CHECK(!a.dwell.passed);
  CHECK_NEAR(*a.dwell.remaining_ms, 600.0, 1e-6);
  CHECK(Blocks(a, RuntimeReason::MinimumDwellNotMet));
  a = s.Assess(Measured(kPathB, 0.3, 0.2, 5.0), 1200 * kMs);
  CHECK(a.dwell.passed);
  CHECK(a.hysteresis.passed);  // second consecutive preference
  CHECK(a.allowed);
}

void CooldownBlockedThenSatisfied() {
  SwitchingStabilityController s(Config());
  s.OnInitialActivation(0);
  s.OnCommitted(5000 * kMs);  // a switch at t = 5 s
  s.Assess(Measured(kPathA, 0.3, 0.2, 5.0), 6100 * kMs);
  auto a = s.Assess(Measured(kPathA, 0.3, 0.2, 5.0), 6200 * kMs);
  CHECK(a.dwell.passed);       // 1.2 s since the switch
  CHECK(!a.cooldown.passed);   // cooldown 2 s
  CHECK(Blocks(a, RuntimeReason::CooldownActive));
  CHECK_NEAR(s.CooldownRemainingMs(6200 * kMs), 800.0, 1e-6);
  a = s.Assess(Measured(kPathA, 0.3, 0.2, 5.0), 7100 * kMs);
  CHECK(a.cooldown.passed);
  CHECK(a.allowed);
}

void FailedActivationCooldownAndBackoff() {
  SwitchingStabilityController s(Config());
  s.OnInitialActivation(0);
  s.OnActivationFailed(kPathB, 2000 * kMs);
  // Ordinary switching is blocked for 500 ms after any failed activation.
  CHECK_NEAR(s.CooldownRemainingMs(2100 * kMs), 400.0, 1e-6);
  // The failed candidate backs off for 1 s, then 2 s, ... (max 4 s).
  CHECK_NEAR(s.BackoffRemainingMs(kPathB, 2000 * kMs), 1000.0, 1e-6);
  s.OnActivationFailed(kPathB, 3500 * kMs);
  CHECK_NEAR(s.BackoffRemainingMs(kPathB, 3500 * kMs), 2000.0, 1e-6);
  s.OnActivationFailed(kPathB, 6000 * kMs);
  s.OnActivationFailed(kPathB, 11000 * kMs);
  CHECK_NEAR(s.BackoffRemainingMs(kPathB, 11000 * kMs), 4000.0, 1e-6);  // capped
  // Other candidates are unaffected; the backoff always expires.
  CHECK(s.BackoffRemainingMs(kPathA, 11000 * kMs) == 0.0);
  CHECK(s.BackoffRemainingMs(kPathB, 15001 * kMs) == 0.0);
  s.Assess(Measured(kPathB, 0.3, 0.2, 5.0), 16000 * kMs);
  const auto a = s.Assess(Measured(kPathB, 0.3, 0.2, 5.0), 16100 * kMs);
  CHECK(a.backoff.passed);
  CHECK(a.allowed);
}

void ImprovementMargin() {
  SwitchingStabilityController s(Config());
  s.OnInitialActivation(0);
  // Measured 5% < 10% margin.
  s.Assess(Measured(kPathB, 0.05, 0.01, 5.0), 2000 * kMs);
  auto a = s.Assess(Measured(kPathB, 0.05, 0.01, 5.0), 2100 * kMs);
  CHECK(!a.margin.passed);
  CHECK(Blocks(a, RuntimeReason::ImprovementMarginNotMet));
  // 30% but the confidence bound crosses zero.
  a = s.Assess(Measured(kPathB, 0.3, -0.01, 5.0), 2200 * kMs);
  CHECK(!a.margin.passed);
  // 30% relative but only 0.2 ms per request.
  a = s.Assess(Measured(kPathB, 0.3, 0.2, 0.2), 2300 * kMs);
  CHECK(!a.margin.passed);
  a = s.Assess(Measured(kPathB, 0.3, 0.2, 5.0), 2400 * kMs);
  CHECK(a.margin.passed);
  CHECK(a.allowed);
  // Estimate only: the larger 20% margin applies.
  SwitchProposal e;
  e.path = kPathA;
  e.evidence = EvidenceKind::EstimateOnly;
  e.evidence_valid = true;
  e.estimated_improvement_relative = 0.15;
  s.Assess(e, 2500 * kMs);
  a = s.Assess(e, 2600 * kMs);
  CHECK(!a.margin.passed);
  e.estimated_improvement_relative = 0.25;
  a = s.Assess(e, 2700 * kMs);
  CHECK(a.margin.passed);
  // Reliability repair passes the margin on failure-rate evidence.
  SwitchProposal r = Measured(kPathA, -0.1, -0.2, -1.0);
  r.evidence = EvidenceKind::ReliabilityRepair;
  a = s.Assess(r, 2800 * kMs);
  CHECK(a.margin.passed);
}

void Hysteresis() {
  SwitchingStabilityController s(Config());
  s.OnInitialActivation(0);
  auto a = s.Assess(Measured(kPathB, 0.3, 0.2, 5.0), 2000 * kMs);
  CHECK(!a.hysteresis.passed);
  CHECK(Blocks(a, RuntimeReason::HysteresisNotSatisfied));
  // A different candidate restarts the streak.
  a = s.Assess(Measured(kPathA, 0.3, 0.2, 5.0), 2100 * kMs);
  CHECK(!a.hysteresis.passed);
  CHECK(s.streak() == 1);
  // An evaluation without a challenger resets it too.
  s.NoChallenger();
  a = s.Assess(Measured(kPathA, 0.3, 0.2, 5.0), 2200 * kMs);
  CHECK(!a.hysteresis.passed);
  a = s.Assess(Measured(kPathA, 0.3, 0.2, 5.0), 2300 * kMs);
  CHECK(a.hysteresis.passed);
  CHECK(a.allowed);
}

void SafetyOverride() {
  SwitchingStabilityController s(Config());
  s.OnInitialActivation(0);
  s.OnCommitted(100 * kMs);
  SwitchProposal p;
  p.path = kPathA;
  p.evidence = EvidenceKind::EstimateOnly;
  p.evidence_valid = true;
  p.safety_reason = RuntimeReason::ActivePathFailed;
  // 150 ms after a switch: dwell and cooldown would block; margin none; streak 1.
  auto a = s.Assess(p, 150 * kMs);
  CHECK(a.safety_override);
  CHECK(a.allowed);
  CHECK(!a.dwell.applicable && !a.cooldown.applicable && !a.margin.applicable && !a.hysteresis.applicable);
  CHECK(aafr::test::Contains(a.dwell.detail, "ACTIVE_PATH_FAILED"));
  // Evidence and backoff still apply.
  p.evidence_valid = false;
  a = s.Assess(p, 160 * kMs);
  CHECK(!a.allowed);
  p.evidence_valid = true;
  s.OnActivationFailed(kPathA, 170 * kMs);
  a = s.Assess(p, 180 * kMs);
  CHECK(!a.allowed);
  CHECK(Blocks(a, RuntimeReason::CandidateBackoff));
}

void DisabledConfigAllowsImmediately() {
  SwitchingStabilityController s(StabilityConfig::Disabled());
  s.OnInitialActivation(0);
  const auto a = s.Assess(Measured(kPathB, 0.01, 0.001, 0.01), 1 * kMs);
  CHECK(a.allowed);
  CHECK(StabilityConfig::Disabled().Validate().empty());
  StabilityConfig bad;
  bad.sustain_evaluations = 0;
  CHECK(!bad.Validate().empty());
}

void BidirectionalGatesAreSymmetric() {
  // A -> B after the gates pass, then B -> A under the same rules: the
  // controller holds no memory that could lock either direction out.
  SwitchingStabilityController s(Config());
  s.OnInitialActivation(0);
  s.Assess(Measured(kPathB, 0.5, 0.4, 20.0), 1500 * kMs);
  CHECK(s.Assess(Measured(kPathB, 0.5, 0.4, 20.0), 1600 * kMs).allowed);
  s.OnCommitted(1700 * kMs);
  CHECK(!s.Assess(Measured(kPathA, 0.5, 0.4, 20.0), 2000 * kMs).allowed);  // dwell, cooldown
  CHECK(s.Assess(Measured(kPathA, 0.5, 0.4, 20.0), 3800 * kMs).allowed);
}

void ReasonCodesAreStableAndUnique() {
  std::set<std::string> texts;
  for (RuntimeReason r : AllRuntimeReasons()) {
    const std::string text = ToString(r);
    CHECK(text != "UNKNOWN");
    CHECK(std::string(Meaning(r)) != "unknown reason");
    CHECK(texts.insert(text).second);
  }
  CHECK(texts.size() == AllRuntimeReasons().size());
  // Spot-check the names the specification lists.
  for (const char* name : {"INITIAL_ACTIVATION", "NETWORK_CHANGE_MEANINGFUL", "NETWORK_CHANGE_IGNORED",
                           "CONSTRAINT_CHANGED", "CANDIDATE_INELIGIBLE", "RECOMMENDATION_UNAVAILABLE",
                           "CANDIDATE_SAME_AS_ACTIVE", "PROBING_UNAVAILABLE", "PROBE_TIMEOUT", "INSUFFICIENT_SAMPLES",
                           "STATISTICAL_EVIDENCE_INVALID", "STATISTICAL_EVIDENCE_INSUFFICIENT", "IMPROVEMENT_CONFIRMED",
                           "IMPROVEMENT_MARGIN_NOT_MET", "MINIMUM_DWELL_NOT_MET", "COOLDOWN_ACTIVE",
                           "HYSTERESIS_NOT_SATISFIED", "ACTIVE_PATH_FAILED", "SAFETY_OVERRIDE", "SWITCH_REQUESTED",
                           "SWITCH_COMMITTED", "SWITCH_FAILED", "RECOVERY_REQUESTED", "RECOVERY_SUCCEEDED",
                           "RECOVERY_FAILED", "ACTIVE_CONFIGURATION_UNKNOWN"}) {
    CHECK(texts.count(name) == 1);
  }
  CHECK(ReasonsText({RuntimeReason::SwitchRequested, RuntimeReason::SwitchCommitted}) ==
        "SWITCH_REQUESTED,SWITCH_COMMITTED");
}

void LifecycleTransitions() {
  using L = RuntimeLifecycle;
  CHECK(IsValidTransition(L::Uninitialized, L::Initializing));
  CHECK(IsValidTransition(L::Initializing, L::Ready));
  CHECK(IsValidTransition(L::Initializing, L::Degraded));
  CHECK(IsValidTransition(L::Initializing, L::Failed));
  CHECK(IsValidTransition(L::Ready, L::Evaluating));
  CHECK(IsValidTransition(L::Evaluating, L::Switching));
  CHECK(IsValidTransition(L::Evaluating, L::Ready));
  CHECK(IsValidTransition(L::Switching, L::Ready));
  CHECK(IsValidTransition(L::Switching, L::Degraded));
  CHECK(IsValidTransition(L::Degraded, L::Evaluating));
  CHECK(IsValidTransition(L::Ready, L::Stopping));
  CHECK(IsValidTransition(L::Switching, L::Stopping));
  CHECK(IsValidTransition(L::Stopping, L::Stopped));
  CHECK(IsValidTransition(L::Failed, L::Stopping));
  // Invalid ones.
  CHECK(!IsValidTransition(L::Ready, L::Switching));  // no switch without an evaluation
  CHECK(!IsValidTransition(L::Uninitialized, L::Ready));
  CHECK(!IsValidTransition(L::Stopped, L::Ready));
  CHECK(!IsValidTransition(L::Stopped, L::Initializing));
  CHECK(!IsValidTransition(L::Degraded, L::Switching));
  CHECK(!IsValidTransition(L::Failed, L::Ready));
  CHECK(!IsValidTransition(L::Ready, L::Ready));
  CHECK(Priority(EvaluationTrigger::FailureRecovery) > Priority(EvaluationTrigger::MeaningfulChange));
  CHECK(IsSafetyTrigger(EvaluationTrigger::FailureRecovery));
  CHECK(!IsSafetyTrigger(EvaluationTrigger::Manual));
}

}  // namespace

int main() {
  return aafr::test::RunTests({
      {"dwell_blocked_then_satisfied", DwellBlockedThenSatisfied},
      {"cooldown_blocked_then_satisfied", CooldownBlockedThenSatisfied},
      {"failed_activation_cooldown_and_backoff", FailedActivationCooldownAndBackoff},
      {"improvement_margin", ImprovementMargin},
      {"hysteresis", Hysteresis},
      {"safety_override", SafetyOverride},
      {"disabled_config_allows_immediately", DisabledConfigAllowsImmediately},
      {"bidirectional_gates_are_symmetric", BidirectionalGatesAreSymmetric},
      {"reason_codes_are_stable_and_unique", ReasonCodesAreStableAndUnique},
      {"lifecycle_transitions", LifecycleTransitions},
  });
}
