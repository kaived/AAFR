// Genetic Algorithm on variable-length path chromosomes.
//
// Representation: a chromosome is a simple path (node sequence) from source
// to destination; no continuous encoding is involved.
// Initialization: random simple paths (randomized DFS).
// Selection: tournament of three, or fitness-proportional roulette on
// 1 / (1 + rank).
// Crossover (common-node, Ahn and Ramakrishna 2002): pick an intermediate
// node both parents share; each child takes one parent's prefix up to it and
// the other's suffix after it; any loop that creates is cut out (repair).
// Mutation: pick a position, keep the prefix up to it, and regrow the rest
// with a random walk that may not re-enter the prefix.
// Elitism: the best chromosome always survives.

#include <algorithm>
#include <limits>
#include <map>

#include "algorithms/common/metaheuristic_support.h"

namespace aafr::algorithms::detail {
namespace {

std::vector<int> RemoveLoops(const std::vector<int>& path) {
  std::vector<int> out;
  std::map<int, std::size_t> position;
  for (const int node : path) {
    const auto found = position.find(node);
    if (found != position.end()) {
      for (std::size_t i = found->second + 1; i < out.size(); ++i) position.erase(out[i]);
      out.resize(found->second + 1);
      continue;
    }
    position[node] = out.size();
    out.push_back(node);
  }
  return out;
}

class Genetic final : public IPathAlgorithm {
 public:
  AlgorithmId id() const override { return AlgorithmId::GeneticAlgorithm; }
  std::string version() const override { return "1.0.0"; }
  bool deterministic() const override { return false; }
  std::string encoding() const override { return "node_sequence_path"; }
  AlgorithmParameters DefaultParameters() const override {
    return {{"population_size", "20"}, {"crossover_rate", "0.9"}, {"mutation_rate", "0.2"},
            {"selection_method", "tournament"}};
  }
  std::vector<std::string> ValidateParameters(const AlgorithmParameters& p) const override {
    std::vector<std::string> issues;
    RequireRange(p, "population_size", 2, 10000, issues, true);
    RequireRange(p, "crossover_rate", 0.0, 1.0, issues);
    RequireRange(p, "mutation_rate", 0.0, 1.0, issues);
    const std::string& method = p.at("selection_method");
    if (method != "tournament" && method != "roulette") {
      issues.push_back("parameter 'selection_method' must be 'tournament' or 'roulette'");
    }
    return issues;
  }

 protected:
  void Solve(const AlgorithmRequest& request, const forwarding::ProblemView& view, const AlgorithmParameters& p,
             AlgorithmResult& result) const override {
    if (!StartSearch(view, result)) return;
    SearchState s(request, view);
    const std::size_t size = ParamUInt(p, "population_size");
    const double crossover = ParamDouble(p, "crossover_rate");
    const double mutation = ParamDouble(p, "mutation_rate");
    const bool tournament = p.at("selection_method") == "tournament";
    const double inf = std::numeric_limits<double>::infinity();
    std::size_t longest = 0;

    std::vector<std::vector<int>> population;
    std::vector<double> fit;
    for (std::size_t i = 0; i < size && s.tracker.CanEvaluate(); ++i) {
      population.push_back(RandomPath(view, s.rng));
      fit.push_back(s.Score(population.back()));
    }

    const auto select = [&]() -> const std::vector<int>& {
      if (tournament) {
        std::size_t best = s.rng.Index(population.size());
        for (int k = 0; k < 2; ++k) {
          const std::size_t other = s.rng.Index(population.size());
          if (fit[other] < fit[best]) best = other;
        }
        return population[best];
      }
      // Roulette on rank weights, robust to negative or infinite fitness.
      std::vector<std::size_t> order(population.size());
      for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
      std::stable_sort(order.begin(), order.end(), [&fit](std::size_t a, std::size_t b) { return fit[a] < fit[b]; });
      double total = 0.0;
      for (std::size_t r = 0; r < order.size(); ++r) total += 1.0 / (1.0 + r);
      double pick = s.rng.Uniform() * total;
      for (std::size_t r = 0; r < order.size(); ++r) {
        pick -= 1.0 / (1.0 + r);
        if (pick < 0.0) return population[order[r]];
      }
      return population[order.back()];
    };

    const auto mutate = [&](std::vector<int> path) {
      if (path.size() < 2) return path;
      const std::size_t cut = s.rng.Index(path.size() - 1);
      std::vector<char> blocked(view.node_ids.size(), 0);
      for (std::size_t i = 0; i < cut; ++i) blocked[static_cast<std::size_t>(path[i])] = 1;
      std::vector<int> tail = RandomPath(view, s.rng, path[cut], &blocked);
      if (tail.empty()) return path;
      path.resize(cut);
      path.insert(path.end(), tail.begin(), tail.end());
      return path;
    };

    while (!s.tracker.Exhausted() && !population.empty()) {
      s.tracker.CountIteration();
      const std::size_t elite = static_cast<std::size_t>(std::min_element(fit.begin(), fit.end()) - fit.begin());
      std::vector<std::vector<int>> next{population[elite]};
      std::vector<double> next_fit{fit[elite]};
      while (next.size() < population.size() && s.tracker.CanEvaluate()) {
        std::vector<int> a = select();
        std::vector<int> b = select();
        if (s.rng.Uniform() < crossover) {
          std::vector<std::pair<std::size_t, std::size_t>> common;
          for (std::size_t i = 1; i + 1 < a.size(); ++i) {
            for (std::size_t j = 1; j + 1 < b.size(); ++j) {
              if (a[i] == b[j]) common.push_back({i, j});
            }
          }
          if (!common.empty()) {
            const auto [i, j] = common[s.rng.Index(common.size())];
            std::vector<int> child_a(a.begin(), a.begin() + static_cast<long>(i));
            child_a.insert(child_a.end(), b.begin() + static_cast<long>(j), b.end());
            std::vector<int> child_b(b.begin(), b.begin() + static_cast<long>(j));
            child_b.insert(child_b.end(), a.begin() + static_cast<long>(i), a.end());
            a = RemoveLoops(child_a);
            b = RemoveLoops(child_b);
          }
        }
        for (std::vector<int>* child : {&a, &b}) {
          if (next.size() >= population.size() || !s.tracker.CanEvaluate()) break;
          if (s.rng.Uniform() < mutation) *child = mutate(*child);
          longest = std::max(longest, child->size());
          next_fit.push_back(s.Score(*child));
          next.push_back(std::move(*child));
        }
      }
      population = std::move(next);
      fit = std::move(next_fit);
      if (fit.empty()) fit.assign(population.size(), inf);
    }
    s.Finish(result, 2 * size * std::max<std::size_t>(longest, 2) * sizeof(int) + size * sizeof(double));
  }
};

}  // namespace

std::unique_ptr<IPathAlgorithm> MakeGenetic() { return std::make_unique<Genetic>(); }

}  // namespace aafr::algorithms::detail
