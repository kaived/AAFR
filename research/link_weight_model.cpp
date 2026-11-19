#include "research/link_weight_model.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <sstream>

#include "research/json_reader.h"
#include "research/sha256.h"
#include "xgboost/c_api.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace aafr::research {

// The XGBoost entry points used here, typed from the official header and
// resolved from the shared library at run time. The library stays loaded for
// the life of the process (XGBoost keeps worker threads; unloading it while
// they exist is unsafe).
struct LinkWeightModel::Library {
  decltype(&::XGBoostVersion) version{nullptr};
  decltype(&::XGBGetLastError) last_error{nullptr};
  decltype(&::XGBoosterCreate) booster_create{nullptr};
  decltype(&::XGBoosterFree) booster_free{nullptr};
  decltype(&::XGBoosterLoadModel) load_model{nullptr};
  decltype(&::XGBoosterGetNumFeature) num_feature{nullptr};
  decltype(&::XGBoosterBoostedRounds) boosted_rounds{nullptr};
  decltype(&::XGDMatrixCreateFromMat) dmatrix_from_mat{nullptr};
  decltype(&::XGDMatrixFree) dmatrix_free{nullptr};
  decltype(&::XGBoosterPredict) predict{nullptr};

  std::string Error() const {
    const char* e = last_error ? last_error() : nullptr;
    return e ? std::string(e) : std::string("unknown XGBoost error");
  }
};

namespace {

void* OpenLibrary(const std::string& path, std::string& error) {
#if defined(_WIN32)
  const int n = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
  std::wstring wide(static_cast<std::size_t>(n > 0 ? n : 1), L'\0');
  if (n > 0) MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, wide.data(), n);
  // Resolve the library's own dependencies from its directory.
  HMODULE h = LoadLibraryExW(wide.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
  if (!h) error = "cannot load XGBoost library '" + path + "' (Windows error " + std::to_string(GetLastError()) + ")";
  return reinterpret_cast<void*>(h);
#else
  void* h = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
  if (!h) error = std::string("cannot load XGBoost library: ") + dlerror();
  return h;
#endif
}

void* Symbol(void* handle, const char* name) {
#if defined(_WIN32)
  return reinterpret_cast<void*>(GetProcAddress(reinterpret_cast<HMODULE>(handle), name));
#else
  return dlsym(handle, name);
#endif
}

template <typename F>
bool Bind(void* handle, const char* name, F& out, std::string& error) {
  out = reinterpret_cast<F>(Symbol(handle, name));
  if (!out && error.empty()) error = std::string("XGBoost library lacks ") + name;
  return out != nullptr;
}

std::string Join(const std::string& dir, const std::string& file) {
  if (dir.empty()) return file;
  const char last = dir.back();
  return (last == '/' || last == '\\') ? dir + file : dir + "/" + file;
}

std::optional<JsonNode> ReadJson(const std::string& path, std::string& error) {
  const auto bytes = ReadFileBytes(path);
  if (!bytes) {
    error = "cannot read " + path;
    return std::nullopt;
  }
  std::string why;
  auto node = ParseJson(*bytes, &why);
  if (!node) error = path + ": " + why;
  return node;
}

// Verifies every artifact listed in provenance.json; returns the parsed
// manifest, or nullopt with `error`.
std::optional<JsonNode> VerifyArtifacts(const std::string& dir, std::vector<std::string>* verified, std::string& error) {
  auto manifest = ReadJson(Join(dir, "provenance.json"), error);
  if (!manifest) return std::nullopt;
  const JsonNode* artifacts = manifest->Find("artifacts");
  if (!artifacts || artifacts->type != JsonNode::Type::Array || artifacts->array.empty()) {
    error = "provenance.json lists no artifacts";
    return std::nullopt;
  }
  for (const JsonNode& a : artifacts->array) {
    const auto file = a.String("file");
    const auto sha = a.String("sha256");
    const auto bytes = a.Number("bytes");
    if (!file || !sha || !bytes) {
      error = "provenance.json has an incomplete artifact entry";
      return std::nullopt;
    }
    const auto data = ReadFileBytes(Join(dir, *file));
    if (!data) {
      error = "missing artifact " + *file;
      return std::nullopt;
    }
    const std::string digest = Sha256Hex(*data);
    if (digest != *sha || static_cast<double>(data->size()) != *bytes) {
      error = "artifact " + *file + " does not match provenance.json (sha256 " + digest + ", " +
              std::to_string(data->size()) + " bytes)";
      return std::nullopt;
    }
    if (verified) verified->push_back(*file + ": " + digest);
  }
  return manifest;
}

bool SameFeatures(const std::vector<std::string>& names) {
  if (names.size() != kLinkWeightFeatures.size()) return false;
  for (std::size_t i = 0; i < names.size(); ++i) {
    if (names[i] != kLinkWeightFeatures[i]) return false;
  }
  return true;
}

// Validates one input row; on success fills x (contract order).
PredictionStatus CheckInput(const LinkWeightInput& in, std::array<double, 3>& x, std::string& message) {
  if (!in.delay_ms || !in.loss_ratio || !in.inv_bottleneck_bw) {
    message = std::string("missing ") + (!in.delay_ms ? "delay_ms" : !in.loss_ratio ? "loss_ratio" : "inv_bottleneck_bw");
    return PredictionStatus::MissingInput;
  }
  x = {*in.delay_ms, *in.loss_ratio, *in.inv_bottleneck_bw};
  if (!std::isfinite(x[0]) || x[0] < 0.0) {
    message = "delay_ms must be finite and >= 0";
    return PredictionStatus::InvalidInput;
  }
  if (!std::isfinite(x[1]) || x[1] < 0.0 || x[1] > 1.0) {
    message = "loss_ratio must lie within [0, 1]";
    return PredictionStatus::InvalidInput;
  }
  if (!std::isfinite(x[2]) || !(x[2] > 0.0)) {
    message = "inv_bottleneck_bw must be finite and > 0";
    return PredictionStatus::InvalidInput;
  }
  return PredictionStatus::Ok;
}

}  // namespace

