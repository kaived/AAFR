#include "runtime/adaptive/model_trace.h"

#include <cmath>
#include <cstdio>
#include <sstream>

namespace aafr::runtime {

const char* ToString(RecommendationMode value) {
  switch (value) {
    case RecommendationMode::Deterministic: return "deterministic";
    case RecommendationMode::Shadow: return "shadow";
    case RecommendationMode::AiPreferred: return "ai_preferred";
  }
  return "unknown";
}

std::optional<RecommendationMode> ParseRecommendationMode(const std::string& text) {
  if (text == "deterministic" || text == "off" || text == "disabled") return RecommendationMode::Deterministic;
  if (text == "shadow") return RecommendationMode::Shadow;
  if (text == "ai_preferred" || text == "preferred" || text == "ai") return RecommendationMode::AiPreferred;
  return std::nullopt;
}

const char* ToString(RecommendationSource value) {
  switch (value) {
    case RecommendationSource::Deterministic: return "deterministic";
    case RecommendationSource::ExpertModel: return "expert_model";
    case RecommendationSource::DeterministicFallback: return "deterministic_fallback";
    case RecommendationSource::DeterministicShadow: return "deterministic_shadow";
  }
  return "unknown";
}

namespace {
std::string Quote(const std::string& s) {
  std::string out = "\"";
  for (unsigned char c : s) {
    if (c == '"' || c == '\\') {
      out.push_back('\\');
      out.push_back(static_cast<char>(c));
    } else if (c < 0x20) {
      char buf[8];
      std::snprintf(buf, sizeof(buf), "\\u%04x", c);
      out += buf;
    } else {
      out.push_back(static_cast<char>(c));
    }
  }
  return out + "\"";
}

std::string Num(const std::optional<double>& v) {
  if (!v || !std::isfinite(*v)) return "null";
  std::ostringstream o;
  o.precision(10);
  o << *v;
  return o.str();
}

std::string List(const std::vector<std::string>& v) {
  std::string out = "[";
  for (std::size_t i = 0; i < v.size(); ++i) out += (i ? "," : "") + Quote(v[i]);
  return out + "]";
}

std::string Alg(const std::optional<algorithms::AlgorithmId>& id) {
  return id ? Quote(algorithms::ToString(*id)) : "null";
}
}  // namespace

std::string ToJson(const ModelRecommendationTrace& t) {
  std::ostringstream o;
  o << "{\"mode\":" << Quote(ToString(t.mode)) << ",\"source\":" << Quote(ToString(t.source))
    << ",\"model_name\":" << Quote(t.model_name) << ",\"model_version\":" << Quote(t.model_version)
    << ",\"feature_contract_version\":" << Quote(t.feature_contract_version)
    << ",\"request_id\":" << Quote(t.request_id) << ",\"evaluation_id\":" << Quote(t.evaluation_id)
    << ",\"network_state_id\":" << t.network_state_id << ",\"graph_version\":" << t.graph_version
    << ",\"inference_status\":" << Quote(t.inference_status) << ",\"error_code\":" << Quote(t.error_code)
    << ",\"error\":" << Quote(t.error) << ",\"cached\":" << (t.cached ? "true" : "false")
    << ",\"model_label\":" << (t.model_label.empty() ? "null" : Quote(t.model_label))
    << ",\"predicted_algorithm\":" << Alg(t.predicted_algorithm) << ",\"label_source\":" << Quote(t.label_source)
    << ",\"gate_probability\":" << Num(t.gate_probability) << ",\"gate_threshold\":" << Num(t.gate_threshold)
    << ",\"stage2_ran\":" << (t.stage2_ran ? "true" : "false") << ",\"stage2_probabilities\":";
  if (t.stage2_probabilities.empty()) {
    o << "null";
  } else {
    o << "{";
    for (std::size_t i = 0; i < t.stage2_probabilities.size(); ++i) {
      o << (i ? "," : "") << Quote(t.stage2_probabilities[i].first) << ":" << Num(t.stage2_probabilities[i].second);
    }
    o << "}";
  }
  o << ",\"probabilities_calibrated\":" << (t.probabilities_calibrated ? "true" : "false")
    << ",\"inference_ms\":" << Num(t.inference_ms) << ",\"round_trip_ms\":" << Num(t.round_trip_ms)
    << ",\"feature_ms\":" << Num(t.feature_ms) << ",\"feature_summary\":" << Quote(t.feature_summary)
    << ",\"missing_features\":" << List(t.missing_features)
    << ",\"substituted_features\":" << List(t.substituted_features)
    << ",\"out_of_range_features\":" << List(t.out_of_range_features)
    << ",\"distribution_warnings\":" << List(t.distribution_warnings)
    << ",\"candidate_check\":" << Quote(t.candidate_check)
    << ",\"controlled_ranking\":" << (t.controlled_ranking ? "true" : "false")
    << ",\"fallback_reason\":" << Quote(t.fallback_reason)
    << ",\"deterministic_algorithm\":" << Alg(t.deterministic_algorithm) << ",\"agrees_with_deterministic\":"
    << (t.agrees_with_deterministic ? (*t.agrees_with_deterministic ? "true" : "false") : "null")
    << ",\"acquisition_contract_version\":" << Quote(t.acquisition_contract_version)
    << ",\"input_domain\":" << Quote(t.input_domain) << ",\"features\":{\"required\":" << t.features_required
    << ",\"valid\":" << t.features_valid << ",\"missing\":" << t.features_missing << ",\"stale\":" << t.features_stale
    << ",\"insufficient\":" << t.features_insufficient << ",\"invalid\":" << t.features_invalid
    << ",\"unsupported\":" << t.features_unsupported << ",\"incompatible\":" << t.features_incompatible
    << ",\"estimates\":" << t.estimates_available << "}"
    << ",\"abstention_features\":" << List(t.abstention_features)
    << ",\"compatibility_status\":" << Quote(t.compatibility_status)
    << ",\"compatibility_reasons\":" << List(t.compatibility_reasons)
    << ",\"forwarding_version\":" << t.forwarding_version << "}";
  return o.str();
}

}  // namespace aafr::runtime
