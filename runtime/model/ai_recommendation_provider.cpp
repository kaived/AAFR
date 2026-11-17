#include "runtime/model/ai_recommendation_provider.h"

#include <cmath>
#include <sstream>

namespace aafr::model {

namespace rt = aafr::runtime;
namespace alg = aafr::algorithms;
namespace fwd = aafr::forwarding;

namespace {
std::string Fixed(double v, int precision = 3) {
  std::ostringstream o;
  o.setf(std::ios::fixed);
  o.precision(precision);
  o << v;
  return o.str();
}

std::string Join(const std::vector<std::string>& v, std::size_t max_items = 8) {
  std::string out;
  for (std::size_t i = 0; i < v.size() && i < max_items; ++i) out += (i ? "," : "") + v[i];
  if (v.size() > max_items) out += ",+" + std::to_string(v.size() - max_items);
  return out;
}

std::string StatusText(InferenceStatus s) {
  switch (s) {
    case InferenceStatus::Ok: return "ok";
    case InferenceStatus::Timeout: return "timeout";
    case InferenceStatus::WorkerUnavailable:
    case InferenceStatus::Busy: return "unavailable";
    case InferenceStatus::Stale: return "stale";
    default: return "error";
  }
}
}  // namespace

AIRecommendationProvider::AIRecommendationProvider(IModelInference* inference, const ITelemetryFeatureSource* telemetry,
                                                   AIProviderConfig config, ModelEventLog* events)
    : inference_(inference),
      telemetry_(telemetry),
      config_(config),
      events_(events),
      deterministic_(config.deterministic),
      adapter_(config.features),
      mode_(config.mode) {}

void AIRecommendationProvider::SetContextValidator(std::function<bool(const rt::EvaluationContext&)> validator) {
  std::lock_guard<std::mutex> lock(mutex_);
  validator_ = std::move(validator);
}

std::optional<rt::ModelRecommendationTrace> AIRecommendationProvider::LastTrace() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return last_trace_;
}

AIProviderCounters AIRecommendationProvider::Counters() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return counters_;
}

rt::Recommendation AIRecommendationProvider::Recommend(const rt::RecommendationInput& input) const {
  try {
    return RecommendImpl(input);
  } catch (const std::exception& e) {
    rt::Recommendation r = deterministic_.Recommend(input);
    rt::ModelRecommendationTrace t;
    t.mode = mode_.load();
    t.source = t.mode == rt::RecommendationMode::Deterministic ? rt::RecommendationSource::Deterministic
                                                                : rt::RecommendationSource::DeterministicFallback;
    t.inference_status = "error";
    t.error = std::string("provider exception: ") + e.what();
    t.fallback_reason = t.error;
    r.model = t;
    return r;
  } catch (...) {
    rt::Recommendation r = deterministic_.Recommend(input);
    rt::ModelRecommendationTrace t;
    t.mode = mode_.load();
    t.inference_status = "error";
    t.error = "provider exception";
    t.fallback_reason = t.error;
    t.source = rt::RecommendationSource::DeterministicFallback;
    r.model = t;
    return r;
  }
}

