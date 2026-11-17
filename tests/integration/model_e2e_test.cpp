// V0.7.2 controlled end-to-end verification with the REAL expert model:
// persistent Python worker + pinned artifacts + AIRecommendationProvider +
// AAFRRuntime + V0.5 algorithms + real loopback TCP relays.
//
//   1-3  start the worker, load and verify the artifacts
//   4    a NetworkState snapshot is available to the runtime
//   5-13 model recommendation -> mapping -> existing algorithm -> validation
//        -> probing/arbitration -> gates -> activation -> confirmation
//        (relay logs) -> DecisionRecord -> outcome observation
//   +    a valid model prediction that the runtime correctly retains
//   14-15 worker killed -> deterministic fallback, runtime keeps forwarding
//   16-17 worker restored -> controlled recovery of AI recommendations
//
// The telemetry the model needs (23 of its 35 inputs) has no AAFR
// measurement source yet, so two CONFIGURED profiles are used, copied from
// rows of the model's own training data: row 1992-like "calm" (the model
// answers Dijkstra) and row 2614 (the model answers WOA). They are labelled
// configured in every trace. This test verifies integration, not that the
// model improves real browser performance.

#include "runtime/adaptive/aafr_runtime.h"

#include <algorithm>
#include <thread>

#include "routing/forwarding/loopback_forwarding.h"
#include "runtime/model/ai_recommendation_provider.h"
#include "tests/support/model_test_support.h"
#include "tests/support/runtime_test_support.h"
#include "tests/support/test_support.h"

using namespace aafr;
using namespace aafr::runtime;
using namespace aafr::model;
using algorithms::AlgorithmId;
using aafr::test::kPathA;
using aafr::test::kPathB;

using aafr::test::Contains;

