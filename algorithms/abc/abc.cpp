// Artificial Bee Colony (Karaboga 2005).
//
// Discrete adaptation: a food source is a random-key vector in [0,1]^|V|
// decoded by DecodePriorities. The original operators are kept:
//  - employed bees: for source i, pick another source k and one dimension j,
//    v_ij = x_ij + phi (x_ij - x_kj) with phi ~ U(-1, 1); greedy replacement;
//  - onlooker bees: choose sources with probability fit_i / sum(fit), where
//    fit = 1 / (1 + f) for f >= 0 and 1 + |f| otherwise, and apply the same
//    neighbour search;
//  - scouts: a source not improved for abandonment_limit trials is replaced
//    by a random one.
// There are colony_size / 2 food sources (employed = onlookers = sources).

#include <algorithm>
#include <cmath>
#include <limits>

#include "algorithms/common/metaheuristic_support.h"

namespace aafr::algorithms::detail {
namespace {

class BeeColony final : public IPathAlgorithm {
 public:
  AlgorithmId id() const override { return AlgorithmId::ArtificialBeeColony; }
  std::string version() const override { return "1.0.0"; }
  bool deterministic() const override { return false; }
  std::string encoding() const override { return "random_key_priority"; }
  AlgorithmParameters DefaultParameters() const override {
    return {{"colony_size", "20"}, {"abandonment_limit", "20"}};
  }
  std::vector<std::string> ValidateParameters(const AlgorithmParameters& p) const override {
    std::vector<std::string> issues;
    RequireRange(p, "colony_size", 4, 10000, issues, true);
    RequireRange(p, "abandonment_limit", 1, 100000, issues, true);
    return issues;
  }

 protected:
  void Solve(const AlgorithmRequest& request, const forwarding::ProblemView& view, const AlgorithmParameters& p,
             AlgorithmResult& result) const override {
    if (!StartSearch(view, result)) return;
    SearchState s(request, view);
    const std::size_t sources = ParamUInt(p, "colony_size") / 2;
    const std::uint64_t limit = ParamUInt(p, "abandonment_limit");
    const std::size_t d = view.node_ids.size();
    const double inf = std::numeric_limits<double>::infinity();

    std::vector<Keys> food(sources);
    std::vector<double> f(sources, inf);
    std::vector<std::uint64_t> trials(sources, 0);
    for (std::size_t i = 0; i < sources && s.tracker.CanEvaluate(); ++i) {
      food[i] = s.RandomKeys();
      f[i] = s.ScoreKeys(food[i]);
    }
    for (std::size_t i = 0; i < sources; ++i) {
      if (food[i].empty()) food[i] = s.RandomKeys();
    }
    const auto quality = [](double value) {
      if (!std::isfinite(value)) return 0.0;
      return value >= 0.0 ? 1.0 / (1.0 + value) : 1.0 + std::fabs(value);
    };
    const auto neighbour = [&](std::size_t i) {
      if (!s.tracker.CanEvaluate()) return;
      std::size_t k = s.rng.Index(sources - 1);
      if (k >= i) ++k;
      const std::size_t j = s.rng.Index(d);
      Keys candidate = food[i];
      candidate[j] += s.rng.Uniform(-1.0, 1.0) * (food[i][j] - food[k][j]);
      SearchState::Clamp(candidate);
      const double value = s.ScoreKeys(candidate);
      if (value < f[i]) {
        food[i] = std::move(candidate);
        f[i] = value;
        trials[i] = 0;
      } else {
        ++trials[i];
      }
    };

    while (!s.tracker.Exhausted()) {
      s.tracker.CountIteration();
      for (std::size_t i = 0; i < sources; ++i) neighbour(i);  // Employed bees.
      double total = 0.0;
      for (std::size_t i = 0; i < sources; ++i) total += quality(f[i]);
      for (std::size_t bee = 0; bee < sources; ++bee) {  // Onlooker bees.
        std::size_t chosen = s.rng.Index(sources);
        if (total > 0.0) {
          double pick = s.rng.Uniform() * total;
          for (std::size_t i = 0; i < sources; ++i) {
            pick -= quality(f[i]);
            if (pick < 0.0) {
              chosen = i;
              break;
            }
          }
        }
        neighbour(chosen);
      }
      const std::size_t worst = static_cast<std::size_t>(std::max_element(trials.begin(), trials.end()) - trials.begin());
      if (trials[worst] > limit && s.tracker.CanEvaluate()) {  // Scout bee.
        food[worst] = s.RandomKeys();
        f[worst] = s.ScoreKeys(food[worst]);
        trials[worst] = 0;
      }
    }
    s.Finish(result, sources * (d + 2) * sizeof(double));
  }
};

}  // namespace

std::unique_ptr<IPathAlgorithm> MakeBeeColony() { return std::make_unique<BeeColony>(); }

}  // namespace aafr::algorithms::detail
