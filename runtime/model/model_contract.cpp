#include "runtime/model/model_contract.h"

#include <algorithm>
#include <fstream>
#include <sstream>

#include "runtime/model/json_value.h"

namespace aafr::model {

namespace alg = aafr::algorithms;

const char* ToString(FeatureGroup value) {
  switch (value) {
    case FeatureGroup::Topology: return "topology";
    case FeatureGroup::Request: return "request";
    case FeatureGroup::Telemetry: return "telemetry";
    case FeatureGroup::LatencyDistribution: return "latency_distribution";
    case FeatureGroup::Dynamics: return "dynamics";
  }
  return "unknown";
}

const std::vector<RawFeatureSpec>& RawFeatureContract() {
  using G = FeatureGroup;
  static const std::vector<RawFeatureSpec> kContract = {
      // Topology: computed from the graph exactly as simulator/topology.py does.
      {"num_nodes", "count", G::Topology, true, 0.0, std::nullopt},
      {"num_edges", "count", G::Topology, true, 0.0, std::nullopt},
      {"network_density", "ratio", G::Topology, false, 0.0, 1.0},
      {"avg_node_degree", "count", G::Topology, false, 0.0, std::nullopt},
      {"diameter", "hops", G::Topology, true, 0.0, std::nullopt},
      {"avg_clustering_coefficient", "ratio", G::Topology, false, 0.0, 1.0},
      {"algebraic_connectivity", "eigenvalue", G::Topology, false, 0.0, std::nullopt},
      // Request: the source/destination pair on the directed graph.
      {"req_shortest_hops", "hops", G::Request, true, 0.0, std::nullopt},
      {"source_in_degree", "count", G::Request, true, 0.0, std::nullopt},
      {"source_out_degree", "count", G::Request, true, 0.0, std::nullopt},
      {"dest_in_degree", "count", G::Request, true, 0.0, std::nullopt},
      {"dest_out_degree", "count", G::Request, true, 0.0, std::nullopt},
      // Telemetry: network-wide packet statistics in the training simulator.
      {"latency", "ms", G::Telemetry, false, 0.0, std::nullopt},
      {"jitter", "ms", G::Telemetry, false, 0.0, std::nullopt},
      {"loss", "ratio", G::Telemetry, false, 0.0, 1.0},
      {"throughput", "mbps", G::Telemetry, false, 0.0, std::nullopt},
      {"mean_link_utilization", "percent", G::Telemetry, false, 0.0, std::nullopt},
      {"max_link_utilization", "percent", G::Telemetry, false, 0.0, std::nullopt},
      {"link_utilization_std", "percent", G::Telemetry, false, 0.0, std::nullopt},
      {"avg_queue_delay_ms", "ms", G::Telemetry, false, 0.0, std::nullopt},
      {"max_queue_depth", "packets", G::Telemetry, true, 0.0, std::nullopt},
      {"avg_queue_occupancy", "ratio", G::Telemetry, false, 0.0, 1.0},
      {"offered_load_mbps", "mbps", G::Telemetry, false, 0.0, std::nullopt},
      {"packet_delivery_ratio", "ratio", G::Telemetry, false, 0.0, 1.0},
      {"jains_fairness_index", "ratio", G::Telemetry, false, 0.0, 1.0},
      // Latency distribution of delivered packets.
      {"latency_p50", "ms", G::LatencyDistribution, false, 0.0, std::nullopt},
      {"latency_p90", "ms", G::LatencyDistribution, false, 0.0, std::nullopt},
      {"latency_p95", "ms", G::LatencyDistribution, false, 0.0, std::nullopt},
      {"latency_p99", "ms", G::LatencyDistribution, false, 0.0, std::nullopt},
      {"latency_min", "ms", G::LatencyDistribution, false, 0.0, std::nullopt},
      {"latency_max", "ms", G::LatencyDistribution, false, 0.0, std::nullopt},
      {"latency_std", "ms", G::LatencyDistribution, false, 0.0, std::nullopt},
      // Dynamics during the observation window.
      {"link_failure_events", "count", G::Dynamics, true, 0.0, std::nullopt},
      {"link_recovery_events", "count", G::Dynamics, true, 0.0, std::nullopt},
      {"reroute_count", "count", G::Dynamics, true, 0.0, std::nullopt},
  };
  return kContract;
}

const RawFeatureSpec* FindRawFeature(const std::string& name) {
  for (const RawFeatureSpec& s : RawFeatureContract()) {
    if (s.name == name) return &s;
  }
  return nullptr;
}

const std::vector<AlgorithmMapping>& AlgorithmMappings() {
  using A = alg::AlgorithmId;
  static const std::vector<AlgorithmMapping> kMappings = [] {
    std::vector<AlgorithmMapping> m = {
        {A::Dijkstra, "", "Dijkstra", std::string("Dijkstra")},
        {A::AStar, "", "A*", std::nullopt},
        {A::BellmanFord, "", "Bellman-Ford", std::nullopt},
        {A::AntColonyOptimization, "", "Ant Colony Optimization (ACO)", std::string("ACO")},
        {A::ParticleSwarmOptimization, "", "Particle Swarm Optimization (PSO)", std::string("PSO")},
        {A::GeneticAlgorithm, "", "Genetic Algorithm (GA)", std::nullopt},
        {A::ArtificialBeeColony, "", "Artificial Bee Colony (ABC)", std::nullopt},
        {A::GreyWolfOptimizer, "", "Grey Wolf Optimizer (GWO)", std::string("GWO")},
        {A::FireflyAlgorithm, "", "Firefly Algorithm", std::nullopt},
        {A::WhaleOptimizationAlgorithm, "", "Whale Optimization Algorithm (WOA)", std::string("WOA")},
    };
    for (AlgorithmMapping& row : m) row.registry_name = alg::ToString(row.id);
    return m;
  }();
  return kMappings;
}

std::optional<alg::AlgorithmId> MapModelLabel(const std::string& label) {
  for (const AlgorithmMapping& row : AlgorithmMappings()) {
    if (row.model_label && *row.model_label == label) return row.id;
  }
  return std::nullopt;
}

std::string DisplayName(alg::AlgorithmId id) {
  for (const AlgorithmMapping& row : AlgorithmMappings()) {
    if (row.id == id) return row.display_name;
  }
  return alg::ToString(id);
}

ManifestLoadResult ParseManifest(const std::string& json_text) {
  ManifestLoadResult r;
  const JsonParseResult parsed = ParseJson(json_text);
  if (!parsed.value) {
    r.errors.push_back("manifest is not valid JSON: " + parsed.error);
    return r;
  }
  const JsonValue& j = *parsed.value;
  if (j.GetString("schema").value_or("") != kManifestSchema) {
    r.errors.push_back(std::string("manifest schema is not ") + kManifestSchema);
    return r;
  }
  ModelManifest m;
  m.model_name = j.GetString("model_name").value_or("");
  m.model_version = j.GetString("model_version").value_or("");
  m.feature_contract_version = j.GetString("feature_contract_version").value_or("");
  m.protocol_version = static_cast<int>(j.GetNumber("protocol_version").value_or(0));
  m.calibration = j.GetString("calibration").value_or("");
  if (m.model_version.empty()) r.errors.push_back("manifest has no model_version");
  if (const JsonValue* raw = j.Find("raw_features"); raw && raw->IsArray()) {
    for (const JsonValue& f : raw->AsArray()) {
      ManifestFeature mf;
      mf.name = f.GetString("name").value_or("");
      mf.unit = f.GetString("unit").value_or("");
      mf.group = f.GetString("group").value_or("");
      mf.training_min = f.GetNumber("training_min");
      mf.training_max = f.GetNumber("training_max");
      m.raw_features.push_back(mf);
    }
  } else {
    r.errors.push_back("manifest has no raw_features array");
  }
  if (const JsonValue* cols = j.Find("model_feature_columns"); cols && cols->IsArray()) {
    for (const JsonValue& c : cols->AsArray()) m.model_feature_columns.push_back(c.IsString() ? c.AsString() : "");
  }
  if (const JsonValue* s1 = j.Find("stage1"); s1 && s1->IsObject()) {
    m.gate_threshold = s1->GetNumber("threshold");
    m.gate_meaning = s1->GetString("meaning").value_or("");
  }
  if (!m.gate_threshold) r.errors.push_back("manifest has no stage1.threshold");
  if (const JsonValue* s2 = j.Find("stage2"); s2 && s2->IsObject()) {
    if (const JsonValue* classes = s2->Find("classes"); classes && classes->IsArray()) {
      for (const JsonValue& c : classes->AsArray()) m.stage2_classes.push_back(c.IsString() ? c.AsString() : "");
    }
  }
  if (const JsonValue* labels = j.Find("label_map"); labels && labels->IsObject()) {
    for (const auto& e : labels->AsObject()) {
      m.label_map.emplace_back(e.first, e.second.IsString() ? e.second.AsString() : "");
    }
  } else {
    r.errors.push_back("manifest has no label_map");
  }
  if (r.errors.empty()) r.manifest = std::move(m);
  return r;
}

ManifestLoadResult LoadManifest(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    ManifestLoadResult r;
    r.errors.push_back("manifest not found: " + path);
    return r;
  }
  std::ostringstream text;
  text << in.rdbuf();
  if (text.str().size() > 4 * 1024 * 1024) {
    ManifestLoadResult r;
    r.errors.push_back("manifest larger than 4 MiB");
    return r;
  }
  return ParseManifest(text.str());
}

