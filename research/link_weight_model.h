#pragma once

// The imported XGBoost v3 link-weight regressor (research/models/xgboost_v3),
// run through the official XGBoost C API. Plain C++17; the XGBoost shared
// library (xgboost.dll / libxgboost.so, version 3.x) is loaded at run time
// from an explicit path, so a missing library is a reported error, not a
// build failure. Not used by the live runtime or the browser.
//
// Model contract aafr.research.link_weight.v1:
//   inputs, in this order:  delay_ms (ms), loss_ratio (0..1),
//                           inv_bottleneck_bw (1 / (Mb/s))
//   normalization:          z = (x - mean) / scale from scaler_regressor.json,
//                           computed in double, passed to XGBoost as float32
//   output:                 raw float32 prediction; W_link = max(0, raw)
//
// Two ways to build the inverse-bandwidth input, kept apart on purpose:
//   TrainingInput  reproduces the training preprocessing,
//                  1 / max(throughput, 1e-6), throughput = a scenario's
//                  aggregate delivered traffic (the training proxy)
//   LinkInput      for a link: 1 / available bandwidth c (1 - u)
// They share a unit, not a meaning: delivered traffic is not link capacity.
//
// Nothing is defaulted: a missing or invalid input gives an explicit
// unavailable result for that row and no weight. There is no fallback model.

#include <array>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "research/normalization.h"
#include "research/path_algorithms.h"
#include "research/path_metrics.h"

namespace aafr::research {

inline constexpr const char* kLinkWeightContract = "aafr.research.link_weight.v1";
inline constexpr std::array<const char*, 3> kLinkWeightFeatures = {"delay_ms", "loss_ratio", "inv_bottleneck_bw"};
inline constexpr double kTrainingThroughputFloor = 1e-6;  // From the training script.

struct LinkWeightInput {
  std::optional<double> delay_ms;
  std::optional<double> loss_ratio;
  std::optional<double> inv_bottleneck_bw;
};

// Training preprocessing: inv_bottleneck_bw = 1 / max(throughput, 1e-6).
LinkWeightInput TrainingInput(std::optional<double> latency_ms, std::optional<double> loss_ratio,
                              std::optional<double> throughput_mbps);
// Link input: delay and loss of the link, inv_bottleneck_bw = 1 / (c (1 - u)).
// The inverse bandwidth is left unavailable when c, u is missing or invalid
// or the available bandwidth is 0 (no floor is applied).
LinkWeightInput LinkInput(const LinkObservation& link);

enum class PredictionStatus {
  Ok,
  MissingInput,  // An input is absent.
  InvalidInput,  // Non-finite, negative delay, loss outside [0, 1], inverse bandwidth not positive.
  ModelError,    // XGBoost reported an error or a non-finite prediction.
};

struct LinkWeightPrediction {
  PredictionStatus status{PredictionStatus::MissingInput};
  std::optional<float> raw;      // XGBoost output before the floor.
  std::optional<double> weight;  // max(0, raw).
  std::string message;
};

struct ModelInfo {
  std::string model_id;
  std::string source_repository;
  std::string source_commit;
  std::string library_path;
  int xgboost_major{0}, xgboost_minor{0}, xgboost_patch{0};
  std::uint64_t num_features{0};
  int boosted_rounds{0};
  std::array<double, 3> mean{};
  std::array<double, 3> scale{};
  std::vector<std::string> verified_artifacts;  // file: sha256
};

class LinkWeightModel;

struct LoadResult {
  std::unique_ptr<LinkWeightModel> model;
  std::string error;  // Empty on success.
};

class LinkWeightModel {
 public:
  // Verifies every artifact against provenance.json (SHA-256 and size), the
  // feature order in all files, the scaler, the XGBoost library version
  // (major 3) and the loaded model (3 features, the recorded tree count).
  static LoadResult Load(const std::string& model_dir, const std::string& xgboost_library);
  ~LinkWeightModel();
  LinkWeightModel(const LinkWeightModel&) = delete;
  LinkWeightModel& operator=(const LinkWeightModel&) = delete;

  // One XGBoost call for all valid rows; invalid rows are reported per row.
  std::vector<LinkWeightPrediction> Predict(const std::vector<LinkWeightInput>& inputs) const;
  LinkWeightPrediction Predict(const LinkWeightInput& input) const;

  // The float32 features exactly as passed to XGBoost; nullopt when invalid.
  std::optional<std::array<float, 3>> Normalize(const LinkWeightInput& input) const;

  const ModelInfo& info() const { return info_; }

 private:
  struct Library;
  LinkWeightModel() = default;
  std::unique_ptr<Library> library_;
  void* booster_{nullptr};
  ModelInfo info_;
  mutable std::mutex mutex_;  // One prediction at a time on the booster.
};

// Statistics of normalization.json as the frozen statistics of J_eng and G.
// They were computed over all 3,500 training scenarios (test rows included,
// population standard deviation), from scenario-level proxies:
//   Delay <- latency, Loss <- loss, InverseBandwidth <- 1 / throughput,
//   Hops <- avg_hop_count, Bandwidth <- offered_load_mbps,
//   Rtt <- latency (one-way scenario latency, not an RTT),
//   Throughput <- throughput (delivered traffic).
// nullopt (with `error`) when the file is missing, malformed or fails its hash.
std::optional<NormalizationStatistics> PathObjectiveStatistics(const std::string& model_dir, std::string* error = nullptr);

// Sets predicted_weight on every available link of `graph` from the model;
// a link whose prediction is unavailable gets no weight (A* then refuses with
// MissingCost and names it). Returns one prediction per link, in link order
// (unavailable links get MissingInput with a note).
std::vector<LinkWeightPrediction> AssignPredictedWeights(const LinkWeightModel& model, ResearchGraph& graph);

const char* ToString(PredictionStatus status);

}  // namespace aafr::research
