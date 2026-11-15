// A* on the problem view.
//
// Heuristics (parameter "heuristic"):
//   zero            h(n) = 0. Always admissible; A* then equals Dijkstra.
//   hop_lower_bound h(n) = hops(n -> t) * c_min, where hops is the fewest
//                   usable edges to the destination (reverse BFS) and c_min
//                   the smallest usable edge cost. Since every edge costs at
//                   least c_min, h never overestimates and is consistent:
//                   h(u) <= c_min + h(v) <= c(u, v) + h(v).
// "heuristic_weight" w multiplies h. w = 1 keeps admissibility; w > 1 is
// weighted A*, which trades optimality for fewer expansions, and the result
// then claims no optimality.
//
// Optimality is claimed only for an admissible heuristic with nonnegative
// costs. Negative costs are refused (as for Dijkstra). If c_min is not
// positive the hop bound collapses to zero, which is the safe fallback.

#include <cmath>
#include <functional>
#include <limits>
#include <queue>
#include <tuple>

#include "algorithms/common/algorithm_makers.h"

namespace aafr::algorithms::detail {
namespace {

class AStar final : public IPathAlgorithm {
 public:
  AlgorithmId id() const override { return AlgorithmId::AStar; }
  std::string version() const override { return "1.0.0"; }
  bool deterministic() const override { return true; }
  std::string encoding() const override { return "graph_search"; }
  AlgorithmParameters DefaultParameters() const override {
    return {{"heuristic", "hop_lower_bound"}, {"heuristic_weight", "1"}};
  }
  std::vector<std::string> ValidateParameters(const AlgorithmParameters& p) const override {
    std::vector<std::string> issues;
    const std::string& heuristic = p.at("heuristic");
    if (heuristic != "zero" && heuristic != "hop_lower_bound") {
      issues.push_back("parameter 'heuristic' must be 'zero' or 'hop_lower_bound'");
    }
    RequireRange(p, "heuristic_weight", 1.0, 100.0, issues);
    return issues;
  }

 protected:
  void Solve(const AlgorithmRequest& request, const forwarding::ProblemView& view, const AlgorithmParameters& p,
             AlgorithmResult& result) const override {
    const auto start = std::chrono::steady_clock::now();
    if (view.has_negative_cost) {
      result.status = AlgorithmStatus::InvalidInput;
      result.termination = Termination::Error;
      result.message = "a usable edge has a negative cost; A* requires nonnegative costs";
      return;
    }
    const std::size_t n = view.node_ids.size();
    const double inf = std::numeric_limits<double>::infinity();
    const double weight = ParamDouble(p, "heuristic_weight");
    std::string heuristic = p.at("heuristic");

    // Reverse BFS: fewest usable edges from each node to the destination.
    std::vector<std::vector<int>> reverse(n);
    for (std::size_t u = 0; u < n; ++u) {
      for (const auto& edge : view.out[u]) reverse[static_cast<std::size_t>(edge.to)].push_back(static_cast<int>(u));
    }
    std::vector<double> h(n, 0.0);
    std::vector<int> hops(n, -1);
    {
      std::queue<int> queue;
      hops[static_cast<std::size_t>(view.destination)] = 0;
      queue.push(view.destination);
      while (!queue.empty()) {
        const int v = queue.front();
        queue.pop();
        for (const int u : reverse[static_cast<std::size_t>(v)]) {
          if (hops[static_cast<std::size_t>(u)] < 0) {
            hops[static_cast<std::size_t>(u)] = hops[static_cast<std::size_t>(v)] + 1;
            queue.push(u);
          }
        }
      }
    }
    std::string fallback;
    if (heuristic == "hop_lower_bound" && !(view.min_edge_cost > 0.0)) {
      heuristic = "zero";
      fallback = " (hop_lower_bound fell back to zero: smallest edge cost is not positive)";
    }
    for (std::size_t v = 0; v < n; ++v) {
      if (hops[v] < 0) h[v] = inf;  // Cannot reach the destination: never expanded.
      else if (heuristic == "hop_lower_bound") h[v] = weight * hops[v] * view.min_edge_cost;
    }

    std::vector<double> g(n, inf);
    std::vector<int> prev(n, -1);
    using Entry = std::tuple<double, double, int>;  // f, g, node
    std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry>> open;
    std::size_t peak = 0;
    g[static_cast<std::size_t>(view.source)] = 0.0;
    if (std::isfinite(h[static_cast<std::size_t>(view.source)])) open.push({h[static_cast<std::size_t>(view.source)], 0.0, view.source});
    bool reached = false;
    while (!open.empty()) {
      if ((result.iterations & 255) == 0) {
        const Termination t = TimeCheck(request, start);
        if (t != Termination::Completed) {
          result.termination = t;
          result.status = t == Termination::Cancelled ? AlgorithmStatus::Cancelled : AlgorithmStatus::Error;
          result.message = "stopped before the search completed";
          return;
        }
      }
      peak = std::max(peak, open.size());
      const auto [f, gu, u] = open.top();
      open.pop();
      (void)f;
      if (gu > g[static_cast<std::size_t>(u)]) continue;  // Stale entry.
      ++result.iterations;
      if (u == view.destination) {
        reached = true;
        break;
      }
      for (const auto& edge : view.out[static_cast<std::size_t>(u)]) {
        ++result.evaluations;
        const auto v = static_cast<std::size_t>(edge.to);
        const double candidate = gu + edge.cost;
        if (candidate < g[v] && std::isfinite(h[v])) {
          g[v] = candidate;
          prev[v] = u;
          open.push({candidate + h[v], candidate, edge.to});
        }
      }
    }
    result.accounted_memory_bytes =
        n * (3 * sizeof(double) + 2 * sizeof(int)) + view.usable_edge_count * sizeof(int) + peak * sizeof(Entry);
    result.termination = Termination::Completed;
    if (!reached) {
      result.status = AlgorithmStatus::NoPath;
      result.message = "the destination is unreachable over usable edges";
      return;
    }
    std::vector<int> path;
    for (int v = view.destination; v != -1; v = prev[static_cast<std::size_t>(v)]) path.insert(path.begin(), v);
    result.path = ToNames(view, path);
    result.status = AlgorithmStatus::PathFound;
    const bool admissible = weight == 1.0;
    result.optimality_claimed = admissible;
    result.optimality_note = admissible ? "optimal J(P): heuristic '" + heuristic + "' is admissible and consistent" + fallback
                                        : "no optimality claim: heuristic weight " + p.at("heuristic_weight") +
                                              " > 1 makes the heuristic inadmissible" + fallback;
  }
};

}  // namespace

std::unique_ptr<IPathAlgorithm> MakeAStar() { return std::make_unique<AStar>(); }

}  // namespace aafr::algorithms::detail
