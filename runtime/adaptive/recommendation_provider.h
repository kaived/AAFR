#pragma once

// The recommendation contract (replaceable; V0.7 may add a model-backed
// provider) and the V0.6 deterministic provider. Plain C++17.
//
// A provider recommends. It never activates forwarding, never reads the
// clock for gating and never changes runtime state; the runtime decides what
// to do with the recommendation.

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "algorithms/algorithm_id.h"
#include "experiments/benchmark/benchmark_runner.h"
#include "network/network_state/network_constraints.h"
#include "network/network_state/network_state.h"
#include "routing/topology/forwarding_graph.h"
#include "routing/topology/forwarding_problem.h"
#include "runtime/adaptive/runtime_types.h"

namespace aafr::runtime {

// Offline evidence about an algorithm's reliability on the V0.5 benchmark
// catalog (simulated graphs). It describes how often the algorithm found a
// feasible, near-optimal path; it is not a prediction for this network.
struct AlgorithmBenchmarkEvidence {
  std::size_t runs{0};
  std::size_t feasible{0};
  std::size_t at_reference{0};  // Runs whose objective matched the exact reference.
  double mean_gap_percent{0.0};
  double median_computation_ms{0.0};

  double FeasibleRate() const { return runs ? static_cast<double>(feasible) / runs : 0.0; }
};

struct BenchmarkEvidence {
  std::string source;  // e.g. "aafr-benchmark catalog, seeds 1..5".
  int objective_version{forwarding::ForwardingObjective::kVersion};
  std::map<algorithms::AlgorithmId, AlgorithmBenchmarkEvidence> algorithms;
};

// Aggregates a V0.5 benchmark report per algorithm.
BenchmarkEvidence EvidenceFromBenchmark(const benchmark::BenchmarkReport& report, const std::string& source);

enum class EvidenceQuality {
  None,           // Nothing supports the recommendation.
  EstimateOnly,   // Objective computed on the forwarding graph (configured/simulated values).
  WithBenchmark,  // Estimate plus offline benchmark reliability evidence.
};

struct EvaluationContext {
  std::string evaluation_id;
  EvaluationTrigger trigger{EvaluationTrigger::Manual};
  std::uint64_t network_state_id{0};
  std::uint64_t graph_version{0};
};

struct RecommendationInput {
  std::shared_ptr<const netstate::NetworkState> state;  // May be null.
  std::shared_ptr<const netstate::NetworkConstraints> constraints;  // May be null.
  std::shared_ptr<const forwarding::ForwardingGraph> graph;
  forwarding::ForwardingProblem problem;
  // One entry per algorithm, eligibility and computed path included.
  std::vector<CandidateEvaluation> candidates;
  std::optional<algorithms::AlgorithmId> active;
  std::optional<forwarding::Path> active_path;
  const BenchmarkEvidence* benchmark{nullptr};  // May be null.
  EvaluationContext context;
};

struct RankedCandidate {
  algorithms::AlgorithmId algorithm{algorithms::AlgorithmId::Dijkstra};
  forwarding::Path path;
  std::optional<double> objective;
  double computation_ms{0.0};
  std::string note;
};

enum class RecommendationStatus {
  Recommended,           // `recommended` is set.
  InsufficientEvidence,  // Nothing usable to compare; retain the active algorithm.
  Unavailable,           // No eligible algorithm produced a usable path.
  Error,                 // Invalid input.
};

struct Recommendation {
  RecommendationStatus status{RecommendationStatus::Error};
  std::optional<algorithms::AlgorithmId> recommended;
  std::vector<RankedCandidate> ranking;  // Best first.
  std::string provider_id;
  std::string source;  // What the ranking is computed from.
  std::optional<double> estimated_objective;
  // Active objective minus recommended objective, in J(P) units, on the same
  // graph version and objective version. Not a probability, not a measurement.
  std::optional<double> estimated_improvement;
  std::optional<double> estimated_improvement_relative;
  EvidenceQuality evidence{EvidenceQuality::None};
  std::string explanation;
  std::string error;
};

class IRecommendationProvider {
 public:
  virtual ~IRecommendationProvider() = default;
  virtual std::string Id() const = 0;
  virtual std::string Version() const = 0;
  // Deterministic for the same input; never throws.
  virtual Recommendation Recommend(const RecommendationInput& input) const = 0;
};

struct DeterministicProviderConfig {
  // With benchmark evidence, an algorithm whose feasible rate is below this
  // is ranked after every algorithm that meets it (never removed).
  double min_benchmark_feasible_rate{0.9};
  // Objectives within this absolute distance count as tied; ties prefer a
  // candidate on the active path, then AlgorithmId order, then the lower
  // computation time.
  double tie_tolerance{1e-9};
};

// Ranks usable candidates by the shared V0.5 objective J(P) on the current
// graph. Deterministic, explicit rules only; see the .cpp for the order.
class DeterministicRecommendationProvider final : public IRecommendationProvider {
 public:
  explicit DeterministicRecommendationProvider(DeterministicProviderConfig config = {}) : config_(config) {}
  std::string Id() const override { return "deterministic_objective"; }
  std::string Version() const override { return "1"; }
  Recommendation Recommend(const RecommendationInput& input) const override;

 private:
  DeterministicProviderConfig config_;
};

const char* ToString(RecommendationStatus value);
const char* ToString(EvidenceQuality value);

}  // namespace aafr::runtime
