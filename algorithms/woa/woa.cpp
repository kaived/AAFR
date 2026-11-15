// Whale Optimization Algorithm (Mirjalili and Lewis 2016).
//
// Discrete adaptation: each whale is a random-key vector in [0,1]^|V|
// decoded by DecodePriorities. Per whale and iteration, with a decreasing
// linearly from 2 to 0, A = 2 a r - a, C = 2 r', p ~ U(0,1), l ~ U(-1,1):
//   p < 0.5, |A| < 1:  X <- X* - A |C X* - X|          (encircling the best)
//   p < 0.5, |A| >= 1: X <- X_rand - A |C X_rand - X|  (search for prey)
//   p >= 0.5:          X <- |X* - X| e^(b l) cos(2 pi l) + X*  (bubble-net spiral)
// with b = spiral_constant. Positions are clamped to [0,1].

#include <cmath>
#include <limits>

#include "algorithms/common/metaheuristic_support.h"

namespace aafr::algorithms::detail {
namespace {

class Whale final : public IPathAlgorithm {
 public:
  AlgorithmId id() const override { return AlgorithmId::WhaleOptimizationAlgorithm; }
  std::string version() const override { return "1.0.0"; }
  bool deterministic() const override { return false; }
  std::string encoding() const override { return "random_key_priority"; }
  AlgorithmParameters DefaultParameters() const override { return {{"whale_count", "20"}, {"spiral_constant", "1"}}; }
  std::vector<std::string> ValidateParameters(const AlgorithmParameters& p) const override {
    std::vector<std::string> issues;
    RequireRange(p, "whale_count", 2, 10000, issues, true);
    RequireRange(p, "spiral_constant", 0.01, 10.0, issues);
    return issues;
  }

 protected:
  void Solve(const AlgorithmRequest& request, const forwarding::ProblemView& view, const AlgorithmParameters& p,
             AlgorithmResult& result) const override {
    if (!StartSearch(view, result)) return;
    SearchState s(request, view);
    const std::size_t size = ParamUInt(p, "whale_count");
    const double b = ParamDouble(p, "spiral_constant");
    const std::size_t d = view.node_ids.size();
    const double total_iterations = static_cast<double>(request.budget.max_iterations);
    const double two_pi = 6.283185307179586;

    std::vector<Keys> whales(size);
    for (Keys& whale : whales) whale = s.RandomKeys();
    Keys leader = whales[0];
    double leader_fit = std::numeric_limits<double>::infinity();

    while (!s.tracker.Exhausted()) {
      const double a = 2.0 - 2.0 * static_cast<double>(s.tracker.iterations()) / total_iterations;
      s.tracker.CountIteration();
      for (std::size_t i = 0; i < size && s.tracker.CanEvaluate(); ++i) {
        const double value = s.ScoreKeys(whales[i]);
        if (value < leader_fit) {
          leader_fit = value;
          leader = whales[i];
        }
      }
      for (std::size_t i = 0; i < size; ++i) {
        Keys& x = whales[i];
        const double A = 2.0 * a * s.rng.Uniform() - a;
        const double C = 2.0 * s.rng.Uniform();
        const double prob = s.rng.Uniform();
        const double l = s.rng.Uniform(-1.0, 1.0);
        if (prob < 0.5) {
          const Keys& target = std::fabs(A) < 1.0 ? leader : whales[s.rng.Index(size)];
          const Keys reference = target;  // Copy: target may alias x.
          for (std::size_t j = 0; j < d; ++j) x[j] = reference[j] - A * std::fabs(C * reference[j] - x[j]);
        } else {
          const double spiral = std::exp(b * l) * std::cos(two_pi * l);
          for (std::size_t j = 0; j < d; ++j) x[j] = std::fabs(leader[j] - x[j]) * spiral + leader[j];
        }
        SearchState::Clamp(x);
      }
    }
    s.Finish(result, (size + 1) * d * sizeof(double));
  }
};

}  // namespace

std::unique_ptr<IPathAlgorithm> MakeWhale() { return std::make_unique<Whale>(); }

}  // namespace aafr::algorithms::detail
