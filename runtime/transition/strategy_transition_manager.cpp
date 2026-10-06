#include "runtime/transition/strategy_transition_manager.h"

#include <cmath>

namespace ngr {

TransitionDecision StrategyTransitionManager::Evaluate(
    const DecisionResult& proposal,
    const TransitionState& state,
    const TransitionGateConfig& config) const {
  TransitionDecision decision;
  decision.subsystem = proposal.subsystem;
  decision.active_before = state.active;
  decision.active_after = state.active;
  decision.source = proposal;

  if (proposal.ranked.empty()) {
    decision.outcome = TransitionOutcome::DeferredConfidence;
    return decision;
  }

  const ScoredCandidate& best = proposal.ranked.front();
  if (best.id == state.active) {
    decision.active_after = state.active;
    decision.outcome = TransitionOutcome::Applied;
    decision.gates = {true, true, true, true};
    return decision;
  }

  const Timestamp now = proposal.evaluated_at;
  decision.gates.dwell_ok = now - state.active_since >= config.dwell_min;

  double active_score = state.active_score_ewma;
  for (const ScoredCandidate& candidate : proposal.ranked) {
    if (candidate.id == state.active) {
      active_score = candidate.score;
      break;
    }
  }

  const double required_margin =
      std::max(0.0001, std::abs(active_score) * config.hysteresis_margin_ratio);
  decision.gates.hysteresis_ok = (best.score - active_score) >= required_margin;
  decision.gates.confidence_ok = proposal.confidence >= config.confidence_threshold;
  decision.gates.cooldown_ok = now - state.last_switch_at >= config.cooldown;

  if (!decision.gates.dwell_ok) {
    decision.outcome = TransitionOutcome::DeferredDwell;
    return decision;
  }
  if (!decision.gates.hysteresis_ok) {
    decision.outcome = TransitionOutcome::DeferredHysteresis;
    return decision;
  }
  if (!decision.gates.confidence_ok) {
    decision.outcome = TransitionOutcome::DeferredConfidence;
    return decision;
  }
  if (!decision.gates.cooldown_ok) {
    decision.outcome = TransitionOutcome::DeferredCooldown;
    return decision;
  }

  decision.active_after = best.id;
  decision.outcome = TransitionOutcome::Applied;
  return decision;
}

}  // namespace ngr
