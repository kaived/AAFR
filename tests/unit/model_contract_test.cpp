// V0.7.2 expert-model contract: the strict JSON codec of the worker protocol,
// the 35-feature raw contract, the canonical algorithm-label mapping, the
// pinned manifest and the graph/request features computed with the training
// formulas. No Python needed.

#include <cmath>
#include <fstream>
#include <set>
#include <sstream>

#include "runtime/model/json_value.h"
#include "runtime/model/model_contract.h"
#include "runtime/model/model_feature_adapter.h"
#include "tests/support/runtime_test_support.h"
#include "tests/support/test_support.h"

using namespace aafr;
using namespace aafr::model;
using algorithms::AlgorithmId;
namespace fwd = aafr::forwarding;

using aafr::test::Contains;

namespace {

std::string ReadFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream s;
  s << in.rdbuf();
  return s.str();
}

std::string ManifestPath() {
#ifdef AAFR_TEST_MODEL_MANIFEST
  return AAFR_TEST_MODEL_MANIFEST;
#else
  return "";
#endif
}

// ---------------------------------------------------------------- JSON

void JsonParsesAndRejects() {
  const auto ok = ParseJson(R"({"a":1,"b":[true,false,null],"c":"x\u00e9\n","d":-2.5e3,"e":{}})");
  CHECK(ok.value.has_value());
  CHECK(ok.value->GetNumber("a").value_or(0) == 1.0);
  CHECK(ok.value->Find("b")->AsArray().size() == 3);
  CHECK(ok.value->GetString("c").value_or("") == "x\xC3\xA9\n");
  CHECK(ok.value->GetNumber("d").value_or(0) == -2500.0);
  // Strictness: NaN/Infinity tokens, trailing data, duplicates, bad escapes.
  for (const char* bad : {R"({"a":NaN})", R"({"a":Infinity})", R"({"a":-Infinity})", R"({"a":1} x)", R"({"a":1,"a":2})",
                          R"({"a":"\q"})", R"({"a":01})", R"({"a":1.})", "", R"([1,])", R"({"a" 1})",
                          "{\"a\":\"\x01\"}", R"({"a":1e999})"}) {
    CHECK(!ParseJson(bad).value.has_value());
  }
  std::string deep;
  for (int i = 0; i < kMaxJsonDepth + 5; ++i) deep += "[";
  for (int i = 0; i < kMaxJsonDepth + 5; ++i) deep += "]";
  CHECK(!ParseJson(deep).value.has_value());
}

void JsonRoundTrip() {
  JsonValue v = JsonValue::MakeObject();
  v.Set("id", JsonValue::String("r-1 \"quoted\""));
  v.Set("n", JsonValue::Number(0.1 + 0.2));
  v.Set("k", JsonValue::Number(42));
  v.Set("flag", JsonValue::Bool(true));
  const auto back = ParseJson(v.Dump());
  CHECK(back.value.has_value());
  CHECK(back.value->GetString("id").value_or("") == "r-1 \"quoted\"");
  CHECK(back.value->GetNumber("n").value_or(0) == 0.1 + 0.2);  // %.17g is exact
  CHECK(back.value->GetNumber("k").value_or(0) == 42.0);
  CHECK(v.Dump().find("42") != std::string::npos);
}

// ---------------------------------------------------------------- contract

void RawContract() {
  const auto& c = RawFeatureContract();
  CHECK(c.size() == 35);
  std::set<std::string> names;
  for (const auto& f : c) names.insert(f.name);
  CHECK(names.size() == 35);
  CHECK(c.front().name == "num_nodes");
  CHECK(c[7].name == "req_shortest_hops");
  CHECK(c[12].name == "latency" && c[12].unit == "ms");
  CHECK(FindRawFeature("loss")->unit == "ratio");
  CHECK(FindRawFeature("mean_link_utilization")->unit == "percent");
  CHECK(FindRawFeature("throughput")->unit == "mbps");
  CHECK(c.back().name == "reroute_count");
  CHECK(!FindRawFeature("scenario_category"));  // never an input
  CHECK(!FindRawFeature("congestion_ratio"));   // engineered in Python, not here
}

void LabelMapping() {
  const auto& rows = AlgorithmMappings();
  CHECK(rows.size() == 10);
  std::size_t with_label = 0;
  for (std::size_t i = 0; i < rows.size(); ++i) {
    CHECK(rows[i].id == algorithms::kAllAlgorithmIds[i]);
    CHECK(rows[i].registry_name == algorithms::ToString(rows[i].id));
    CHECK(algorithms::ParseAlgorithmId(rows[i].registry_name) == rows[i].id);
    if (rows[i].model_label) ++with_label;
  }
  CHECK(with_label == 5);
  CHECK(MapModelLabel("Dijkstra") == AlgorithmId::Dijkstra);
  CHECK(MapModelLabel("ACO") == AlgorithmId::AntColonyOptimization);
  CHECK(MapModelLabel("GWO") == AlgorithmId::GreyWolfOptimizer);
  CHECK(MapModelLabel("PSO") == AlgorithmId::ParticleSwarmOptimization);
  CHECK(MapModelLabel("WOA") == AlgorithmId::WhaleOptimizationAlgorithm);
  // Outside the trained label space: never mapped to something else.
  for (const char* bad : {"pso", "dijkstra", "A*", "astar", "Bellman-Ford", "GA", "ABC", "Firefly", "Other", "Teleport", ""}) {
    CHECK(!MapModelLabel(bad).has_value());
  }
  CHECK(DisplayName(AlgorithmId::AStar) == "A*");
  CHECK(DisplayName(AlgorithmId::BellmanFord) == "Bellman-Ford");
}

