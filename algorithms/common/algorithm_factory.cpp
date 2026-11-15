#include "algorithms/common/algorithm_makers.h"

namespace aafr::algorithms {

std::unique_ptr<IPathAlgorithm> CreateAlgorithm(AlgorithmId id) {
  switch (id) {
    case AlgorithmId::Dijkstra: return detail::MakeDijkstra();
    case AlgorithmId::AStar: return detail::MakeAStar();
    case AlgorithmId::BellmanFord: return detail::MakeBellmanFord();
    case AlgorithmId::AntColonyOptimization: return detail::MakeAntColony();
    case AlgorithmId::ParticleSwarmOptimization: return detail::MakeParticleSwarm();
    case AlgorithmId::GeneticAlgorithm: return detail::MakeGenetic();
    case AlgorithmId::ArtificialBeeColony: return detail::MakeBeeColony();
    case AlgorithmId::GreyWolfOptimizer: return detail::MakeGreyWolf();
    case AlgorithmId::FireflyAlgorithm: return detail::MakeFirefly();
    case AlgorithmId::WhaleOptimizationAlgorithm: return detail::MakeWhale();
  }
  return nullptr;
}

namespace detail {

Termination TimeCheck(const AlgorithmRequest& request, std::chrono::steady_clock::time_point start) {
  if (request.cancel && request.cancel->load(std::memory_order_relaxed)) return Termination::Cancelled;
  if (request.budget.deadline_ms &&
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() >=
          *request.budget.deadline_ms) {
    return Termination::Deadline;
  }
  return Termination::Completed;
}

}  // namespace detail
}  // namespace aafr::algorithms
