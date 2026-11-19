#pragma once

// ModelFeatureAdapter (V0.7.2): builds the exact raw-feature input of the
// expert model (aafr.model_features.v1) from AAFR state. Plain C++17.
//
// Sources, per feature group:
//   topology (7), request (5)    computed from the immutable forwarding graph
//                                and problem with the training formulas
//                                (simulator/topology.py, routing/requests.py)
//   telemetry, latency
//   distribution, dynamics (23)  an injected ITelemetryFeatureSource
//                                (measured or configured values with their
//                                provenance), or -- only when explicitly
//                                allowed -- NetworkState substitutes, which are
//                                host-wide and semantically different
//   dynamics (3, V0.7.4)         NetworkState forwarding observations
//                                (fwd.topology.*, fwd.runtime.reroute_count),
//                                accepted only when Valid; see
//                                runtime/model/feature_acquisition.h
//   the other 20 (V0.7.4)        no AAFR measurement has the training
//                                meaning: they stay missing (status
//                                Unsupported or Incompatible); related
//                                forwarding observations are reported as
//                                estimates only
//
// Rules: a value is accepted only when it is finite, in the contract unit,
// physically valid and fresh. A missing, stale or invalid required value makes
// the vector incomplete; the provider then abstains (it never sends a default,
// zero or NaN in place of an unknown value). Unknown feature names offered by
// a source are rejected. The adapter does not compute the 20 engineered model
// columns: the model project's own features.py does that inside the worker.

#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "network/network_state/network_state.h"
#include "routing/topology/forwarding_graph.h"
#include "routing/topology/forwarding_problem.h"
#include "runtime/model/model_contract.h"

namespace aafr::model {

enum class FeatureProvenance {
  GraphDerived,  // Computed from the controlled forwarding graph/problem.
  Measured,      // Observed (supplied by a telemetry source as measured).
  Configured,    // Configured or simulated condition of the controlled environment.
  Substitute,    // NetworkState host-wide value standing in for a model feature.
  Missing,
  // V0.7.4: computed from validated NetworkState forwarding observations
  // with the documented formula (category B of the acquisition contract).
  Derived,
  // V0.7.4: a research estimate (category C); never accepted as a feature.
  Estimated,
};
const char* ToString(FeatureProvenance value);

// V0.7.4 per-feature status against the model contract.
enum class FeatureStatus {
  Valid,
  Missing,              // No source offered a value.
  Stale,
  InsufficientSamples,
  InvalidUnit,
  OutOfRange,           // Outside the physically valid range of the contract.
  Invalid,              // Non-finite, fractional integer, duplicate or conflicting.
  Unsupported,          // Category D: AAFR has no measurement source.
  Incompatible,         // Category C or E: no measured equivalent with the training meaning.
};
const char* ToString(FeatureStatus value);

struct ModelFeatureValue {
  std::string name;
  std::string unit;
  std::optional<double> value;  // Set only when accepted.
  FeatureProvenance provenance{FeatureProvenance::Missing};
  std::string source;  // e.g. "forwarding_graph:loopback@v3", "network_state:rtt.handshake.mean_ms".
  std::optional<double> age_ms;
  std::string issue;   // Why the value was not accepted; empty when accepted.
  FeatureStatus status{FeatureStatus::Missing};  // V0.7.4.
  std::string detail;  // V0.7.4: why a missing feature has no AAFR source (category D/C/E).
  char category{'?'};  // V0.7.4 acquisition category 'A'..'E'.
};

struct ModelFeatureVector {
  std::string contract_version{kFeatureContractVersion};
  std::vector<ModelFeatureValue> features;  // Contract order, all 35.
  std::vector<std::string> missing;         // Required but no value.
  std::vector<std::string> invalid;         // Offered but rejected (unit, range, non-finite, unknown name).
  std::vector<std::string> stale;           // Offered but too old.
  std::vector<std::string> substituted;     // Accepted NetworkState substitutes.
  // V0.7.4
  std::vector<std::string> insufficient;    // Offered but with too few samples / too short a window.
  std::vector<std::string> derived;         // Accepted from NetworkState forwarding observations.
  // Category C research estimates from NetworkState (never part of the
  // model input, never counted as accepted).
  std::vector<ModelFeatureValue> estimates;
  std::uint64_t network_state_id{0};
  std::uint64_t forwarding_version{0};      // NetworkState::forwarding->version, 0 when absent.
  double build_ms{0.0};