LinkWeightInput TrainingInput(std::optional<double> latency_ms, std::optional<double> loss_ratio,
                              std::optional<double> throughput_mbps) {
  LinkWeightInput in;
  in.delay_ms = latency_ms;
  in.loss_ratio = loss_ratio;
  if (throughput_mbps && std::isfinite(*throughput_mbps)) {
    in.inv_bottleneck_bw = 1.0 / std::max(*throughput_mbps, kTrainingThroughputFloor);
  }
  return in;
}

LinkWeightInput LinkInput(const LinkObservation& link) {
  LinkWeightInput in;
  in.delay_ms = link.delay_ms;
  in.loss_ratio = link.loss_ratio;
  const PathMetrics m = ComputePathMetrics({link});
  if (m.bandwidth_mbps && *m.bandwidth_mbps > 0.0) in.inv_bottleneck_bw = 1.0 / *m.bandwidth_mbps;
  return in;
}

LoadResult LinkWeightModel::Load(const std::string& model_dir, const std::string& xgboost_library) {
  LoadResult out;
  std::unique_ptr<LinkWeightModel> model(new LinkWeightModel());
  ModelInfo& info = model->info_;
  std::string& error = out.error;

  const auto manifest = VerifyArtifacts(model_dir, &info.verified_artifacts, error);
  if (!manifest) return out;
  info.model_id = manifest->String("model_id").value_or("");
  if (const JsonNode* source = manifest->Find("source")) {
    info.source_repository = source->String("repository").value_or("");
    info.source_commit = source->String("commit").value_or("");
  }
  const JsonNode* model_meta = manifest->Find("model");
  const auto trees = model_meta ? model_meta->Number("num_trees") : std::nullopt;
  if (!trees) {
    error = "provenance.json lacks model.num_trees";
    return out;
  }

  // Feature order must be the contract's in every file that states it.
  const auto txt = ReadFileBytes(Join(model_dir, "features_regressor.txt"));
  std::vector<std::string> listed;
  if (txt) {
    std::istringstream words(*txt);
    for (std::string w; words >> w;) listed.push_back(w);
  }
  if (!SameFeatures(listed)) {
    error = "features_regressor.txt does not list delay_ms, loss_ratio, inv_bottleneck_bw in that order";
    return out;
  }
  const auto scaler = ReadJson(Join(model_dir, "scaler_regressor.json"), error);
  if (!scaler) return out;
  const auto scaler_features = scaler->Strings("features");
  const auto mean = scaler->Numbers("mean");
  const auto scale = scaler->Numbers("scale");
  if (!scaler_features || !SameFeatures(*scaler_features) || !mean || !scale || mean->size() != 3 || scale->size() != 3) {
    error = "scaler_regressor.json does not hold 3 features in contract order with 3 means and 3 scales";
    return out;
  }
  for (std::size_t i = 0; i < 3; ++i) {
    if (!std::isfinite((*mean)[i]) || !std::isfinite((*scale)[i]) || !((*scale)[i] > 0.0)) {
      error = "scaler_regressor.json has a non-finite mean or a non-positive scale";
      return out;
    }
    info.mean[i] = (*mean)[i];
    info.scale[i] = (*scale)[i];
  }
  const auto metrics = ReadJson(Join(model_dir, "regressor_metrics.json"), error);
  if (!metrics) return out;
  const auto metric_inputs = metrics->Strings("inputs");
  if (!metric_inputs || !SameFeatures(*metric_inputs)) {
    error = "regressor_metrics.json inputs differ from the feature contract";
    return out;
  }

  // XGBoost library.
  void* handle = OpenLibrary(xgboost_library, error);
  if (!handle) return out;
  info.library_path = xgboost_library;
  model->library_ = std::make_unique<Library>();
  auto& lib = *model->library_;
  Bind(handle, "XGBoostVersion", lib.version, error);
  Bind(handle, "XGBGetLastError", lib.last_error, error);
  Bind(handle, "XGBoosterCreate", lib.booster_create, error);
  Bind(handle, "XGBoosterFree", lib.booster_free, error);
  Bind(handle, "XGBoosterLoadModel", lib.load_model, error);
  Bind(handle, "XGBoosterGetNumFeature", lib.num_feature, error);
  Bind(handle, "XGBoosterBoostedRounds", lib.boosted_rounds, error);
  Bind(handle, "XGDMatrixCreateFromMat", lib.dmatrix_from_mat, error);
  Bind(handle, "XGDMatrixFree", lib.dmatrix_free, error);
  Bind(handle, "XGBoosterPredict", lib.predict, error);
  if (!error.empty()) return out;
  lib.version(&info.xgboost_major, &info.xgboost_minor, &info.xgboost_patch);
  if (info.xgboost_major != 3) {
    error = "XGBoost " + std::to_string(info.xgboost_major) + "." + std::to_string(info.xgboost_minor) +
            " found; the model needs XGBoost 3.x";
    return out;
  }

  BoosterHandle booster = nullptr;
  if (lib.booster_create(nullptr, 0, &booster) != 0) {
    error = "XGBoosterCreate: " + lib.Error();
    return out;
  }
  model->booster_ = booster;  // Freed by the destructor from here on.
  const std::string model_path = Join(model_dir, "regressor.json");
  if (lib.load_model(booster, model_path.c_str()) != 0) {
    error = "XGBoosterLoadModel: " + lib.Error();
    return out;
  }
  bst_ulong features = 0;
  int rounds = 0;
  if (lib.num_feature(booster, &features) != 0 || lib.boosted_rounds(booster, &rounds) != 0) {
    error = "cannot read the model shape: " + lib.Error();
    return out;
  }
  info.num_features = features;
  info.boosted_rounds = rounds;
  if (features != 3 || rounds != static_cast<int>(*trees)) {
    error = "model has " + std::to_string(features) + " features and " + std::to_string(rounds) +
            " rounds; expected 3 and " + std::to_string(static_cast<int>(*trees));
    return out;
  }
  out.model = std::move(model);
  return out;
}

