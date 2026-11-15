#pragma once

// Internal support shared by the seven metaheuristics: budget-aware candidate
// scoring, best-so-far tracking and result reporting. The search operators
// themselves live in each algorithm's own file.

#include <limits>
#include <vector>

#include "algorithms/common/algorithm_makers.h"

namespace aafr::algorithms::detail {

using Keys = std::vector<double>;

class SearchState {
 public:
  SearchState(const AlgorithmRequest& request, const forwarding::ProblemView& view)
      : request(request), view(view), tracker(request), fitness(view, *request.problem), rng(request.seed) {}

  // Scores one candidate path, counting an objective evaluation. Returns
  // +infinity without evaluating once the evaluation budget is spent.
  double Score(const std::vector<int>& path);
  // Decodes random keys (DecodePriorities) and scores the path.
  double ScoreKeys(const Keys& keys, std::vector<int>* decoded = nullptr);
  Keys RandomKeys();
  static void Clamp(Keys& keys);

  // Fills path, status, counters and termination. Cancellation discards the
  // candidate so nothing half-computed is executed.
  void Finish(AlgorithmResult& result, std::uint64_t memory_bytes);

  const AlgorithmRequest& request;
  const forwarding::ProblemView& view;
  BudgetTracker tracker;
  FitnessEvaluator fitness;
  Rng rng;
  std::vector<int> best_path;
  double best_fitness{std::numeric_limits<double>::infinity()};
};

// Shared preamble: reports NoPath when the destination is unreachable over
// usable edges. Returns true when the search should go ahead.
bool StartSearch(const forwarding::ProblemView& view, AlgorithmResult& result);

inline const char* kNoOptimality = "no optimality guarantee: best candidate found within the budget";

}  // namespace aafr::algorithms::detail
