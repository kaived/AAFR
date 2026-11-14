#pragma once

// Identifiers of the ten algorithms AAFR plans to support. Plain C++17.
// An id names a planned algorithm; it says nothing about whether the
// algorithm is implemented (see ImplementationAvailability).

#include <array>
#include <optional>
#include <string>

namespace aafr::algorithms {

enum class AlgorithmId {
  Dijkstra,
  AStar,
  BellmanFord,
  AntColonyOptimization,
  ParticleSwarmOptimization,
  GeneticAlgorithm,
  ArtificialBeeColony,
  GreyWolfOptimizer,
  FireflyAlgorithm,
  WhaleOptimizationAlgorithm,
};

inline constexpr std::array<AlgorithmId, 10> kAllAlgorithmIds = {
    AlgorithmId::Dijkstra,
    AlgorithmId::AStar,
    AlgorithmId::BellmanFord,
    AlgorithmId::AntColonyOptimization,
    AlgorithmId::ParticleSwarmOptimization,
    AlgorithmId::GeneticAlgorithm,
    AlgorithmId::ArtificialBeeColony,
    AlgorithmId::GreyWolfOptimizer,
    AlgorithmId::FireflyAlgorithm,
    AlgorithmId::WhaleOptimizationAlgorithm,
};

// Stable machine-readable id, e.g. "dijkstra", "aco". Matches the folder
// names under algorithms/.
const char* ToString(AlgorithmId id);
// nullopt for text that names none of the ten algorithms.
std::optional<AlgorithmId> ParseAlgorithmId(const std::string& text);

}  // namespace aafr::algorithms
