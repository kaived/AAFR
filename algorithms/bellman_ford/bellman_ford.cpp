// Bellman-Ford on the problem view.
//
// Edge costs may be negative through a configured policy_cost (a preference
// credit). Latency, loss and bandwidth are never negative; no negative
// measurement is invented. Runs at most V - 1 relaxation rounds over every
// usable edge in a fixed order, stopping early when a round changes nothing,
// then one more round to detect a negative cycle reachable from the source.

#include <limits>

#include "algorithms/common/algorithm_makers.h"

namespace aafr::algorithms::detail {
namespace {

class BellmanFord final : public IPathAlgorithm {
 public:
  AlgorithmId id() const override { return AlgorithmId::BellmanFord; }
  std::string version() const override { return "1.0.0"; }
  bool deterministic() const override { return true; }
  std::string encoding() const override { return "graph_search"; }
  AlgorithmParameters DefaultParameters() const override { return {}; }
  std::vector<std::string> ValidateParameters(const AlgorithmParameters&) const override { return {}; }

 protected:
  void Solve(const AlgorithmRequest& request, const forwarding::ProblemView& view, const AlgorithmParameters&,
             AlgorithmResult& result) const override {
    const auto start = std::chrono::steady_clock::now();
    const std::size_t n = view.node_ids.size();
    const double inf = std::numeric_limits<double>::infinity();
    std::vector<double> dist(n, inf);
    std::vector<int> prev(n, -1);
    dist[static_cast<std::size_t>(view.source)] = 0.0;
    result.accounted_memory_bytes = n * (sizeof(double) + sizeof(int));

    const auto relax_round = [&](int* changed_node) {
      bool changed = false;
      for (std::size_t u = 0; u < n; ++u) {
        if (dist[u] == inf) continue;
        for (const auto& edge : view.out[u]) {
          ++result.evaluations;
          const auto v = static_cast<std::size_t>(edge.to);
          if (dist[u] + edge.cost < dist[v]) {
            dist[v] = dist[u] + edge.cost;
            prev[v] = static_cast<int>(u);
            changed = true;
            if (changed_node) *changed_node = edge.to;
          }
        }
      }
      return changed;
    };

    bool converged = false;
    for (std::size_t round = 0; round + 1 < n; ++round) {
      const Termination t = TimeCheck(request, start);
      if (t != Termination::Completed) {
        result.termination = t;
        result.status = t == Termination::Cancelled ? AlgorithmStatus::Cancelled : AlgorithmStatus::Error;
        result.message = "stopped before the relaxation rounds completed";
        return;
      }
      ++result.iterations;
      if (!relax_round(nullptr)) {
        converged = true;
        break;
      }
    }
    result.termination = Termination::Completed;
    int witness = -1;
    if (!converged && relax_round(&witness)) {
      // Walk back V steps to land inside the cycle, then collect it.
      int v = witness;
      for (std::size_t i = 0; i < n; ++i) v = prev[static_cast<std::size_t>(v)];
      std::vector<int> cycle{v};
      for (int u = prev[static_cast<std::size_t>(v)]; u != v && u != -1; u = prev[static_cast<std::size_t>(u)]) {
        cycle.insert(cycle.begin(), u);
        if (cycle.size() > n) break;
      }
      cycle.push_back(v);
      result.status = AlgorithmStatus::NegativeCycle;
      result.message = "a negative-cost cycle is reachable from the source: " + forwarding::PathText(ToNames(view, cycle));
      return;
    }
    if (dist[static_cast<std::size_t>(view.destination)] == inf) {
      result.status = AlgorithmStatus::NoPath;
      result.message = "the destination is unreachable over usable edges";
      return;
    }
    std::vector<int> path;
    for (int v = view.destination; v != -1; v = prev[static_cast<std::size_t>(v)]) {
      path.insert(path.begin(), v);
      if (path.size() > n) {
        result.status = AlgorithmStatus::Error;
        result.message = "predecessor chain does not terminate";
        return;
      }
    }
    result.path = ToNames(view, path);
    result.status = AlgorithmStatus::PathFound;
    result.optimality_claimed = true;
    result.optimality_note = view.has_negative_cost
                                 ? "optimal J(P): negative costs present, no reachable negative cycle"
                                 : "optimal J(P): all costs nonnegative";
  }
};

}  // namespace

std::unique_ptr<IPathAlgorithm> MakeBellmanFord() { return std::make_unique<BellmanFord>(); }

}  // namespace aafr::algorithms::detail
