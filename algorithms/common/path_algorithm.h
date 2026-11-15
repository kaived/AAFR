#pragma once

// The common execution contract for the ten path-selection algorithms.
// Plain C++17.
//
//   algorithm computation -> candidate validation -> forwarding execution
//
// An algorithm only computes. It receives the problem and graph, returns a
// candidate path with its own account of how it got there, and touches no
// browser or forwarding state. The caller validates the candidate with
// forwarding::ValidatePath before anything is executed.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "algorithms/algorithm_id.h"
#include "routing/topology/forwarding_graph.h"
#include "routing/topology/forwarding_problem.h"

namespace aafr::algorithms {

// Parameter name -> value. Unknown names are rejected; missing ones take the
// algorithm's documented default.
using AlgorithmParameters = std::map<std::string, std::string>;

struct ExecutionBudget {
  // Iterations (generations, colony cycles, ...) for metaheuristics.
  std::uint64_t max_iterations{200};
  // Objective evaluations across the whole run; nullopt: unlimited.
  std::optional<std::uint64_t> max_evaluations;
  // Wall-clock limit for the computation; nullopt: none.
  std::optional<double> deadline_ms;
};

struct AlgorithmRequest {
  const forwarding::ForwardingGraph* graph{nullptr};
  const forwarding::ForwardingProblem* problem{nullptr};
  ExecutionBudget budget;
  AlgorithmParameters parameters;
  std::uint64_t seed{1};
  // Polled between iterations; true stops the run.
  const std::atomic<bool>* cancel{nullptr};
};

enum class AlgorithmStatus {
  PathFound,        // A path was produced (see feasible for constraints).
  NoPath,           // No path exists among the usable edges.
  NoFeasiblePath,   // Paths exist, but the algorithm found none that meets the hard constraints.
  NegativeCycle,    // Bellman-Ford: a reachable negative cycle makes costs undefined.
  InvalidInput,     // The problem, graph or configuration was rejected.
  Cancelled,
  Error,
};

enum class Termination {
  Completed,         // Exact algorithm finished, or search space exhausted.
  IterationBudget,
  EvaluationBudget,
  Deadline,
  Cancelled,
  Error,
};

struct AlgorithmResult {
  AlgorithmId algorithm{AlgorithmId::Dijkstra};
  std::string algorithm_version;
  AlgorithmStatus status{AlgorithmStatus::Error};
  forwarding::Path path;
  bool feasible{false};
  std::optional<double> objective;  // J(P) of `path` from the shared evaluator.
  double duration_ms{0.0};
  std::uint64_t iterations{0};
  std::uint64_t evaluations{0};
  Termination termination{Termination::Error};
  std::string message;
  // Whether the result is claimed optimal, and why or why not.
  bool optimality_claimed{false};
  std::string optimality_note;
  // Reproducibility.
  std::uint64_t seed{0};
  AlgorithmParameters parameters;  // Effective parameters, defaults included.
  std::uint64_t graph_version{0};
  // Bytes held by the algorithm's own containers at their peak. An accounted
  // figure, not process memory.
  std::uint64_t accounted_memory_bytes{0};
};

class IPathAlgorithm {
 public:
  virtual ~IPathAlgorithm() = default;
  virtual AlgorithmId id() const = 0;
  virtual std::string version() const = 0;
  virtual bool deterministic() const = 0;
  // How a candidate solution is represented, e.g. "random_key_priority".
  virtual std::string encoding() const = 0;
  virtual AlgorithmParameters DefaultParameters() const = 0;
  // Problems with the parameters after defaults are applied.
  virtual std::vector<std::string> ValidateParameters(const AlgorithmParameters& parameters) const = 0;
  // Runs to completion or budget. Never throws.
  AlgorithmResult Run(const AlgorithmRequest& request) const;

