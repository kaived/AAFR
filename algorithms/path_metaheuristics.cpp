#include "algorithms/path_search_internal.h"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <random>

namespace aafr::forwarding::detail {
namespace {

using algorithms::AlgorithmId;
using Keys = std::vector<double>;

class Random {
 public:
  explicit Random(std::uint32_t seed) : generator_(seed) {}
  double Unit() { return std::ldexp(static_cast<double>(generator_()), -32); }
  std::size_t Index(std::size_t size) { return static_cast<std::size_t>(Unit() * size); }

 private:
  std::mt19937 generator_;
};

struct Candidate {
  Keys keys;
  double cost{kInfinity};
};

class PopulationSearch {
 public:
  explicit PopulationSearch(SearchContext& context)
      : c_(context), random_(context.config.seed) {}

  void Run() {
    if (c_.result.algorithm == AlgorithmId::AntColonyOptimization) {
      AntColony();
      return;
    }
    std::vector<Candidate> population;
    for (unsigned i = 0; i < c_.config.population; ++i) {
      population.push_back(Initialize());
      if (Stopped()) return;
    }
    auto personal = population;
    std::vector<Keys> velocity(population.size(), Keys(c_.outgoing.size()));
    std::vector<unsigned> trials(population.size());
    for (unsigned iteration = 0; iteration < c_.config.iterations; ++iteration) {
      if (!c_.Step()) return;
      const double a = 2.0 * (1.0 - static_cast<double>(iteration) / c_.config.iterations);
      switch (c_.result.algorithm) {
        case AlgorithmId::ParticleSwarmOptimization: Swarm(population, personal, velocity); break;
        case AlgorithmId::GeneticAlgorithm: Genetic(population); break;
        case AlgorithmId::ArtificialBeeColony: Bees(population, trials); break;
        case AlgorithmId::GreyWolfOptimizer: Wolves(population, a); break;
        case AlgorithmId::FireflyAlgorithm: Fireflies(population); break;
        case AlgorithmId::WhaleOptimizationAlgorithm: Whales(population, a); break;
        default: return;
      }
      if (Stopped()) return;
      ++c_.result.iterations;
    }
  }

 private:
  bool Stopped() const { return c_.result.status != SearchStatus::NoCandidate; }

  template <typename Choose>
  Route Walk(const Choose& choose) {
    Route path{c_.source};
    std::vector<bool> seen(c_.outgoing.size());
    seen[c_.source] = true;
    double latency = 0;
    while (path.back() != c_.destination) {
      if (!c_.Step()) return {};
      std::vector<const Arc*> options;
      for (const auto& arc : c_.outgoing[path.back()]) {
        if (!c_.Step()) return {};
        if (seen[arc.to] || (c_.problem.constraints.max_latency_ms &&
            latency + arc.latency > *c_.problem.constraints.max_latency_ms)) continue;
        options.push_back(&arc);
      }
      if (options.empty()) return {};
      const Arc& next = *choose(options);
      latency += next.latency;
      path.push_back(next.to);
      seen[next.to] = true;
    }
    return path;
  }

  double Record(const Route& route) {
    if (route.empty() || !c_.Step()) return kInfinity;
    auto path = c_.Path(route);
    const auto evaluation = EvaluatePath(c_.problem, path);
    if (!evaluation.score) return kInfinity;
    if (!c_.result.score || *evaluation.score < *c_.result.score) {
      c_.result.score = evaluation.score;
      c_.result.path = std::move(path);
    }
    return *evaluation.score;
  }

  Candidate Evaluate(Keys keys) {
    for (double& key : keys) key = std::clamp(key, 0.0, 1.0);
    const auto route = Walk([&](const auto& options) {
      return *std::max_element(options.begin(), options.end(), [&](const Arc* a, const Arc* b) {
        return keys[a->to] < keys[b->to];
      });
    });
    const double score = Record(route);
    if (score < leader_.cost || leader_.keys.empty()) leader_ = {keys, score};
    return {std::move(keys), score};
  }

