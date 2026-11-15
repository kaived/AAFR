#pragma once

// Verification of the ten path-algorithm implementations, and its effect on
// the V0.4 capability registry. Plain C++17.
//
// A class existing is not enough for an algorithm to count as executable.
// Each implementation is run on fixed fixtures and must:
//  - return a structurally valid, feasible path on every reachable fixture,
//    with the score the shared validator computes;
//  - report NoPath on an unreachable fixture;
//  - stay within its iteration budget and be reproducible for a fixed seed;
//  - exact algorithms: match the brute-force optimum (all simple paths
//    enumerated), and Dijkstra/A* refuse negative costs while Bellman-Ford
//    reports a negative cycle;
//  - metaheuristics: never beat the brute-force optimum (a sanity check of
//    the shared objective) and reach it on the 4-node demo graph.
// Forwarding integration is verified separately by executing each
// algorithm's demo-graph path through a forwarding executor and requiring a
// confirmed path.
//
// Registry effect: an implementation that passes becomes
// ImplementationPresent; it becomes VerifiedExecutable only when forwarding
// integration was verified too, with the measured worst runtime, peak
// accounted memory, largest fixture size and a verification note.

#include <cstdint>
#include <string>
#include <vector>

#include "algorithms/capability_registry.h"
#include "algorithms/common/path_algorithm.h"
#include "routing/forwarding/forwarding_executor.h"

namespace aafr::algorithms {

struct AlgorithmVerification {
  AlgorithmId algorithm{AlgorithmId::Dijkstra};
  std::string version;
  bool implementation_passed{false};
  bool forwarding_passed{false};
  std::vector<std::string> passed_checks;
  std::vector<std::string> failures;
  double worst_runtime_ms{0.0};
  std::uint64_t peak_accounted_memory_bytes{0};
  std::uint64_t max_nodes{0};
  std::uint64_t max_edges{0};
};

struct VerificationReport {
  std::vector<AlgorithmVerification> algorithms;
  std::string forwarding_executor;  // Capability().executor, or empty.
  bool forwarding_real_network{false};
  double total_ms{0.0};

  const AlgorithmVerification* Find(AlgorithmId id) const;
  std::size_t CountImplementationsPassed() const;
  std::size_t CountVerifiedExecutable() const;
};

// `executor` may be null (forwarding is then not verified). When given, it
// must forward over forwarding::LoopbackDemoGraph() as it is built by that
// function. `cancel` aborts between algorithms.
VerificationReport VerifyImplementations(forwarding::IForwardingExecutor* executor,
                                         const std::atomic<bool>* cancel = nullptr);

// Copies `registry` with availability and implementation facts updated from
// the report. Algorithms that fail stay ImplementationUnavailable, with the
// failures recorded in the profile's limitations.
CapabilityRegistry ApplyVerification(const CapabilityRegistry& registry, const VerificationReport& report);

}  // namespace aafr::algorithms
