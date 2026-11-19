#pragma once

// V0.7.4 feature acquisition contract (aafr.feature_acquisition.v1): for each
// of the 35 raw inputs of the existing expert model (aafr.model_features.v1,
// unchanged), what it meant in training, where AAFR could obtain it, how,
// how fresh it must be, what happens when it is missing, and whether an AAFR
// observation can stand for it at all. Plain C++17. It is the authoritative
// availability table that the feature adapter, the compatibility check, the
// Developer Tools and the README inventory all read.
//
// Categories (V0.7.4 section 6):
//   A DirectlyObservable       read exactly from AAFR's forwarding graph and
//                              problem with the training formulas
//   B DerivedFromObservations  computed from validated observations with a
//                              documented formula
//   C Estimated                AAFR has a related observation, but it is a
//                              different quantity; exposed as a research
//                              estimate and never accepted as the feature
//   D Unavailable              no measurement source exists in AAFR
//   E Incompatible             no operational equivalent in application-layer
//                              forwarding over loopback relays
// Only A and B can satisfy the measured-input contract.

#include <string>
#include <vector>

#include "runtime/model/model_contract.h"

namespace aafr::model {

inline constexpr const char* kAcquisitionContractVersion = "aafr.feature_acquisition.v1";

enum class FeatureCategory { DirectlyObservable, DerivedFromObservations, Estimated, Unavailable, Incompatible };
const char* ToString(FeatureCategory value);  // "A_direct", "B_derived", "C_estimated", "D_unavailable", "E_incompatible"
char Letter(FeatureCategory value);           // 'A'..'E'

struct FeatureAcquisitionSpec {
  std::string name;
  int index{0};                    // Position in RAW_INPUT_FEATURES and in the 55 model columns.
  std::string data_type;           // "float64" or "int64" (training CSV)
  std::string unit;                // Contract unit.
  std::string training_definition; // What the simulator computed (simulator/metrics.py, topology.py).
  std::string preprocessing;       // In the worker: engineer_features() + inf/NaN -> 0 (unchanged).
  std::string runtime_source;      // AAFR source (feature id or component), or "none".
  std::string measurement_method;
  std::string freshness;           // Freshness requirement.
  std::string missing_policy;
  FeatureCategory category{FeatureCategory::Unavailable};
  std::string domain_compatibility;
  std::string validation_rule;
  // For C: the NetworkState feature exposed as an estimate (never accepted).
  // For B: the NetworkState feature accepted as the value.
  std::string network_state_feature;
};

// The 35 raw features in contract order (same order as RawFeatureContract()).
const std::vector<FeatureAcquisitionSpec>& FeatureAcquisitionContract();
const FeatureAcquisitionSpec* FindAcquisitionSpec(const std::string& name);

// The 20 model columns that are not raw inputs: 13 engineered by the model
// project's features.py inside the worker (AAFR never re-implements them)
// and 7 scenario-category one-hots.
struct ModelColumnSpec {
  std::string name;
  int index{0};               // Position in the 55 model columns.
  std::string formula;        // As in ml/features.py.
  std::vector<std::string> inputs;
  FeatureCategory category{FeatureCategory::DerivedFromObservations};  // E for the one-hots.
  std::string note;
};
const std::vector<ModelColumnSpec>& EngineeredColumnContract();

// Problems between the acquisition contract and the raw contract or the
// manifest (names, order, units, count). Empty when consistent.
std::vector<std::string> ValidateAcquisitionContract(const ModelManifest* manifest = nullptr);

// Counts per category.
struct CategoryCounts {
  int direct{0}, derived{0}, estimated{0}, unavailable{0}, incompatible{0};
};
CategoryCounts CountCategories();

}  // namespace aafr::model