  Candidate Initialize() {
    Keys keys(c_.outgoing.size());
    for (double& key : keys) key = random_.Unit();
    return Evaluate(std::move(keys));
  }

  void Swarm(std::vector<Candidate>& population, std::vector<Candidate>& personal,
             std::vector<Keys>& velocity) {
    const Keys global = leader_.keys;
    for (std::size_t i = 0; i < population.size() && !Stopped(); ++i) {
      auto keys = population[i].keys;
      for (std::size_t j = 0; j < keys.size(); ++j) {
        double& v = velocity[i][j];
        v = std::clamp(0.7 * v + 1.4 * random_.Unit() * (personal[i].keys[j] - keys[j]) +
                       1.4 * random_.Unit() * (global[j] - keys[j]), -0.5, 0.5);
        keys[j] += v;
      }
      population[i] = Evaluate(std::move(keys));
      if (population[i].cost < personal[i].cost) personal[i] = population[i];
    }
  }

  const Candidate& Tournament(const std::vector<Candidate>& population) {
    const auto& first = population[random_.Index(population.size())];
    const auto& second = population[random_.Index(population.size())];
    return first.cost <= second.cost ? first : second;
  }

  void Genetic(std::vector<Candidate>& population) {
    std::vector<Candidate> children{leader_};
    while (children.size() < population.size() && !Stopped()) {
      const auto& a = Tournament(population);
      const auto& b = Tournament(population);
      auto keys = a.keys;
      if (random_.Unit() < 0.8) {
        for (std::size_t j = 0; j < keys.size(); ++j) {
          if (random_.Unit() < 0.5) keys[j] = b.keys[j];
        }
      }
      for (double& key : keys) {
        if (random_.Unit() < 0.1) key = random_.Unit();
      }
      children.push_back(Evaluate(std::move(keys)));
    }
    population = std::move(children);
  }

  void Bees(std::vector<Candidate>& population, std::vector<unsigned>& trials) {
    const auto explore = [&](std::size_t i) {
      const auto peer = (i + 1 + random_.Index(population.size() - 1)) % population.size();
      auto keys = population[i].keys;
      const auto j = random_.Index(keys.size());
      keys[j] += (2 * random_.Unit() - 1) * (keys[j] - population[peer].keys[j]);
      auto proposal = Evaluate(std::move(keys));
      if (proposal.cost < population[i].cost) {
        population[i] = std::move(proposal);
        trials[i] = 0;
      } else {
        ++trials[i];
      }
    };
    for (std::size_t i = 0; i < population.size() && !Stopped(); ++i) explore(i);
    std::vector<std::size_t> rank(population.size());
    std::iota(rank.begin(), rank.end(), 0);
    std::stable_sort(rank.begin(), rank.end(), [&](auto a, auto b) {
      return population[a].cost < population[b].cost;
    });
    for (std::size_t i = 0; i < rank.size() && !Stopped(); ++i) {
      // Max of two uniform draws, reversed, gives the best sources more onlookers.
      const auto pick = rank.size() - 1 -
          std::max(random_.Index(rank.size()), random_.Index(rank.size()));
      explore(rank[pick]);
    }
    for (std::size_t i = 0; i < population.size() && !Stopped(); ++i) {
      if (trials[i] < 8) continue;
      population[i] = Initialize();
      trials[i] = 0;
    }
  }

  void Wolves(std::vector<Candidate>& population, double a) {
    std::stable_sort(population.begin(), population.end(), [](const auto& x, const auto& y) {
      return x.cost < y.cost;
    });
    const std::array<Keys, 3> leaders{population[0].keys, population[1].keys, population[2].keys};
    for (auto& wolf : population) {
      if (Stopped()) return;
      auto keys = wolf.keys;
      for (std::size_t j = 0; j < keys.size(); ++j) {
        double sum = 0;
        for (const auto& leader : leaders) {
          const double A = a * (2 * random_.Unit() - 1);
          const double C = 2 * random_.Unit();
          sum += leader[j] - A * std::abs(C * leader[j] - keys[j]);
        }
        keys[j] = sum / leaders.size();
      }
      wolf = Evaluate(std::move(keys));
    }
  }