std::vector<std::string> ValidateManifestAgainstContract(const ModelManifest& m) {
  std::vector<std::string> issues;
  if (m.protocol_version != kProtocolVersion) {
    issues.push_back("protocol_version " + std::to_string(m.protocol_version) + " != " + std::to_string(kProtocolVersion));
  }
  if (m.feature_contract_version != kFeatureContractVersion) {
    issues.push_back("feature contract '" + m.feature_contract_version + "' != '" + kFeatureContractVersion + "'");
  }
  const auto& contract = RawFeatureContract();
  if (m.raw_features.size() != contract.size()) {
    issues.push_back("manifest lists " + std::to_string(m.raw_features.size()) + " raw features, contract has " +
                     std::to_string(contract.size()));
  }
  for (std::size_t i = 0; i < std::min(m.raw_features.size(), contract.size()); ++i) {
    if (m.raw_features[i].name != contract[i].name) {
      issues.push_back("raw feature " + std::to_string(i) + " is '" + m.raw_features[i].name + "', contract expects '" +
                       contract[i].name + "'");
    } else if (m.raw_features[i].unit != contract[i].unit) {
      issues.push_back("feature '" + contract[i].name + "' unit '" + m.raw_features[i].unit + "' != contract '" +
                       contract[i].unit + "'");
    }
  }
  for (const auto& entry : m.label_map) {
    const auto mapped = MapModelLabel(entry.first);
    if (!mapped) {
      issues.push_back("manifest label '" + entry.first + "' is not in the AAFR mapping");
    } else if (entry.second != alg::ToString(*mapped)) {
      issues.push_back("manifest maps '" + entry.first + "' to '" + entry.second + "', AAFR maps it to '" +
                       alg::ToString(*mapped) + "'");
    }
  }
  for (const std::string& c : m.stage2_classes) {
    if (!MapModelLabel(c)) issues.push_back("stage 2 class '" + c + "' has no AAFR mapping");
  }
  return issues;
}

}  // namespace aafr::model
