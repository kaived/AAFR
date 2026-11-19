// V0.7.4 feature acquisition contract, training-profile compatibility,
// adapter integration with NetworkState forwarding observations, provider
// abstention policy, and forwarding-aware meaningful-change detection.

#include "runtime/model/model_compatibility.h"

#include <algorithm>
#include <cmath>
#include <set>

#include "network/network_state/forwarding_observations.h"
#include "routing/topology/scenario_catalog.h"
#include "runtime/adaptive/network_change_detector.h"
#include "runtime/model/ai_recommendation_provider.h"
#include "runtime/model/feature_acquisition.h"
#include "tests/support/model_test_support.h"
#include "tests/support/runtime_test_support.h"
#include "tests/support/test_support.h"

using namespace aafr;
using namespace aafr::model;
using algorithms::AlgorithmId;
using aafr::test::Contains;
using aafr::test::FakeModel;
namespace fwd = aafr::forwarding;
namespace ns = aafr::netstate;
namespace rt = aafr::runtime;

namespace {

constexpr std::int64_t kMs = 1'000'000;
constexpr std::int64_t kT0 = 5'000'000 * kMs;
std::int64_t At(std::int64_t ms) { return kT0 + ms * kMs; }

std::shared_ptr<const TrainingProfile> Profile() {
  static const auto loaded = LoadTrainingProfile(aafr::test::Define(AAFR_TEST_TRAINING_PROFILE));
  return loaded.profile;
}

// A vector of 35 accepted values (provenance as given).
ModelFeatureVector VectorFrom(const std::vector<double>& values, FeatureProvenance provenance = FeatureProvenance::Measured) {
  ModelFeatureVector v;
  const auto& contract = RawFeatureContract();
  for (std::size_t i = 0; i < contract.size(); ++i) {
    ModelFeatureValue f;
    f.name = contract[i].name;
    f.unit = contract[i].unit;
    f.value = values[i];
    f.provenance = provenance;
    f.status = FeatureStatus::Valid;
    v.features.push_back(f);
  }
  return v;
}

// The raw vector at a training category's own mean.
std::vector<double> CategoryMean(const TrainingProfile& p, const std::string& category) {
  std::vector<double> x(p.mean.size());
  for (const auto& c : p.categories) {
    if (c.category != category) continue;
    // Clamped to the support: mean + z * scale reconstructs 0 as -4e-15.
    for (std::size_t i = 0; i < x.size(); ++i)
      x[i] = std::clamp(p.mean[i] + c.mean_z[i] * p.scale[i], p.features[i].min, p.features[i].max);
  }
  return x;
}

// ---------------------------------------------------------------- contract

void ContractInventoriesAll35Features() {
  const auto& acq = FeatureAcquisitionContract();
  CHECK(acq.size() == 35);
  const auto c = CountCategories();
  CHECK(c.direct == 12 && c.derived == 3 && c.estimated == 12 && c.unavailable == 5 && c.incompatible == 3);
  for (const auto& s : acq) {
    CHECK(!s.training_definition.empty() && !s.runtime_source.empty() && !s.measurement_method.empty());
    CHECK(!s.freshness.empty() && !s.missing_policy.empty() && !s.domain_compatibility.empty());
    CHECK(!s.validation_rule.empty() && !s.preprocessing.empty());
    CHECK(s.data_type == "int64" || s.data_type == "float64");
    if (s.category == FeatureCategory::Estimated || s.category == FeatureCategory::DerivedFromObservations) {
      CHECK(s.network_state_feature.rfind("fwd.", 0) == 0);
    }
  }
  // Packet-level quantities are never claimed.
  CHECK(FindAcquisitionSpec("loss")->category == FeatureCategory::Unavailable);
  CHECK(FindAcquisitionSpec("packet_delivery_ratio")->category == FeatureCategory::Unavailable);
  CHECK(FindAcquisitionSpec("avg_queue_occupancy")->category == FeatureCategory::Unavailable);
  CHECK(FindAcquisitionSpec("mean_link_utilization")->category == FeatureCategory::Incompatible);
  CHECK(FindAcquisitionSpec("latency")->category == FeatureCategory::Estimated);
  CHECK(EngineeredColumnContract().size() == 20);
}

void ContractOrderMatchesManifest() {
  const auto loaded = LoadManifest(aafr::test::Define(AAFR_TEST_MODEL_MANIFEST));
  CHECK(loaded.manifest.has_value());
  if (!loaded.manifest) return;
  const auto issues = ValidateAcquisitionContract(&*loaded.manifest);
  for (const auto& i : issues) std::cerr << "  " << i << "\n";
  CHECK(issues.empty());
  CHECK(loaded.manifest->model_feature_columns.size() == 55);
  // Raw feature order of the manifest == contract order (feature index).
  for (std::size_t i = 0; i < loaded.manifest->raw_features.size(); ++i)
    CHECK(loaded.manifest->raw_features[i].name == FeatureAcquisitionContract()[i].name);
}

// ---------------------------------------------------------------- profile

void TrainingProfileLoadsAndMatchesModel() {
  const auto loaded = LoadTrainingProfile(aafr::test::Define(AAFR_TEST_TRAINING_PROFILE), "lgbm2stage-20261027_220608");
  for (const auto& e : loaded.errors) std::cerr << "  " << e << "\n";
  CHECK(loaded.profile != nullptr);
  if (!loaded.profile) return;
  const TrainingProfile& p = *loaded.profile;
  CHECK(p.rows == 3500 && p.categories.size() == 7 && p.features.size() == 35);
  CHECK(p.features[0].name == "num_nodes" && p.features[0].min == 20.0 && p.features[0].max == 650.0);
  CHECK(p.dataset_sha256.size() == 64);
  // Another model version is refused.
  CHECK(!LoadTrainingProfile(aafr::test::Define(AAFR_TEST_TRAINING_PROFILE), "other-model").profile);
  CHECK(!ParseTrainingProfile("{\"schema\":\"x\"}").profile);
  // Label distribution: Dijkstra dominates (gate trained on 450 positives of 3500).
  CHECK(!p.algorithm_labels.empty() && p.algorithm_labels.front().first == "Dijkstra");
}

void AafrGraphIsClearlyOutOfDistribution() {
  const auto profile = Profile();
  CHECK(profile != nullptr);
  if (!profile) return;
  const fwd::ForwardingGraph g = fwd::LoopbackDemoGraph();
  ModelFeatureAdapter adapter;
  const ModelFeatureVector v = adapter.Build(g, fwd::LoopbackDemoProblem(g), nullptr, nullptr);
  const auto a = AssessCompatibility(v, profile.get());
  CHECK(a.status == CompatibilityStatus::ClearlyOutOfDistribution);
  std::set<std::string> outside;
  for (const auto& o : a.out_of_support) outside.insert(o.feature);
  CHECK(outside.count("num_nodes") && outside.count("num_edges"));
  CHECK(outside.count("source_in_degree") && outside.count("dest_out_degree"));  // 0 < training min 1
  CHECK(Contains(a.Summary(), "clearly_out_of_distribution"));
}

void DistanceCriterionAndOnehotCap() {
  const auto profile = Profile();
  if (!profile) {
    CHECK(false);
    return;
  }
  const ModelFeatureVector at_mean = VectorFrom(CategoryMean(*profile, "B_Congested"));
  CompatibilityOptions raw_only;
  raw_only.account_for_category_onehots = false;
  auto a = AssessCompatibility(at_mean, profile.get(), raw_only);
  CHECK(a.status == CompatibilityStatus::InDistribution);
  CHECK(a.nearest_category && *a.nearest_category == "B_Congested" && *a.nearest_distance < 1e-6);
  // With the deployed all-zero one-hots, never better than "potentially".
  a = AssessCompatibility(at_mean, profile.get());
  CHECK(a.status == CompatibilityStatus::PotentiallyOutOfDistribution);
  CHECK(Contains(a.reasons.back(), "one-hot"));
  // In support on every axis but far from every category: range checks alone
  // do not establish membership.
  std::vector<double> odd = CategoryMean(*profile, "A_Excellent");
  const auto idx = [](const char* name) {
    const auto& c = RawFeatureContract();
    for (std::size_t i = 0; i < c.size(); ++i)
      if (c[i].name == name) return i;
    return std::size_t{0};
  };
  odd[idx("loss")] = profile->features[idx("loss")].max;              // excellent network, maximal loss
  odd[idx("packet_delivery_ratio")] = profile->features[idx("packet_delivery_ratio")].max;  // yet full delivery
  odd[idx("jains_fairness_index")] = profile->features[idx("jains_fairness_index")].min;
  a = AssessCompatibility(VectorFrom(odd), profile.get(), raw_only);
  CHECK(a.out_of_support.empty());
  CHECK(a.status == CompatibilityStatus::PotentiallyOutOfDistribution);
  CHECK(Contains(a.reasons.back(), "Mahalanobis"));
}

void PartialAndSlightlyOutsideVectors() {
  const auto profile = Profile();
  if (!profile) {
    CHECK(false);
    return;
  }
  // Incomplete, nothing outside: cannot be assessed.
  ModelFeatureVector partial = VectorFrom(CategoryMean(*profile, "A_Excellent"));
  partial.features[14].value.reset();
  partial.features[14].status = FeatureStatus::Unsupported;
  partial.missing.push_back("loss");
  auto a = AssessCompatibility(partial, profile.get());
  CHECK(a.status == CompatibilityStatus::InsufficientInformation);
  CHECK(a.assessed_features == 34);
  // One telemetry feature slightly above the support.
  std::vector<double> x = CategoryMean(*profile, "A_Excellent");
  x[12] = profile->features[12].max * 1.05;  // latency
  a = AssessCompatibility(VectorFrom(x), profile.get());
  CHECK(a.status == CompatibilityStatus::PotentiallyOutOfDistribution && a.out_of_support.size() == 1);
  // No profile: never "in distribution".
  CHECK(AssessCompatibility(VectorFrom(x), nullptr).status == CompatibilityStatus::InsufficientInformation);
  CHECK(std::fabs(MahalanobisDistance(profile->categories[0].mean_z, profile->categories[0])) < 1e-9);
}

// ---------------------------------------------------------------- adapter

// A NetworkState whose forwarding observations cover a full window.
std::shared_ptr<ns::NetworkState> MeasuredState(bool full_window = true, int establishes = 6) {
  ns::ForwardingObservationStore store;
  store.SetCurrentConfiguration("two_path", 1, 1, "entry>relay_a>dest", ns::Impairment::EmulatedEdgeDelay);
  const std::int64_t start = full_window ? At(0) : At(25'000);
  for (auto s : {ns::ObservationSource::Gateway, ns::ObservationSource::Topology, ns::ObservationSource::Runtime})
    store.Heartbeat(s, start);
  for (int i = 0; i < establishes; ++i) {
    ns::ForwardingObservation o;
    o.kind = ns::ObservationKind::TunnelEstablished;
    o.provenance.measurement_id = "c" + std::to_string(i);
    o.provenance.source_type = ns::ObservationSource::Gateway;
    o.provenance.monotonic_ns = At(29'000 + i);
    o.provenance.interval_start_ns = o.provenance.monotonic_ns - 5 * kMs;
    o.provenance.unit = "ms";
    o.provenance.config_generation = 1;
    o.value = 4.0 + i;
    store.Ingest(o, At(29'500));
  }
  for (auto s : {ns::ObservationSource::Gateway, ns::ObservationSource::Topology, ns::ObservationSource::Runtime})
    store.Heartbeat(s, At(31'000));
  auto state = std::make_shared<ns::NetworkState>();
  state->state_id = 77;
  state->forwarding = std::make_shared<const ns::ForwardingFeatureSet>(store.Snapshot(At(31'000)));
  return state;
}

const ModelFeatureValue& Find(const ModelFeatureVector& v, const std::string& name) {
  for (const auto& f : v.features)
    if (f.name == name) return f;
  static ModelFeatureValue none;
  return none;
}

void AdapterUsesMeasuredDynamicsOnly() {
  const fwd::ForwardingGraph g = aafr::test::TwoPathGraph();
  const auto state = MeasuredState();
  ModelFeatureAdapter adapter;
  const ModelFeatureVector v = adapter.Build(g, aafr::test::ProblemFor(g), state.get(), nullptr);
  CHECK(v.features.size() == 35);
  CHECK(v.derived.size() == 3);
  CHECK(Find(v, "link_failure_events").provenance == FeatureProvenance::Derived);
  CHECK(*Find(v, "link_failure_events").value == 0.0);  // a real, fully observed zero
  CHECK(Contains(Find(v, "reroute_count").source, "fwd.runtime.reroute_count#77"));
  CHECK(v.CountStatus(FeatureStatus::Valid) == 15);
  CHECK(v.CountStatus(FeatureStatus::Unsupported) == 5);
  CHECK(v.CountStatus(FeatureStatus::Incompatible) == 15);
  CHECK(v.missing.size() == 20 && !v.Complete());
  // Estimates are reported but never become model input.
  bool latency_estimate = false;
  for (const auto& e : v.estimates) {
    CHECK(e.provenance == FeatureProvenance::Estimated && e.status == FeatureStatus::Incompatible);
    latency_estimate = latency_estimate || e.name == "latency";
  }
  CHECK(latency_estimate);
  CHECK(!Find(v, "latency").value.has_value());
  CHECK(Contains(Find(v, "loss").detail, "unsupported"));
  CHECK(Contains(v.Summary(), "derived=3"));
  CHECK(v.network_state_id == 77 && v.forwarding_version > 0);
}

void AdapterInsufficientWindowIsNotZero() {
  const fwd::ForwardingGraph g = aafr::test::TwoPathGraph();
  const auto state = MeasuredState(false);
  ModelFeatureAdapter adapter;
  const ModelFeatureVector v = adapter.Build(g, aafr::test::ProblemFor(g), state.get(), nullptr);
  CHECK(Find(v, "link_failure_events").status == FeatureStatus::InsufficientSamples);
  CHECK(!Find(v, "link_failure_events").value.has_value());
  CHECK(v.insufficient.size() == 3);
  // Opt-out keeps V0.7.2 behaviour exactly.
  FeatureAdapterConfig off;
  off.use_forwarding_observations = false;
  const ModelFeatureVector w = ModelFeatureAdapter(off).Build(g, aafr::test::ProblemFor(g), MeasuredState().get(), nullptr);
  CHECK(w.derived.empty() && w.missing.size() == 23);
}

// ---------------------------------------------------------------- provider

// A hypothetical source offering every telemetry feature as Measured
// (AAFR has none; this exercises the operational abstention path).
class MeasuredSource final : public ITelemetryFeatureSource {
 public:
  explicit MeasuredSource(std::map<std::string, double> values) : values_(std::move(values)) {}
  std::string Name() const override { return "measured_test"; }
  std::vector<TelemetryFeatureInput> Features() const override {
    std::vector<TelemetryFeatureInput> out;
    for (const auto& e : values_) out.push_back({e.first, e.second, FindRawFeature(e.first)->unit,
                                                 FeatureProvenance::Measured, "measured_test", 0.0});
    return out;
  }

 private:
  std::map<std::string, double> values_;
};

rt::RecommendationInput Input(std::shared_ptr<const fwd::ForwardingGraph> graph,
                              std::shared_ptr<const ns::NetworkState> state = nullptr) {
  rt::RecommendationInput in;
  in.graph = graph;
  in.problem = aafr::test::ProblemFor(*graph);
  in.state = std::move(state);
  for (AlgorithmId id : algorithms::kAllAlgorithmIds) {
    rt::CandidateEvaluation c;
    c.algorithm = id;
    c.eligibility = algorithms::EligibilityStatus::Eligible;
    c.considered = c.computed = c.path_valid = c.path_feasible = true;
    c.path = aafr::test::kPathA;
    c.objective = 3.0;
    in.candidates.push_back(c);
  }
  in.context = {"e1", rt::EvaluationTrigger::Manual, 77, graph->version()};
  return in;
}

std::map<std::string, double> ProfileValues() { return aafr::test::ConfiguredTelemetryProfile(); }

struct ProviderBed {
  std::shared_ptr<const fwd::ForwardingGraph> graph = std::make_shared<const fwd::ForwardingGraph>(aafr::test::TwoPathGraph());
  FakeModel model;
  AIProviderConfig Config(rt::RecommendationMode mode) {
    AIProviderConfig c;
    c.mode = mode;
    c.reuse_identical_features = false;
    c.compatibility.profile = Profile();
    return c;
  }
};

void MeasuredIncompleteAbstainsWithFeatureList() {
  ProviderBed b;
  b.model.Always(FakeModel::Label("PSO"));
  AIRecommendationProvider p(&b.model, nullptr, b.Config(rt::RecommendationMode::AiPreferred));
  const auto r = p.Recommend(Input(b.graph, MeasuredState()));
  CHECK(b.model.calls == 0);  // never asked
  const auto& t = *r.model;
  CHECK(t.inference_status == "abstained" && t.error_code == "E_FEATURES_INCOMPLETE");
  CHECK(t.source == rt::RecommendationSource::DeterministicFallback);
  CHECK(r.recommended == AlgorithmId::Dijkstra);  // deterministic fallback
  CHECK(t.features_required == 35 && t.features_valid == 15 && t.features_unsupported == 5 && t.features_incompatible == 15);
  CHECK(t.abstention_features.size() == 20);
  CHECK(t.input_domain == "measured");
  CHECK(t.compatibility_status == "clearly_out_of_distribution");  // assessed even when abstaining
  CHECK(t.estimates_available > 0 && t.forwarding_version > 0);
  CHECK(Contains(rt::ToJson(t), "\"compatibility_status\":\"clearly_out_of_distribution\""));
}

void MeasuredOutOfDistributionAbstains() {
  ProviderBed b;
  b.model.Always(FakeModel::Label("PSO"));
  MeasuredSource source(ProfileValues());
  AIRecommendationProvider p(&b.model, &source, b.Config(rt::RecommendationMode::AiPreferred));
  const auto r = p.Recommend(Input(b.graph));
  CHECK(b.model.calls == 0);
  CHECK(r.model->error_code == "E_OUT_OF_DISTRIBUTION");
  CHECK(r.model->features_valid == 35);
  bool nodes = false;
  for (const auto& f : r.model->abstention_features) nodes = nodes || f == "num_nodes:out_of_support";
  CHECK(nodes);
  CHECK(r.recommended == AlgorithmId::Dijkstra);
  CHECK(p.Counters().abstained == 1);
}

void MeasuredWithoutProfileAbstains() {
  ProviderBed b;
  MeasuredSource source(ProfileValues());
  auto c = b.Config(rt::RecommendationMode::Shadow);
  c.compatibility.profile.reset();
  AIRecommendationProvider p(&b.model, &source, c);
  const auto r = p.Recommend(Input(b.graph));
  CHECK(b.model.calls == 0);
  CHECK(r.model->error_code == "E_COMPATIBILITY_UNKNOWN");
  CHECK(r.model->source == rt::RecommendationSource::DeterministicShadow);
}

void ConfiguredProfileStillRecordedAsWhatIf() {
  // V0.7.2 controlled profiles stay usable: out of distribution is recorded.
  ProviderBed b;
  b.model.Always(FakeModel::Label("GWO"));
  ConfiguredTelemetrySource configured;
  configured.SetProfile(ProfileValues(), "controlled");
  AIRecommendationProvider p(&b.model, &configured, b.Config(rt::RecommendationMode::AiPreferred));
  const auto r = p.Recommend(Input(b.graph));
  CHECK(b.model.calls == 1);
  CHECK(r.model->inference_status == "ok" && r.model->input_domain == "configured_profile");
  CHECK(r.model->compatibility_status == "clearly_out_of_distribution");
  CHECK(r.recommended == AlgorithmId::GreyWolfOptimizer);
  // ...unless measured inputs are required.
  auto strict = b.Config(rt::RecommendationMode::AiPreferred);
  strict.compatibility.require_measured_inputs = true;
  AIRecommendationProvider q(&b.model, &configured, strict);
  const auto s = q.Recommend(Input(b.graph));
  CHECK(b.model.calls == 1);
  CHECK(s.model->error_code == "E_CONFIGURED_INPUTS" && s.recommended == AlgorithmId::Dijkstra);
  // Opt-in operational recording (research) lets the model answer.
  MeasuredSource source(ProfileValues());
  auto record = b.Config(rt::RecommendationMode::Shadow);
  record.compatibility.measured_clearly_ood = OodAction::RecordOnly;
  AIRecommendationProvider shadow(&b.model, &source, record);
  const auto t = shadow.Recommend(Input(b.graph));
  CHECK(b.model.calls == 2 && t.model->inference_status == "ok");
  CHECK(t.recommended == AlgorithmId::Dijkstra);  // shadow: deterministic ranking
}

void DeterministicModeUnaffected() {
  ProviderBed b;
  AIRecommendationProvider p(&b.model, nullptr, b.Config(rt::RecommendationMode::Deterministic));
  const auto r = p.Recommend(Input(b.graph, MeasuredState()));
  CHECK(b.model.calls == 0 && r.model->inference_status == "skipped" && r.recommended == AlgorithmId::Dijkstra);
}

// ---------------------------------------------------------------- change detection

std::shared_ptr<ns::NetworkState> StateWithP50(std::uint64_t id, std::uint64_t generation, double p50) {
  ns::ForwardingObservationStore store;
  store.SetCurrentConfiguration("g", 1, generation, "p", ns::Impairment::None);
  store.Heartbeat(ns::ObservationSource::Gateway, At(0));
  for (int i = 0; i < 6; ++i) {
    ns::ForwardingObservation o;
    o.kind = ns::ObservationKind::TunnelEstablished;
    o.provenance.measurement_id = "x" + std::to_string(i);
    o.provenance.source_type = ns::ObservationSource::Gateway;
    o.provenance.monotonic_ns = At(1'000 + i);
    o.provenance.interval_start_ns = o.provenance.monotonic_ns - static_cast<std::int64_t>(p50 * kMs);
    o.provenance.unit = "ms";
    o.provenance.config_generation = generation;
    o.value = p50;
    store.Ingest(o, At(1'100));
  }
  auto s = std::make_shared<ns::NetworkState>();
  s->state_id = id;
  s->forwarding = std::make_shared<const ns::ForwardingFeatureSet>(store.Snapshot(At(2'000)));
  return s;
}

const rt::FeatureChange* Change(const rt::ChangeAssessment& a, const std::string& id) {
  for (const auto& f : a.features)
    if (f.feature_id == id) return &f;
  return nullptr;
}

void ForwardingChangeNeedsSustainAndSameGeneration() {
  rt::NetworkChangeDetector d;
  const std::string id = "fwd.path.establish_ms.p50";
  auto obs = [&](std::int64_t ms, const ns::NetworkState& s) {
    return rt::MakeObservation(ms * kMs, &s, nullptr, nullptr, d.config());
  };
  auto a = d.Observe(obs(0, *StateWithP50(1, 1, 5.0)));
  CHECK(Change(a, id) && Change(a, id)->status == "baseline_set");
  // A switch to another generation: the new path's value is a new baseline,
  // not a measurement change (no evaluation loop after switching).
  a = d.Observe(obs(1000, *StateWithP50(2, 2, 60.0)));
  CHECK(Change(a, id)->status == "baseline_reset:context" && !a.meaningful);
  // Noise below the floor is ignored.
  a = d.Observe(obs(2000, *StateWithP50(3, 2, 70.0)));
  CHECK(Change(a, id)->status == "within_threshold" && !a.meaningful);
  // Sustained degradation on the same generation: meaningful after 2.
  a = d.Observe(obs(3000, *StateWithP50(4, 2, 150.0)));
  CHECK(Change(a, id)->status == "pending_sustain" && !a.meaningful);
  a = d.Observe(obs(4000, *StateWithP50(5, 2, 155.0)));
  CHECK(a.meaningful && Change(a, id)->status == "meaningful");
  // Missing data never triggers.
  auto empty = std::make_shared<ns::NetworkState>();
  empty->state_id = 6;
  a = d.Observe(obs(5000, *empty));
  CHECK(!a.meaningful && Contains(Change(a, id)->status, "unusable"));
}

}  // namespace

int main() {
  return aafr::test::RunTests({
      {"contract_inventories_all_35", ContractInventoriesAll35Features},
      {"contract_order_matches_manifest", ContractOrderMatchesManifest},
      {"training_profile_loads", TrainingProfileLoadsAndMatchesModel},
      {"aafr_graph_clearly_ood", AafrGraphIsClearlyOutOfDistribution},
      {"distance_criterion_and_onehot_cap", DistanceCriterionAndOnehotCap},
      {"partial_and_slightly_outside", PartialAndSlightlyOutsideVectors},
      {"adapter_uses_measured_dynamics_only", AdapterUsesMeasuredDynamicsOnly},
      {"adapter_insufficient_window_not_zero", AdapterInsufficientWindowIsNotZero},
      {"measured_incomplete_abstains", MeasuredIncompleteAbstainsWithFeatureList},
      {"measured_ood_abstains", MeasuredOutOfDistributionAbstains},
      {"measured_without_profile_abstains", MeasuredWithoutProfileAbstains},
      {"configured_profile_recorded", ConfiguredProfileStillRecordedAsWhatIf},
      {"deterministic_mode_unaffected", DeterministicModeUnaffected},
      {"forwarding_change_sustain_and_generation", ForwardingChangeNeedsSustainAndSameGeneration},
  });
}
