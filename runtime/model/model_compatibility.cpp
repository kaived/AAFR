#include "runtime/model/model_compatibility.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <sstream>

#include "runtime/model/json_value.h"

namespace aafr::model {

const char* ToString(CompatibilityStatus value) {
  switch (value) {
    case CompatibilityStatus::InDistribution: return "in_distribution";
    case CompatibilityStatus::PotentiallyOutOfDistribution: return "potentially_out_of_distribution";
    case CompatibilityStatus::ClearlyOutOfDistribution: return "clearly_out_of_distribution";
    case CompatibilityStatus::InsufficientInformation: return "insufficient_information";
  }
  return "unknown";
}

namespace {

std::optional<std::vector<double>> Numbers(const JsonValue* v, std::size_t expected) {
  if (!v || !v->IsArray() || v->AsArray().size() != expected) return std::nullopt;
  std::vector<double> out;
  for (const JsonValue& x : v->AsArray()) {
    if (!x.IsNumber() || !std::isfinite(x.AsNumber())) return std::nullopt;
    out.push_back(x.AsNumber());
  }
  return out;
}

std::vector<std::pair<std::string, int>> Counts(const JsonValue* v) {
  std::vector<std::pair<std::string, int>> out;
  if (!v || !v->IsObject()) return out;
  for (const auto& e : v->AsObject()) {
    if (e.second.IsNumber()) out.emplace_back(e.first, static_cast<int>(e.second.AsNumber()));
  }
  std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
  return out;
}

}  // namespace

TrainingProfileLoad ParseTrainingProfile(const std::string& text, const std::string& expected_model_version) {
  TrainingProfileLoad r;
  const JsonParseResult parsed = ParseJson(text);
  if (!parsed.value || !parsed.value->IsObject()) {
    r.errors.push_back("training profile is not a JSON object: " + parsed.error);
    return r;
  }
  const JsonValue& j = *parsed.value;
  if (j.GetString("schema").value_or("") != "aafr.training_profile.v1") r.errors.push_back("unknown profile schema");
  auto p = std::make_shared<TrainingProfile>();
  p->model_version = j.GetString("model_version").value_or("");
  p->feature_contract_version = j.GetString("feature_contract_version").value_or("");
  p->dataset_sha256 = j.GetString("dataset_sha256").value_or("");
  p->rows = static_cast<int>(j.GetNumber("rows").value_or(0.0));
  if (!expected_model_version.empty() && p->model_version != expected_model_version) {
    r.errors.push_back("profile is for model " + p->model_version + ", expected " + expected_model_version);
  }
  if (p->feature_contract_version != kFeatureContractVersion) {
    r.errors.push_back("profile feature contract " + p->feature_contract_version + " != " + kFeatureContractVersion);
  }
  const auto& contract = RawFeatureContract();
  const std::size_t n = contract.size();
  const JsonValue* standard = j.Find("standardisation");
  const auto mean = Numbers(standard ? standard->Find("mean") : nullptr, n);
  const auto scale = Numbers(standard ? standard->Find("scale") : nullptr, n);
  if (!mean || !scale) {
    r.errors.push_back("standardisation must hold 35 finite means and scales");
  } else {
    p->mean = *mean;
    p->scale = *scale;
    for (double s : p->scale) {
      if (!(s > 0.0)) r.errors.push_back("nonpositive standardisation scale");
    }
  }
  const JsonValue* raw = j.Find("raw_features");
  if (!raw || !raw->IsArray() || raw->AsArray().size() != n) {
    r.errors.push_back("raw_features must list the 35 contract features");
  } else {
    for (std::size_t i = 0; i < n; ++i) {
      const JsonValue& f = raw->AsArray()[i];
      TrainingFeatureStats s;
      s.name = f.GetString("name").value_or("");
      if (s.name != contract[i].name) r.errors.push_back("profile feature " + std::to_string(i) + " is '" + s.name + "'");
      s.min = f.GetNumber("min").value_or(std::nan(""));
      s.max = f.GetNumber("max").value_or(std::nan(""));
      s.mean = f.GetNumber("mean").value_or(std::nan(""));
      s.std = f.GetNumber("std").value_or(std::nan(""));
      s.p01 = f.GetNumber("p01").value_or(std::nan(""));
      s.p50 = f.GetNumber("p50").value_or(std::nan(""));
      s.p99 = f.GetNumber("p99").value_or(std::nan(""));
      if (!std::isfinite(s.min) || !std::isfinite(s.max) || s.max < s.min) r.errors.push_back("bad range for " + s.name);
      p->features.push_back(s);
    }
  }
  const JsonValue* cats = j.Find("categories");
  if (!cats || !cats->IsArray() || cats->AsArray().empty()) {
    r.errors.push_back("profile has no categories");
  } else {
    for (const JsonValue& c : cats->AsArray()) {
      TrainingCategory t;
      t.category = c.GetString("category").value_or("");
      t.rows = static_cast<int>(c.GetNumber("rows").value_or(0.0));
      const auto mz = Numbers(c.Find("mean_z"), n);
      const JsonValue* inv = c.Find("inv_cov_z");
      bool ok = mz.has_value() && inv && inv->IsArray() && inv->AsArray().size() == n;
      if (ok) {
        t.mean_z = *mz;
        for (const JsonValue& row : inv->AsArray()) {
          const auto values = Numbers(&row, n);
          if (!values) {
            ok = false;
            break;
          }
          t.inv_cov.push_back(*values);
        }
      }
      t.distance_p95 = c.GetNumber("distance_p95").value_or(std::nan(""));
      t.distance_p99 = c.GetNumber("distance_p99").value_or(std::nan(""));
      t.distance_max = c.GetNumber("distance_max").value_or(std::nan(""));
      t.num_nodes_min = c.GetNumber("num_nodes_min").value_or(std::nan(""));
      t.num_nodes_max = c.GetNumber("num_nodes_max").value_or(std::nan(""));
      if (!ok || !(t.distance_p99 > 0.0)) {
        r.errors.push_back("category '" + t.category + "' is incomplete");
        continue;
      }
      p->categories.push_back(std::move(t));
    }
  }
  if (const JsonValue* labels = j.Find("labels")) {
    p->algorithm_labels = Counts(labels->Find("target_algo_eps1"));
    p->gate_labels = Counts(labels->Find("invoke_metaheuristic"));
  }
  if (r.errors.empty()) r.profile = std::move(p);
  return r;
}

TrainingProfileLoad LoadTrainingProfile(const std::string& path, const std::string& expected_model_version) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    TrainingProfileLoad r;
    r.errors.push_back("cannot open training profile " + path);
    return r;
  }
  std::ostringstream text;
  text << in.rdbuf();
  if (text.str().size() > 8u * 1024u * 1024u) {
    TrainingProfileLoad r;
    r.errors.push_back("training profile too large");
    return r;
  }
  return ParseTrainingProfile(text.str(), expected_model_version);
}

