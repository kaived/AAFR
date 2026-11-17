// V0.7.2 ModelFeatureAdapter: exact feature mapping and order, units,
// provenance, missing/stale/invalid inputs, NaN and infinity, unsupported
// names, graph-feature conflicts and opt-in NetworkState substitutes.

#include <cmath>
#include <limits>

#include "runtime/model/model_feature_adapter.h"
#include "tests/support/model_test_support.h"
#include "tests/support/runtime_test_support.h"
#include "tests/support/test_support.h"

using namespace aafr;
using namespace aafr::model;
namespace fwd = aafr::forwarding;

using aafr::test::Contains;

namespace {

struct Inputs {
  fwd::ForwardingGraph graph = aafr::test::TwoPathGraph();
  fwd::ForwardingProblem problem = aafr::test::ProblemFor(graph);
};

ModelFeatureVector Build(const ITelemetryFeatureSource* source, const netstate::NetworkState* state = nullptr,
                         FeatureAdapterConfig config = {}) {
  Inputs in;
  return ModelFeatureAdapter(config).Build(in.graph, in.problem, state, source);
}

const ModelFeatureValue& Find(const ModelFeatureVector& v, const std::string& name) {
  for (const auto& f : v.features)
    if (f.name == name) return f;
  static ModelFeatureValue none;
  return none;
}

bool Has(const std::vector<std::string>& v, const std::string& item) {
  for (const auto& x : v)
    if (x == item || x.rfind(item, 0) == 0) return true;
  return false;
}

void CompleteVectorInContractOrder() {
  ConfiguredTelemetrySource source;
  source.SetProfile(aafr::test::ConfiguredTelemetryProfile(), "test");
  const auto v = Build(&source);
  CHECK(v.Complete());
  CHECK(v.features.size() == 35);
  const auto& contract = RawFeatureContract();
  for (std::size_t i = 0; i < contract.size(); ++i) {
    CHECK(v.features[i].name == contract[i].name);
    CHECK(v.features[i].unit == contract[i].unit);
    CHECK(v.features[i].value.has_value());
  }
  CHECK(Find(v, "num_nodes").provenance == FeatureProvenance::GraphDerived);
  CHECK(Contains(Find(v, "num_nodes").source, "forwarding_graph:two_path@v"));
  CHECK(Find(v, "latency").provenance == FeatureProvenance::Configured);
  CHECK(*Find(v, "latency").value == 20.0);
  CHECK(Contains(v.Summary(), "graph=12") && Contains(v.Summary(), "configured=23") && Contains(v.Summary(), "missing=0"));
  CHECK(v.Values().size() == 35);
}

void MissingTelemetryAbstains() {
  const auto v = Build(nullptr);
  CHECK(!v.Complete());
  CHECK(v.missing.size() == 23);  // every non-graph feature
  CHECK(Has(v.missing, "mean_link_utilization"));
  CHECK(!Find(v, "latency").value.has_value());  // unknown never becomes 0
  CHECK(Find(v, "latency").issue == "no source provides this feature");
  ConfiguredTelemetrySource partial;
  auto profile = aafr::test::ConfiguredTelemetryProfile();
  profile.erase("avg_queue_delay_ms");
  partial.SetProfile(profile, "partial");
  const auto p = Build(&partial);
  CHECK(!p.Complete());
  CHECK(p.missing == std::vector<std::string>({"avg_queue_delay_ms"}));
}

void InvalidValuesRejected() {
  const double nan = std::numeric_limits<double>::quiet_NaN();
  const double inf = std::numeric_limits<double>::infinity();
  struct Case {
    std::string name;
    double value;
    std::string unit;
  };
  for (const Case& c : std::vector<Case>{{"latency", nan, "ms"},
                                         {"jitter", inf, "ms"},
                                         {"loss", 1.5, "ratio"},          // ratio above 1
                                         {"throughput", -1.0, "mbps"},    // negative
                                         {"max_queue_depth", 2.5, "packets"},  // fractional count
                                         {"latency", 0.02, "s"},          // wrong unit
                                         {"mean_link_utilization", 0.4, "ratio"}}) {  // percent expected
    ConfiguredTelemetrySource source;
    auto profile = aafr::test::ConfiguredTelemetryProfile();
    profile.erase(c.name);
    source.SetProfile(profile, "p");
    auto inputs = source.Features();
    inputs.push_back({c.name, c.value, c.unit, FeatureProvenance::Configured, "test", std::nullopt});
    source.SetInputs(inputs);
    const auto v = Build(&source);
    CHECK(!v.Complete());
    CHECK(Has(v.invalid, c.name));
    CHECK(!Find(v, c.name).value.has_value());
    CHECK(!Find(v, c.name).issue.empty());
  }
}

void StaleMeasuredValues() {
  ConfiguredTelemetrySource source;
  auto profile = aafr::test::ConfiguredTelemetryProfile();
  profile.erase("latency");
  profile.erase("jitter");
  source.SetProfile(profile, "p");
  auto inputs = source.Features();
  inputs.push_back({"latency", 12.0, "ms", FeatureProvenance::Measured, "probe", 60'000.0});  // too old
  inputs.push_back({"jitter", 1.0, "ms", FeatureProvenance::Measured, "probe", std::nullopt});  // age unknown
  source.SetInputs(inputs);
  auto v = Build(&source);
  CHECK(!v.Complete());
  CHECK(Has(v.stale, "latency") && Has(v.stale, "jitter"));
  // Fresh measurements are accepted with their provenance.
  inputs.pop_back();
  inputs.pop_back();
  inputs.push_back({"latency", 12.0, "ms", FeatureProvenance::Measured, "probe", 500.0});
  inputs.push_back({"jitter", 1.0, "ms", FeatureProvenance::Measured, "probe", 500.0});
  source.SetInputs(inputs);
  v = Build(&source);
  CHECK(v.Complete());
  CHECK(Find(v, "latency").provenance == FeatureProvenance::Measured);
  CHECK(*Find(v, "latency").age_ms == 500.0);
}

void UnsupportedAndConflictingInputs() {
  ConfiguredTelemetrySource source;
  source.SetProfile(aafr::test::ConfiguredTelemetryProfile(), "p");
  auto inputs = source.Features();
  inputs.push_back({"scenario_category", 1.0, "count", FeatureProvenance::Configured, "test", std::nullopt});
  source.SetInputs(inputs);
  auto v = Build(&source);
  CHECK(!v.Complete());  // an unknown feature makes the contract ambiguous
  CHECK(Has(v.invalid, "unsupported:scenario_category"));
  // A telemetry source must not override a graph-derived feature.
  inputs.pop_back();
  inputs.push_back({"num_nodes", 500.0, "count", FeatureProvenance::Configured, "test", std::nullopt});
  source.SetInputs(inputs);
  v = Build(&source);
  CHECK(!v.Complete());
  CHECK(Has(v.invalid, "conflict:num_nodes"));
  CHECK(*Find(v, "num_nodes").value == 4.0);  // the graph value stands
  // A feature offered twice is ambiguous: neither value is used.
  inputs.pop_back();
  inputs.push_back({"latency", 5.0, "ms", FeatureProvenance::Configured, "test", std::nullopt});
  source.SetInputs(inputs);
  v = Build(&source);
  CHECK(Has(v.invalid, "latency"));
  CHECK(!Find(v, "latency").value.has_value());
  // Provenance other than measured/configured is refused.
  inputs.pop_back();
  for (auto it = inputs.begin(); it != inputs.end(); ++it) {
    if (it->name == "latency") {
      inputs.erase(it);
      break;
    }
  }
  inputs.push_back({"latency", 5.0, "ms", FeatureProvenance::Substitute, "test", 1.0});
  source.SetInputs(inputs);
  v = Build(&source);
  CHECK(Has(v.invalid, "latency"));
}

void NetworkStateSubstitutesOptIn() {
  ConfiguredTelemetrySource source;
  auto profile = aafr::test::ConfiguredTelemetryProfile();
  for (const char* n : {"latency", "latency_p50", "latency_min", "latency_max", "jitter", "throughput"}) profile.erase(n);
  source.SetProfile(profile, "p");
  netstate::NetworkState state = aafr::test::MakeState(
      7, {aafr::test::MakeFeature("rtt.handshake.mean_ms", 25.0), aafr::test::MakeFeature("rtt.handshake.median_ms", 22.0),
          aafr::test::MakeFeature("rtt.handshake.min_ms", 10.0), aafr::test::MakeFeature("rtt.handshake.max_ms", 80.0),
          aafr::test::MakeFeature("ack_delay.jitter_ms", 3.0), aafr::test::MakeFeature("throughput.mean_mbps", 30.0)});
  for (auto& f : state.windows[0].features) f.unit = Contains(f.id, "mbps") ? netstate::Unit::Mbps : netstate::Unit::Milliseconds;
  // Off by default: host-wide values do not stand in for model features.
  auto v = Build(&source, &state);
  CHECK(!v.Complete());
  CHECK(v.missing.size() == 6);
  // Opted in: accepted, labelled as substitutes.
  FeatureAdapterConfig config;
  config.allow_network_state_substitutes = true;
  v = Build(&source, &state, config);
  CHECK(v.Complete());
  CHECK(v.substituted.size() == 6);
  CHECK(Find(v, "latency").provenance == FeatureProvenance::Substitute);
  CHECK(Contains(Find(v, "latency").source, "network_state:rtt.handshake.mean_ms#7"));
  // Stale or invalid NetworkState features are not used.
  state.windows[0].features[0].age_ms = 120'000.0;
  state.windows[0].features[1].quality = netstate::FeatureQuality::InsufficientSamples;
  v = Build(&source, &state, config);
  CHECK(!v.Complete());
  CHECK(Has(v.stale, "latency"));
  CHECK(Has(v.missing, "latency_p50"));
}

void UnreachableDestinationInvalid() {
  ConfiguredTelemetrySource source;
  source.SetProfile(aafr::test::ConfiguredTelemetryProfile(), "p");
  Inputs in;
  in.graph.SetEdgeAvailable("relay_a", "dest", false);
  in.graph.SetEdgeAvailable("relay_b", "dest", false);
  const auto v = ModelFeatureAdapter().Build(in.graph, in.problem, nullptr, &source);
  CHECK(!v.Complete());
  CHECK(Has(v.invalid, "req_shortest_hops"));
}

void SerializationMatchesPythonContract() {
  // The request the client sends: exactly the 35 contract names, finite
  // numbers, nothing else (the worker rejects unknown or missing names).
  ConfiguredTelemetrySource source;
  source.SetProfile(aafr::test::ConfiguredTelemetryProfile(), "p");
  const auto values = Build(&source).Values();
  JsonValue features = JsonValue::MakeObject();
  for (const auto& e : values) features.Set(e.first, JsonValue::Number(e.second));
  const auto back = ParseJson(features.Dump());
  CHECK(back.value && back.value->AsObject().size() == 35);
  for (const auto& spec : RawFeatureContract()) {
    CHECK(back.value->GetNumber(spec.name).has_value());
    CHECK(*back.value->GetNumber(spec.name) == values.at(spec.name));
  }
}

}  // namespace

int main() {
  return aafr::test::RunTests({
      {"complete_vector_in_contract_order", CompleteVectorInContractOrder},
      {"missing_telemetry_abstains", MissingTelemetryAbstains},
      {"invalid_values_rejected", InvalidValuesRejected},
      {"stale_measured_values", StaleMeasuredValues},
      {"unsupported_and_conflicting_inputs", UnsupportedAndConflictingInputs},
      {"network_state_substitutes_opt_in", NetworkStateSubstitutesOptIn},
      {"unreachable_destination_invalid", UnreachableDestinationInvalid},
      {"serialization_matches_python_contract", SerializationMatchesPythonContract},
  });
}
