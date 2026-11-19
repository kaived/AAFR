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
//   ACO, GA   minimize the non-additive engine cost J_eng(P) of whole simple
//             paths (24 ants x 30 iterations; population 30 x 40
//             generations). J_eng is never split into edge weights: every
//             candidate is a complete path scored with EngineCost().
//
// ACO and GA reuse the production search operators (algorithms/aco,
// algorithms/ga, version 1.0.0) where they do not depend on an additive edge
// cost, and the production helpers RandomPath / Reachable / Rng unchanged.
// The specification fixes only the objective and the budgets; every other
// operator choice is an AAFR implementation choice, listed here:
//   ACO  ants walk from the source choosing an unvisited neighbour with
//        probability proportional to tau(e)^alpha * eta(e)^beta; a dead end
//        fails the ant. eta(e) = 1 for every edge (J_eng has no edge-level
//        term, so no heuristic information is added; beta has no effect).
//        tau starts at 1; each iteration tau <- (1 - rho) tau, then every
//        scored ant deposits on its edges, then the best-so-far path deposits
//        once more, then tau is clamped to [0.01, 10]. alpha = 1, rho = 0.3
//        (production defaults). Deposit for a path P:
//          1 / (1 + J_eng(P) - J_best)
//        with J_best the lowest J_eng seen so far in the run. It is positive
//        for any sign of J_eng, equals 1 for the best path and decreases
//        strictly with J_eng, so it preserves the ranking of the objective.
//   GA   chromosomes are simple node-sequence paths; initial population from
//        randomized DFS (RandomPath); tournament selection of 3 (comparisons
//        only, so negative J_eng keeps its ranking); common-node crossover
//        with rate 0.9 followed by loop removal; regrow mutation with rate
//        0.2 (keep a prefix, regrow the tail by randomized DFS that may not
//        re-enter it); elitism of 1; duplicates allowed (production
//        behaviour). Every child is validated as a simple source-destination
//        path over available links; an invalid child is rejected (counted).
//   Both: a path whose J_eng is undefined (e.g. B = 0) is rejected, never
//        defaulted; randomness comes only from Rng(seed), so a fixed seed
//        gives a fixed result; the run stops after the iteration budget.
//
// A cost or measurement that is missing on an available link is never
// defaulted: the algorithm refuses with MissingCost and names the link.
// Unavailable links are excluded. Ties between equal-cost paths are broken
// by node order (smaller index first) in Dijkstra and A*, exactly as in the
// production implementations; the specification does not define
// tie-breaking.
//
// Different algorithms may return the same path; DistinctChallengers turns
// candidates into one arm per distinct path, labelled with every algorithm
// that returned it, and leaves out the installed path.

#include <array>
#include <cstddef>
#include <cstdint>
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

// ants / iterations and population / generations are specified; the rest are
// AAFR implementation choices (production defaults, see above).
struct AntColonyParameters {
  std::size_t ants{24};
  std::size_t iterations{30};
  double alpha{1.0};
  double rho{0.3};
  std::uint64_t seed{1};
};
struct GeneticParameters {
  std::size_t population{30};
  std::size_t generations{40};
  double crossover_rate{0.9};
  double mutation_rate{0.2};
  std::size_t tournament_size{3};
  std::uint64_t seed{1};
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
  InvalidInput,    // Malformed graph, unknown endpoints, parameters or statistics.
  NoScoredPath,    // ACO / GA: no path they built has a defined J_eng.
};

struct Candidate {
  PoolAlgorithm algorithm{PoolAlgorithm::Dijkstra};
  CandidateStatus status{CandidateStatus::InvalidInput};
  std::vector<std::string> path;
  CostKind cost_kind{CostKind::StartupDelayMs};
  std::optional<double> optimization_cost;  // In the algorithm's own cost; never J.
  std::size_t expansions{0};       // Dijkstra / A*: nodes expanded.
  std::size_t iterations{0};       // ACO / GA: iterations or generations run.
  std::size_t evaluations{0};      // ACO / GA: complete paths scored.
  std::size_t rejected_paths{0};   // ACO / GA: failed ants, undefined J_eng, invalid children.
  std::uint64_t seed{0};           // ACO / GA.
  double duration_ms{0.0};
  std::string message;
};

Candidate StaticDelayDijkstra(const ResearchGraph& graph, const std::string& source, const std::string& destination);
Candidate PredictedWeightAStar(const ResearchGraph& graph, const std::string& source, const std::string& destination);
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