std::string DefaultTrainingProfilePath() {
#ifdef _MSC_VER
  char* value = nullptr;
  std::size_t length = 0;
  std::string env;
  if (_dupenv_s(&value, &length, "AAFR_TRAINING_PROFILE") == 0 && value) {
    env = value;
    free(value);
  }
#else
  const char* raw = std::getenv("AAFR_TRAINING_PROFILE");
  const std::string env = raw ? raw : "";
#endif
  if (!env.empty()) return env;
#ifdef AAFR_DEFAULT_TRAINING_PROFILE
  return AAFR_DEFAULT_TRAINING_PROFILE;
#else
  return "";
#endif
}

double MahalanobisDistance(const std::vector<double>& z, const TrainingCategory& c) {
  const std::size_t n = z.size();
  if (c.mean_z.size() != n || c.inv_cov.size() != n) return std::numeric_limits<double>::infinity();
  std::vector<double> d(n);
  for (std::size_t i = 0; i < n; ++i) d[i] = z[i] - c.mean_z[i];
  double sum = 0.0;
  for (std::size_t i = 0; i < n; ++i) {
    double row = 0.0;
    for (std::size_t k = 0; k < n; ++k) row += c.inv_cov[i][k] * d[k];
    sum += d[i] * row;
  }
  return std::sqrt(std::max(0.0, sum));
}

