#pragma once

#include <atomic>
#include <chrono>
#include <map>

#include "algorithms/algorithm_id.h"
#include "routing/topology/forwarding_problem.h"

namespace aafr::forwarding {

inline constexpr int kAlgorithmVersion = 1;

enum class SearchStatus {
  Success, InvalidInput, Infeasible, NoCandidate, NegativeCycle, Deadline, Budget, Cancelled, Ineligible
};

struct SearchConfig {
  std::uint32_t seed{42};
  unsigned population{16};
  unsigned iterations{32};
  std::uint64_t work_limit{500000};
  std::size_t memory_budget_bytes{16 * 1024 * 1024};
  std::chrono::milliseconds deadline{500};
  // Optional estimate by node. Accepted only if finite, nonnegative,
  // zero at the destination and consistent on every enabled edge.
  std::map<std::string, double> heuristic;
};

struct SearchResult {
  algorithms::AlgorithmId algorithm{};
  SearchStatus status{SearchStatus::InvalidInput};
  ForwardingPath path;
  std::optional<double> score;
  double computation_ms{0};
  unsigned iterations{0};
  std::uint64_t work{0};
  std::uint32_t seed{0};
  bool optimal{false};
  bool heuristic_used{false};
  std::string reason;
};

std::string ValidateSearchConfig(const SearchConfig&);
bool IsMetaheuristic(algorithms::AlgorithmId);
SearchResult Search(algorithms::AlgorithmId, const ForwardingProblem&, const SearchConfig&,
                    const std::atomic<bool>& cancelled);
const char* ToString(SearchStatus);

}  // namespace aafr::forwarding
