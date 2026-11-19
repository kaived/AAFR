#pragma once

// V0.7.4 model input compatibility. Plain C++17.
//
// Decides whether a raw feature vector (ModelFeatureVector) resembles the
// data the expert model was trained on, using the training profile
// (expert_model/models/training_profile.json, built by
// expert_model/evaluation/build_training_profile.py from the model project's
// own training CSV). The model and its preprocessing are not touched.
//
// Classification:
//   ClearlyOutOfDistribution  a topology/request feature lies outside the
//                             training support (no training row had such a
//                             graph), or >= 3 features lie outside it, or one
//                             lies outside it by more than the training range
//   PotentiallyOutOfDistribution
//                             1-2 features slightly outside the support, or
//                             the vector is complete and in support but its
//                             Mahalanobis distance to every training category
//                             exceeds that category's own empirical 99th
//                             percentile, or the deployed model input is
//                             known to differ from every training row (the
//                             scenario one-hots, see below)
//   InDistribution            complete, every feature inside the support and
//                             within the empirical p99 distance of at least
//                             one training category
//   InsufficientInformation   no profile, or the vector is incomplete and no
//                             available feature is outside the support (a
//                             partial vector cannot be shown to be in
//                             distribution)
//
// Range checks alone never yield InDistribution: the distance criterion is
// required. The thresholds are empirical quantiles of the training rows'
// own distances (the features are skewed and partly constant, so no
// chi-square assumption is made). This is a screening rule, not a
// statistical test with a stated error rate.
//
// The scenario one-hots: training rows carry their category's one-hot
// (cat_C_Unreliable has stage-1 importance 1.0); features.py sets all seven
// to 0 when no category is given, which no training row had. With
// `account_for_category_onehots` (the default) no input can therefore be
// classified better than PotentiallyOutOfDistribution.

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "runtime/model/model_feature_adapter.h"

namespace aafr::model {

struct TrainingFeatureStats {
  std::string name;
  double min{0.0}, max{0.0}, mean{0.0}, std{0.0};
  double p01{0.0}, p50{0.0}, p99{0.0};
};

struct TrainingCategory {
  std::string category;
  int rows{0};
  std::vector<double> mean_z;                // 35, standardized.
  std::vector<std::vector<double>> inv_cov;  // 35x35, of standardized features (ridge-regularized).
  double distance_p95{0.0}, distance_p99{0.0}, distance_max{0.0};
  double num_nodes_min{0.0}, num_nodes_max{0.0};
};

struct TrainingProfile {
  std::string model_version;
  std::string feature_contract_version;
  std::string dataset_sha256;
  int rows{0};
  std::vector<double> mean, scale;  // Standardization, contract order.
  std::vector<TrainingFeatureStats> features;  // Contract order.
  std::vector<TrainingCategory> categories;
  std::vector<std::pair<std::string, int>> algorithm_labels;  // target_algo_eps1 counts.
  std::vector<std::pair<std::string, int>> gate_labels;       // invoke_metaheuristic counts.
};

struct TrainingProfileLoad {
  std::shared_ptr<const TrainingProfile> profile;
  std::vector<std::string> errors;
};
TrainingProfileLoad ParseTrainingProfile(const std::string& json_text, const std::string& expected_model_version = "");
TrainingProfileLoad LoadTrainingProfile(const std::string& path, const std::string& expected_model_version = "");

enum class CompatibilityStatus {
  InDistribution,
  PotentiallyOutOfDistribution,
  ClearlyOutOfDistribution,
  InsufficientInformation,
};
const char* ToString(CompatibilityStatus value);

struct OutOfSupport {
  std::string feature;
  double value{0.0};
  double training_min{0.0}, training_max{0.0};
  double excess_ranges{0.0};  // Distance outside the range, in training ranges.
};

struct CompatibilityOptions {
  bool account_for_category_onehots{true};
  int clearly_ood_feature_count{3};
  double clearly_ood_excess_ranges{1.0};
};

struct CompatibilityAssessment {
  CompatibilityStatus status{CompatibilityStatus::InsufficientInformation};
  std::vector<std::string> reasons;
  std::vector<OutOfSupport> out_of_support;
  int assessed_features{0};  // Accepted features compared with the profile.
  std::optional<std::string> nearest_category;
  std::optional<double> nearest_distance;
  std::optional<double> nearest_threshold;  // That category's empirical p99.
  double assess_ms{0.0};

  std::string Summary() const;
};

CompatibilityAssessment AssessCompatibility(const ModelFeatureVector& features, const TrainingProfile* profile,
                                            const CompatibilityOptions& options = {});

// AAFR_TRAINING_PROFILE from the environment, else the build's
// expert_model/models/training_profile.json.
std::string DefaultTrainingProfilePath();

// Mahalanobis distance of standardized `z` to a category; exposed for tests.
double MahalanobisDistance(const std::vector<double>& z, const TrainingCategory& category);

}  // namespace aafr::model
