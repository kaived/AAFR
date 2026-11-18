// V0.7.3 AI regression with real browser-style forwarding: the V0.7.2
// AIRecommendationProvider inside the real runtime over real loopback relays,
// with the browser forwarding gateway in front. In every mode the gateway
// forwards over the runtime's confirmed configuration only; a model
// prediction never reaches the gateway or the relays directly.
//
// The model is the scripted FakeModel (the real-model cases stay in
// model_e2e / model_worker): the point here is the control flow, not the
// model's accuracy, and no claim of AI benefit follows from these tests.

#include <memory>
#include <thread>

#include "routing/forwarding/loopback_forwarding.h"
#include "routing/gateway/browser_forwarding_gateway.h"
#include "routing/gateway/runtime_gateway_binding.h"
#include "runtime/adaptive/aafr_runtime.h"
#include "runtime/model/ai_recommendation_provider.h"
#include "tests/support/http_test_server.h"
#include "tests/support/model_test_support.h"
#include "tests/support/runtime_test_support.h"
#include "tests/support/test_support.h"

using namespace aafr;
using namespace aafr::runtime;
using namespace aafr::model;
using aafr::test::FakeModel;
using aafr::test::kPathA;
using aafr::test::kPathB;

namespace {

void Sleep(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

struct Bed {
  aafr::test::GraphHolder graph{aafr::test::TwoPathGraph()};
  std::unique_ptr<forwarding::LoopbackForwardingNetwork> network;
  std::unique_ptr<forwarding::LoopbackForwardingExecutor> executor;
  std::unique_ptr<algorithms::AlgorithmExecutionEngine> engine;
  ConfiguredTelemetrySource telemetry;
  FakeModel model;
  ModelEventLog events;
  std::unique_ptr<AIRecommendationProvider> provider;
  std::unique_ptr<AAFRRuntime> runtime;
  aafr::test::HttpTestServer origin;
  std::unique_ptr<gateway::AAFRBrowserForwardingGateway> gw;
  bool ok{false};

  explicit Bed(std::optional<RecommendationMode> mode) {
    forwarding::LoopbackOptions options;
    options.inject_edge_latency = true;
    network = std::make_unique<forwarding::LoopbackForwardingNetwork>(*graph.Get(), options);
    network->SetEgressPolicy(forwarding::EgressPolicy::LocalTesting());
    executor = std::make_unique<forwarding::LoopbackForwardingExecutor>(*network);
    engine = std::make_unique<algorithms::AlgorithmExecutionEngine>(nullptr, aafr::test::VerifiedRegistry(), executor.get());
    RuntimeDependencies d;
    d.graph_source = [this] { return graph.Get(); };
    d.problem_for = [](const forwarding::ForwardingGraph& g) { return aafr::test::ProblemFor(g); };
    d.executor = executor.get();
    d.eligibility = engine.get();
    d.executor_injects_delay = true;
    if (mode) {
      telemetry.SetProfile(aafr::test::ConfiguredTelemetryProfile(), "controlled");
      AIProviderConfig pc;
      pc.mode = *mode;
      pc.reuse_identical_features = false;
      provider = std::make_unique<AIRecommendationProvider>(&model, &telemetry, pc, &events);
      provider->SetContextValidator([this](const EvaluationContext& c) { return graph.Get()->version() == c.graph_version; });
      d.provider = provider.get();
    }
    RuntimeConfig config;
    config.session_id = "gwai";
    config.scheduler.min_interval_ms = 100;
    config.stability.min_dwell_ms = 200;
    config.stability.cooldown_ms = 300;
    config.stability.sustain_evaluations = 2;
    config.probe.rounds = 12;
    config.health_check_interval_ms = 0;
    config.review_interval_ms = 0;
    runtime = std::make_unique<AAFRRuntime>(std::move(d), config);
    gateway::GatewayConfig gc;
    gc.session_id = "gwai";
    gc.egress = forwarding::EgressPolicy::LocalTesting();
    gc.configuration_wait_ms = 200;
    gc.restrict_to_process = true;
    gw = std::make_unique<gateway::AAFRBrowserForwardingGateway>(gc);
    std::string error;
    ok = network->Start(&error) && origin.Start() && gw->Start(&error);
    gateway::RuntimeBindingSources s;
    s.runtime = runtime.get();
    s.graph_source = [this] { return graph.Get(); };
    s.problem_for = [](const forwarding::ForwardingGraph& g) { return aafr::test::ProblemFor(g); };
    s.executor = executor.get();
    s.network = network.get();
    gw->SetBindings(gateway::MakeRuntimeBindings(s));
  }
  ~Bed() {
    gw->Detach();
    gw->Stop();
    runtime->Stop();
    origin.Stop();
    network->Stop();
  }
  // One browser-style request; the gateway record for it.
  gateway::GatewayConnectionRecord Request() {
    const auto r = aafr::test::ProxyGet(gw->port(), origin.Base() + "/page");
    CHECK(r.status == 200);
    for (int i = 0; i < 300; ++i) {
      const auto all = gw->Connections(1);
      if (!all.empty() && all.back().finished_ns != 0) return all.back();
      Sleep(10);
    }
    return gw->Connections(1).back();
  }
  // The gateway follows the confirmed configuration, whatever the model said.
  void CheckFollowsConfirmed(const gateway::GatewayConnectionRecord& rec) {
    const auto active = runtime->ConfirmedConfiguration();
    CHECK(active.has_value());
    if (!active) return;
    CHECK(rec.path_confirmed);
    CHECK(rec.generation == active->config_version);
    CHECK(rec.algorithm == algorithms::ToString(active->algorithm));
    CHECK(rec.observed_path == active->path);
  }
};

void DeterministicOnly() {
  Bed bed(std::nullopt);
  CHECK(bed.ok);
  CHECK(bed.runtime->Initialize());
  CHECK(bed.runtime->Status().recommendation_source == "deterministic");
  bed.CheckFollowsConfirmed(bed.Request());
}

void ShadowModeNeverControlsForwarding() {
  Bed bed(RecommendationMode::Shadow);
  CHECK(bed.ok);
  bed.model.Always(FakeModel::Label("WOA"));
  CHECK(bed.runtime->Initialize());
  const DecisionRecord r = bed.runtime->Decisions(1).back();
  CHECK(r.model && r.model->predicted_algorithm == algorithms::AlgorithmId::WhaleOptimizationAlgorithm);
  CHECK(r.confirmed_algorithm == algorithms::AlgorithmId::Dijkstra);  // deterministic stays authoritative
  const auto rec = bed.Request();
  bed.CheckFollowsConfirmed(rec);
  CHECK(rec.algorithm == algorithms::ToString(algorithms::AlgorithmId::Dijkstra));
  CHECK(bed.model.calls.load() >= 1);
}

void AiPreferredFollowsConfirmedActivation() {
  Bed bed(RecommendationMode::AiPreferred);
  CHECK(bed.ok);
  bed.model.Always(FakeModel::Label("GWO"));
  CHECK(bed.runtime->Initialize());
  const DecisionRecord r = bed.runtime->Decisions(1).back();
  CHECK(r.recommendation_source == "expert_model");
  CHECK(r.activation == ActivationStatus::Confirmed);  // activation confirmation still mandatory
  const auto rec = bed.Request();
  bed.CheckFollowsConfirmed(rec);
  CHECK(rec.algorithm == algorithms::ToString(algorithms::AlgorithmId::GreyWolfOptimizer));
  CHECK(rec.observed_path == kPathA);  // GWO's validated path, carried by the relays
}

void ModelPredictionAloneChangesNothing() {
  Bed bed(RecommendationMode::AiPreferred);
  CHECK(bed.ok);
  bed.model.Always(FakeModel::Label("Dijkstra"));
  CHECK(bed.runtime->Initialize());
  const auto before = bed.runtime->ConfirmedConfiguration();
  // The model now prefers another algorithm, but nothing in the network
  // changed: the stability gates retain the incumbent, and the gateway keeps
  // forwarding over it.
  bed.model.Always(FakeModel::Label("PSO"));
  Sleep(400);
  bed.runtime->RequestEvaluation(EvaluationTrigger::Manual, "model changed its mind");
  bed.runtime->Step();
  const DecisionRecord r = bed.runtime->Decisions(1).back();
  CHECK(r.model && r.model->predicted_algorithm == algorithms::AlgorithmId::ParticleSwarmOptimization);
  const auto after = bed.runtime->ConfirmedConfiguration();
  CHECK(after && before && after->path == before->path);
  const auto rec = bed.Request();
  bed.CheckFollowsConfirmed(rec);
  CHECK(rec.observed_path == before->path);
}

void ModelUnavailableFallsBack() {
  Bed bed(RecommendationMode::AiPreferred);
  CHECK(bed.ok);
  bed.model.Always(FakeModel::Failure(InferenceStatus::WorkerUnavailable));
  CHECK(bed.runtime->Initialize());
  const DecisionRecord r = bed.runtime->Decisions(1).back();
  CHECK(r.recommendation_source == "deterministic_fallback");
  CHECK(bed.events.Count(ModelEventType::DeterministicFallbackActivated) >= 1);
  bed.CheckFollowsConfirmed(bed.Request());
}

void AllTenAlgorithmsStillEvaluated() {
  Bed bed(RecommendationMode::Shadow);
  CHECK(bed.ok);
  CHECK(bed.runtime->Initialize());
  const DecisionRecord r = bed.runtime->Decisions(1).back();
  CHECK(r.eligible_algorithms.size() == 10);
  CHECK(r.ranking.size() == 10);
}

}  // namespace

int main() {
  return aafr::test::RunTests({
      {"DeterministicOnly", DeterministicOnly},
      {"ShadowModeNeverControlsForwarding", ShadowModeNeverControlsForwarding},
      {"AiPreferredFollowsConfirmedActivation", AiPreferredFollowsConfirmedActivation},
      {"ModelPredictionAloneChangesNothing", ModelPredictionAloneChangesNothing},
      {"ModelUnavailableFallsBack", ModelUnavailableFallsBack},
      {"AllTenAlgorithmsStillEvaluated", AllTenAlgorithmsStillEvaluated},
  });
}
