// Firefly Algorithm (Yang 2008).
//
// Discrete adaptation: each firefly is a random-key vector in [0,1]^|V|
// decoded by DecodePriorities; brightness is the negated shared fitness.
// For every pair (i, j) with j brighter than i, i moves towards j:
//   x_i <- x_i + beta0 exp(-gamma r^2) (x_j - x_i) + alpha (u - 0.5)
// with u ~ U(0,1) per dimension. r^2 is the squared distance divided by
// |V|, so gamma keeps the same meaning for every graph size. The brightest
// firefly makes a random step. alpha decays by 0.97 per iteration. The
// pairwise comparison makes one iteration cost O(n^2) evaluations; the
// evaluation budget bounds it.

#include <algorithm>
#include <cmath>
#include <limits>

#include "algorithms/common/metaheuristic_support.h"

namespace aafr::algorithms::detail {
namespace {

class Firefly final : public IPathAlgorithm {
 public:
  AlgorithmId id() const override { return AlgorithmId::FireflyAlgorithm; }
  std::string version() const override { return "1.0.0"; }
  bool deterministic() const override { return false; }
  std::string encoding() const override { return "random_key_priority"; }
  AlgorithmParameters DefaultParameters() const override {
    return {{"firefly_count", "15"}, {"attractiveness", "1"}, {"absorption_coefficient", "1"}, {"randomization", "0.2"}};
  }
  std::vector<std::string> ValidateParameters(const AlgorithmParameters& p) const override {
    std::vector<std::string> issues;
    RequireRange(p, "firefly_count", 2, 1000, issues, true);
    RequireRange(p, "attractiveness", 0.0, 10.0, issues);
    RequireRange(p, "absorption_coefficient", 0.0, 100.0, issues);
    RequireRange(p, "randomization", 0.0, 1.0, issues);
    return issues;
  }

 protected:
  void Solve(const AlgorithmRequest& request, const forwarding::ProblemView& view, const AlgorithmParameters& p,
             AlgorithmResult& result) const override {
    if (!StartSearch(view, result)) return;
    SearchState s(request, view);
    const std::size_t size = ParamUInt(p, "firefly_count");
    const double beta0 = ParamDouble(p, "attractiveness");
    const double gamma = ParamDouble(p, "absorption_coefficient");
    double alpha = ParamDouble(p, "randomization");
    const std::size_t d = view.node_ids.size();
    const double inf = std::numeric_limits<double>::infinity();

    std::vector<Keys> x(size);
    std::vector<double> f(size, inf);
    for (std::size_t i = 0; i < size && s.tracker.CanEvaluate(); ++i) {
      x[i] = s.RandomKeys();
      f[i] = s.ScoreKeys(x[i]);
    }
    for (Keys& keys : x) {
      if (keys.empty()) keys = s.RandomKeys();
    }
    while (!s.tracker.Exhausted()) {
      s.tracker.CountIteration();
      for (std::size_t i = 0; i < size; ++i) {
        for (std::size_t j = 0; j < size; ++j) {
          if (!(f[j] < f[i]) || !s.tracker.CanEvaluate()) continue;
          double r2 = 0.0;
          for (std::size_t k = 0; k < d; ++k) r2 += (x[i][k] - x[j][k]) * (x[i][k] - x[j][k]);
          const double beta = beta0 * std::exp(-gamma * r2 / static_cast<double>(d));
          for (std::size_t k = 0; k < d; ++k) {
            x[i][k] += beta * (x[j][k] - x[i][k]) + alpha * (s.rng.Uniform() - 0.5);
          }
          SearchState::Clamp(x[i]);
          f[i] = s.ScoreKeys(x[i]);
        }
      }
      const std::size_t brightest = static_cast<std::size_t>(std::min_element(f.begin(), f.end()) - f.begin());
      if (s.tracker.CanEvaluate()) {
        Keys step = x[brightest];
        for (double& key : step) key += alpha * (s.rng.Uniform() - 0.5);
        SearchState::Clamp(step);
        const double value = s.ScoreKeys(step);
        if (value < f[brightest]) {
          x[brightest] = std::move(step);
          f[brightest] = value;
        }
      }
      alpha *= 0.97;
    }
    s.Finish(result, size * (d + 1) * sizeof(double));
  }
};

}  // namespace

std::unique_ptr<IPathAlgorithm> MakeFirefly() { return std::make_unique<Firefly>(); }

}  // namespace aafr::algorithms::detail