std::string CompatibilityAssessment::Summary() const {
  std::ostringstream o;
  o << ToString(status) << " (assessed " << assessed_features << ", outside support " << out_of_support.size();
  if (nearest_category) {
    o.setf(std::ios::fixed);
    o.precision(2);
    o << ", nearest " << *nearest_category << " d=" << *nearest_distance << " vs p99 " << *nearest_threshold;
  }
  o << ")";
  return o.str();
}

CompatibilityAssessment AssessCompatibility(const ModelFeatureVector& v, const TrainingProfile* profile,
                                            const CompatibilityOptions& options) {
  const auto started = std::chrono::steady_clock::now();
  CompatibilityAssessment a;
  const auto done = [&]() {
    a.assess_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    return a;
  };
  if (!profile) {
    a.status = CompatibilityStatus::InsufficientInformation;
    a.reasons.push_back("no training profile loaded");
    return done();
  }
  const auto& contract = RawFeatureContract();
  std::vector<std::optional<double>> values(contract.size());
  for (std::size_t i = 0; i < contract.size() && i < v.features.size(); ++i) {
    if (v.features[i].name == contract[i].name) values[i] = v.features[i].value;
  }

  bool topology_outside = false;
  bool large_excess = false;
  for (std::size_t i = 0; i < contract.size(); ++i) {
    if (!values[i] || i >= profile->features.size()) continue;
    ++a.assessed_features;
    const TrainingFeatureStats& s = profile->features[i];
    const double x = *values[i];
    if (x >= s.min && x <= s.max) continue;
    const double range = std::max(s.max - s.min, 1e-9);
    OutOfSupport o{s.name, x, s.min, s.max, (x < s.min ? s.min - x : x - s.max) / range};
    if (contract[i].group == FeatureGroup::Topology || contract[i].group == FeatureGroup::Request) topology_outside = true;
    if (o.excess_ranges > options.clearly_ood_excess_ranges) large_excess = true;
    a.out_of_support.push_back(o);
  }
  for (const OutOfSupport& o : a.out_of_support) {
    std::ostringstream r;
    r << o.feature << "=" << o.value << " outside training [" << o.training_min << ", " << o.training_max << "]";
    a.reasons.push_back(r.str());
  }
  const bool complete = v.Complete() && a.assessed_features == static_cast<int>(contract.size());

  if (topology_outside || large_excess ||
      static_cast<int>(a.out_of_support.size()) >= options.clearly_ood_feature_count) {
    a.status = CompatibilityStatus::ClearlyOutOfDistribution;
    if (topology_outside) a.reasons.push_back("graph or request outside the training support: no training row had such a topology");
    return done();
  }
  if (!complete) {
    a.status = a.out_of_support.empty() ? CompatibilityStatus::InsufficientInformation
                                        : CompatibilityStatus::PotentiallyOutOfDistribution;
    a.reasons.push_back("incomplete vector (" + std::to_string(a.assessed_features) +
                        " of 35 features): distribution membership cannot be established");
    return done();
  }

  // Distance criterion on the complete, standardized raw vector.
  std::vector<double> z(contract.size());
  for (std::size_t i = 0; i < contract.size(); ++i) z[i] = (*values[i] - profile->mean[i]) / profile->scale[i];
  bool within = false;
  double best_ratio = std::numeric_limits<double>::infinity();
  for (const TrainingCategory& c : profile->categories) {
    const double d = MahalanobisDistance(z, c);
    if (d / c.distance_p99 < best_ratio) {
      best_ratio = d / c.distance_p99;
      a.nearest_category = c.category;
      a.nearest_distance = d;
      a.nearest_threshold = c.distance_p99;
    }
    within = within || d <= c.distance_p99;
  }
  if (!a.out_of_support.empty()) {
    a.status = CompatibilityStatus::PotentiallyOutOfDistribution;
  } else if (!within) {
    a.status = CompatibilityStatus::PotentiallyOutOfDistribution;
    a.reasons.push_back("Mahalanobis distance exceeds the empirical p99 of every training category");
  } else {
    a.status = CompatibilityStatus::InDistribution;
  }
  if (options.account_for_category_onehots && a.status == CompatibilityStatus::InDistribution) {
    a.status = CompatibilityStatus::PotentiallyOutOfDistribution;
    a.reasons.push_back("raw features within training distribution, but the deployed model input sets every scenario "
                        "one-hot to 0, a combination absent from training");
  }
  return done();
}

}  // namespace aafr::model