  void Fireflies(std::vector<Candidate>& population) {
    const auto snapshot = population;
    for (std::size_t i = 0; i < population.size() && !Stopped(); ++i) {
      for (const auto& brighter : snapshot) {
        if (!c_.Step()) return;
        if (brighter.cost >= population[i].cost) continue;
        auto keys = population[i].keys;
        double distance_squared = 0;
        for (std::size_t j = 0; j < keys.size(); ++j) {
          const double difference = keys[j] - brighter.keys[j];
          distance_squared += difference * difference;
        }
        const double beta = std::exp(-4 * distance_squared / keys.size());
        for (std::size_t j = 0; j < keys.size(); ++j)
          keys[j] += beta * (brighter.keys[j] - keys[j]) + 0.12 * (random_.Unit() - 0.5);
        population[i] = Evaluate(std::move(keys));
        if (Stopped()) return;
      }
      auto keys = population[i].keys;
      for (double& key : keys) key += 0.12 * (random_.Unit() - 0.5);
      population[i] = Evaluate(std::move(keys));
    }
  }

  void Whales(std::vector<Candidate>& population, double a) {
    const Keys best = leader_.keys;
    const auto snapshot = population;
    for (auto& whale : population) {
      if (Stopped()) return;
      auto keys = whale.keys;
      const double A = a * (2 * random_.Unit() - 1);
      const double C = 2 * random_.Unit();
      if (random_.Unit() < 0.5) {
        const auto& target = std::abs(A) < 1 ? best : snapshot[random_.Index(snapshot.size())].keys;
        for (std::size_t j = 0; j < keys.size(); ++j)
          keys[j] = target[j] - A * std::abs(C * target[j] - keys[j]);
      } else {
        constexpr double tau = 6.283185307179586;
        const double l = 2 * random_.Unit() - 1;
        const double spiral = std::exp(l) * std::cos(tau * l);
        for (std::size_t j = 0; j < keys.size(); ++j)
          keys[j] = best[j] + std::abs(best[j] - keys[j]) * spiral;
      }
      whale = Evaluate(std::move(keys));
    }
  }

  void AntColony() {
    Keys pheromone(c_.edge_count, 1);
    for (unsigned iteration = 0; iteration < c_.config.iterations; ++iteration) {
      Keys deposit(c_.edge_count);
      for (unsigned ant = 0; ant < c_.config.population; ++ant) {
        const auto route = Walk([&](const auto& options) {
          const auto cheapest = *std::min_element(options.begin(), options.end(),
              [](const Arc* a, const Arc* b) { return a->cost < b->cost; });
          Keys weights;
          for (const auto* arc : options) {
            const double visibility = 1 / (1 + (arc->cost - cheapest->cost));
            weights.push_back(pheromone[arc->index] * std::max(visibility * visibility, 1e-12));
          }
          double pick = random_.Unit() * std::accumulate(weights.begin(), weights.end(), 0.0);
          for (std::size_t i = 0; i + 1 < weights.size(); ++i) {
            pick -= weights[i];
            if (pick < 0) return options[i];
          }
          return options.back();
        });
        const double score = Record(route);
        if (Stopped()) return;
        if (!std::isfinite(score)) continue;
        const double reward = 1 / (1 + std::atan(score) + 1.5707963267948966);
        for (std::size_t i = 1; i < route.size(); ++i) {
          for (const auto& arc : c_.outgoing[route[i - 1]]) {
            if (!c_.Step()) return;
            if (arc.to != route[i]) continue;
            deposit[arc.index] += reward;
            break;
          }
        }
      }
      for (std::size_t i = 0; i < pheromone.size(); ++i) {
        if (!c_.Step()) return;
        pheromone[i] = std::clamp(0.8 * pheromone[i] + deposit[i], 0.01, 100.0);
      }
      ++c_.result.iterations;
    }
  }

  SearchContext& c_;
  Random random_;
  Candidate leader_;
};

}  // namespace

void Metaheuristic(SearchContext& context) { PopulationSearch(context).Run(); }

}  // namespace aafr::forwarding::detail