rt::Recommendation AIRecommendationProvider::RecommendImpl(const rt::RecommendationInput& input) const {
  const rt::RecommendationMode mode = mode_.load();
  rt::Recommendation det = deterministic_.Recommend(input);
  rt::ModelRecommendationTrace t;
  t.mode = mode;
  t.evaluation_id = input.context.evaluation_id;
  t.network_state_id = input.context.network_state_id;
  t.graph_version = input.context.graph_version;
  t.feature_contract_version = kFeatureContractVersion;
  t.deterministic_algorithm = det.recommended;
  if (inference_) {
    t.model_name = inference_->ModelName();
    t.model_version = inference_->ExpectedModelVersion();
  }

  const auto finish = [&](rt::Recommendation r, const rt::ModelRecommendationTrace& trace) {
    std::lock_guard<std::mutex> lock(mutex_);
    ++counters_.evaluations;
    last_trace_ = trace;
    r.model = trace;
    return r;
  };

  if (mode == rt::RecommendationMode::Deterministic) {
    t.source = rt::RecommendationSource::Deterministic;
    t.inference_status = "skipped";
    return finish(det, t);
  }

  // ---- Model inference ----
  InferenceResponse response;
  bool have_response = false;
  if (!input.graph) {
    t.inference_status = "error";
    t.error_code = "E_NO_GRAPH";
    t.error = "no forwarding graph";
  } else if (!inference_) {
    t.inference_status = "unavailable";
    t.error_code = "E_NO_WORKER";
    t.error = "no model worker configured";
  } else {
    const ModelFeatureVector features = adapter_.Build(*input.graph, input.problem, input.state.get(), telemetry_);
    t.feature_ms = features.build_ms;
    t.feature_summary = features.Summary();
    t.substituted_features = features.substituted;
    t.missing_features = features.missing;
    for (const auto& s : features.stale) t.missing_features.push_back(s + " (stale)");
    for (const auto& s : features.invalid) t.missing_features.push_back(s + " (invalid)");
    if (!features.Complete()) {
      t.inference_status = "abstained";
      t.error_code = "E_FEATURES_INCOMPLETE";
      t.error = std::to_string(features.missing.size()) + " missing, " + std::to_string(features.stale.size()) +
                " stale, " + std::to_string(features.invalid.size()) + " invalid feature(s): " +
                Join(t.missing_features);
      std::lock_guard<std::mutex> lock(mutex_);
      ++counters_.abstained;
    } else {
      const auto values = features.Values();
      std::optional<InferenceResponse> cached;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        if (config_.reuse_identical_features && cached_response_ && cached_features_ == values &&
            cached_model_version_ == inference_->ExpectedModelVersion()) {
          cached = cached_response_;
          ++counters_.cache_hits;
        }
      }
      if (cached) {
        response = *cached;
        response.round_trip_ms = 0.0;
        t.cached = true;
        t.request_id = cached->request_id + "#cached";
      } else {
        InferenceRequest request;
        {
          std::lock_guard<std::mutex> lock(mutex_);
          request.request_id = "inf-" + (input.context.evaluation_id.empty() ? std::string("x") : input.context.evaluation_id) +
                               "-" + std::to_string(++sequence_);
          ++counters_.inferences;
        }
        request.evaluation_id = input.context.evaluation_id;
        request.network_state_id = input.context.network_state_id;
        request.graph_version = input.context.graph_version;
        request.features = values;
        t.request_id = request.request_id;
        response = inference_->Infer(request);
        if (response.status == InferenceStatus::Ok) {
          std::lock_guard<std::mutex> lock(mutex_);
          cached_features_ = values;
          cached_response_ = response;
          cached_model_version_ = inference_->ExpectedModelVersion();
        }
      }
      have_response = true;
      t.inference_status = StatusText(response.status);
      if (response.status != InferenceStatus::Ok) {
        t.error_code = response.error_code.empty() ? ToString(response.status) : response.error_code;
        t.error = response.message;
      }
      t.round_trip_ms = response.round_trip_ms;
      t.inference_ms = response.inference_ms;
    }
  }

  // Stale protection: the evaluation that asked must still be current.
  if (have_response && response.status == InferenceStatus::Ok) {
    std::function<bool(const rt::EvaluationContext&)> validator;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      validator = validator_;
    }
    if (validator && !validator(input.context)) {
      t.inference_status = "stale";
      t.error_code = "E_CONTEXT_CHANGED";
      t.error = "the evaluation context changed during inference";
      if (events_) events_->Emit(ModelEventType::StaleInferenceDiscarded, t.error, t.request_id);
    }
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (t.inference_status == "stale") ++counters_.stale;
    else if (t.inference_status != "ok" && t.inference_status != "abstained") ++counters_.failed;
  }

  // ---- Model output, mapping and candidate check ----
  const rt::CandidateEvaluation* candidate = nullptr;
  if (t.inference_status == "ok") {
    t.model_label = response.model_label;
    t.label_source = response.label_source;
    t.gate_probability = response.gate_probability;
    t.gate_threshold = response.gate_threshold;
    t.stage2_ran = response.stage2_ran;
    t.stage2_probabilities = response.stage2_probabilities;
    t.probabilities_calibrated = response.probabilities_calibrated;
    t.out_of_range_features = response.out_of_training_range;
    t.distribution_warnings = response.distribution_warnings;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      ++counters_.model_ok;
    }
    const auto mapped = MapModelLabel(response.model_label);
    if (!mapped) {
      t.candidate_check = "unknown_label: '" + response.model_label + "'";
    } else {
      t.predicted_algorithm = mapped;
      for (const rt::CandidateEvaluation& c : input.candidates) {
        if (c.algorithm == *mapped) candidate = &c;
      }
      if (!candidate) {
        t.candidate_check = "not_in_registry: " + std::string(alg::ToString(*mapped)) + " has no candidate evaluation";
      } else if (!candidate->considered) {
        t.candidate_check = "not_eligible: " + candidate->eligibility_reasons;
        candidate = nullptr;
      } else if (!candidate->Usable()) {
        t.candidate_check = "no_usable_path: " + (candidate->note.empty() ? candidate->computation_status : candidate->note);
        candidate = nullptr;
      } else {
        t.candidate_check = "usable";
      }
      if (det.recommended) {
        t.agrees_with_deterministic = *mapped == *det.recommended;
        std::lock_guard<std::mutex> lock(mutex_);
        ++counters_.comparisons;
        if (*t.agrees_with_deterministic) ++counters_.agreements;
      }
    }
    if (!candidate) {
      std::lock_guard<std::mutex> lock(mutex_);
      ++counters_.rejected;
    }
    if (!candidate && events_) {
      events_->Emit(ModelEventType::RecommendationRejected, response.model_label + ": " + t.candidate_check, t.request_id);
    }
  }

  const std::string model_id = t.model_name + "/" + t.model_version + "/" + t.feature_contract_version;

  // ---- Shadow: record only ----
  if (mode == rt::RecommendationMode::Shadow) {
    t.source = rt::RecommendationSource::DeterministicShadow;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      ++counters_.shadow;
    }
    rt::Recommendation r = det;
    r.provider_id = det.provider_id + " (shadow: " + model_id + ")";
    std::string note = "shadow: model ";
    if (t.inference_status == "ok") {
      note += "predicted " + t.model_label + " (" + t.candidate_check + ")";
    } else {
      note += t.inference_status + (t.error.empty() ? "" : " (" + t.error + ")");
    }
    r.explanation = det.explanation + " [" + note + "; deterministic ranking used]";
    return finish(r, t);
  }

  // ---- AI preferred ----
  if (!candidate) {
    t.source = rt::RecommendationSource::DeterministicFallback;
    t.fallback_reason = t.inference_status == "ok" ? t.candidate_check
                                                   : t.inference_status + (t.error_code.empty() ? "" : " " + t.error_code) +
                                                         (t.error.empty() ? "" : ": " + t.error);
    {
      std::lock_guard<std::mutex> lock(mutex_);
      ++counters_.fallbacks;
    }
    if (events_) events_->Emit(ModelEventType::DeterministicFallbackActivated, t.fallback_reason, t.request_id);
    rt::Recommendation r = det;
    r.provider_id = det.provider_id + " (fallback from " + model_id + ")";
    r.explanation = "deterministic fallback (" + t.fallback_reason + "): " + det.explanation;
    return finish(r, t);
  }

  t.source = rt::RecommendationSource::ExpertModel;
  t.controlled_ranking = true;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    ++counters_.ai_ranked_first;
  }
  rt::Recommendation r = det;  // Same evidence quality: J(P) estimate on this graph.
  r.status = rt::RecommendationStatus::Recommended;
  r.provider_id = model_id;
  r.ranking.clear();
  rt::RankedCandidate first;
  first.algorithm = candidate->algorithm;
  first.path = candidate->path;
  first.objective = candidate->objective;
  first.computation_ms = candidate->computation_ms;
  first.note = "expert model recommendation";
  r.ranking.push_back(first);
  for (const rt::RankedCandidate& c : det.ranking) {
    if (c.algorithm != candidate->algorithm) r.ranking.push_back(c);
  }
  r.recommended = candidate->algorithm;
  r.estimated_objective = candidate->objective;
  r.estimated_improvement.reset();
  r.estimated_improvement_relative.reset();
  std::optional<double> active_objective;
  if (input.active_path && input.graph) {
    const fwd::PathValidation v = fwd::ValidatePath(*input.graph, input.problem, *input.active_path);
    if (v.valid && v.objective) active_objective = v.objective;
  }
  if (active_objective && candidate->objective) {
    r.estimated_improvement = *active_objective - *candidate->objective;
    if (std::fabs(*active_objective) > 1e-12) {
      r.estimated_improvement_relative = *r.estimated_improvement / std::fabs(*active_objective);
    }
  }
  r.source = "expert model " + t.model_version + " (first), then forwarding_objective_v" +
             std::to_string(fwd::ForwardingObjective::kVersion) + " ranking on graph version " +
             std::to_string(input.graph->version());
  std::ostringstream text;
  text << "expert model " << t.model_version << ": gate P(metaheuristic)=" << Fixed(*t.gate_probability)
       << (t.stage2_ran ? " >= " : " < ") << "threshold " << Fixed(*t.gate_threshold, 2) << " -> " << t.model_label;
  if (t.stage2_ran) {
    for (const auto& p : t.stage2_probabilities) {
      if (p.first == t.model_label) text << " (stage 2 P=" << Fixed(p.second) << ")";
    }
  }
  text << "; " << alg::ToString(candidate->algorithm) << " candidate is eligible and valid with J(P)="
       << Fixed(*candidate->objective) << " on " << fwd::PathText(candidate->path);
  if (active_objective) text << "; active path J(P)=" << Fixed(*active_objective);
  if (det.recommended && *det.recommended != candidate->algorithm) {
    text << "; the deterministic ranking prefers " << alg::ToString(*det.recommended);
  }
  text << ". Model probabilities are uncalibrated scores, not evidence of improvement; the runtime's evidence "
          "checks and stability gates still decide.";
  r.explanation = text.str();
  return finish(r, t);
}

}  // namespace aafr::model
