// V0.7.2: the AIRecommendationProvider inside the real AAFRRuntime, with a
// scripted model and the deterministic test executor/clock. Verifies that a
// model recommendation passes through every existing guarantee: eligibility,
// path validation, probing + statistical arbitration, improvement margin,
// dwell, cooldown, hysteresis, confirmed activation, recovery and stale
// protection; that shadow mode never controls forwarding; and that the
// DecisionRecord keeps the prediction, the selected candidate and the
// confirmed algorithm apart.

#include "runtime/adaptive/aafr_runtime.h"

#include <algorithm>

#include "runtime/model/ai_recommendation_provider.h"
#include "tests/support/model_test_support.h"
#include "tests/support/runtime_test_support.h"
#include "tests/support/test_support.h"

using namespace aafr;
using namespace aafr::runtime;
using namespace aafr::model;
using algorithms::AlgorithmId;
using aafr::test::FakeModel;
using aafr::test::kPathA;
using aafr::test::kPathB;

using aafr::test::Contains;

namespace {

RuntimeConfig BaseConfig() {
  RuntimeConfig c;
  c.session_id = "ai";
  c.scheduler.min_interval_ms = 1000;
  c.stability.min_dwell_ms = 1000;
  c.stability.cooldown_ms = 2000;
  c.stability.failed_activation_cooldown_ms = 500;
  c.stability.candidate_backoff_initial_ms = 1000;
  c.stability.sustain_evaluations = 2;
  c.probe.rounds = 12;
  c.health_check_interval_ms = 0;
  c.degraded_retry_ms = 1000;
  c.activation_attempts = 1;
  c.review_interval_ms = 0;
  return c;
}

struct Fixture {
  aafr::test::GraphHolder graph{aafr::test::TwoPathGraph()};
  aafr::test::TestExecutor executor{graph};
  ManualClock clock;
  algorithms::AlgorithmExecutionEngine engine;
  ConfiguredTelemetrySource telemetry;
  FakeModel model;
  ModelEventLog events;
  std::unique_ptr<AIRecommendationProvider> provider;
  std::unique_ptr<AAFRRuntime> runtime;

