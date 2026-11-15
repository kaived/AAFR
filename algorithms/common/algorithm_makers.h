#pragma once

// Internal: one factory function per algorithm, defined in each algorithm's
// own folder. Use algorithms::CreateAlgorithm instead.

#include <memory>

#include "algorithms/common/path_algorithm.h"

namespace aafr::algorithms::detail {

std::unique_ptr<IPathAlgorithm> MakeDijkstra();
std::unique_ptr<IPathAlgorithm> MakeAStar();
std::unique_ptr<IPathAlgorithm> MakeBellmanFord();
std::unique_ptr<IPathAlgorithm> MakeAntColony();
std::unique_ptr<IPathAlgorithm> MakeParticleSwarm();
std::unique_ptr<IPathAlgorithm> MakeGenetic();
std::unique_ptr<IPathAlgorithm> MakeBeeColony();
std::unique_ptr<IPathAlgorithm> MakeGreyWolf();
std::unique_ptr<IPathAlgorithm> MakeFirefly();
std::unique_ptr<IPathAlgorithm> MakeWhale();

// Checks cancellation and the deadline only; exact algorithms are not
// bounded by an iteration budget. Returns the termination to report, or
// Completed when the run may continue.
Termination TimeCheck(const AlgorithmRequest& request, std::chrono::steady_clock::time_point start);

}  // namespace aafr::algorithms::detail
