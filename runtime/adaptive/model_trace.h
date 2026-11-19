#pragma once

// V0.7.2: what an expert-model recommendation provider asked, what the model
// answered and what became of the answer. Plain C++17, no model code.
//
// The trace is attached to a Recommendation and copied into the
// DecisionRecord, so a record always shows three different things apart:
//   - the model's prediction (model_label / predicted_algorithm),
//   - the algorithm the runtime selected as candidate (DecisionRecord), and
//   - the confirmed active algorithm after activation (DecisionRecord).
// Missing model outputs stay empty; nothing here is ever filled with a
// default that the model did not produce.

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "algorithms/algorithm_id.h"

namespace aafr::runtime {

enum class RecommendationMode {
  Deterministic,  // AI disabled: the deterministic provider only.
  Shadow,         // AI predicts and is recorded, never ranks or switches.
  AiPreferred,    // AI ranks first when its recommendation is usable; deterministic fallback otherwise.
};

const char* ToString(RecommendationMode value);
std::optional<RecommendationMode> ParseRecommendationMode(const std::string& text);

// Where the ranking the runtime acted on came from.
enum class RecommendationSource {
  Deterministic,          // Deterministic provider, AI disabled.
  ExpertModel,            // The model's usable recommendation ranks first.
  DeterministicFallback,  // AI preferred, but the model result was not usable.
  DeterministicShadow,    // Shadow mode: deterministic ranking, model only recorded.
};

const char* ToString(RecommendationSource value);

struct ModelRecommendationTrace {
  RecommendationMode mode{RecommendationMode::Deterministic};
  RecommendationSource source{RecommendationSource::Deterministic};
  std::string model_name;
  std::string model_version;
  std::string feature_contract_version;
  std::string request_id;
  std::string evaluation_id;
  std::uint64_t network_state_id{0};
  std::uint64_t graph_version{0};

  // ok | abstained | error | timeout | unavailable | stale | skipped
  std::string inference_status{"skipped"};
  std::string error_code;
  std::string error;
  bool cached{false};  // Same feature vector as the previous request; result reused.

  // Model output (empty when the model produced none).
  std::string model_label;                           // Raw label, e.g. "PSO".
  std::optional<algorithms::AlgorithmId> predicted_algorithm;  // After the validated mapping.
  std::string label_source;                          // stage1_below_threshold | stage2_argmax
  std::optional<double> gate_probability;            // Stage 1 P(metaheuristic), uncalibrated.
  std::optional<double> gate_threshold;              // From model metadata, unchanged.
  bool stage2_ran{false};
  std::vector<std::pair<std::string, double>> stage2_probabilities;  // Uncalibrated.
  bool probabilities_calibrated{false};

  // Timing (ms). inference_ms is reported by the worker; round_trip_ms is
  // request write to validated response; feature_ms is the C++ adapter.
  std::optional<double> inference_ms;
  std::optional<double> round_trip_ms;
  std::optional<double> feature_ms;

  // Inputs
  std::string feature_summary;  // e.g. "graph=12 configured=23 substitute=0".
  std::vector<std::string> missing_features;
  std::vector<std::string> substituted_features;
  std::vector<std::string> out_of_range_features;
  std::vector<std::string> distribution_warnings;

  // What the provider did with it.
  std::string candidate_check;  // usable | not_in_registry | unknown_label | not_eligible: ... | no_usable_path: ...
  bool controlled_ranking{false};  // The model's candidate was ranked first.
  std::string fallback_reason;     // Why the deterministic ranking was used (AI preferred only).
  std::optional<algorithms::AlgorithmId> deterministic_algorithm;
  std::optional<bool> agrees_with_deterministic;

  // V0.7.4 feature acquisition and compatibility. Counts are over the 35
  // raw features; each feature has exactly one status.
  std::string acquisition_contract_version;
  std::string input_domain;  // measured | configured_profile | mixed | none
  int features_required{35};
  int features_valid{0};
  int features_missing{0};
  int features_stale{0};
  int features_insufficient{0};
  int features_invalid{0};       // Invalid, invalid unit or out of range.
  int features_unsupported{0};   // Category D.
  int features_incompatible{0};  // Category C or E.
  int estimates_available{0};    // Category-C estimates reported (never used).
  std::vector<std::string> abstention_features;  // The features that caused an abstention.
  // in_distribution | potentially_out_of_distribution |
  // clearly_out_of_distribution | insufficient_information | "" (not assessed)
  std::string compatibility_status;
  std::vector<std::string> compatibility_reasons;
  std::uint64_t forwarding_version{0};  // NetworkState::forwarding->version used.
};

std::string ToJson(const ModelRecommendationTrace& trace);

}  // namespace aafr::runtime
