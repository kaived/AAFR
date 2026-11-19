// Research alignment, Phase 4: the imported XGBoost v3 link-weight model
// through the XGBoost C API (research/link_weight_model). Parity against the
// Python XGBoost reference (tests/research/xgboost_reference.py), artifact
// verification, input handling, the low-delay behaviour on the six-switch
// testbed, the research A* on predicted weights, and Release performance
// figures (RESULT lines). Fails when the XGBoost library is not available:
// inference verification is required, never skipped. No network access.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <set>
#include <string>

#include "research/json_reader.h"
#include "research/link_weight_model.h"
#include "research/sha256.h"
#include "routing/topology/forwarding_problem.h"
#include "tests/fixtures/research/reference_values.h"
#include "tests/fixtures/research/xgboost_reference_values.h"
#include "tests/support/test_support.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>
#endif

using namespace aafr::research;
namespace rf = aafr::research_fixture;
namespace xr = aafr::xgboost_reference;
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;
using Path = std::vector<std::string>;

namespace {

const std::string kModelDir = AAFR_TEST_XGBOOST_MODEL_DIR;
const std::string kLibrary = AAFR_TEST_XGBOOST_LIBRARY;
std::unique_ptr<LinkWeightModel> g_model;

double Ms(Clock::time_point a, Clock::time_point b) { return std::chrono::duration<double, std::milli>(b - a).count(); }

std::size_t WorkingSetBytes() {
#if defined(_WIN32)
  PROCESS_MEMORY_COUNTERS pmc{};
  if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) return pmc.WorkingSetSize;
#endif
  return 0;
}

LinkWeightInput Input(double d, double l, double inv) { return LinkWeightInput{d, l, inv}; }

ResearchGraph Testbed() {
  ResearchGraph g;
  g.nodes = {"s1", "s2", "s3", "s4", "s5", "s6"};
  for (const auto& l : rf::kTestbedLinks) {
    for (const auto& [from, to] : {std::pair{l.a, l.b}, std::pair{l.b, l.a}}) {
      ResearchLink link;
      link.observation = LinkObservation{from, to, l.delay_ms, 0.0, l.capacity_mbps, 0.0};
      link.startup_delay_ms = l.delay_ms;
      g.links.push_back(link);
    }
  }
  return g;
}

ResearchLink* Find(ResearchGraph& g, const std::string& from, const std::string& to) {
  for (auto& l : g.links) {
    if (l.observation.from == from && l.observation.to == to) return &l;
  }
  return nullptr;
}

std::vector<Path> AllSimplePaths(const ResearchGraph& g, const std::string& s, const std::string& t) {
  std::vector<Path> out;
  Path path{s};
  std::set<std::string> seen{s};
  std::function<void(const std::string&)> visit = [&](const std::string& u) {
    if (u == t) {
      out.push_back(path);
      return;
    }
    for (const auto& l : g.links) {
      if (!l.available || l.observation.from != u || seen.count(l.observation.to)) continue;
      seen.insert(l.observation.to);
      path.push_back(l.observation.to);
      visit(l.observation.to);
      path.pop_back();
      seen.erase(l.observation.to);
    }
  };
  visit(s);
  return out;
}

double PathWeight(const ResearchGraph& g, const Path& p) {
  double sum = 0.0;
  for (std::size_t i = 0; i + 1 < p.size(); ++i) {
    for (const auto& l : g.links) {
      if (l.observation.from == p[i] && l.observation.to == p[i + 1]) sum += *l.predicted_weight;
    }
  }
  return sum;
}

// ---- Utilities ----

