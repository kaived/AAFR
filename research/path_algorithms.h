#pragma once

// The four-algorithm pool of the research method, on its own graph model.
// Plain C++17, pure functions, no I/O. Not used by the live runtime, whose
// algorithms (algorithms/) are unchanged.
//
// Each algorithm optimizes its own cost; none of them uses the independent
// quality score J, which only evaluates the paths they return.
//   Dijkstra  additive sum of the link delays measured at start-up (a frozen
//             snapshot, so it models a static, load-unaware baseline)
//   A*        additive sum of the predicted link weights W_link, with
//             heuristic h(n) = hops(n -> t) * min W_link (hops by reverse BFS
//             over available links)
//   ACO, GA   minimize the non-additive engine cost J_eng(P) over simple
//             paths (24 ants x 30 iterations; population 30 x 40 generations).
//             Their search operators are not defined by the specification, so
//             they are declared here and report NotImplemented.
//
// A cost that is missing on an available link is never defaulted: the
// algorithm refuses with MissingCost and names the link. Unavailable links
// are excluded. Ties between equal-cost paths are broken by node order
// (smaller index first), exactly as in the production implementations; the
// specification does not define tie-breaking.
//
// Different algorithms may return the same path; DistinctChallengers turns
// candidates into one arm per distinct path, labelled with every algorithm
// that returned it, and leaves out the installed path.

#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include "research/normalization.h"
#include "research/path_metrics.h"

namespace aafr::research {

struct ResearchLink {
  LinkObservation observation;             // Live measurements; from / to name the link.
  std::optional<double> startup_delay_ms;  // Snapshot taken at engine start (Dijkstra's cost).
  std::optional<double> predicted_weight;  // W_link from the link-weight regressor (A*'s cost), >= 0.
  bool available{true};
};

struct ResearchGraph {
  std::vector<std::string> nodes;
  std::vector<ResearchLink> links;  // Directed.
};

// Problems; empty when well formed (unique non-empty nodes, links between
// known distinct nodes, at most one link per direction).
std::vector<std::string> ValidateGraph(const ResearchGraph& graph);

enum class PoolAlgorithm { Dijkstra, AStar, AntColony, Genetic };
inline constexpr std::array<PoolAlgorithm, 4> kPool = {PoolAlgorithm::Dijkstra, PoolAlgorithm::AStar,
                                                       PoolAlgorithm::AntColony, PoolAlgorithm::Genetic};

// Specified iteration budgets; the search operators are still open.
struct AntColonyParameters {
  std::size_t ants{24};
  std::size_t iterations{30};
};
struct GeneticParameters {
  std::size_t population{30};
  std::size_t generations{40};
};

enum class CostKind {
  StartupDelayMs,   // Dijkstra.
  PredictedWeight,  // A*.
  EngineCost,       // ACO, GA (J_eng).
};

enum class CandidateStatus {
  PathFound,
  NoPath,          // The destination is unreachable over available links.
  MissingCost,     // An available link has no value for the algorithm's cost.
  InvalidCost,     // A cost is negative or not finite.
  InvalidInput,    // Malformed graph or unknown endpoints.
  NotImplemented,  // ACO / GA: operators not defined by the specification.
};

struct Candidate {
  PoolAlgorithm algorithm{PoolAlgorithm::Dijkstra};
  CandidateStatus status{CandidateStatus::InvalidInput};
  std::vector<std::string> path;
  CostKind cost_kind{CostKind::StartupDelayMs};
  std::optional<double> optimization_cost;  // In the algorithm's own cost; never J.
  std::size_t expansions{0};
  std::string message;
};

Candidate StaticDelayDijkstra(const ResearchGraph& graph, const std::string& source, const std::string& destination);
Candidate PredictedWeightAStar(const ResearchGraph& graph, const std::string& source, const std::string& destination);
// NotImplemented until the operators are decided.
Candidate AntColonyOptimization(const ResearchGraph& graph, const std::string& source,
                                const std::string& destination, const NormalizationStatistics& statistics,
                                const AntColonyParameters& parameters = {});
Candidate GeneticAlgorithm(const ResearchGraph& graph, const std::string& source, const std::string& destination,
                           const NormalizationStatistics& statistics, const GeneticParameters& parameters = {});

// Runs every pool algorithm in kPool order.
std::vector<Candidate> RunPool(const ResearchGraph& graph, const std::string& source, const std::string& destination,
                               const NormalizationStatistics& statistics);

// Live link observations along `path`, or nullopt when a hop is not an
// available link of the graph.
std::optional<std::vector<LinkObservation>> PathLinks(const ResearchGraph& graph, const std::vector<std::string>& path);

// Independent evaluation of a path on live observations, separate from any
// algorithm's optimization cost.
struct PathEvaluation {
  bool on_graph{false};
  PathMetrics metrics;
  Score quality;      // J
  Score engine_cost;  // J_eng
};
PathEvaluation EvaluatePath(const ResearchGraph& graph, const std::vector<std::string>& path,
                            const NormalizationStatistics& statistics);

struct Arm {
  std::vector<std::string> path;
  std::vector<PoolAlgorithm> algorithms;  // Every algorithm that returned this path, in pool order.
};

struct CandidateArms {
  std::vector<Arm> challengers;                       // Distinct paths other than the installed one.
  std::vector<PoolAlgorithm> installed_algorithms;    // Algorithms that returned the installed path.
};

CandidateArms DistinctChallengers(const std::vector<Candidate>& candidates, const std::vector<std::string>& installed_path);

const char* ToString(PoolAlgorithm algorithm);
const char* ToString(CandidateStatus status);
const char* ToString(CostKind kind);

}  // namespace aafr::research