  explicit Fixture(RecommendationMode mode = RecommendationMode::AiPreferred, RuntimeConfig config = BaseConfig(),
                   algorithms::CapabilityRegistry registry = aafr::test::VerifiedRegistry())
      : engine(nullptr, std::move(registry), &executor) {
    telemetry.SetProfile(aafr::test::ConfiguredTelemetryProfile(), "controlled");
    AIProviderConfig pc;
    pc.mode = mode;
    pc.reuse_identical_features = false;  // every evaluation asks the scripted model
    provider = std::make_unique<AIRecommendationProvider>(&model, &telemetry, pc, &events);
    provider->SetContextValidator([this](const EvaluationContext& c) { return graph.Get()->version() == c.graph_version; });
    RuntimeDependencies d;
    d.graph_source = [this] { return graph.Get(); };
    d.problem_for = [](const forwarding::ForwardingGraph& g) { return aafr::test::ProblemFor(g); };
    d.executor = &executor;
    d.eligibility = &engine;
    d.clock = &clock;
    d.provider = provider.get();
    runtime = std::make_unique<AAFRRuntime>(std::move(d), std::move(config));
  }
  void StepAfter(double ms) {
    clock.AdvanceMs(ms);
    runtime->Step();
  }
  void Evaluate(double after_ms = 1100) {
    clock.AdvanceMs(after_ms);
    runtime->RequestEvaluation(EvaluationTrigger::Manual, "test");
    runtime->Step();
  }
  DecisionRecord Last() const { return runtime->Decisions(1).back(); }
  ActiveConfiguration Active() const { return *runtime->Status().active; }
};

bool Has(const DecisionRecord& r, RuntimeReason reason) { return r.HasReason(reason); }

void AllTenAlgorithmsStillComputed() {
  Fixture f;
  f.model.Always(FakeModel::Label("Dijkstra"));
  CHECK(f.runtime->Initialize());
  const DecisionRecord r = f.Last();
  CHECK(r.eligible_algorithms.size() == 10);  // the existing V0.5 registry, unchanged
  CHECK(r.ranking.size() == 10);              // every implementation produced a usable path
  CHECK(r.model && r.model->source == RecommendationSource::ExpertModel);
  CHECK(r.confirmed_algorithm == AlgorithmId::Dijkstra);
  CHECK(Has(r, RuntimeReason::ModelRecommendationUsed));
  CHECK(r.recommendation_mode == "ai_preferred" && r.recommendation_source == "expert_model");
}

void InitialActivationFollowsModel() {
  Fixture f;
  f.model.Always(FakeModel::Label("GWO"));
  CHECK(f.runtime->Initialize());
  const DecisionRecord r = f.Last();
  CHECK(r.type == DecisionType::InitialActivation);
  CHECK(r.model->predicted_algorithm == AlgorithmId::GreyWolfOptimizer);
  CHECK(r.candidate_algorithm == AlgorithmId::GreyWolfOptimizer);
  CHECK(r.activation == ActivationStatus::Confirmed);  // confirmation still mandatory
  CHECK(f.Active().algorithm == AlgorithmId::GreyWolfOptimizer);
  CHECK(f.Active().path == kPathA);  // GWO's own (validated) path
}

void DifferentPredictionButRetained() {
  // The model names another algorithm, but its path is the active one:
  // a valid prediction does not justify a switch.
  Fixture f;
  f.model.Script({FakeModel::Label("Dijkstra"), FakeModel::Label("PSO")});
  f.runtime->Initialize();
  f.Evaluate(1500);
  const DecisionRecord r = f.Last();
  CHECK(r.model->predicted_algorithm == AlgorithmId::ParticleSwarmOptimization);
  CHECK(r.type == DecisionType::Retain);
  CHECK(Has(r, RuntimeReason::CandidateSameAsActive));
  CHECK(r.activation == ActivationStatus::NotRequested);
  CHECK(r.confirmed_algorithm == AlgorithmId::Dijkstra);  // prediction != confirmed
  CHECK(f.Active().algorithm == AlgorithmId::Dijkstra);
  const RuntimeStatus s = f.runtime->Status();
  CHECK(s.last_model && s.last_model->predicted_algorithm == AlgorithmId::ParticleSwarmOptimization);
  CHECK(s.active->algorithm == AlgorithmId::Dijkstra);
}

void SwitchNeedsEvidenceAndHysteresis() {
  Fixture f;
  f.model.Script({FakeModel::Label("Dijkstra"), FakeModel::Label("WOA")});
  f.runtime->Initialize();
  f.graph.SetLatency("entry", "relay_a", 100.0);  // A: J 102, B: J 21
  f.StepAfter(1500);
  DecisionRecord r = f.Last();
  CHECK(r.candidate_algorithm == AlgorithmId::WhaleOptimizationAlgorithm);
  CHECK(r.probing_attempted && r.evidence_validity == "challenger_better");
  CHECK(r.type == DecisionType::SwitchRejected);  // hysteresis: first preference only
  CHECK(Has(r, RuntimeReason::HysteresisNotSatisfied));
  CHECK(f.Active().algorithm == AlgorithmId::Dijkstra);
  f.Evaluate();
  r = f.Last();
  CHECK(r.type == DecisionType::Switch);
  CHECK(Has(r, RuntimeReason::ImprovementConfirmed));
  CHECK(Has(r, RuntimeReason::ModelRecommendationUsed));
  CHECK(r.activation == ActivationStatus::Confirmed);
  CHECK(r.confirmed_algorithm == AlgorithmId::WhaleOptimizationAlgorithm);
  CHECK(f.Active().algorithm == AlgorithmId::WhaleOptimizationAlgorithm && f.Active().path == kPathB);
  CHECK(r.model->deterministic_algorithm == AlgorithmId::Dijkstra);  // what the deterministic ranking would have chosen
  const std::string json = ToJson(r);
  CHECK(Contains(json, "\"recommendation_source\":\"expert_model\""));
  CHECK(Contains(json, "\"predicted_algorithm\":\"woa\""));
  CHECK(Contains(json, "\"confirmed_algorithm\":\"woa\""));
}

void DwellAndCooldownStillBlock() {
  Fixture f;
  f.model.Script({FakeModel::Label("Dijkstra"), FakeModel::Label("WOA"), FakeModel::Label("WOA"), FakeModel::Label("Dijkstra")});
  f.runtime->Initialize();
  f.graph.SetLatency("entry", "relay_a", 100.0);
  f.StepAfter(1500);
  f.Evaluate();
  CHECK(f.Active().path == kPathB);
  f.graph.SetLatency("entry", "relay_a", 1.0);  // A is better again; the model says Dijkstra
  f.StepAfter(1050);
  const DecisionRecord r = f.Last();
  CHECK(r.model->predicted_algorithm == AlgorithmId::Dijkstra);
  CHECK(r.type == DecisionType::SwitchRejected);
  CHECK(Has(r, RuntimeReason::MinimumDwellNotMet) || Has(r, RuntimeReason::CooldownActive));
  CHECK(f.Active().path == kPathB);
}

void ImprovementMarginStillBlocks() {
  RuntimeConfig c = BaseConfig();
  c.statistical_arbitration = false;
  Fixture f(RecommendationMode::AiPreferred, c);
  f.model.Script({FakeModel::Label("Dijkstra"), FakeModel::Label("PSO")});
  f.runtime->Initialize();
  // A: J 3.4, B: J 3.0 -> B is 11.8 % better, below the 20 % estimate margin.
  f.graph.SetLatency("entry", "relay_a", 1.2);
  f.graph.SetLatency("relay_a", "dest", 1.2);
  f.graph.SetLatency("entry", "relay_b", 1.0);
  f.graph.SetLatency("relay_b", "dest", 1.0);
  f.StepAfter(1500);
  f.Evaluate();
  const DecisionRecord r = f.Last();
  CHECK(r.candidate_algorithm == AlgorithmId::ParticleSwarmOptimization);
  CHECK(Has(r, RuntimeReason::ImprovementMarginNotMet));
  CHECK(f.Active().path == kPathA);
}

void IneligibleRecommendationNotExecuted() {
  algorithms::CapabilityRegistry registry = aafr::test::VerifiedRegistry();
  registry.SetAvailability(AlgorithmId::ParticleSwarmOptimization, algorithms::ImplementationAvailability::Unsupported);
  Fixture f(RecommendationMode::AiPreferred, BaseConfig(), registry);
  f.model.Always(FakeModel::Label("PSO"));
  CHECK(f.runtime->Initialize());
  const DecisionRecord r = f.Last();
  CHECK(r.model->predicted_algorithm == AlgorithmId::ParticleSwarmOptimization);
  CHECK(Contains(r.model->candidate_check, "not_eligible"));
  CHECK(r.model->source == RecommendationSource::DeterministicFallback);
  CHECK(Has(r, RuntimeReason::ModelFallbackDeterministic));
  CHECK(r.candidate_algorithm != AlgorithmId::ParticleSwarmOptimization);
  CHECK(f.Active().algorithm == AlgorithmId::Dijkstra);
  CHECK(f.events.Count(ModelEventType::RecommendationRejected) == 1);
}

void ActivationFailureKeepsPrevious() {
  Fixture f;
  f.model.Script({FakeModel::Label("Dijkstra"), FakeModel::Label("WOA")});
  f.runtime->Initialize();
  f.graph.SetLatency("entry", "relay_a", 100.0);
  f.StepAfter(1500);
  {
    std::lock_guard<std::mutex> lock(f.executor.mutex_);
    f.executor.unconfirmed_activation_paths.insert(kPathB);
  }
  f.Evaluate();
  const DecisionRecord r = f.Last();
  CHECK(r.candidate_algorithm == AlgorithmId::WhaleOptimizationAlgorithm);
  CHECK(r.activation == ActivationStatus::Failed);
  CHECK(Has(r, RuntimeReason::SwitchFailed));
  CHECK(r.confirmed_algorithm == AlgorithmId::Dijkstra);
  CHECK(f.Active().algorithm == AlgorithmId::Dijkstra && f.Active().path == kPathA);
}

void SafetyRecoveryUsesValidatedCandidate() {
  RuntimeConfig c = BaseConfig();
  c.health_check_interval_ms = 500;
  c.failures_for_safety_override = 2;
  Fixture f(RecommendationMode::AiPreferred, c);
  f.model.Script({FakeModel::Label("Dijkstra"), FakeModel::Label("WOA")});
  f.runtime->Initialize();
  {
    std::lock_guard<std::mutex> lock(f.executor.mutex_);
    f.executor.fail_paths.insert(kPathA);
  }
  f.StepAfter(600);
  f.StepAfter(600);
  const DecisionRecord r = f.Last();
  CHECK(r.safety_override && Has(r, RuntimeReason::ActivePathFailed));
  CHECK(r.type == DecisionType::SafetySwitch);
  CHECK(r.activation == ActivationStatus::Confirmed);
  CHECK(f.Active().path == kPathB);
}

void StaleInferenceCannotSwitch() {
  Fixture f;
  f.model.Script({FakeModel::Label("Dijkstra"), FakeModel::Label("WOA")});
  f.runtime->Initialize();
  f.graph.SetLatency("entry", "relay_a", 100.0);
  f.StepAfter(1500);
  // The graph changes while the model is answering.
  f.model.during_inference = [&](const InferenceRequest&) { f.graph.SetLatency("relay_b", "dest", 10.5); };
  f.Evaluate();
  const DecisionRecord r = f.Last();
  CHECK(r.model && r.model->inference_status == "stale");
  CHECK(r.model->source == RecommendationSource::DeterministicFallback);
  CHECK(r.candidate_algorithm != AlgorithmId::WhaleOptimizationAlgorithm);  // the obsolete answer is never used
  // The runtime's own stale check also refuses to commit on the old graph.
  CHECK(f.Active().path == kPathA);
  CHECK(f.events.Count(ModelEventType::StaleInferenceDiscarded) >= 1);
}

void ShadowModeNeverControls() {
  Fixture f(RecommendationMode::Shadow);
  f.model.Always(FakeModel::Label("WOA"));
  f.runtime->Initialize();
  CHECK(f.Active().algorithm == AlgorithmId::Dijkstra);
  f.graph.SetLatency("entry", "relay_a", 100.0);
  f.StepAfter(1500);
  f.Evaluate();
  const DecisionRecord r = f.Last();
  CHECK(r.type == DecisionType::Switch);
  CHECK(r.confirmed_algorithm == AlgorithmId::Dijkstra);  // deterministic stays authoritative
  CHECK(r.model->predicted_algorithm == AlgorithmId::WhaleOptimizationAlgorithm);  // recorded
  CHECK(r.model->source == RecommendationSource::DeterministicShadow);
  CHECK(r.model->agrees_with_deterministic == false);
  CHECK(Has(r, RuntimeReason::ModelShadowOnly));
  CHECK(r.recommendation_mode == "shadow");
  const AIProviderCounters c = f.provider->Counters();
  CHECK(c.shadow >= 3 && c.comparisons >= 3 && c.ai_ranked_first == 0);
}

void WorkerFailureFallbackAndRecovery() {
  Fixture f;
  f.model.Script({FakeModel::Label("Dijkstra"), FakeModel::Failure(InferenceStatus::Timeout),
                  FakeModel::Failure(InferenceStatus::WorkerUnavailable), FakeModel::Label("GWO")});
  f.runtime->Initialize();
  f.Evaluate(1500);
  DecisionRecord r = f.Last();
  CHECK(r.model->inference_status == "timeout" && r.recommendation_source == "deterministic_fallback");
  CHECK(r.type == DecisionType::Retain);  // the runtime keeps working
  f.Evaluate(1500);
  r = f.Last();
  CHECK(r.model->inference_status == "unavailable");
  f.Evaluate(1500);
  r = f.Last();
  CHECK(r.model->inference_status == "ok" && r.recommendation_source == "expert_model");
  CHECK(f.runtime->Status().lifecycle == RuntimeLifecycle::Ready);
  CHECK(f.events.Count(ModelEventType::DeterministicFallbackActivated) == 2);
}

void DisabledEqualsDeterministic() {
  Fixture ai(RecommendationMode::Deterministic);
  ai.model.Always(FakeModel::Label("WOA"));
  ai.runtime->Initialize();
  ai.graph.SetLatency("entry", "relay_a", 100.0);
  ai.StepAfter(1500);
  ai.Evaluate();
  CHECK(ai.model.calls == 0);
  CHECK(ai.Active().algorithm == AlgorithmId::Dijkstra && ai.Active().path == kPathB);
  CHECK(ai.Last().recommendation_source == "deterministic");
}

}  // namespace

int main() {
  return aafr::test::RunTests({
      {"all_ten_algorithms_still_computed", AllTenAlgorithmsStillComputed},
      {"initial_activation_follows_model", InitialActivationFollowsModel},
      {"different_prediction_but_retained", DifferentPredictionButRetained},
      {"switch_needs_evidence_and_hysteresis", SwitchNeedsEvidenceAndHysteresis},
      {"dwell_and_cooldown_still_block", DwellAndCooldownStillBlock},
      {"improvement_margin_still_blocks", ImprovementMarginStillBlocks},
      {"ineligible_recommendation_not_executed", IneligibleRecommendationNotExecuted},
      {"activation_failure_keeps_previous", ActivationFailureKeepsPrevious},
      {"safety_recovery_uses_validated_candidate", SafetyRecoveryUsesValidatedCandidate},
      {"stale_inference_cannot_switch", StaleInferenceCannotSwitch},
      {"shadow_mode_never_controls", ShadowModeNeverControls},
      {"worker_failure_fallback_and_recovery", WorkerFailureFallbackAndRecovery},
      {"disabled_equals_deterministic", DisabledEqualsDeterministic},
  });
}
