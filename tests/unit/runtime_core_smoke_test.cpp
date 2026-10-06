#include <cassert>
#include <chrono>

#include "runtime/decision/weighted_decision_model.h"
#include "runtime/transition/strategy_transition_manager.h"

int main() {
  ngr::PolicyRegistry registry;
  registry.SetDecisionModelVersion("test-seed");
  registry.SetPolicy(
      ngr::SubsystemId::Forwarding,
      {"connection_reuse",
       {
           {"connection_reuse", {{"latency_score", 0.3}, {"reliability_score", 0.4}}},
           {"multiplexing", {{"throughput_score", 0.5}, {"request_burst_score", 0.2}}},
       }});

  ngr::EnvironmentSnapshot snapshot;
  snapshot.latency_ms = 40.0;
  snapshot.throughput_mbps = 80.0;
  snapshot.packet_loss_pct = 0.1;
  snapshot.request_burst_rate = 40.0;
  snapshot.stability_confidence = 0.95;

  ngr::WeightedDecisionModel model;
  const ngr::DecisionResult decision =
      model.Evaluate(snapshot, ngr::SubsystemId::Forwarding, registry);

  assert(decision.confidence == 0.95);
  assert(!decision.ranked.empty());

  ngr::TransitionState state;
  state.active = "connection_reuse";
  state.active_since = std::chrono::system_clock::now() - std::chrono::seconds(30);
  state.last_switch_at = std::chrono::system_clock::now() - std::chrono::seconds(30);

  ngr::TransitionGateConfig config;
  config.confidence_threshold = 0.5;

  ngr::StrategyTransitionManager transition_manager;
  const ngr::TransitionDecision transition =
      transition_manager.Evaluate(decision, state, config);

  assert(transition.source.ranked.size() == decision.ranked.size());
  assert(transition.gates.confidence_ok);

  return 0;
}
