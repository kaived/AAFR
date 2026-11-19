#pragma once

// AIRecommendationProvider (V0.7.2): the expert model as one more
// IRecommendationProvider. Plain C++17.
//
// The runtime calls Recommend() after eligibility, algorithm computation and
// path validation, with one CandidateEvaluation per algorithm. This provider:
//   1. always computes the deterministic ranking (DeterministicRecommendationProvider);
//   2. in Shadow or AiPreferred mode builds the model's raw features
//      (ModelFeatureAdapter) and asks the worker (IModelInference), unless the
//      features are incomplete, in which case the model abstains;
//   3. validates the answer and maps the label through the canonical mapping;
//   4. checks that the mapped algorithm's candidate is Usable() -- eligible,
//      computed by the existing V0.5 implementation, valid and feasible;
//   5. AiPreferred: ranks that candidate first and keeps the deterministic
//      order for the rest; otherwise falls back to the deterministic ranking
//      and says why. Shadow: always returns the deterministic ranking.
//   6. attaches a ModelRecommendationTrace to the Recommendation.
//
// It never activates forwarding, changes runtime state, bypasses eligibility
// or a stability gate, or treats a model probability as evidence. Probing,
// arbitration, the stability gates, activation confirmation and recovery stay
// in the runtime. Unlike the deterministic provider, its result can depend on
// the worker (timeouts); every such case is recorded in the trace.

#include <atomic>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>

#include "runtime/adaptive/model_trace.h"
#include "runtime/adaptive/recommendation_provider.h"
#include "runtime/model/model_events.h"
#include "runtime/model/model_compatibility.h"
#include "runtime/model/model_feature_adapter.h"
#include "runtime/model/model_worker_client.h"

namespace aafr::model {

// V0.7.4: what the provider does with an input the compatibility check
// does not clear.
enum class OodAction { Abstain, RecordOnly };
const char* ToString(OodAction value);

struct CompatibilityPolicy {
  // The model's training profile; without it nothing can be shown to be in
  // distribution (InsufficientInformation).
  std::shared_ptr<const TrainingProfile> profile;
  CompatibilityOptions options;
  // Operational inputs (no configured value: graph-derived, measured,
  // derived or substitute values only).
  OodAction measured_clearly_ood{OodAction::Abstain};
  OodAction measured_potentially_ood{OodAction::RecordOnly};
  OodAction measured_insufficient{OodAction::Abstain};
  // Inputs with configured values (an explicitly supplied controlled
  // scenario profile, V0.7.2): a what-if, recorded but not blocked.
  OodAction configured_ood{OodAction::RecordOnly};
  // Abstain whenever any feature value is configured rather than observed.
  bool require_measured_inputs{false};
};

struct AIProviderConfig {
  runtime::RecommendationMode mode{runtime::RecommendationMode::Shadow};
  FeatureAdapterConfig features;
  CompatibilityPolicy compatibility;  // V0.7.4
  // Reuse the previous model answer when the feature vector is identical
  // (the model is a pure function of its inputs); avoids needless inference
  // when nothing meaningful changed.
  bool reuse_identical_features{true};
  runtime::DeterministicProviderConfig deterministic;
};

struct AIProviderCounters {
  std::uint64_t evaluations{0};
  std::uint64_t inferences{0};      // Requests sent to the worker.
  std::uint64_t cache_hits{0};
  std::uint64_t model_ok{0};
  std::uint64_t abstained{0};
  std::uint64_t failed{0};          // Timeout, unavailable, invalid, version mismatch.
  std::uint64_t stale{0};
  std::uint64_t rejected{0};        // Unknown label, ineligible or unusable candidate.
  std::uint64_t ai_ranked_first{0};
  std::uint64_t fallbacks{0};
  std::uint64_t shadow{0};
  std::uint64_t agreements{0};      // Model and deterministic chose the same algorithm.
  std::uint64_t comparisons{0};
};

class AIRecommendationProvider final : public runtime::IRecommendationProvider {
 public:
  // `inference` and `telemetry` may be null (the model then never answers or
  // abstains for lack of telemetry); `events` may be null.
  AIRecommendationProvider(IModelInference* inference, const ITelemetryFeatureSource* telemetry,
                           AIProviderConfig config = {}, ModelEventLog* events = nullptr);

  std::string Id() const override { return "expert_model_provider"; }
  std::string Version() const override { return "1"; }
  runtime::Recommendation Recommend(const runtime::RecommendationInput& input) const override;

  void SetMode(runtime::RecommendationMode mode) { mode_.store(mode); }
  runtime::RecommendationMode Mode() const { return mode_.load(); }
  // Optional: called after inference; false means the evaluation context
  // (graph version, NetworkState) is no longer current, so the answer is
  // discarded as stale.
  void SetContextValidator(std::function<bool(const runtime::EvaluationContext&)> validator);
  std::optional<runtime::ModelRecommendationTrace> LastTrace() const;
  AIProviderCounters Counters() const;

 private:
  runtime::Recommendation RecommendImpl(const runtime::RecommendationInput& input) const;

  IModelInference* inference_;
  const ITelemetryFeatureSource* telemetry_;
  const AIProviderConfig config_;
  ModelEventLog* events_;
  runtime::DeterministicRecommendationProvider deterministic_;
  ModelFeatureAdapter adapter_;
  std::atomic<runtime::RecommendationMode> mode_;

  mutable std::mutex mutex_;
  std::function<bool(const runtime::EvaluationContext&)> validator_;
  mutable std::optional<runtime::ModelRecommendationTrace> last_trace_;
  mutable AIProviderCounters counters_;
  mutable std::uint64_t sequence_{0};
  // Cache of the last successful answer, keyed by the exact feature vector.
  mutable std::map<std::string, double> cached_features_;
  mutable std::optional<InferenceResponse> cached_response_;
  mutable std::string cached_model_version_;
};

}  // namespace aafr::model
