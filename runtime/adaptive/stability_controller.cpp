#include "runtime/adaptive/stability_controller.h"

#include <algorithm>
#include <cmath>
#include <sstream>

#include "runtime/adaptive/runtime_clock.h"

namespace aafr::runtime {

std::vector<std::string> StabilityConfig::Validate() const {
  std::vector<std::string> issues;
  const auto nonneg = [&](double v, const char* name) {
    if (!(v >= 0.0) || !std::isfinite(v)) issues.push_back(std::string(name) + " must be finite and nonnegative");
  };
  nonneg(min_dwell_ms, "min_dwell_ms");
  nonneg(cooldown_ms, "cooldown_ms");
  nonneg(failed_activation_cooldown_ms, "failed_activation_cooldown_ms");
  nonneg(candidate_backoff_initial_ms, "candidate_backoff_initial_ms");
  nonneg(min_improvement_ms, "min_improvement_ms");
  nonneg(improvement_margin_relative, "improvement_margin_relative");
  nonneg(estimate_margin_relative, "estimate_margin_relative");
  if (candidate_backoff_max_ms < candidate_backoff_initial_ms) issues.push_back("candidate_backoff_max_ms below initial");
  if (improvement_margin_relative >= 1.0 || estimate_margin_relative >= 1.0) issues.push_back("margins must be below 1");
  if (sustain_evaluations < 1) issues.push_back("sustain_evaluations must be at least 1");
  return issues;
}

StabilityConfig StabilityConfig::Disabled() {
  StabilityConfig c;
  c.min_dwell_ms = 0.0;
  c.cooldown_ms = 0.0;
  c.failed_activation_cooldown_ms = 0.0;
  c.candidate_backoff_initial_ms = 0.0;
  c.candidate_backoff_max_ms = 0.0;
  c.improvement_margin_relative = 0.0;
  c.min_improvement_ms = 0.0;
  c.estimate_margin_relative = 0.0;
  c.sustain_evaluations = 1;
  return c;
}

namespace {
std::string Fixed(double v, int precision = 1) {
  std::ostringstream out;
  out.setf(std::ios::fixed);
  out.precision(precision);
  out << v;
  return out.str();
}
std::string Percent(double v) { return Fixed(v * 100.0, 1) + "%"; }
}  // namespace

double SwitchingStabilityController::DwellRemainingMs(std::int64_t now_ns) const {
  if (!active_since_ns_) return 0.0;
  return std::max(0.0, config_.min_dwell_ms - NsToMs(now_ns - *active_since_ns_));
}

double SwitchingStabilityController::CooldownRemainingMs(std::int64_t now_ns) const {
  double remaining = 0.0;
  if (last_switch_ns_) remaining = std::max(remaining, config_.cooldown_ms - NsToMs(now_ns - *last_switch_ns_));
  if (last_failure_ns_) {
    remaining = std::max(remaining, config_.failed_activation_cooldown_ms - NsToMs(now_ns - *last_failure_ns_));
  }
  return std::max(0.0, remaining);
}

double SwitchingStabilityController::BackoffRemainingMs(const forwarding::Path& path, std::int64_t now_ns) const {
  const auto found = backoff_.find(path);
  if (found == backoff_.end()) return 0.0;
  return std::max(0.0, NsToMs(found->second.until_ns - now_ns));
}

void SwitchingStabilityController::NoChallenger() {
  streak_ = 0;
  streak_path_.reset();
}

void SwitchingStabilityController::OnInitialActivation(std::int64_t now_ns) {
  active_since_ns_ = now_ns;
  NoChallenger();
}

void SwitchingStabilityController::OnCommitted(std::int64_t now_ns) {
  active_since_ns_ = now_ns;
  last_switch_ns_ = now_ns;
  NoChallenger();
}

void SwitchingStabilityController::OnActivationFailed(const forwarding::Path& candidate_path, std::int64_t now_ns) {
  last_failure_ns_ = now_ns;
  Backoff& b = backoff_[candidate_path];
  b.next_ms = b.next_ms <= 0.0 ? config_.candidate_backoff_initial_ms
                               : std::min(config_.candidate_backoff_max_ms, b.next_ms * 2.0);
  b.until_ns = now_ns + MsToNs(b.next_ms);
  NoChallenger();
}

StabilityAssessment SwitchingStabilityController::Assess(const SwitchProposal& p, std::int64_t now_ns) {
  StabilityAssessment a;
  a.safety_override = p.safety_reason.has_value();

  // Hysteresis streak (advanced for every assessed proposal).
  if (streak_path_ && *streak_path_ == p.path) ++streak_;
  else {
    streak_path_ = p.path;
    streak_ = 1;
  }

  // Backoff: applies to every switch, including safety overrides.
  const double backoff = BackoffRemainingMs(p.path, now_ns);
  a.backoff.passed = backoff <= 0.0;
  a.backoff.remaining_ms = backoff;
  a.backoff.detail = a.backoff.passed ? "candidate not backing off"
                                      : "candidate failed activation; retry in " + Fixed(backoff) + " ms";

  // Evidence validity: applies to every switch.
  a.evidence.passed = p.evidence_valid && p.evidence != EvidenceKind::None;
  a.evidence.detail = std::string(ToString(p.evidence)) + (p.evidence_valid ? ", valid" : ", not valid") +
                      (p.evidence_note.empty() ? "" : ": " + p.evidence_note);

  const double dwell = DwellRemainingMs(now_ns);
  const double cooldown = CooldownRemainingMs(now_ns);
  if (a.safety_override) {
    for (GateResult* g : {&a.dwell, &a.cooldown, &a.margin, &a.hysteresis}) {
      g->applicable = false;
      g->passed = true;
      g->detail = std::string("bypassed by safety override (") + ToString(*p.safety_reason) + ")";
    }
    a.dwell.remaining_ms = dwell;
    a.cooldown.remaining_ms = cooldown;
  } else {
    a.dwell.passed = dwell <= 0.0;
    a.dwell.remaining_ms = dwell;
    a.dwell.detail = a.dwell.passed ? "active for at least " + Fixed(config_.min_dwell_ms) + " ms"
                                    : Fixed(dwell) + " ms of minimum dwell remaining";
    a.cooldown.passed = cooldown <= 0.0;
    a.cooldown.remaining_ms = cooldown;
    a.cooldown.detail = a.cooldown.passed ? "no cooldown active" : Fixed(cooldown) + " ms of cooldown remaining";

    switch (p.evidence) {
      case EvidenceKind::Measured: {
        const double rel = p.measured_improvement_relative.value_or(-1.0);
        const double lower = p.measured_improvement_lower.value_or(-1.0);
        const double ms = p.measured_improvement_ms.value_or(0.0);
        a.margin.passed = rel >= config_.improvement_margin_relative && lower > 0.0 && ms >= config_.min_improvement_ms;
        a.margin.detail = "measured improvement " + Percent(rel) + " (bound " + Percent(lower) + ", " +
                          Fixed(ms, 3) + " ms) vs required " + Percent(config_.improvement_margin_relative) +
                          " and " + Fixed(config_.min_improvement_ms, 3) + " ms";
        break;
      }
      case EvidenceKind::ReliabilityRepair:
        a.margin.passed = true;
        a.margin.detail = "incumbent exceeds the failure budget; challenger is within it";
        break;
      case EvidenceKind::EstimateOnly: {
        const double rel = p.estimated_improvement_relative.value_or(-1.0);
        a.margin.passed = rel >= config_.estimate_margin_relative;
        a.margin.detail = "estimated J(P) improvement " + Percent(rel) + " vs required " +
                          Percent(config_.estimate_margin_relative) + " (no measurement)";
        break;
      }
      case EvidenceKind::None:
        a.margin.passed = false;
        a.margin.detail = "no evidence of improvement";
        break;
    }
    a.hysteresis.passed = streak_ >= config_.sustain_evaluations;
    a.hysteresis.detail = "preferred in " + std::to_string(streak_) + " of " +
                          std::to_string(config_.sustain_evaluations) + " required consecutive evaluations";
  }

  if (!a.evidence.passed) {
    a.blocking.push_back(p.evidence == EvidenceKind::None ? RuntimeReason::StatisticalEvidenceInsufficient
                                                          : RuntimeReason::StatisticalEvidenceInvalid);
  }
  if (!a.backoff.passed) a.blocking.push_back(RuntimeReason::CandidateBackoff);
  if (!a.dwell.passed) a.blocking.push_back(RuntimeReason::MinimumDwellNotMet);
  if (!a.cooldown.passed) a.blocking.push_back(RuntimeReason::CooldownActive);
  if (!a.margin.passed) a.blocking.push_back(RuntimeReason::ImprovementMarginNotMet);
  if (!a.hysteresis.passed) a.blocking.push_back(RuntimeReason::HysteresisNotSatisfied);
  a.allowed = a.blocking.empty();

  std::string text;
  for (const GateResult& g : a.Gates()) {
    text += (text.empty() ? "" : "; ") + g.gate + (g.applicable ? (g.passed ? " passed" : " blocked") : " bypassed") +
            " (" + g.detail + ")";
  }
  a.explanation = text;
  return a;
}

const char* ToString(EvidenceKind value) {
  switch (value) {
    case EvidenceKind::None: return "none";
    case EvidenceKind::EstimateOnly: return "estimate_only";
    case EvidenceKind::Measured: return "measured";
    case EvidenceKind::ReliabilityRepair: return "reliability_repair";
  }
  return "unknown";
}

}  // namespace aafr::runtime