void Sha256KnownVectors() {
  CHECK(Sha256Hex("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  CHECK(Sha256Hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  CHECK(Sha256Hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
        "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
  CHECK(Sha256Hex(std::string(1000, 'a')) == "41edece42d63e8d9bf515a9ba6932e1c20cbc9f5a5d134645adb5db1b9737ea3");
}

void JsonReaderIsStrict() {
  const auto ok = ParseJson(R"({"a": [1, -2.5e-3, 0.1], "b": "x\n", "c": {"d": true, "e": null}})");
  CHECK(ok && ok->Numbers("a") && (*ok->Numbers("a"))[1] == -2.5e-3 && *ok->String("b") == "x\n");
  CHECK(ok->Find("c") && ok->Find("c")->Find("d")->boolean);
  for (const char* bad : {"{\"a\": NaN}", "{\"a\": 1} x", "{\"a\": 1, \"a\": 2}", "[1,]", "{\"a\": 01}", "\"\\q\"", "{"}) {
    std::string error;
    CHECK(!ParseJson(bad, &error) && !error.empty());
  }
  // Python's json.dump output round-trips exactly.
  const auto scaler = ParseJson(*ReadFileBytes(kModelDir + "/scaler_regressor.json"));
  CHECK(scaler && (*scaler->Numbers("mean"))[0] == 32.06701428571427);
  CHECK((*scaler->Numbers("scale"))[2] == 0.03852036555887816);
}

// ---- Loading and artifact validation ----

void ModelLoadsAndArtifactsVerify() {
  const ModelInfo& i = g_model->info();
  CHECK(i.model_id == "xgboost-link-weight-v3");
  CHECK(i.source_commit == "6b6c008fc484bcbadfb8d8447acf36fe5f39e10d");
  CHECK(i.xgboost_major == 3);
  CHECK(i.num_features == 3 && i.boosted_rounds == 400);
  CHECK(i.verified_artifacts.size() == 5);
  CHECK(i.mean[0] == 32.06701428571427 && i.scale[1] == 0.16968993154340556);
  std::cout << "RESULT library: xgboost " << i.xgboost_major << "." << i.xgboost_minor << "." << i.xgboost_patch
            << " from " << i.library_path << "\n";
}

void LoadFailuresAreReported() {
  CHECK(!LinkWeightModel::Load(kModelDir, "no_such_xgboost_library.dll").model);
  const auto missing = LinkWeightModel::Load(kModelDir + "/does_not_exist", kLibrary);
  CHECK(!missing.model && !missing.error.empty());
  // A copy with one changed byte in the scaler fails its hash.
  const fs::path tmp = fs::temp_directory_path() / "aafr_xgboost_tamper_test";
  fs::remove_all(tmp);
  fs::create_directories(tmp);
  for (const auto& entry : fs::directory_iterator(kModelDir)) fs::copy_file(entry.path(), tmp / entry.path().filename());
  {
    std::string bytes = *ReadFileBytes((tmp / "scaler_regressor.json").string());
    const auto pos = bytes.find("32.067");
    bytes[pos + 5] = '8';
    std::ofstream((tmp / "scaler_regressor.json").string(), std::ios::binary) << bytes;
  }
  const auto tampered = LinkWeightModel::Load(tmp.string(), kLibrary);
  CHECK(!tampered.model && aafr::test::Contains(tampered.error, "scaler_regressor.json"));
  std::string stats_error;
  CHECK(!PathObjectiveStatistics(tmp.string(), &stats_error) && !stats_error.empty());
  fs::remove_all(tmp);
}

// ---- Parity with Python XGBoost ----

void NormalizationMatchesPythonBitForBit() {
  for (const auto& c : xr::kCases) {
    const auto z = g_model->Normalize(Input(c.delay_ms, c.loss_ratio, c.inv_bottleneck_bw));
    CHECK(z.has_value());
    for (int k = 0; z && k < 3; ++k) CHECK(std::memcmp(&(*z)[k], &c.z[k], sizeof(float)) == 0);
  }
}

void PredictionsMatchPython() {
  std::vector<LinkWeightInput> inputs;
  for (const auto& c : xr::kCases) inputs.push_back(Input(c.delay_ms, c.loss_ratio, c.inv_bottleneck_bw));
  const auto out = g_model->Predict(inputs);
  CHECK(out.size() == xr::kCases.size());
  double max_abs = 0.0;
  std::size_t identical = 0, negative = 0;
  for (std::size_t i = 0; i < out.size(); ++i) {
    const auto& c = xr::kCases[i];
    CHECK(out[i].status == PredictionStatus::Ok && out[i].raw);
    if (!out[i].raw) continue;
    const double diff = std::fabs(static_cast<double>(*out[i].raw) - static_cast<double>(c.raw));
    max_abs = std::max(max_abs, diff);
    identical += std::memcmp(&*out[i].raw, &c.raw, sizeof(float)) == 0;
    // Same float32 inputs, same trees, same library: the sum of 400 float32
    // leaf values may differ only by accumulation order; allow 4 float32 ulp
    // at the magnitude of the output (about 1e-5 at 100).
    const float magnitude = std::fabs(c.raw);
    const double ulp = static_cast<double>(std::nextafter(magnitude, std::numeric_limits<float>::infinity()) - magnitude);
    CHECK(diff <= 4.0 * ulp + 1e-7);
    // Floor.
    if (c.raw < 0.0f) {
      ++negative;
      CHECK(*out[i].weight == 0.0);
    } else {
      CHECK(*out[i].weight == static_cast<double>(*out[i].raw));
    }
  }
  CHECK(negative > 0);  // Negative raw predictions are covered.
  std::cout << "RESULT parity: cases=" << out.size() << " bitwise_identical=" << identical << " max_abs_diff=" << max_abs
            << " negative_raw=" << negative << "\n";
}

void PredictionIsDeterministic() {
  std::vector<LinkWeightInput> inputs;
  for (const auto& c : xr::kCases) inputs.push_back(Input(c.delay_ms, c.loss_ratio, c.inv_bottleneck_bw));
  const auto a = g_model->Predict(inputs);
  const auto b = g_model->Predict(inputs);
  for (std::size_t i = 0; i < a.size(); ++i) {
    CHECK(a[i].raw == b[i].raw);
    const auto single = g_model->Predict(inputs[i]);  // Batch and single agree.
    CHECK(single.raw == a[i].raw);
  }
}

// ---- Inputs ----

void InvalidAndMissingInputsAreUnavailable() {
  const double nan = std::numeric_limits<double>::quiet_NaN();
  const double inf = std::numeric_limits<double>::infinity();
  CHECK(g_model->Predict(LinkWeightInput{std::nullopt, 0.0, 0.01}).status == PredictionStatus::MissingInput);
  CHECK(g_model->Predict(LinkWeightInput{5.0, std::nullopt, 0.01}).status == PredictionStatus::MissingInput);
  CHECK(g_model->Predict(LinkWeightInput{5.0, 0.0, std::nullopt}).status == PredictionStatus::MissingInput);
  for (const auto& in : {Input(nan, 0.0, 0.01), Input(inf, 0.0, 0.01), Input(-1.0, 0.0, 0.01), Input(5.0, 1.5, 0.01),
                         Input(5.0, -0.1, 0.01), Input(5.0, nan, 0.01), Input(5.0, 0.0, 0.0), Input(5.0, 0.0, -0.01),
                         Input(5.0, 0.0, inf)}) {
    const auto p = g_model->Predict(in);
    CHECK(p.status == PredictionStatus::InvalidInput && !p.raw && !p.weight && !p.message.empty());
    CHECK(!g_model->Normalize(in));
  }
  // Invalid rows in a batch leave the valid rows untouched.
  const auto mixed = g_model->Predict({Input(5.0, 0.0, 0.01), Input(nan, 0.0, 0.01), Input(40.0, 0.1, 0.025)});
  CHECK(mixed[0].status == PredictionStatus::Ok && mixed[1].status == PredictionStatus::InvalidInput &&
        mixed[2].status == PredictionStatus::Ok);
  CHECK(mixed[0].raw == g_model->Predict(Input(5.0, 0.0, 0.01)).raw);
  CHECK(mixed[2].raw == g_model->Predict(Input(40.0, 0.1, 0.025)).raw);
  CHECK(g_model->Predict(std::vector<LinkWeightInput>{}).empty());
}

void TrainingAndLinkInputsAreDistinct() {
  // Training proxy: 1 / max(throughput, 1e-6).
  CHECK(*TrainingInput(30.0, 0.05, 40.0).inv_bottleneck_bw == 1.0 / 40.0);
  CHECK(*TrainingInput(30.0, 0.05, 0.0).inv_bottleneck_bw == 1e6);
  CHECK(!TrainingInput(30.0, 0.05, std::nullopt).inv_bottleneck_bw);
  CHECK(!TrainingInput(std::nullopt, 0.05, 40.0).delay_ms);
  // Link input: 1 / available bandwidth c (1 - u); no floor.
  CHECK(*LinkInput(LinkObservation{"a", "b", 5.0, 0.0, 100.0, 0.5}).inv_bottleneck_bw == 1.0 / 50.0);
  CHECK(!LinkInput(LinkObservation{"a", "b", 5.0, 0.0, 100.0, 1.0}).inv_bottleneck_bw);  // B = 0.
  CHECK(!LinkInput(LinkObservation{"a", "b", 5.0, 0.0, 100.0, std::nullopt}).inv_bottleneck_bw);
  CHECK(!LinkInput(LinkObservation{"a", "b", std::nullopt, 0.0, 100.0, 0.0}).delay_ms);
  CHECK(g_model->Predict(LinkInput(LinkObservation{"a", "b", 5.0, 0.0, 100.0, 1.0})).status == PredictionStatus::MissingInput);
}

// ---- Path-objective statistics ----

void PathObjectiveStatisticsAreTheOriginalOnes() {
  std::string error;
  const auto s = PathObjectiveStatistics(kModelDir, &error);
  CHECK(s && error.empty() && s->Validate().empty());
  if (!s) return;
  const auto& d = s->features.at(NormalizedFeature::Delay);
  CHECK(d.mean == 31.961039142857143 && d.standard_deviation == 26.801945453202976 && d.ddof == 0 && d.count == 3500);
  CHECK(s->features.at(NormalizedFeature::Rtt).mean == d.mean);  // Same column: one-way latency used as RTT.
  CHECK(s->features.at(NormalizedFeature::Bandwidth).mean == 45.94502142857143);
  CHECK(s->features.at(NormalizedFeature::Throughput).mean == 39.12583257142857);
  CHECK(s->features.at(NormalizedFeature::InverseBandwidth).standard_deviation == 0.039257621570771825);
  CHECK(s->features.at(NormalizedFeature::Hops).mean == 2.8870839999999998);
  CHECK(s->features.size() == 7);
  CHECK(aafr::test::Contains(s->source, "incl. test rows"));
  // Usable by J_eng on a testbed path.
  const ResearchGraph g = Testbed();
  const PathEvaluation e = EvaluatePath(g, {"s1", "s3", "s6"}, *s);
  CHECK(e.engine_cost.value.has_value());
}

// ---- Low delay and A* ----

void LowDelayTestbedAndAStar() {
  ResearchGraph g = Testbed();
  const auto predictions = AssignPredictedWeights(*g_model, g);
  std::size_t zero = 0, negative_raw = 0;
  double max_weight = 0.0;
  for (std::size_t i = 0; i < g.links.size(); ++i) {
    const auto& p = predictions[i];
    CHECK(p.status == PredictionStatus::Ok && g.links[i].predicted_weight);
    const std::string name = "testbed_" + g.links[i].observation.from + "_" + g.links[i].observation.to;
    const std::string reverse = "testbed_" + g.links[i].observation.to + "_" + g.links[i].observation.from;
    for (const auto& c : xr::kCases) {  // Same raw value as Python for this link (either direction).
      if (name == c.label || reverse == c.label) CHECK(*p.raw == c.raw);
    }
    zero += *g.links[i].predicted_weight == 0.0;
    negative_raw += *p.raw < 0.0f;
    max_weight = std::max(max_weight, *g.links[i].predicted_weight);
    std::cout << "RESULT testbed_link " << g.links[i].observation.from << "-" << g.links[i].observation.to
              << " delay_ms=" << *g.links[i].observation.delay_ms << " inv_bw=" << *LinkInput(g.links[i].observation).inv_bottleneck_bw
              << " raw=" << *p.raw << " W_link=" << *g.links[i].predicted_weight << "\n";
  }
  CHECK(max_weight < 0.01);  // Every testbed link is nearly free to the model.
  const Candidate a = PredictedWeightAStar(g, "s1", "s6");
  CHECK(a.status == CandidateStatus::PathFound);
  double best = std::numeric_limits<double>::infinity();
  std::size_t at_best = 0;
  const auto paths = AllSimplePaths(g, "s1", "s6");
  for (const Path& p : paths) best = std::min(best, PathWeight(g, p));
  for (const Path& p : paths) at_best += PathWeight(g, p) == best;
  CHECK(*a.optimization_cost == best);
  CHECK(PredictedWeightAStar(g, "s1", "s6").path == a.path);  // Deterministic.
  std::cout << "RESULT testbed_astar: zero_weight_links=" << zero << "/" << g.links.size()
            << " negative_raw=" << negative_raw << " path=" << aafr::forwarding::PathText(a.path)
            << " W=" << *a.optimization_cost << " brute_force_min=" << best << " paths_at_min=" << at_best
            << " simple_paths=" << paths.size() << "\n";

  // Loss scenario: 15 % loss each way on s1-s2.
  ResearchGraph loss = Testbed();
  Find(loss, "s1", "s2")->observation.loss_ratio = 0.15;
  Find(loss, "s2", "s1")->observation.loss_ratio = 0.15;
  AssignPredictedWeights(*g_model, loss);
  const Candidate la = PredictedWeightAStar(loss, "s1", "s6");
  double lbest = std::numeric_limits<double>::infinity();
  for (const Path& p : AllSimplePaths(loss, "s1", "s6")) lbest = std::min(lbest, PathWeight(loss, p));
  CHECK(la.status == CandidateStatus::PathFound && *la.optimization_cost == lbest);
  std::cout << "RESULT loss_s1s2_0.15: W(s1-s2)=" << *Find(loss, "s1", "s2")->predicted_weight
            << " astar_path=" << aafr::forwarding::PathText(la.path) << " W=" << *la.optimization_cost << "\n";
}

void MissingPredictionsBlockAStar() {
  ResearchGraph g = Testbed();
  Find(g, "s3", "s5")->observation.utilization.reset();  // No bandwidth input for this link.
  const auto predictions = AssignPredictedWeights(*g_model, g);
  CHECK(!Find(g, "s3", "s5")->predicted_weight);
  const Candidate a = PredictedWeightAStar(g, "s1", "s6");
  CHECK(a.status == CandidateStatus::MissingCost && aafr::test::Contains(a.message, "s3->s5"));
  // A link that is down needs no prediction.
  ResearchGraph down = Testbed();
  Find(down, "s3", "s5")->observation.utilization.reset();
  Find(down, "s3", "s5")->available = false;
  AssignPredictedWeights(*g_model, down);
  CHECK(PredictedWeightAStar(down, "s1", "s6").status == CandidateStatus::PathFound);
  // Zero available bandwidth: no prediction (no floor), so A* refuses.
  ResearchGraph full = Testbed();
  Find(full, "s1", "s2")->observation.utilization = 1.0;
  AssignPredictedWeights(*g_model, full);
  CHECK(!Find(full, "s1", "s2")->predicted_weight);
  CHECK(PredictedWeightAStar(full, "s1", "s6").status == CandidateStatus::MissingCost);
}

// ---- Performance ----

void Performance() {
  // Model load (artifact hashing, JSON, XGBoost load) in this process.
  const std::size_t before = WorkingSetBytes();
  const auto t0 = Clock::now();
  const auto loaded = LinkWeightModel::Load(kModelDir, kLibrary);
  const auto t1 = Clock::now();
  const std::size_t after = WorkingSetBytes();
  CHECK(loaded.model != nullptr);
  // Single-link inference (median of 500).
  std::vector<double> single;
  const LinkWeightInput one = Input(5.0, 0.0, 0.01);
  for (int i = 0; i < 500; ++i) {
    const auto a = Clock::now();
    const auto p = loaded.model->Predict(one);
    const auto b = Clock::now();
    CHECK(p.status == PredictionStatus::Ok);
    single.push_back(Ms(a, b));
  }
  std::sort(single.begin(), single.end());
  // Feature preparation only (LinkInput + normalization), 18 testbed links.
  ResearchGraph g = Testbed();
  const auto p0 = Clock::now();
  std::size_t prepared = 0;
  for (int rep = 0; rep < 1000; ++rep) {
    for (const auto& l : g.links) prepared += loaded.model->Normalize(LinkInput(l.observation)).has_value();
  }
  const auto p1 = Clock::now();
  CHECK(prepared == 1000 * g.links.size());
  // Batched inference: the 18 testbed links, and 1,000 rows.
  std::vector<LinkWeightInput> links;
  for (const auto& l : g.links) links.push_back(LinkInput(l.observation));
  std::vector<double> batch18;
  for (int i = 0; i < 200; ++i) {
    const auto a = Clock::now();
    loaded.model->Predict(links);
    batch18.push_back(Ms(a, Clock::now()));
  }
  std::sort(batch18.begin(), batch18.end());
  std::vector<LinkWeightInput> many;
  for (const auto& c : xr::kCases) many.push_back(Input(c.delay_ms, c.loss_ratio, c.inv_bottleneck_bw));
  while (many.size() < 1000) many.push_back(many[many.size() % xr::kCases.size()]);
  const auto m0 = Clock::now();
  const auto big = loaded.model->Predict(many);
  const auto m1 = Clock::now();
  CHECK(big.size() == 1000);
  // A* on predicted weights (search only; weights already assigned).
  AssignPredictedWeights(*loaded.model, g);
  std::vector<double> astar;
  for (int i = 0; i < 500; ++i) {
    const auto a = Clock::now();
    const Candidate c = PredictedWeightAStar(g, "s1", "s6");
    astar.push_back(Ms(a, Clock::now()));
    CHECK(c.status == CandidateStatus::PathFound);
  }
  std::sort(astar.begin(), astar.end());
#if defined(NDEBUG)
  const char* build = "release";
#else
  const char* build = "debug";
#endif
  std::cout << "RESULT performance[" << build << "]: load_ms=" << Ms(t0, t1)
            << " working_set_delta_mb=" << (after > before ? (after - before) / 1048576.0 : 0.0)
            << " single_predict_median_ms=" << single[single.size() / 2] << " single_predict_p99_ms=" << single[single.size() * 99 / 100]
            << " feature_prep_per_link_us=" << Ms(p0, p1) * 1000.0 / (1000.0 * g.links.size())
            << " batch18_median_ms=" << batch18[batch18.size() / 2] << " batch1000_ms=" << Ms(m0, m1)
            << " batch1000_per_row_us=" << Ms(m0, m1) * 1000.0 / 1000.0
            << " astar_median_ms=" << astar[astar.size() / 2] << "\n";
}

}  // namespace

int main() {
  if (kLibrary.empty() || !fs::exists(kLibrary)) {
    // Required: research verification fails, never skips, without the model runtime.
    std::cout << "FAIL: XGBoost library not available (AAFR_XGBOOST_LIBRARY='" << kLibrary
              << "'); install xgboost==3.0.4 in .venv or set AAFR_XGBOOST_LIBRARY\n";
    return 1;
  }
  auto loaded = LinkWeightModel::Load(kModelDir, kLibrary);
  if (!loaded.model) {
    std::cout << "FAIL: model did not load: " << loaded.error << "\n";
    return 1;
  }
  g_model = std::move(loaded.model);
  const int rc = aafr::test::RunTests({
      {"sha256_known_vectors", Sha256KnownVectors},
      {"json_reader_is_strict", JsonReaderIsStrict},
      {"model_loads_and_artifacts_verify", ModelLoadsAndArtifactsVerify},
      {"load_failures_are_reported", LoadFailuresAreReported},
      {"normalization_matches_python_bit_for_bit", NormalizationMatchesPythonBitForBit},
      {"predictions_match_python", PredictionsMatchPython},
      {"prediction_is_deterministic", PredictionIsDeterministic},
      {"invalid_and_missing_inputs_are_unavailable", InvalidAndMissingInputsAreUnavailable},
      {"training_and_link_inputs_are_distinct", TrainingAndLinkInputsAreDistinct},
      {"path_objective_statistics_are_the_original_ones", PathObjectiveStatisticsAreTheOriginalOnes},
      {"low_delay_testbed_and_astar", LowDelayTestbedAndAStar},
      {"missing_predictions_block_astar", MissingPredictionsBlockAStar},
      {"performance", Performance},
  });
  g_model.reset();
  return rc;
}