  bool Complete() const { return missing.empty() && invalid.empty() && stale.empty() && insufficient.empty(); }
  // Features with the given status (all 35 are counted exactly once).
  int CountStatus(FeatureStatus status) const;
  std::string Summary() const;  // e.g. "graph=12 configured=23 substitute=0 missing=0".
  // name -> value for accepted features (only meaningful when Complete()).
  std::map<std::string, double> Values() const;
};

// A value a telemetry source offers for one model feature.
struct TelemetryFeatureInput {
  std::string name;
  double value{0.0};
  std::string unit;
  FeatureProvenance provenance{FeatureProvenance::Configured};  // Measured or Configured.
  std::string source;
  std::optional<double> age_ms;  // nullopt: age unknown (accepted only for Configured values).
};

class ITelemetryFeatureSource {
 public:
  virtual ~ITelemetryFeatureSource() = default;
  virtual std::string Name() const = 0;
  virtual std::vector<TelemetryFeatureInput> Features() const = 0;
};

// Values configured for a controlled scenario (provenance Configured). The
// set is replaced atomically; thread-safe.
class ConfiguredTelemetrySource final : public ITelemetryFeatureSource {
 public:
  explicit ConfiguredTelemetrySource(std::string name = "configured_profile") : name_(std::move(name)) {}
  std::string Name() const override { return name_; }
  std::vector<TelemetryFeatureInput> Features() const override;
  // Values in the contract unit; provenance Configured.
  void SetProfile(const std::map<std::string, double>& values, const std::string& profile_name);
  void SetInputs(std::vector<TelemetryFeatureInput> inputs);
  void Clear();

 private:
  std::string name_;
  mutable std::mutex mutex_;
  std::vector<TelemetryFeatureInput> inputs_;
};

struct FeatureAdapterConfig {
  // Use host-wide NetworkState values for latency/jitter/throughput when the
  // telemetry source has none. Off by default: they measure something else.
  bool allow_network_state_substitutes{false};
  // Measured and substitute values older than this are stale.
  double max_feature_age_ms{30'000.0};
  // V0.7.4: accept category-B features (dynamics) from validated
  // NetworkState forwarding observations when no telemetry source has them.
  bool use_forwarding_observations{true};
};

// The 12 graph/request features with the training formulas.
struct GraphFeatureResult {
  std::map<std::string, double> values;
  std::vector<std::string> issues;  // e.g. destination unreachable.
};
GraphFeatureResult ComputeGraphFeatures(const forwarding::ForwardingGraph& graph,
                                        const forwarding::ForwardingProblem& problem);

class ModelFeatureAdapter {
 public:
  explicit ModelFeatureAdapter(FeatureAdapterConfig config = {}) : config_(config) {}
  ModelFeatureVector Build(const forwarding::ForwardingGraph& graph, const forwarding::ForwardingProblem& problem,
                           const netstate::NetworkState* state, const ITelemetryFeatureSource* telemetry) const;
  const FeatureAdapterConfig& config() const { return config_; }

 private:
  FeatureAdapterConfig config_;
};

// Symmetric eigenvalues (ascending) by cyclic Jacobi rotation. For the
// Laplacian's algebraic connectivity; exposed for tests.
std::vector<double> SymmetricEigenvalues(std::vector<std::vector<double>> matrix);

}  // namespace aafr::model
