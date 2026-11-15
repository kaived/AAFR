#pragma once

#include <limits>

#include "algorithms/path_search.h"

namespace aafr::forwarding::detail {

inline constexpr double kInfinity = std::numeric_limits<double>::infinity();
using Route = std::vector<int>;

struct Arc {
  int to;
  double cost;
  double latency;
  std::size_t index;
};

struct SearchContext {
  using Clock = std::chrono::steady_clock;
  const ForwardingProblem& problem;
  const SearchConfig& config;
  const std::atomic<bool>& cancelled;
  SearchResult& result;
  Clock::time_point stop;
  std::vector<std::vector<Arc>> outgoing;
  int source;
  int destination;
  std::size_t edge_count{0};
  bool negative{false};

  bool Step();
  ForwardingPath Path(const Route& route) const;
  double Score(const Route& route) const;
};

void Metaheuristic(SearchContext& context);

}  // namespace aafr::forwarding::detail