LinkWeightModel::~LinkWeightModel() {
  if (booster_ && library_ && library_->booster_free) library_->booster_free(booster_);
}

std::optional<std::array<float, 3>> LinkWeightModel::Normalize(const LinkWeightInput& input) const {
  std::array<double, 3> x{};
  std::string message;
  if (CheckInput(input, x, message) != PredictionStatus::Ok) return std::nullopt;
  std::array<float, 3> z{};
  for (std::size_t i = 0; i < 3; ++i) z[i] = static_cast<float>((x[i] - info_.mean[i]) / info_.scale[i]);
  return z;
}

std::vector<LinkWeightPrediction> LinkWeightModel::Predict(const std::vector<LinkWeightInput>& inputs) const {
  std::vector<LinkWeightPrediction> out(inputs.size());
  std::vector<float> matrix;
  std::vector<std::size_t> rows;
  for (std::size_t i = 0; i < inputs.size(); ++i) {
    std::array<double, 3> x{};
    out[i].status = CheckInput(inputs[i], x, out[i].message);
    if (out[i].status != PredictionStatus::Ok) continue;
    for (std::size_t k = 0; k < 3; ++k) matrix.push_back(static_cast<float>((x[k] - info_.mean[k]) / info_.scale[k]));
    rows.push_back(i);
  }
  if (rows.empty()) return out;
  const auto& lib = *library_;
  const auto fail_all = [&](const std::string& message) {
    for (const std::size_t i : rows) {
      out[i].status = PredictionStatus::ModelError;
      out[i].message = message;
    }
    return out;
  };
  std::lock_guard<std::mutex> lock(mutex_);
  DMatrixHandle dmatrix = nullptr;
  if (lib.dmatrix_from_mat(matrix.data(), rows.size(), 3, std::numeric_limits<float>::quiet_NaN(), &dmatrix) != 0) {
    return fail_all("XGDMatrixCreateFromMat: " + lib.Error());
  }
  bst_ulong length = 0;
  const float* result = nullptr;
  const int rc = lib.predict(booster_, dmatrix, 0, 0, 0, &length, &result);
  if (rc != 0 || length != rows.size() || !result) {
    const std::string message = rc != 0 ? "XGBoosterPredict: " + lib.Error() : "XGBoosterPredict returned a wrong length";
    lib.dmatrix_free(dmatrix);
    return fail_all(message);
  }
  for (std::size_t r = 0; r < rows.size(); ++r) {
    LinkWeightPrediction& p = out[rows[r]];
    p.raw = result[r];
    if (!std::isfinite(result[r])) {
      p.status = PredictionStatus::ModelError;
      p.message = "non-finite prediction";
      p.weight.reset();
      continue;
    }
    p.weight = std::max(0.0, static_cast<double>(result[r]));
  }
  lib.dmatrix_free(dmatrix);
  return out;
}

