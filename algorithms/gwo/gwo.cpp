// Grey Wolf Optimizer (Mirjalili, Mirjalili and Lewis 2014).
//
// Discrete adaptation: each wolf is a random-key vector in [0,1]^|V| decoded
// by DecodePriorities. The hierarchy and hunting equations are the
// original ones: alpha, beta and delta are the three best wolves so far;
// a decreases linearly from 2 to 0 over max_iterations; for each leader L,
// A = 2 a r1 - a, C = 2 r2, D = |C X_L - X|, X_L' = X_L - A D; the new
// position is the mean of the three, clamped to [0,1].

#include <algorithm>
#include <cmath>
#include <limits>

#include "algorithms/common/metaheuristic_support.h"

namespace aafr::algorithms::detail {
namespace {

class GreyWolf final : public IPathAlgorithm {
 public:
  AlgorithmId id() const override { return AlgorithmId::GreyWolfOptimizer; }
  std::string version() const override { return "1.0.0"; }
  bool deterministic() const override { return false; }
  std::string encoding() const override { return "random_key_priority"; }
  AlgorithmParameters DefaultParameters() const override { return {{"pack_size", "20"}}; }
  std::vector<std::string> ValidateParameters(const AlgorithmParameters& p) const override {
    std::vector<std::string> issues;
    RequireRange(p, "pack_size", 3, 10000, issues, true);
    return issues;
  }

 protected:
  void Solve(const AlgorithmRequest& request, const forwarding::ProblemView& view, const AlgorithmParameters& p,
             AlgorithmResult& result) const override {
    if (!StartSearch(view, result)) return;
    SearchState s(request, view);
    const std::size_t size = ParamUInt(p, "pack_size");
    const std::size_t d = view.node_ids.size();
    const double total_iterations = static_cast<double>(request.budget.max_iterations);
    const double inf = std::numeric_limits<double>::infinity();

    std::vector<Keys> wolves(size);
    for (Keys& wolf : wolves) wolf = s.RandomKeys();
    Keys leaders[3] = {wolves[0], wolves[0], wolves[0]};
    double leader_fit[3] = {inf, inf, inf};
    const auto rank = [&](const Keys& wolf, double value) {
      for (int k = 0; k < 3; ++k) {
        if (value < leader_fit[k]) {
          for (int m = 2; m > k; --m) {
            leaders[m] = leaders[m - 1];
            leader_fit[m] = leader_fit[m - 1];
          }
          leaders[k] = wolf;
          leader_fit[k] = value;
          return;
        }
      }
    };

    while (!s.tracker.Exhausted()) {
      const double a = 2.0 - 2.0 * static_cast<double>(s.tracker.iterations()) / total_iterations;
      s.tracker.CountIteration();
      for (std::size_t i = 0; i < size && s.tracker.CanEvaluate(); ++i) rank(wolves[i], s.ScoreKeys(wolves[i]));
      for (Keys& wolf : wolves) {
        for (std::size_t j = 0; j < d; ++j) {
          double sum = 0.0;
          for (const Keys& leader : leaders) {
            const double A = 2.0 * a * s.rng.Uniform() - a;
            const double C = 2.0 * s.rng.Uniform();
            const double D = std::fabs(C * leader[j] - wolf[j]);
            sum += leader[j] - A * D;
          }
          wolf[j] = sum / 3.0;
        }
        SearchState::Clamp(wolf);
      }
    }
    s.Finish(result, (size + 3) * d * sizeof(double));
  }
};

}  // namespace

std::unique_ptr<IPathAlgorithm> MakeGreyWolf() { return std::make_unique<GreyWolf>(); }

}  // namespace aafr::algorithms::detail
