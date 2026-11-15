// Particle Swarm Optimization (Shi and Eberhart inertia-weight form).
//
// Discrete adaptation: each particle is a random-key vector x in [0,1]^|V|,
// one key per node, decoded to a path by DecodePriorities. The continuous
// update is applied unchanged to the keys:
//   v <- w v + c1 r1 (pbest - x) + c2 r2 (gbest - x),  x <- x + v
// with r1, r2 ~ U(0,1) per dimension, |v| clamped to max_velocity and x
// clamped to [0,1]. Fitness is the shared penalized objective of the decoded
// path, so constraint violations are handled by penalty.

#include <cmath>
#include <limits>

#include "algorithms/common/metaheuristic_support.h"

namespace aafr::algorithms::detail {
namespace {

class ParticleSwarm final : public IPathAlgorithm {
 public:
  AlgorithmId id() const override { return AlgorithmId::ParticleSwarmOptimization; }
  std::string version() const override { return "1.0.0"; }
  bool deterministic() const override { return false; }
  std::string encoding() const override { return "random_key_priority"; }
  AlgorithmParameters DefaultParameters() const override {
    return {{"swarm_size", "20"}, {"inertia_weight", "0.7"}, {"cognitive_coefficient", "1.5"},
            {"social_coefficient", "1.5"}, {"max_velocity", "0.5"}};
  }
  std::vector<std::string> ValidateParameters(const AlgorithmParameters& p) const override {
    std::vector<std::string> issues;
    RequireRange(p, "swarm_size", 1, 10000, issues, true);
    RequireRange(p, "inertia_weight", 0.0, 1.5, issues);
    RequireRange(p, "cognitive_coefficient", 0.0, 4.0, issues);
    RequireRange(p, "social_coefficient", 0.0, 4.0, issues);
    RequireRange(p, "max_velocity", 0.01, 1.0, issues);
    return issues;
  }

 protected:
  void Solve(const AlgorithmRequest& request, const forwarding::ProblemView& view, const AlgorithmParameters& p,
             AlgorithmResult& result) const override {
    if (!StartSearch(view, result)) return;
    SearchState s(request, view);
    const std::size_t size = ParamUInt(p, "swarm_size");
    const double w = ParamDouble(p, "inertia_weight");
    const double c1 = ParamDouble(p, "cognitive_coefficient");
    const double c2 = ParamDouble(p, "social_coefficient");
    const double vmax = ParamDouble(p, "max_velocity");
    const std::size_t d = view.node_ids.size();
    const double inf = std::numeric_limits<double>::infinity();

    std::vector<Keys> x(size), v(size, Keys(d, 0.0)), pbest(size);
    std::vector<double> pbest_fit(size, inf);
    Keys gbest;
    double gbest_fit = inf;
    for (std::size_t i = 0; i < size; ++i) {
      x[i] = s.RandomKeys();
      for (double& value : v[i]) value = s.rng.Uniform(-vmax, vmax);
      pbest[i] = x[i];
    }
    while (!s.tracker.Exhausted()) {
      s.tracker.CountIteration();
      for (std::size_t i = 0; i < size && s.tracker.CanEvaluate(); ++i) {
        const double f = s.ScoreKeys(x[i]);
        if (f < pbest_fit[i]) {
          pbest_fit[i] = f;
          pbest[i] = x[i];
        }
        if (f < gbest_fit) {
          gbest_fit = f;
          gbest = x[i];
        }
      }
      if (gbest.empty()) continue;
      for (std::size_t i = 0; i < size; ++i) {
        for (std::size_t k = 0; k < d; ++k) {
          double velocity = w * v[i][k] + c1 * s.rng.Uniform() * (pbest[i][k] - x[i][k]) +
                            c2 * s.rng.Uniform() * (gbest[k] - x[i][k]);
          velocity = std::fmax(-vmax, std::fmin(vmax, velocity));
          v[i][k] = velocity;
          x[i][k] += velocity;
        }
        SearchState::Clamp(x[i]);
      }
    }
    s.Finish(result, 4 * size * d * sizeof(double));
  }
};

}  // namespace

std::unique_ptr<IPathAlgorithm> MakeParticleSwarm() { return std::make_unique<ParticleSwarm>(); }

}  // namespace aafr::algorithms::detail
