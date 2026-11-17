// V0.7.2 AIRecommendationProvider with a scripted model: modes (deterministic,
// shadow, AI preferred), label mapping, eligibility and path checks,
// abstention, every fallback reason, stale protection, caching and the
// trace that separates the model's prediction from the runtime's choice.

#include "runtime/model/ai_recommendation_provider.h"

#include "tests/support/model_test_support.h"
#include "tests/support/runtime_test_support.h"
#include "tests/support/test_support.h"

using namespace aafr;
using namespace aafr::model;
using namespace aafr::runtime;
using algorithms::AlgorithmId;
using aafr::test::FakeModel;
using aafr::test::kPathA;
using aafr::test::kPathB;
namespace fwd = aafr::forwarding;

using aafr::test::Contains;

namespace {

struct Setup {
  std::shared_ptr<const fwd::ForwardingGraph> graph =
      std::make_shared<const fwd::ForwardingGraph>(aafr::test::TwoPathGraph());
  ConfiguredTelemetrySource telemetry;
  FakeModel model;
  ModelEventLog events;

  Setup() { telemetry.SetProfile(aafr::test::ConfiguredTelemetryProfile(), "test"); }

  // Dijkstra/A*/Bellman-Ford on path A (J 3); PSO and WOA on path B (J 21);
  // GA not eligible; ACO computed but no path; the rest on path A.
  RecommendationInput Input(bool active_on_a = true) const {
    RecommendationInput in;
    in.graph = graph;
    in.problem = aafr::test::ProblemFor(*graph);
    for (AlgorithmId id : algorithms::kAllAlgorithmIds) {
      CandidateEvaluation c;
      c.algorithm = id;
      c.eligibility = algorithms::EligibilityStatus::Eligible;
      c.considered = true;
      c.computed = true;
      c.path_valid = true;
      c.path_feasible = true;
      c.path = kPathA;
      c.objective = 3.0;
      if (id == AlgorithmId::ParticleSwarmOptimization || id == AlgorithmId::WhaleOptimizationAlgorithm) {
        c.path = kPathB;
        c.objective = 21.0;
      }
      if (id == AlgorithmId::GeneticAlgorithm) {
        c.considered = false;
        c.computed = false;
        c.eligibility = algorithms::EligibilityStatus::Ineligible;
        c.eligibility_reasons = "IMPLEMENTATION_UNAVAILABLE";
      }
      if (id == AlgorithmId::AntColonyOptimization) {
        c.path_valid = false;
        c.objective.reset();
        c.note = "no path within budget";
      }
      in.candidates.push_back(c);
    }
    if (active_on_a) {
      in.active = AlgorithmId::Dijkstra;
      in.active_path = kPathA;
    }
    in.context = {"e7", EvaluationTrigger::Manual, 42, graph->version()};
    return in;
  }

