#include "algorithms/common/metaheuristic_support.h"

#include <algorithm>
#include <cmath>

namespace aafr::algorithms::detail {

double SearchState::Score(const std::vector<int>& path) {
  if (!tracker.CanEvaluate()) return std::numeric_limits<double>::infinity();
  tracker.CountEvaluation();
  const double value = fitness.Fitness(path);
  if (value < best_fitness) {
    best_fitness = value;
    best_path = path;
  }
  return value;
}

double SearchState::ScoreKeys(const Keys& keys, std::vector<int>* decoded) {
  std::vector<int> path = DecodePriorities(view, keys);
  const double value = Score(path);
  if (decoded) *decoded = std::move(path);
  return value;
}

Keys SearchState::RandomKeys() {
  Keys keys(view.node_ids.size());
  for (double& key : keys) key = rng.Uniform();
  return keys;
}

void SearchState::Clamp(Keys& keys) {
  for (double& key : keys) key = std::isfinite(key) ? std::min(1.0, std::max(0.0, key)) : 0.5;
}

void SearchState::Finish(AlgorithmResult& result, std::uint64_t memory_bytes) {
  result.iterations = tracker.iterations();
  result.evaluations = tracker.evaluations();
  result.termination = tracker.reason();
  result.accounted_memory_bytes = memory_bytes;
  result.optimality_claimed = false;
  result.optimality_note = kNoOptimality;
  if (result.termination == Termination::Cancelled) {
    result.status = AlgorithmStatus::Cancelled;
    result.message = "cancelled; the best candidate so far was discarded";
    return;
  }
  if (best_path.empty() || !std::isfinite(best_fitness)) {
    result.status = AlgorithmStatus::NoFeasiblePath;
    result.message = "no candidate path was found within the budget";
    return;
  }
  result.path = ToNames(view, best_path);
  result.status = AlgorithmStatus::PathFound;
}

bool StartSearch(const forwarding::ProblemView& view, AlgorithmResult& result) {
  if (Reachable(view)) return true;
  result.status = AlgorithmStatus::NoPath;
  result.termination = Termination::Completed;
  result.message = "the destination is unreachable over usable edges";
  result.optimality_note = kNoOptimality;
  return false;
}

}  // namespace aafr::algorithms::detail