 protected:
  // Called by Run() with validated inputs and effective parameters.
  virtual void Solve(const AlgorithmRequest& request, const forwarding::ProblemView& view,
                     const AlgorithmParameters& parameters, AlgorithmResult& result) const = 0;
};

// One instance of each algorithm; nullptr for an unknown id.
std::unique_ptr<IPathAlgorithm> CreateAlgorithm(AlgorithmId id);

const char* ToString(AlgorithmStatus value);
const char* ToString(Termination value);

// ---- Helpers shared by the implementations ----

// Portable deterministic generator: identical sequences on every platform
// and standard library for the same seed (unlike std distributions).
class Rng {
 public:
  explicit Rng(std::uint64_t seed);
  std::uint64_t Next();
  double Uniform();                         // [0, 1)
  double Uniform(double low, double high);  // [low, high)
  std::size_t Index(std::size_t size);      // [0, size)
  double Normal();                          // Standard normal (Box-Muller).

 private:
  std::uint64_t state_[4];
};

// Tracks the budget and cancellation for one run.
class BudgetTracker {
 public:
  BudgetTracker(const AlgorithmRequest& request);
  // True when the run must stop now; sets the reason.
  bool Exhausted();
  bool CanEvaluate();
  void CountEvaluation() { ++evaluations_; }
  void CountIteration() { ++iterations_; }
  std::uint64_t iterations() const { return iterations_; }
  std::uint64_t evaluations() const { return evaluations_; }
  Termination reason() const { return reason_; }

 private:
  const AlgorithmRequest& request_;
  std::chrono::steady_clock::time_point start_;
  std::uint64_t iterations_{0};
  std::uint64_t evaluations_{0};
  Termination reason_{Termination::Completed};
};

// Scores simple index paths against the problem with one shared rule set.
// Used by every metaheuristic for fitness: the true objective plus a penalty
// proportional to path-level constraint violation. Reported scores always
// come from forwarding::ValidatePath, never from the penalized fitness.
class FitnessEvaluator {
 public:
  FitnessEvaluator(const forwarding::ProblemView& view, const forwarding::ForwardingProblem& problem);
  // Cost of a decoded index path; infinity when it is not a usable path.
  double Fitness(const std::vector<int>& path) const;
  bool Feasible(const std::vector<int>& path) const;
  double Objective(const std::vector<int>& path) const;

 private:
  double Violation(const std::vector<int>& path) const;
  const forwarding::ProblemView& view_;
  const forwarding::ForwardingProblem& problem_;
  double penalty_scale_;
};

// Random-key (priority) decoding: the discrete adaptation used by the
// continuous metaheuristics (PSO, ABC, GWO, Firefly, WOA). Each node has a
// real-valued key; the path is built by a depth-first walk from the source
// that always tries the unvisited neighbour with the highest key first and
// backtracks out of dead ends. It always yields a simple path when the
// destination is reachable, and the same keys always yield the same path.
std::vector<int> DecodePriorities(const forwarding::ProblemView& view, const std::vector<double>& keys);

// A random simple path from source to destination by randomized DFS; empty
// when the destination is unreachable. Used for initialization and mutation.
// `blocked` marks nodes the walk must not enter (for example the prefix a
// mutation keeps).
std::vector<int> RandomPath(const forwarding::ProblemView& view, Rng& rng, int from = -1,
                            const std::vector<char>* blocked = nullptr);

// Whether the destination is reachable from the source at all.
bool Reachable(const forwarding::ProblemView& view);

forwarding::Path ToNames(const forwarding::ProblemView& view, const std::vector<int>& path);

// Parameter parsing with validation messages.
double ParamDouble(const AlgorithmParameters& parameters, const std::string& name);
std::uint64_t ParamUInt(const AlgorithmParameters& parameters, const std::string& name);
void RequireRange(const AlgorithmParameters& parameters, const std::string& name, double low, double high,
                  std::vector<std::string>& issues, bool integer = false);

}  // namespace aafr::algorithms