  AIRecommendationProvider Provider(RecommendationMode mode, bool with_telemetry = true, bool cache = false) {
    AIProviderConfig c;
    c.mode = mode;
    c.reuse_identical_features = cache;  // only IdenticalFeaturesReuseAnswer tests the cache
    return AIRecommendationProvider(&model, with_telemetry ? &telemetry : nullptr, c, &events);
  }
};

void DeterministicModeNeverCallsModel() {
  Setup s;
  auto p = s.Provider(RecommendationMode::Deterministic);
  const Recommendation r = p.Recommend(s.Input());
  CHECK(s.model.calls == 0);
  CHECK(r.status == RecommendationStatus::Recommended);
  CHECK(r.recommended == AlgorithmId::Dijkstra);
  CHECK(r.model && r.model->source == RecommendationSource::Deterministic);
  CHECK(r.model->inference_status == "skipped");
  CHECK(Contains(r.provider_id, "deterministic_objective"));
}

void AiPreferredRanksUsableModelChoiceFirst() {
  Setup s;
  s.model.Always(FakeModel::Label("PSO"));
  auto p = s.Provider(RecommendationMode::AiPreferred);
  const Recommendation r = p.Recommend(s.Input());
  CHECK(s.model.calls == 1);
  CHECK(r.recommended == AlgorithmId::ParticleSwarmOptimization);
  CHECK(r.ranking.front().algorithm == AlgorithmId::ParticleSwarmOptimization);
  CHECK(r.ranking.front().path == kPathB);
  CHECK(r.ranking.size() == 8);  // 8 usable candidates, PSO once
  CHECK(r.ranking[1].algorithm == AlgorithmId::Dijkstra);  // the rest in deterministic order
  // The J(P) estimate is honest: PSO's path is worse than the active one.
  CHECK(r.estimated_improvement && *r.estimated_improvement < 0.0);
  const ModelRecommendationTrace& t = *r.model;
  CHECK(t.source == RecommendationSource::ExpertModel && t.controlled_ranking);
  CHECK(t.model_label == "PSO" && t.predicted_algorithm == AlgorithmId::ParticleSwarmOptimization);
  CHECK(t.candidate_check == "usable");
  CHECK(t.deterministic_algorithm == AlgorithmId::Dijkstra);
  CHECK(t.agrees_with_deterministic == false);
  CHECK(t.gate_probability && *t.gate_probability == 0.7 && t.gate_threshold);
  CHECK(t.stage2_ran && t.stage2_probabilities.size() == 4);
  CHECK(!t.probabilities_calibrated);
  CHECK(Contains(r.explanation, "uncalibrated"));
  CHECK(Contains(r.provider_id, "fake_model/fake-model-1/aafr.model_features.v1"));
  // The request carried the evaluation context and all 35 features.
  CHECK(s.model.last_request.evaluation_id == "e7");
  CHECK(s.model.last_request.network_state_id == 42);
  CHECK(s.model.last_request.features.size() == 35);
}

void ShadowRecordsButNeverRanks() {
  Setup s;
  s.model.Always(FakeModel::Label("WOA"));
  auto p = s.Provider(RecommendationMode::Shadow);
  const Recommendation r = p.Recommend(s.Input());
  CHECK(s.model.calls == 1);
  CHECK(r.recommended == AlgorithmId::Dijkstra);  // deterministic stays authoritative
  CHECK(r.ranking.front().algorithm == AlgorithmId::Dijkstra);
  CHECK(r.model->source == RecommendationSource::DeterministicShadow);
  CHECK(!r.model->controlled_ranking);
  CHECK(r.model->predicted_algorithm == AlgorithmId::WhaleOptimizationAlgorithm);
  CHECK(r.model->agrees_with_deterministic == false);
  CHECK(Contains(r.provider_id, "shadow"));
  CHECK(p.Counters().shadow == 1 && p.Counters().comparisons == 1 && p.Counters().agreements == 0);
  s.model.Always(FakeModel::Label("Dijkstra"));
  p.Recommend(s.Input(false));
  CHECK(p.Counters().agreements == 1);
}

void UnknownLabelRejected() {
  Setup s;
  FakeModel::Step step = FakeModel::Label("Dijkstra");
  step.label = "Teleport";
  s.model.Always(step);
  auto p = s.Provider(RecommendationMode::AiPreferred);
  const Recommendation r = p.Recommend(s.Input());
  CHECK(r.recommended == AlgorithmId::Dijkstra);
  CHECK(r.model->source == RecommendationSource::DeterministicFallback);
  CHECK(!r.model->predicted_algorithm);  // never silently mapped to another algorithm
  CHECK(Contains(r.model->candidate_check, "unknown_label"));
  CHECK(Contains(r.model->fallback_reason, "unknown_label"));
  CHECK(s.events.Count(ModelEventType::RecommendationRejected) == 1);
  CHECK(s.events.Count(ModelEventType::DeterministicFallbackActivated) == 1);
}

void IneligibleOrUnusableRecommendationRejected() {
  // The model space has no GA, so make ACO ineligible-by-path and test both
  // an ineligible and a no-path candidate through the mapped labels.
  Setup s;
  s.model.Always(FakeModel::Label("ACO"));  // ACO computed but no valid path
  auto p = s.Provider(RecommendationMode::AiPreferred);
  Recommendation r = p.Recommend(s.Input());
  CHECK(r.recommended == AlgorithmId::Dijkstra);
  CHECK(Contains(r.model->candidate_check, "no_usable_path"));
  CHECK(r.model->predicted_algorithm == AlgorithmId::AntColonyOptimization);
  RecommendationInput in = s.Input();
  for (auto& c : in.candidates) {
    if (c.algorithm == AlgorithmId::GreyWolfOptimizer) {
      c.considered = false;
      c.eligibility = algorithms::EligibilityStatus::Ineligible;
      c.eligibility_reasons = "CONSTRAINT_VIOLATION";
    }
  }
  s.model.Always(FakeModel::Label("GWO"));
  r = p.Recommend(in);
  CHECK(r.recommended == AlgorithmId::Dijkstra);
  CHECK(Contains(r.model->candidate_check, "not_eligible: CONSTRAINT_VIOLATION"));
  CHECK(p.Counters().rejected == 2 && p.Counters().fallbacks == 2);
}

void EveryFailureFallsBack() {
  struct Case {
    FakeModel::Step step;
    std::string status;
  };
  const std::vector<Case> cases = {
      {FakeModel::Failure(InferenceStatus::Timeout), "timeout"},
      {FakeModel::Failure(InferenceStatus::WorkerUnavailable), "unavailable"},
      {FakeModel::Failure(InferenceStatus::Busy), "unavailable"},
      {FakeModel::Failure(InferenceStatus::InvalidResponse), "error"},
      {FakeModel::Failure(InferenceStatus::VersionMismatch, "E_MODEL_VERSION_MISMATCH"), "error"},
      {FakeModel::Failure(InferenceStatus::WorkerError, "E_INFERENCE_FAILED"), "error"},
      {FakeModel::Failure(InferenceStatus::ExcessiveLatency), "error"},
      {FakeModel::Failure(InferenceStatus::Stale), "stale"},
  };
  for (const Case& c : cases) {
    Setup s;
    s.model.Always(c.step);
    auto p = s.Provider(RecommendationMode::AiPreferred);
    const Recommendation r = p.Recommend(s.Input());
    CHECK(r.status == RecommendationStatus::Recommended);
    CHECK(r.recommended == AlgorithmId::Dijkstra);
    CHECK(r.model->source == RecommendationSource::DeterministicFallback);
    CHECK(r.model->inference_status == c.status);
    CHECK(!r.model->fallback_reason.empty());
    CHECK(!r.model->predicted_algorithm);
    CHECK(Contains(r.explanation, "deterministic fallback"));
    CHECK(s.events.Count(ModelEventType::DeterministicFallbackActivated) == 1);
  }
}

void IncompleteFeaturesAbstainWithoutInference() {
  Setup s;
  auto p = s.Provider(RecommendationMode::AiPreferred, /*with_telemetry=*/false);
  const Recommendation r = p.Recommend(s.Input());
  CHECK(s.model.calls == 0);  // nothing is sent with unknown inputs
  CHECK(r.model->inference_status == "abstained");
  CHECK(r.model->error_code == "E_FEATURES_INCOMPLETE");
  CHECK(r.model->missing_features.size() == 23);
  CHECK(r.model->source == RecommendationSource::DeterministicFallback);
  CHECK(r.recommended == AlgorithmId::Dijkstra);
  CHECK(p.Counters().abstained == 1);
}

void NoWorkerConfigured() {
  Setup s;
  AIProviderConfig c;
  c.mode = RecommendationMode::AiPreferred;
  AIRecommendationProvider p(nullptr, &s.telemetry, c, &s.events);
  const Recommendation r = p.Recommend(s.Input());
  CHECK(r.model->inference_status == "unavailable");
  CHECK(r.model->error_code == "E_NO_WORKER");
  CHECK(r.recommended == AlgorithmId::Dijkstra);
}

void StaleContextDiscarded() {
  Setup s;
  s.model.Always(FakeModel::Label("PSO"));
  auto p = s.Provider(RecommendationMode::AiPreferred);
  bool current = true;
  s.model.during_inference = [&](const InferenceRequest&) { current = false; };  // graph moved on meanwhile
  p.SetContextValidator([&](const EvaluationContext&) { return current; });
  const Recommendation r = p.Recommend(s.Input());
  CHECK(r.model->inference_status == "stale");
  CHECK(r.model->error_code == "E_CONTEXT_CHANGED");
  CHECK(r.recommended == AlgorithmId::Dijkstra);  // never the obsolete PSO answer
  CHECK(r.model->source == RecommendationSource::DeterministicFallback);
  CHECK(s.events.Count(ModelEventType::StaleInferenceDiscarded) == 1);
  CHECK(p.Counters().stale == 1);
}

void IdenticalFeaturesReuseAnswer() {
  Setup s;
  s.model.Script({FakeModel::Label("PSO"), FakeModel::Label("WOA")});
  auto p = s.Provider(RecommendationMode::AiPreferred, true, true);
  const Recommendation a = p.Recommend(s.Input());
  const Recommendation b = p.Recommend(s.Input());
  CHECK(s.model.calls == 1);  // unchanged inputs: no second inference
  CHECK(b.model->cached);
  CHECK(b.model->model_label == "PSO");
  // Changed telemetry: a new inference.
  auto profile = aafr::test::ConfiguredTelemetryProfile();
  profile["latency"] = 90.0;
  s.telemetry.SetProfile(profile, "changed");
  const Recommendation c = p.Recommend(s.Input());
  CHECK(s.model.calls == 2);
  CHECK(!c.model->cached && c.model->model_label == "WOA");
  CHECK(p.Counters().cache_hits == 1);
}

void FallbackFailsSafely() {
  // Nothing usable at all: the deterministic result (Unavailable) passes
  // through; the model cannot conjure a candidate.
  Setup s;
  s.model.Always(FakeModel::Label("PSO"));
  auto p = s.Provider(RecommendationMode::AiPreferred);
  RecommendationInput in = s.Input();
  for (auto& c : in.candidates) {
    c.considered = false;
    c.computed = false;
  }
  const Recommendation r = p.Recommend(in);
  CHECK(r.status == RecommendationStatus::Unavailable);
  CHECK(!r.recommended);
  CHECK(r.ranking.empty());
  CHECK(r.model->source == RecommendationSource::DeterministicFallback);
  CHECK(Contains(r.model->candidate_check, "not_eligible"));
}

void ModeCanChangeAtRuntime() {
  Setup s;
  s.model.Always(FakeModel::Label("PSO"));
  auto p = s.Provider(RecommendationMode::Shadow);
  CHECK(p.Recommend(s.Input()).recommended == AlgorithmId::Dijkstra);
  p.SetMode(RecommendationMode::AiPreferred);
  CHECK(p.Recommend(s.Input()).recommended == AlgorithmId::ParticleSwarmOptimization);
  p.SetMode(RecommendationMode::Deterministic);
  CHECK(p.Recommend(s.Input()).recommended == AlgorithmId::Dijkstra);
  CHECK(p.LastTrace()->mode == RecommendationMode::Deterministic);
}

void TraceSerializes() {
  Setup s;
  s.model.Always(FakeModel::Label("GWO"));
  auto p = s.Provider(RecommendationMode::AiPreferred);
  const Recommendation r = p.Recommend(s.Input());
  const std::string json = ToJson(*r.model);
  const auto parsed = ParseJson(json);
  CHECK(parsed.value.has_value());
  CHECK(parsed.value->GetString("model_label").value_or("") == "GWO");
  CHECK(parsed.value->GetString("predicted_algorithm").value_or("") == "gwo");
  CHECK(parsed.value->GetString("source").value_or("") == "expert_model");
  CHECK(parsed.value->GetBool("probabilities_calibrated") == false);
}

}  // namespace

int main() {
  return aafr::test::RunTests({
      {"deterministic_mode_never_calls_model", DeterministicModeNeverCallsModel},
      {"ai_preferred_ranks_usable_model_choice_first", AiPreferredRanksUsableModelChoiceFirst},
      {"shadow_records_but_never_ranks", ShadowRecordsButNeverRanks},
      {"unknown_label_rejected", UnknownLabelRejected},
      {"ineligible_or_unusable_recommendation_rejected", IneligibleOrUnusableRecommendationRejected},
      {"every_failure_falls_back", EveryFailureFallsBack},
      {"incomplete_features_abstain_without_inference", IncompleteFeaturesAbstainWithoutInference},
      {"no_worker_configured", NoWorkerConfigured},
      {"stale_context_discarded", StaleContextDiscarded},
      {"identical_features_reuse_answer", IdenticalFeaturesReuseAnswer},
      {"fallback_fails_safely", FallbackFailsSafely},
      {"mode_can_change_at_runtime", ModeCanChangeAtRuntime},
      {"trace_serializes", TraceSerializes},
  });
}
