// Dijkstra's algorithm on the problem view (binary heap, lazy deletion).
// Exact for J(P) when every usable edge cost is nonnegative; refuses
// otherwise. Ties are broken by node index, so results are deterministic.

#include <functional>
#include <limits>
#include <queue>

#include "algorithms/common/algorithm_makers.h"

namespace aafr::algorithms::detail {
namespace {

class Dijkstra final : public IPathAlgorithm {
 public:
  AlgorithmId id() const override { return AlgorithmId::Dijkstra; }
  std::string version() const override { return "1.0.0"; }
  bool deterministic() const override { return true; }
  std::string encoding() const override { return "graph_search"; }
  AlgorithmParameters DefaultParameters() const override { return {}; }
  std::vector<std::string> ValidateParameters(const AlgorithmParameters&) const override { return {}; }

 protected:
  void Solve(const AlgorithmRequest& request, const forwarding::ProblemView& view, const AlgorithmParameters&,
             AlgorithmResult& result) const override {
    const auto start = std::chrono::steady_clock::now();
    if (view.has_negative_cost) {
      result.status = AlgorithmStatus::InvalidInput;
      result.termination = Termination::Error;
      result.message = "a usable edge has a negative cost; Dijkstra requires nonnegative costs";
      return;
    }
    const std::size_t n = view.node_ids.size();
    const double inf = std::numeric_limits<double>::infinity();
    std::vector<double> dist(n, inf);
    std::vector<int> prev(n, -1);
    std::vector<char> settled(n, 0);
    using Entry = std::pair<double, int>;
    std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry>> heap;
    std::size_t peak_heap = 0;
    dist[static_cast<std::size_t>(view.source)] = 0.0;
    heap.push({0.0, view.source});
    while (!heap.empty()) {
      if ((result.iterations & 255) == 0) {
        const Termination t = TimeCheck(request, start);
        if (t != Termination::Completed) {
          result.termination = t;
          result.status = t == Termination::Cancelled ? AlgorithmStatus::Cancelled : AlgorithmStatus::Error;
          result.message = "stopped before the search completed";
          return;
        }
      }
      peak_heap = std::max(peak_heap, heap.size());
      const auto [d, u] = heap.top();
      heap.pop();
      if (settled[static_cast<std::size_t>(u)] || d > dist[static_cast<std::size_t>(u)]) continue;
      settled[static_cast<std::size_t>(u)] = 1;
      ++result.iterations;
      if (u == view.destination) break;
      for (const auto& edge : view.out[static_cast<std::size_t>(u)]) {
        ++result.evaluations;  // Edge relaxations.
        const double candidate = d + edge.cost;
        const auto v = static_cast<std::size_t>(edge.to);
        if (candidate < dist[v]) {
          dist[v] = candidate;
          prev[v] = u;
          heap.push({candidate, edge.to});
        }
      }
    }
    result.accounted_memory_bytes = n * (sizeof(double) + sizeof(int) + sizeof(char)) + peak_heap * sizeof(Entry);
    result.termination = Termination::Completed;
    if (dist[static_cast<std::size_t>(view.destination)] == inf) {
      result.status = AlgorithmStatus::NoPath;
      result.message = "the destination is unreachable over usable edges";
      return;
    }
    std::vector<int> path;
    for (int v = view.destination; v != -1; v = prev[static_cast<std::size_t>(v)]) path.insert(path.begin(), v);
    result.path = ToNames(view, path);
    result.status = AlgorithmStatus::PathFound;
    result.optimality_claimed = true;
    result.optimality_note =
        "optimal J(P) over usable edges (nonnegative costs verified); path-level limits are checked afterwards, "
        "not optimized";
  }
};

}  // namespace

std::unique_ptr<IPathAlgorithm> MakeDijkstra() { return std::make_unique<Dijkstra>(); }

}  // namespace aafr::algorithms::detail