void RepositoryManifestMatchesContract() {
  const auto loaded = LoadManifest(ManifestPath());
  CHECK(loaded.manifest.has_value());
  if (!loaded.manifest) return;
  const ModelManifest& m = *loaded.manifest;
  CHECK(ValidateManifestAgainstContract(m).empty());
  CHECK(m.raw_features.size() == 35);
  CHECK(m.model_feature_columns.size() == 55);
  CHECK(m.stage2_classes == std::vector<std::string>({"ACO", "GWO", "PSO", "WOA"}));
  CHECK(m.gate_threshold && std::fabs(*m.gate_threshold - 0.51) < 1e-9);
  CHECK(m.label_map.size() == 5);
  CHECK(Contains(m.calibration, "not evaluated"));
  for (const auto& f : m.raw_features) CHECK(f.training_min && f.training_max && *f.training_min <= *f.training_max);
}

void ManifestMismatchesDetected() {
  const std::string text = ReadFile(ManifestPath());
  const auto replaced = [&](const std::string& from, const std::string& to) {
    std::string t = text;
    const auto pos = t.find(from);
    if (pos != std::string::npos) t.replace(pos, from.size(), to);
    return t;
  };
  const auto issues = [](const std::string& t) {
    const auto r = ParseManifest(t);
    return r.manifest ? ValidateManifestAgainstContract(*r.manifest) : std::vector<std::string>{"unparseable"};
  };
  CHECK(!issues(replaced("\"name\": \"jitter\"", "\"name\": \"jitter_ms\"")).empty());           // renamed feature
  CHECK(!issues(replaced("\"unit\": \"percent\"", "\"unit\": \"ratio\"")).empty());              // unit change
  CHECK(!issues(replaced("aafr.model_features.v1", "aafr.model_features.v9")).empty());          // contract version
  CHECK(!issues(replaced("\"PSO\": \"pso\"", "\"PSO\": \"ga\"")).empty());                       // label remapped
  CHECK(!issues(replaced("\"protocol_version\": 1", "\"protocol_version\": 2")).empty());        // protocol
  CHECK(!ParseManifest(replaced("aafr.expert_model_manifest.v1", "other")).manifest.has_value());  // schema
  CHECK(!ParseManifest("{not json").manifest.has_value());
  CHECK(!LoadManifest("Z:/does/not/exist.json").manifest.has_value());
}

// ---------------------------------------------------------------- graph features

fwd::ForwardingGraph Bidirectional(const std::vector<std::string>& nodes,
                                   const std::vector<std::pair<std::string, std::string>>& links) {
  fwd::ForwardingGraph g("t");
  for (std::size_t i = 0; i < nodes.size(); ++i) {
    g.AddNode({nodes[i], i == 0 ? fwd::NodeRole::Entry : (i + 1 == nodes.size() ? fwd::NodeRole::Destination : fwd::NodeRole::Relay), true});
  }
  for (const auto& l : links) {
    for (int dir = 0; dir < 2; ++dir) {
      fwd::ForwardingEdge e;
      e.from = dir ? l.second : l.first;
      e.to = dir ? l.first : l.second;
      e.latency_ms = fwd::Measurement::Of(1.0, fwd::Provenance::Configured);
      e.loss_ratio = fwd::Measurement::Of(0.0, fwd::Provenance::Configured);
      e.bandwidth_mbps = fwd::Measurement::Of(10.0, fwd::Provenance::Configured);
      g.AddEdge(e);
    }
  }
  return g;
}

fwd::ForwardingProblem Problem(const std::string& s, const std::string& d) {
  fwd::ForwardingProblem p;
  p.source = s;
  p.destination = d;
  return p;
}

void TwoPathGraphFeatures() {
  const fwd::ForwardingGraph g = aafr::test::TwoPathGraph();
  const GraphFeatureResult r = ComputeGraphFeatures(g, aafr::test::ProblemFor(g));
  CHECK(r.issues.empty());
  CHECK(r.values.at("num_nodes") == 4);
  CHECK(r.values.at("num_edges") == 4);  // directed edges
  CHECK(r.values.at("avg_node_degree") == 2.0);
  CHECK_NEAR(r.values.at("network_density"), 0.666667, 1e-9);
  CHECK(r.values.at("avg_clustering_coefficient") == 0.0);
  CHECK(r.values.at("diameter") == 2);
  CHECK_NEAR(r.values.at("algebraic_connectivity"), 2.0, 1e-9);  // 4-cycle: eigenvalues 0,2,2,4
  CHECK(r.values.at("req_shortest_hops") == 2);
  CHECK(r.values.at("source_in_degree") == 0 && r.values.at("source_out_degree") == 2);
  CHECK(r.values.at("dest_in_degree") == 2 && r.values.at("dest_out_degree") == 0);
}

