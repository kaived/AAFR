#include <chrono>
#include <iostream>

#include "runtime/decision/weighted_decision_model.h"
#include "runtime/telemetry/jsonl_telemetry_sink.h"
#include "runtime/transition/strategy_transition_manager.h"

namespace {

ngr::PolicyRegistry BuildSeedPolicy() {
  ngr::PolicyRegistry registry;
  registry.SetDecisionModelVersion("aafr-seed-0.1");

  registry.SetPolicy(
      ngr::SubsystemId::Forwarding,
      {"connection_reuse",
       {
           {"connection_reuse",
            {{"connection_reuse_benefit", 0.45},
             {"latency_score", 0.20},
             {"reliability_score", 0.20},
             {"retry_risk", -0.15}}},
           {"multiplexing",
            {{"multiplexing_headroom", 0.45},
             {"throughput_score", 0.25},
             {"request_burst_score", 0.20},
             {"retry_risk", -0.10}}},
           {"retry_backoff",
            {{"reliability_score", 0.35},
             {"jitter_stability_score", 0.20},
             {"retry_risk", -0.35},
             {"latency_score", 0.10}}},
       }});

  return registry;
}

}  // namespace

int main() {
  const ngr::PolicyRegistry registry = BuildSeedPolicy();

  ngr::EnvironmentSnapshot snapshot;
  snapshot.latency_ms = 120.0;
  snapshot.throughput_mbps = 8.0;
  snapshot.packet_loss_pct = 1.2;
  snapshot.jitter_ms = 25.0;
  snapshot.cpu_load_pct = 32.0;
  snapshot.mem_pressure_pct = 48.0;
  snapshot.open_connections = 6;
  snapshot.request_burst_rate = 18.0;
  snapshot.object_reuse_ratio = 0.42;
  snapshot.cache_locality_score = 0.60;
  snapshot.content_class = ngr::ContentClass::Text;
  snapshot.stability_confidence = 0.78;

  ngr::WeightedDecisionModel model;
  ngr::DecisionResult decision =
      model.Evaluate(snapshot, ngr::SubsystemId::Forwarding, registry);

  ngr::TransitionState state;
  state.active = "connection_reuse";
  state.active_since = std::chrono::system_clock::now() - std::chrono::seconds(10);
  state.last_switch_at = std::chrono::system_clock::now() - std::chrono::seconds(10);
  state.active_score_ewma = 0.0;

  ngr::TransitionGateConfig gate_config;
  ngr::StrategyTransitionManager transition_manager;
  ngr::TransitionDecision transition =
      transition_manager.Evaluate(decision, state, gate_config);

  ngr::JsonlTelemetrySink telemetry("aafr-browser-demo.jsonl");
  telemetry.Record(transition);

  std::cout << "AAFR browser seed\n";
  std::cout << "decision_model_version=" << registry.DecisionModelVersion() << "\n";
  if (!decision.ranked.empty()) {
    std::cout << "best_forwarding_strategy=" << decision.ranked.front().id << "\n";
    std::cout << "score=" << decision.ranked.front().score << "\n";
  }
  std::cout << "transition_outcome=" << ngr::ToString(transition.outcome) << "\n";
  std::cout << "telemetry=aafr-browser-demo.jsonl\n";

  return 0;
}