LinkWeightPrediction LinkWeightModel::Predict(const LinkWeightInput& input) const { return Predict(std::vector<LinkWeightInput>{input}).front(); }

std::optional<NormalizationStatistics> PathObjectiveStatistics(const std::string& model_dir, std::string* error) {
  std::string why;
  const auto fail = [&](const std::string& message) -> std::optional<NormalizationStatistics> {
    if (error) *error = message;
    return std::nullopt;
  };
  if (!VerifyArtifacts(model_dir, nullptr, why)) return fail(why);
  const auto norm = ReadJson(Join(model_dir, "normalization.json"), why);
  if (!norm) return fail(why);
  const auto metrics = ReadJson(Join(model_dir, "regressor_metrics.json"), why);
  if (!metrics) return fail(why);
  const auto n_train = metrics->Number("n_train");
  const auto n_test = metrics->Number("n_test");
  if (!n_train || !n_test) return fail("regressor_metrics.json lacks n_train / n_test");
  NormalizationStatistics s;
  s.source = "xgboost-link-weight-v3 normalization.json: all " + std::to_string(static_cast<long>(*n_train + *n_test)) +
             " scenarios incl. test rows, ddof 0; scenario-level proxies (Rtt <- one-way latency, "
             "Bandwidth <- offered_load_mbps, Throughput <- delivered throughput, InverseBandwidth <- 1/throughput)";
  const std::pair<NormalizedFeature, const char*> map[] = {
      {NormalizedFeature::Delay, "delay_ms"},     {NormalizedFeature::Loss, "loss"},
      {NormalizedFeature::InverseBandwidth, "inv_bw"}, {NormalizedFeature::Hops, "hops"},
      {NormalizedFeature::Bandwidth, "bandwidth_mbps"}, {NormalizedFeature::Rtt, "latency_ms"},
      {NormalizedFeature::Throughput, "throughput_mbps"}};
  for (const auto& [feature, key] : map) {
    const JsonNode* entry = norm->Find(key);
    const auto mean = entry ? entry->Number("mean") : std::nullopt;
    const auto sd = entry ? entry->Number("std") : std::nullopt;
    if (!mean || !sd) return fail(std::string("normalization.json lacks ") + key);
    s.features[feature] = FeatureStatistics{static_cast<std::size_t>(*n_train + *n_test), *mean, *sd, 0};
  }
  const auto issues = s.Validate();
  if (!issues.empty()) return fail("normalization.json: " + issues.front());
  return s;
}

std::vector<LinkWeightPrediction> AssignPredictedWeights(const LinkWeightModel& model, ResearchGraph& graph) {
  std::vector<LinkWeightPrediction> out(graph.links.size());
  std::vector<LinkWeightInput> inputs;
  std::vector<std::size_t> index;
  for (std::size_t i = 0; i < graph.links.size(); ++i) {
    graph.links[i].predicted_weight.reset();
    if (!graph.links[i].available) {
      out[i].status = PredictionStatus::MissingInput;
      out[i].message = "link unavailable";
      continue;
    }
    inputs.push_back(LinkInput(graph.links[i].observation));
    index.push_back(i);
  }
  const auto predictions = model.Predict(inputs);
  for (std::size_t k = 0; k < index.size(); ++k) {
    out[index[k]] = predictions[k];
    if (predictions[k].status == PredictionStatus::Ok) graph.links[index[k]].predicted_weight = predictions[k].weight;
  }
  return out;
}

const char* ToString(PredictionStatus status) {
  switch (status) {
    case PredictionStatus::Ok: return "ok";
    case PredictionStatus::MissingInput: return "missing_input";
    case PredictionStatus::InvalidInput: return "invalid_input";
    case PredictionStatus::ModelError: return "model_error";
  }
  return "unknown";
}

}  // namespace aafr::research