void CompleteGraphFeatures() {
  const auto g = Bidirectional({"a", "b", "c", "d"}, {{"a", "b"}, {"a", "c"}, {"a", "d"}, {"b", "c"}, {"b", "d"}, {"c", "d"}});
  const auto r = ComputeGraphFeatures(g, Problem("a", "d"));
  CHECK(r.values.at("num_edges") == 12);
  CHECK(r.values.at("network_density") == 1.0);
  CHECK(r.values.at("avg_clustering_coefficient") == 1.0);
  CHECK(r.values.at("diameter") == 1);
  CHECK_NEAR(r.values.at("algebraic_connectivity"), 4.0, 1e-9);  // K_n: n
  CHECK(r.values.at("req_shortest_hops") == 1);
  CHECK(r.values.at("source_in_degree") == 3 && r.values.at("dest_out_degree") == 3);
}

void PathAndTriangleFeatures() {
  // Path a-b-c: algebraic connectivity 1, diameter 2, clustering 0.
  const auto p = ComputeGraphFeatures(Bidirectional({"a", "b", "c"}, {{"a", "b"}, {"b", "c"}}), Problem("a", "c"));
  CHECK_NEAR(p.values.at("algebraic_connectivity"), 1.0, 1e-9);
  CHECK(p.values.at("diameter") == 2);
  // Triangle a-b-c plus pendant d on c: clustering (1 + 1 + 1/3 + 0) / 4 = 0.5833.
  const auto t = ComputeGraphFeatures(Bidirectional({"a", "b", "c", "d"}, {{"a", "b"}, {"b", "c"}, {"a", "c"}, {"c", "d"}}),
                                      Problem("a", "d"));
  CHECK_NEAR(t.values.at("avg_clustering_coefficient"), 0.5833, 1e-9);
  CHECK(t.values.at("req_shortest_hops") == 2);
}

void DisconnectedAndUnavailable() {
  // Two components: a-b-c and d-e. Algebraic connectivity 0 (training rule),
  // diameter of the largest component.
  auto g = Bidirectional({"a", "b", "c", "d", "e"}, {{"a", "b"}, {"b", "c"}, {"d", "e"}});
  auto r = ComputeGraphFeatures(g, Problem("a", "e"));
  CHECK(r.values.at("algebraic_connectivity") == 0.0);
  CHECK(r.values.at("diameter") == 2);
  CHECK(!r.values.count("req_shortest_hops"));  // unreachable: no value
  CHECK(!r.issues.empty());
  // An unavailable node and its edges leave the active topology.
  g = aafr::test::TwoPathGraph();
  g.SetNodeAvailable("relay_b", false);
  r = ComputeGraphFeatures(g, aafr::test::ProblemFor(g));
  CHECK(r.values.at("num_nodes") == 3);
  CHECK(r.values.at("num_edges") == 2);
  CHECK(r.values.at("source_out_degree") == 1);
}

void JacobiEigenvalues() {
  const auto e = SymmetricEigenvalues({{2, -1, 0}, {-1, 2, -1}, {0, -1, 2}});
  CHECK(e.size() == 3);
  CHECK_NEAR(e[0], 2.0 - std::sqrt(2.0), 1e-9);
  CHECK_NEAR(e[1], 2.0, 1e-9);
  CHECK_NEAR(e[2], 2.0 + std::sqrt(2.0), 1e-9);
}

void LoopbackDemoGraphFeatures() {
  const fwd::ForwardingGraph g = fwd::LoopbackDemoGraph();
  const auto r = ComputeGraphFeatures(g, fwd::LoopbackDemoProblem(g));
  CHECK(r.issues.empty());
  CHECK(r.values.size() == 12);
  CHECK(r.values.at("num_nodes") >= 3);
}

}  // namespace

int main() {
  return aafr::test::RunTests({
      {"json_parses_and_rejects", JsonParsesAndRejects},
      {"json_round_trip", JsonRoundTrip},
      {"raw_contract", RawContract},
      {"label_mapping", LabelMapping},
      {"repository_manifest_matches_contract", RepositoryManifestMatchesContract},
      {"manifest_mismatches_detected", ManifestMismatchesDetected},
      {"two_path_graph_features", TwoPathGraphFeatures},
      {"complete_graph_features", CompleteGraphFeatures},
      {"path_and_triangle_features", PathAndTriangleFeatures},
      {"disconnected_and_unavailable", DisconnectedAndUnavailable},
      {"jacobi_eigenvalues", JacobiEigenvalues},
      {"loopback_demo_graph_features", LoopbackDemoGraphFeatures},
  });
}
