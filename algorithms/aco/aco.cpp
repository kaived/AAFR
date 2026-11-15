// Ant Colony Optimization (Ant System with best-so-far reinforcement and
// MAX-MIN pheromone bounds).
//
// Representation: pheromone tau(e) on each usable edge. Construction: each
// ant walks from the source, choosing among unvisited neighbours with
// probability proportional to tau(e)^alpha * eta(e)^beta, where
// eta(e) = 1 / (1 + c(e) - c_min) is the shifted edge cost (always > 0 even
// with negative policy costs). An ant that reaches a dead end fails and
// deposits nothing. Update: tau <- (1 - rho) * tau, then each feasible ant
// deposits 1 / J'(P) on its edges, where J'(P) = sum (c(e) - c_min + 1), and
// the best-so-far path deposits once more. Infeasible paths deposit nothing
// (constraint handling by rejection). tau is clamped to [0.01, 10] so the
// colony neither stagnates nor forgets edges completely.

#include <algorithm>
#include <cmath>

#include "algorithms/common/metaheuristic_support.h"

namespace aafr::algorithms::detail {
namespace {

constexpr double kTauMin = 0.01;
constexpr double kTauMax = 10.0;

class AntColony final : public IPathAlgorithm {
 public:
  AlgorithmId id() const override { return AlgorithmId::AntColonyOptimization; }
  std::string version() const override { return "1.0.0"; }
  bool deterministic() const override { return false; }
  std::string encoding() const override { return "edge_pheromone_path_construction"; }
  AlgorithmParameters DefaultParameters() const override {
    return {{"ant_count", "20"}, {"evaporation_rate", "0.3"}, {"pheromone_weight", "1"}, {"heuristic_weight", "2"}};
  }
  std::vector<std::string> ValidateParameters(const AlgorithmParameters& p) const override {
    std::vector<std::string> issues;
    RequireRange(p, "ant_count", 1, 10000, issues, true);
    RequireRange(p, "evaporation_rate", 0.0001, 1.0, issues);
    RequireRange(p, "pheromone_weight", 0.0, 10.0, issues);
    RequireRange(p, "heuristic_weight", 0.0, 10.0, issues);
    return issues;
  }

 protected:
  void Solve(const AlgorithmRequest& request, const forwarding::ProblemView& view, const AlgorithmParameters& p,
             AlgorithmResult& result) const override {
    if (!StartSearch(view, result)) return;
    SearchState s(request, view);
    const std::size_t ants = ParamUInt(p, "ant_count");
    const double rho = ParamDouble(p, "evaporation_rate");
    const double alpha = ParamDouble(p, "pheromone_weight");
    const double beta = ParamDouble(p, "heuristic_weight");
    const std::size_t n = view.node_ids.size();

    std::vector<std::vector<double>> tau(n), eta(n);
    for (std::size_t u = 0; u < n; ++u) {
      tau[u].assign(view.out[u].size(), 1.0);
      for (const auto& edge : view.out[u]) eta[u].push_back(1.0 / (1.0 + edge.cost - view.min_edge_cost));
    }
    const auto shifted = [&view](const std::vector<int>& path) {
      double total = 0.0;
      for (std::size_t i = 0; i + 1 < path.size(); ++i) {
        for (const auto& edge : view.out[static_cast<std::size_t>(path[i])]) {
          if (edge.to == path[i + 1]) total += edge.cost - view.min_edge_cost + 1.0;
        }
      }
      return total;
    };
    const auto deposit = [&](const std::vector<int>& path, double amount) {
      for (std::size_t i = 0; i + 1 < path.size(); ++i) {
        const auto u = static_cast<std::size_t>(path[i]);
        for (std::size_t k = 0; k < view.out[u].size(); ++k) {
          if (view.out[u][k].to == path[i + 1]) tau[u][k] += amount;
        }
      }
    };

    std::vector<char> visited(n);
    std::vector<double> weights;
    std::vector<std::vector<int>> colony;
    while (!s.tracker.Exhausted()) {
      s.tracker.CountIteration();
      colony.clear();
      for (std::size_t a = 0; a < ants && s.tracker.CanEvaluate(); ++a) {
        std::fill(visited.begin(), visited.end(), 0);
        std::vector<int> path{view.source};
        visited[static_cast<std::size_t>(view.source)] = 1;
        while (path.back() != view.destination) {
          const auto u = static_cast<std::size_t>(path.back());
          weights.assign(view.out[u].size(), 0.0);
          double total = 0.0;
          for (std::size_t k = 0; k < view.out[u].size(); ++k) {
            if (visited[static_cast<std::size_t>(view.out[u][k].to)]) continue;
            weights[k] = std::pow(tau[u][k], alpha) * std::pow(eta[u][k], beta);
            total += weights[k];
          }
          if (total <= 0.0) break;  // Dead end: this ant fails.
          double pick = s.rng.Uniform() * total;
          std::size_t chosen = 0;
          for (std::size_t k = 0; k < weights.size(); ++k) {
            if (weights[k] <= 0.0) continue;
            chosen = k;
            pick -= weights[k];
            if (pick < 0.0) break;
          }
          const int next = view.out[u][chosen].to;
          visited[static_cast<std::size_t>(next)] = 1;
          path.push_back(next);
        }
        if (path.back() != view.destination) {
          s.tracker.CountEvaluation();  // A failed construction still costs an evaluation.
          continue;
        }
        s.Score(path);
        colony.push_back(std::move(path));
      }
      for (auto& row : tau) {
        for (double& value : row) value *= 1.0 - rho;
      }
      for (const auto& path : colony) {
        if (s.fitness.Feasible(path)) deposit(path, 1.0 / shifted(path));
      }
      if (!s.best_path.empty() && s.fitness.Feasible(s.best_path)) deposit(s.best_path, 1.0 / shifted(s.best_path));
      for (auto& row : tau) {
        for (double& value : row) value = std::min(kTauMax, std::max(kTauMin, value));
      }
    }
    s.Finish(result, 2 * view.usable_edge_count * sizeof(double) + ants * n * sizeof(int) + n);
  }
};

}  // namespace

std::unique_ptr<IPathAlgorithm> MakeAntColony() { return std::make_unique<AntColony>(); }

}  // namespace aafr::algorithms::detail
