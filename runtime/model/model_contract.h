#pragma once

// V0.7.2 expert-model contract on the AAFR side. Plain C++17.
//
// - The raw input feature contract (aafr.model_features.v1): the 35 raw
//   inputs of the existing two-stage LightGBM selector, in the model's
//   RAW_INPUT_FEATURES order, with units and groups. The model project's
//   features.py turns them into the 55 model columns inside the worker; AAFR
//   never re-implements that transformation.
// - The canonical algorithm mapping between model class labels, AAFR
//   AlgorithmIds, registry names and user-facing names.
// - The manifest (expert_model/models/model_manifest.json) that pins the
//   artifacts, and its validation against this contract.

#include <optional>
#include <string>
#include <vector>

#include "algorithms/algorithm_id.h"

namespace aafr::model {

inline constexpr int kProtocolVersion = 1;
inline constexpr const char* kFeatureContractVersion = "aafr.model_features.v1";
inline constexpr const char* kManifestSchema = "aafr.expert_model_manifest.v1";

enum class FeatureGroup { Topology, Request, Telemetry, LatencyDistribution, Dynamics };
const char* ToString(FeatureGroup value);

struct RawFeatureSpec {
  std::string name;
  std::string unit;  // count | ratio | hops | eigenvalue | ms | mbps | percent | packets
  FeatureGroup group{FeatureGroup::Topology};
  // Integer-valued in training (counts, hops); fractional values are invalid.
  bool integer{false};
  // Valid physical range (not the training range): values outside are invalid.
  double min_valid{0.0};
  std::optional<double> max_valid;
};

// The 35 raw features in contract order.
const std::vector<RawFeatureSpec>& RawFeatureContract();
const RawFeatureSpec* FindRawFeature(const std::string& name);

// ---------------------------------------------------------------- labels

struct AlgorithmMapping {
  algorithms::AlgorithmId id{algorithms::AlgorithmId::Dijkstra};
  std::string registry_name;              // algorithms::ToString(id), e.g. "bellman_ford".
  std::string display_name;               // e.g. "Bellman-Ford".
  std::optional<std::string> model_label; // Label in the trained label space, if any.
};

// All ten AAFR algorithms, in AlgorithmId order. Only five have a model label.
const std::vector<AlgorithmMapping>& AlgorithmMappings();
// Exact, case-sensitive mapping of a trained model label. nullopt for any
// label outside the trained label space ("Dijkstra", "ACO", "GWO", "PSO",
// "WOA"); an unknown label is never mapped to a different algorithm.
std::optional<algorithms::AlgorithmId> MapModelLabel(const std::string& label);
std::string DisplayName(algorithms::AlgorithmId id);

// ---------------------------------------------------------------- manifest

struct ManifestFeature {
  std::string name;
  std::string unit;
  std::string group;
  std::optional<double> training_min;
  std::optional<double> training_max;
};

struct ModelManifest {
  std::string model_name;
  std::string model_version;
  std::string feature_contract_version;
  int protocol_version{0};
  std::vector<ManifestFeature> raw_features;
  std::vector<std::string> model_feature_columns;
  std::vector<std::string> stage2_classes;
  std::optional<double> gate_threshold;
  std::string gate_meaning;
  std::vector<std::pair<std::string, std::string>> label_map;  // model label -> registry name
  std::string calibration;
};

struct ManifestLoadResult {
  std::optional<ModelManifest> manifest;
  std::vector<std::string> errors;
};

ManifestLoadResult ParseManifest(const std::string& json_text);
ManifestLoadResult LoadManifest(const std::string& path);
// Problems between a manifest and the AAFR contract (feature names, order,
// units, contract version, protocol version, label map). Empty when they match.
std::vector<std::string> ValidateManifestAgainstContract(const ModelManifest& manifest);

}  // namespace aafr::model