namespace {

void Sleep(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

bool Has(const std::vector<RuntimeReason>& reasons, RuntimeReason r) {
  return std::find(reasons.begin(), reasons.end(), r) != reasons.end();
}

std::map<std::string, double> CalmProfile() {  // training row with the lowest gate on this graph
  return {{"latency", 5.624}, {"jitter", 1.548}, {"loss", 0.00183}, {"throughput", 92.177},
          {"mean_link_utilization", 0.054}, {"max_link_utilization", 3.993}, {"link_utilization_std", 0.291},
          {"avg_queue_delay_ms", 0.001}, {"max_queue_depth", 3.0}, {"avg_queue_occupancy", 0.0004},
          {"offered_load_mbps", 92.379}, {"packet_delivery_ratio", 0.99786}, {"jains_fairness_index", 0.9684},
          {"latency_p50", 5.994}, {"latency_p90", 7.354}, {"latency_p95", 7.421}, {"latency_p99", 7.608},
          {"latency_min", 2.201}, {"latency_max", 7.689}, {"latency_std", 1.438}, {"link_failure_events", 0.0},
          {"link_recovery_events", 0.0}, {"reroute_count", 0.0}};
}

std::map<std::string, double> DynamicProfile() {  // training row 2614 (F_Dynamic-like churn)
  return {{"latency", 18.904}, {"jitter", 4.847}, {"loss", 0.02633}, {"throughput", 20.281},
          {"mean_link_utilization", 0.563}, {"max_link_utilization", 4.225}, {"link_utilization_std", 0.914},
          {"avg_queue_delay_ms", 0.001}, {"max_queue_depth", 2.0}, {"avg_queue_occupancy", 0.0033},
          {"offered_load_mbps", 20.901}, {"packet_delivery_ratio", 0.97097}, {"jains_fairness_index", 0.9717},
          {"latency_p50", 19.755}, {"latency_p90", 25.136}, {"latency_p95", 25.138}, {"latency_p99", 25.622},
          {"latency_min", 10.464}, {"latency_max", 25.778}, {"latency_std", 4.203}, {"link_failure_events", 6.0},
          {"link_recovery_events", 6.0}, {"reroute_count", 12.0}};
}

forwarding::ForwardingGraph SlowTwoPathGraph() {
  aafr::test::GraphHolder holder(aafr::test::TwoPathGraph());
  holder.SetLatency("entry", "relay_a", 5.0);
  holder.SetLatency("relay_a", "dest", 5.0);
  holder.SetLatency("entry", "relay_b", 25.0);
  holder.SetLatency("relay_b", "dest", 25.0);
  return *holder.Get();
}

DecisionRecord Last(const AAFRRuntime& rt) { return rt.Decisions(1).back(); }

void ControlledEndToEnd() {
  WorkerConfig wc = aafr::test::RealWorkerConfig();
  if (!wc.Validate().empty()) {
    std::cout << "SKIP controlled_end_to_end: real model not configured\n";
    return;
  }
  wc.request_timeout_ms = 2000.0;
  wc.restart_backoff_ms = 100.0;

  // 1-3. Worker and artifacts.
  ModelEventLog events;
  ModelWorkerClient worker(wc, &events);
  CHECK(worker.Start());
  CHECK(worker.WaitUntilReady(60'000));
  if (worker.Status().state != WorkerState::Ready) {
    std::cout << "worker: " << worker.Status().detail << "\n";
    return;
  }
  CHECK(events.Count(ModelEventType::ModelLoaded) == 1 && events.Count(ModelEventType::ModelReady) == 1);

  aafr::test::GraphHolder graph(SlowTwoPathGraph());
  forwarding::LoopbackOptions options;
  options.inject_edge_latency = true;
  forwarding::LoopbackForwardingNetwork network(*graph.Get(), options);
  std::string error;
  CHECK(network.Start(&error));
  if (!network.Running()) return;
  forwarding::LoopbackForwardingExecutor executor(network);
  algorithms::AlgorithmExecutionEngine engine(nullptr, aafr::test::VerifiedRegistry(), &executor);
  const auto mutate = [&](const char* from, const char* to, double ms) {
    graph.SetLatency(from, to, ms);
    std::string sync_error;
    CHECK(network.SyncWith(*graph.Get(), &sync_error));
  };

  ConfiguredTelemetrySource telemetry("controlled_profile");
  telemetry.SetProfile(CalmProfile(), "training_row_calm");
  AIProviderConfig pc;
  pc.mode = RecommendationMode::AiPreferred;
  AIRecommendationProvider provider(&worker, &telemetry, pc, &events);
  provider.SetContextValidator([&](const EvaluationContext& c) { return graph.Get()->version() == c.graph_version; });

  // 4. A NetworkState snapshot (host telemetry is not a model input here).
  auto snapshot = std::make_shared<netstate::StateSnapshot>();
  snapshot->state.state_id = 77;
  snapshot->state.primary_window = "short";
  snapshot->state.windows.push_back({"short", 10'000, {}});

  RuntimeConfig config;
  config.session_id = "e2e";
  config.scheduler.min_interval_ms = 100;
  config.stability.min_dwell_ms = 200;
  config.stability.cooldown_ms = 300;
  config.stability.sustain_evaluations = 2;
  config.probe.rounds = 16;
  config.probe.probe_timeout_ms = 2000;
  config.health_check_interval_ms = 0;
  config.review_interval_ms = 0;
  RuntimeDependencies d;
  d.graph_source = [&] { return graph.Get(); };
  d.problem_for = [](const forwarding::ForwardingGraph& g) { return aafr::test::ProblemFor(g); };
  d.state_source = [&] { return std::shared_ptr<const netstate::StateSnapshot>(snapshot); };
  d.executor = &executor;
  d.eligibility = &engine;
  d.provider = &provider;
  d.executor_injects_delay = true;
  AAFRRuntime runtime(std::move(d), config);

  // 5-13 (initial). The real model answers Dijkstra for the calm profile.
  CHECK(runtime.Initialize());
  DecisionRecord r = Last(runtime);
  CHECK(r.network_state_id == 77);
  CHECK(r.model && r.model->inference_status == "ok");
  CHECK(r.model->model_version == "lgbm2stage-20261027_220608");
  CHECK(r.model->model_label == "Dijkstra" && !r.model->stage2_ran);
  CHECK(r.model->gate_probability && *r.model->gate_probability < *r.model->gate_threshold);
  CHECK(Contains(r.model->feature_summary, "graph=12") && Contains(r.model->feature_summary, "configured=23"));
  CHECK(r.recommendation_source == "expert_model");
  CHECK(r.activation == ActivationStatus::Confirmed);
  CHECK(network.NodeLogged("relay_a", r.activation_request_id));  // confirmed by the relays themselves
  CHECK(runtime.Status().active->algorithm == AlgorithmId::Dijkstra);
  std::cout << "  initial: model " << r.model->model_label << " gate " << *r.model->gate_probability << ", round trip "
            << r.model->round_trip_ms.value_or(-1) << " ms (model " << r.model->inference_ms.value_or(-1)
            << " ms, features " << r.model->feature_ms.value_or(-1) << " ms)\n";

  // A valid model prediction that the runtime correctly retains: the
  // dynamic profile makes the model choose WOA, but WOA's path is the active
  // one, so nothing justifies a switch.
  telemetry.SetProfile(DynamicProfile(), "training_row_2614");
  Sleep(150);
  runtime.RequestEvaluation(EvaluationTrigger::Manual, "e2e retain check");
  runtime.Step();
  r = Last(runtime);
  CHECK(r.model->model_label == "WOA" && r.model->stage2_ran);
  CHECK(r.model->predicted_algorithm == AlgorithmId::WhaleOptimizationAlgorithm);
  CHECK(r.type == DecisionType::Retain);
  CHECK(Has(r.reasons, RuntimeReason::CandidateSameAsActive));
  CHECK(r.confirmed_algorithm == AlgorithmId::Dijkstra);
  std::cout << "  retain: model WOA (gate " << *r.model->gate_probability << "), runtime retained dijkstra\n";

  // 6-13. Path A congests: WOA (model) on path B is probed, must show
  // measured improvement, passes hysteresis on the second evaluation, and is
  // activated and confirmed.
  mutate("entry", "relay_a", 150.0);
  Sleep(150);
  runtime.Step();
  r = Last(runtime);
  CHECK(r.candidate_algorithm == AlgorithmId::WhaleOptimizationAlgorithm);
  CHECK(r.probing_attempted);
  bool switched = r.type == DecisionType::Switch;
  for (int i = 0; i < 4 && !switched; ++i) {
    Sleep(150);
    runtime.RequestEvaluation(EvaluationTrigger::Manual, "e2e follow-up");
    runtime.Step();
    r = Last(runtime);
    switched = r.type == DecisionType::Switch;
  }
  CHECK(switched);
  CHECK(Has(r.reasons, RuntimeReason::ImprovementConfirmed));
  CHECK(Has(r.reasons, RuntimeReason::ModelRecommendationUsed));
  CHECK(r.activation == ActivationStatus::Confirmed);
  CHECK(network.NodeLogged("relay_b", r.activation_request_id));
  CHECK(runtime.Status().active->algorithm == AlgorithmId::WhaleOptimizationAlgorithm);
  CHECK(runtime.Status().active->path == kPathB);
  const std::string switch_id = r.decision_id;
  std::cout << "  switch: " << r.explanation << "\n";

  // Outcome observation linked to the AI-assisted decision.
  for (int i = 0; i < 3; ++i) CHECK(runtime.ForwardActive("app-" + std::to_string(i), "hello", 2000).path_confirmed);
  const auto outcome = runtime.Outcome(switch_id);
  CHECK(outcome && outcome->application_requests == 3);

  // 14-15. The worker dies: deterministic fallback, forwarding continues.
  worker.KillWorkerForTesting();
  Sleep(300);
  telemetry.SetProfile(CalmProfile(), "training_row_calm");  // new features: no cache reuse
  runtime.RequestEvaluation(EvaluationTrigger::Manual, "after worker failure");
  runtime.Step();
  r = Last(runtime);
  CHECK(r.model->inference_status == "unavailable");
  CHECK(r.recommendation_source == "deterministic_fallback");
  CHECK(Has(r.reasons, RuntimeReason::ModelFallbackDeterministic));
  CHECK(runtime.Status().active_verified);
  CHECK(runtime.ForwardActive("app-after-crash", "hello", 2000).path_confirmed);
  CHECK(events.Count(ModelEventType::DeterministicFallbackActivated) >= 1);

  // 16-17. Controlled recovery: the bounded automatic restart brings the
  // worker back; AI recommendations resume.
  for (int i = 0; i < 100 && worker.Status().state != WorkerState::Ready; ++i) {
    runtime.RequestEvaluation(EvaluationTrigger::Manual, "while restarting");
    runtime.Step();
    Sleep(200);
  }
  CHECK(worker.Status().state == WorkerState::Ready);
  CHECK(worker.Status().restarts == 1);
  telemetry.SetProfile(DynamicProfile(), "training_row_2614");
  Sleep(150);
  runtime.RequestEvaluation(EvaluationTrigger::Manual, "after restart");
  runtime.Step();
  r = Last(runtime);
  CHECK(r.model->inference_status == "ok");
  CHECK(r.recommendation_source == "expert_model");
  std::cout << "  worker restart to ready " << worker.Status().last_restart_ms.value_or(-1) << " ms\n";
  CHECK(events.Count(ModelEventType::ModelWorkerRestarted) == 1);

  runtime.Stop();
  worker.Stop();
  network.Stop();
}

}  // namespace

int main() { return aafr::test::RunTests({{"controlled_end_to_end", ControlledEndToEnd}}); }
