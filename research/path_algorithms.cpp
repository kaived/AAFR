#include "research/path_algorithms.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <functional>
#include <limits>
#include <map>
#include <queue>
#include <set>
#include <tuple>

#include "algorithms/common/path_algorithm.h"

namespace aafr::research {

namespace alg = aafr::algorithms;
namespace fwd = aafr::forwarding;

namespace {

using CostOf = std::function<std::optional<double>(const ResearchLink&)>;

std::string LinkName(const ResearchLink& l) { return l.observation.from + "->" + l.observation.to; }

Candidate Refuse(PoolAlgorithm algorithm, CostKind kind, CandidateStatus status, std::string message) {
  Candidate c;
  c.algorithm = algorithm;
  c.cost_kind = kind;
  c.status = status;
  c.message = std::move(message);
  return c;
}

// Dijkstra (no heuristic) or A* (hop heuristic) over the available links,
// with the same queue order and tie-breaking as the production code.
Candidate ShortestPath(const ResearchGraph& graph, const std::string& source, const std::string& destination,
                       PoolAlgorithm algorithm, CostKind kind, const CostOf& cost_of, bool hop_heuristic) {
  const auto issues = ValidateGraph(graph);
  if (!issues.empty()) return Refuse(algorithm, kind, CandidateStatus::InvalidInput, "invalid graph: " + issues.front());
  std::map<std::string, int> index;
  for (std::size_t i = 0; i < graph.nodes.size(); ++i) index[graph.nodes[i]] = static_cast<int>(i);
  if (!index.count(source) || !index.count(destination)) {
    return Refuse(algorithm, kind, CandidateStatus::InvalidInput, "unknown source or destination");
  }
  if (source == destination) return Refuse(algorithm, kind, CandidateStatus::InvalidInput, "source equals destination");

  struct Edge {
    int to;
    double cost;
  };
  const std::size_t n = graph.nodes.size();
  std::vector<std::vector<Edge>> out(n);
  std::vector<std::vector<int>> reverse(n);
  std::string missing, invalid;
  double min_cost = std::numeric_limits<double>::infinity();
  for (const ResearchLink& link : graph.links) {
    if (!link.available) continue;
    const auto cost = cost_of(link);
    if (!cost) {
      missing += (missing.empty() ? "" : ", ") + LinkName(link);
      continue;
    }
    if (!(std::isfinite(*cost) && *cost >= 0.0)) {
      invalid += (invalid.empty() ? "" : ", ") + LinkName(link);
      continue;
    }
    const int u = index[link.observation.from];
    const int v = index[link.observation.to];
    out[static_cast<std::size_t>(u)].push_back({v, *cost});
    reverse[static_cast<std::size_t>(v)].push_back(u);
    min_cost = std::min(min_cost, *cost);
  }
  if (!invalid.empty()) return Refuse(algorithm, kind, CandidateStatus::InvalidCost, "negative or non-finite cost on " + invalid);
  if (!missing.empty()) return Refuse(algorithm, kind, CandidateStatus::MissingCost, "no cost on available link(s) " + missing);

  const int s = index[source];
  const int t = index[destination];
  const double inf = std::numeric_limits<double>::infinity();
  // Fewest available links from each node to the destination.
  std::vector<int> hops(n, -1);
  {
    std::queue<int> queue;
    hops[static_cast<std::size_t>(t)] = 0;
    queue.push(t);
    while (!queue.empty()) {
      const int v = queue.front();
      queue.pop();
      for (const int u : reverse[static_cast<std::size_t>(v)]) {
        if (hops[static_cast<std::size_t>(u)] < 0) {
          hops[static_cast<std::size_t>(u)] = hops[static_cast<std::size_t>(v)] + 1;
          queue.push(u);
        }
      }
    }
  }
  const double weight_floor = std::isfinite(min_cost) ? min_cost : 0.0;
  std::vector<double> h(n, 0.0);
  for (std::size_t v = 0; v < n; ++v) {
    if (hops[v] < 0) h[v] = inf;  // Cannot reach the destination: never expanded.
    else if (hop_heuristic) h[v] = static_cast<double>(hops[v]) * weight_floor;
  }

  Candidate c;
  c.algorithm = algorithm;
  c.cost_kind = kind;
  std::vector<double> g(n, inf);
  std::vector<int> prev(n, -1);
  using Entry = std::tuple<double, double, int>;  // f, g, node
  std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry>> open;
  g[static_cast<std::size_t>(s)] = 0.0;
  if (std::isfinite(h[static_cast<std::size_t>(s)])) open.push({h[static_cast<std::size_t>(s)], 0.0, s});
  bool reached = false;
  while (!open.empty()) {
    const auto [f, gu, u] = open.top();
    open.pop();
    (void)f;
    if (gu > g[static_cast<std::size_t>(u)]) continue;  // Stale entry.
    ++c.expansions;
    if (u == t) {
      reached = true;
      break;
    }
    for (const Edge& e : out[static_cast<std::size_t>(u)]) {
      const auto v = static_cast<std::size_t>(e.to);
      const double candidate = gu + e.cost;
      if (candidate < g[v] && std::isfinite(h[v])) {
        g[v] = candidate;
        prev[v] = u;
        open.push({candidate + h[v], candidate, e.to});
      }
    }
  }
  if (!reached) {
    c.status = CandidateStatus::NoPath;
    c.message = "the destination is unreachable over available links";
    return c;
  }
  for (int v = t; v != -1; v = prev[static_cast<std::size_t>(v)]) c.path.push_back(graph.nodes[static_cast<std::size_t>(v)]);
  std::reverse(c.path.begin(), c.path.end());
  c.optimization_cost = g[static_cast<std::size_t>(t)];
  c.status = CandidateStatus::PathFound;
  return c;
}

}  // namespace

std::vector<std::string> ValidateGraph(const ResearchGraph& graph) {
  std::vector<std::string> issues;
  std::set<std::string> nodes;
  for (const std::string& node : graph.nodes) {
    if (node.empty()) issues.push_back("a node has an empty name");
    else if (!nodes.insert(node).second) issues.push_back("duplicate node '" + node + "'");
  }
  std::set<std::pair<std::string, std::string>> seen;
  for (const ResearchLink& link : graph.links) {
    const auto& from = link.observation.from;
    const auto& to = link.observation.to;
    if (!nodes.count(from) || !nodes.count(to)) issues.push_back("link " + from + "->" + to + " names an unknown node");
    if (from == to) issues.push_back("link " + from + "->" + to + " is a self-loop");
    if (!seen.insert({from, to}).second) issues.push_back("duplicate link " + from + "->" + to);
  }
  return issues;
}

Candidate StaticDelayDijkstra(const ResearchGraph& graph, const std::string& source, const std::string& destination) {
  return ShortestPath(graph, source, destination, PoolAlgorithm::Dijkstra, CostKind::StartupDelayMs,
                      [](const ResearchLink& l) { return l.startup_delay_ms; }, false);
}

Candidate PredictedWeightAStar(const ResearchGraph& graph, const std::string& source, const std::string& destination) {
  return ShortestPath(graph, source, destination, PoolAlgorithm::AStar, CostKind::PredictedWeight,
                      [](const ResearchLink& l) { return l.predicted_weight; }, true);
}

namespace {

constexpr double kTauMin = 0.01;  // Production ACO pheromone bounds.
constexpr double kTauMax = 10.0;

// Shared set-up and bookkeeping of the two metaheuristics: input checks, the
// production ProblemView (successor lists only; no edge cost is used), path
// validation, J_eng scoring of complete paths and the best-so-far path.
class EngineCostSearch {
 public:
  EngineCostSearch(const ResearchGraph& graph, const std::string& source, const std::string& destination,
                   const NormalizationStatistics& statistics, PoolAlgorithm algorithm, std::uint64_t seed)
      : graph_(graph), statistics_(statistics), rng_(seed) {
    result_.algorithm = algorithm;
    result_.cost_kind = CostKind::EngineCost;
    result_.seed = seed;
    const auto issues = ValidateGraph(graph);
    if (!issues.empty()) { Fail(CandidateStatus::InvalidInput, "invalid graph: " + issues.front()); return; }
    const auto stat_issues = statistics.Validate();
    if (!stat_issues.empty()) { Fail(CandidateStatus::InvalidInput, "normalization statistics: " + stat_issues.front()); return; }
    for (const NormalizedFeature f : {NormalizedFeature::Delay, NormalizedFeature::Loss,
                                      NormalizedFeature::InverseBandwidth, NormalizedFeature::Hops}) {
      if (!statistics.features.count(f)) {
        { Fail(CandidateStatus::InvalidInput, std::string("normalization statistics lack ") + ToString(f)); return; }
      }
    }
    std::map<std::string, int> index;
    for (std::size_t i = 0; i < graph.nodes.size(); ++i) index[graph.nodes[i]] = static_cast<int>(i);
    if (!index.count(source) || !index.count(destination)) { Fail(CandidateStatus::InvalidInput, "unknown source or destination"); return; }
    if (source == destination) { Fail(CandidateStatus::InvalidInput, "source equals destination"); return; }
    // Every available link must carry the measurements J_eng needs; a gap is
    // reported, never defaulted.
    std::string missing, invalid;
    view_.node_ids = graph.nodes;
    view_.out.assign(graph.nodes.size(), {});
    for (const ResearchLink& link : graph.links) {
      if (!link.available) continue;
      const PathMetrics m = ComputePathMetrics({link.observation});
      const bool has_missing = m.Has(PathIssue::MissingDelay) || m.Has(PathIssue::MissingLoss) ||
                               m.Has(PathIssue::MissingCapacity) || m.Has(PathIssue::MissingUtilization);
      const bool has_invalid = m.Has(PathIssue::InvalidDelay) || m.Has(PathIssue::InvalidLoss) ||
                               m.Has(PathIssue::InvalidCapacity) || m.Has(PathIssue::InvalidUtilization);
      if (has_invalid) invalid += (invalid.empty() ? "" : ", ") + LinkName(link);
      else if (has_missing) missing += (missing.empty() ? "" : ", ") + LinkName(link);
      fwd::UsableEdge e;
      e.to = index[link.observation.to];
      view_.out[static_cast<std::size_t>(index[link.observation.from])].push_back(e);
      ++view_.usable_edge_count;
    }
    if (!invalid.empty()) { Fail(CandidateStatus::InvalidCost, "invalid measurement on " + invalid); return; }
    if (!missing.empty()) { Fail(CandidateStatus::MissingCost, "missing measurement on available link(s) " + missing); return; }
    view_.source = index[source];
    view_.destination = index[destination];
    if (!alg::Reachable(view_)) { Fail(CandidateStatus::NoPath, "the destination is unreachable over available links"); return; }
    ready_ = true;
  }

  bool ready() const { return ready_; }
  const fwd::ProblemView& view() const { return view_; }
  alg::Rng& rng() { return rng_; }
  Candidate& result() { return result_; }

  // A simple source-destination path over available links.
  bool Valid(const std::vector<int>& path) const {
    if (path.size() < 2 || path.front() != view_.source || path.back() != view_.destination) return false;
    std::vector<char> seen(view_.node_ids.size(), 0);
    for (std::size_t i = 0; i < path.size(); ++i) {
      const auto u = static_cast<std::size_t>(path[i]);
      if (path[i] < 0 || u >= seen.size() || seen[u]) return false;
      seen[u] = 1;
      if (i + 1 < path.size()) {
        const auto& out = view_.out[u];
        if (std::none_of(out.begin(), out.end(), [&](const fwd::UsableEdge& e) { return e.to == path[i + 1]; })) return false;
      }
    }
    return true;
  }

  // J_eng of a complete path; nullopt (rejected) when undefined.
  std::optional<double> Score(const std::vector<int>& path) {
    ++result_.evaluations;
    const auto names = alg::ToNames(view_, path);
    const auto links = PathLinks(graph_, names);
    std::optional<double> cost;
    if (links) cost = EngineCost(ComputePathMetrics(*links), statistics_).value;
    if (!cost) {
      ++result_.rejected_paths;
      return std::nullopt;
    }
    if (!best_cost_ || *cost < *best_cost_) {
      best_cost_ = cost;
      best_path_ = path;
    }
    return cost;
  }

  const std::optional<double>& best_cost() const { return best_cost_; }
  const std::vector<int>& best_path() const { return best_path_; }

  Candidate Finish(double duration_ms) {
    result_.duration_ms = duration_ms;
    if (!best_cost_) {
      result_.status = CandidateStatus::NoScoredPath;
      result_.message = "no constructed path has a defined J_eng";
      return result_;
    }
    result_.status = CandidateStatus::PathFound;
    result_.path = alg::ToNames(view_, best_path_);
    result_.optimization_cost = best_cost_;
    return result_;
  }

 private:
  void Fail(CandidateStatus status, std::string message) {
    result_.status = status;
    result_.message = std::move(message);
  }

  const ResearchGraph& graph_;
  const NormalizationStatistics& statistics_;
  alg::Rng rng_;
  fwd::ProblemView view_;
  Candidate result_;
  bool ready_{false};
  std::optional<double> best_cost_;
  std::vector<int> best_path_;
};

double ElapsedMs(std::chrono::steady_clock::time_point start) {
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

// Production GA loop repair (algorithms/ga): cut out the loop a crossover creates.
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

}  // namespace

Candidate AntColonyOptimization(const ResearchGraph& graph, const std::string& source, const std::string& destination,
                                const NormalizationStatistics& statistics, const AntColonyParameters& p) {
  const auto start = std::chrono::steady_clock::now();
  EngineCostSearch s(graph, source, destination, statistics, PoolAlgorithm::AntColony, p.seed);
  if (!s.ready()) return s.result();
  if (p.ants == 0 || p.iterations == 0 || !(p.alpha >= 0.0) || !(p.rho > 0.0 && p.rho <= 1.0)) {
    s.result().status = CandidateStatus::InvalidInput;
    s.result().message = "invalid ACO parameters";
    return s.result();
  }
  const fwd::ProblemView& view = s.view();
  const std::size_t n = view.node_ids.size();
  std::vector<std::vector<double>> tau(n);
  for (std::size_t u = 0; u < n; ++u) tau[u].assign(view.out[u].size(), 1.0);
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
  std::vector<std::pair<std::vector<int>, double>> colony;
  for (std::size_t iteration = 0; iteration < p.iterations; ++iteration) {
    ++s.result().iterations;
    colony.clear();
    for (std::size_t a = 0; a < p.ants; ++a) {
      std::fill(visited.begin(), visited.end(), 0);
      std::vector<int> path{view.source};
      visited[static_cast<std::size_t>(view.source)] = 1;
      while (path.back() != view.destination) {
        const auto u = static_cast<std::size_t>(path.back());
        weights.assign(view.out[u].size(), 0.0);
        double total = 0.0;
        for (std::size_t k = 0; k < view.out[u].size(); ++k) {
          if (visited[static_cast<std::size_t>(view.out[u][k].to)]) continue;
          weights[k] = std::pow(tau[u][k], p.alpha);  // eta = 1.
          total += weights[k];
        }
        if (total <= 0.0) break;  // Dead end: this ant fails.
        double pick = s.rng().Uniform() * total;
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
      if (path.back() != view.destination || !s.Valid(path)) {
        ++s.result().rejected_paths;
        continue;
      }
      if (const auto cost = s.Score(path)) colony.push_back({std::move(path), *cost});
    }
    for (auto& row : tau) {
      for (double& value : row) value *= 1.0 - p.rho;
    }
    if (s.best_cost()) {
      // Positive for any sign of J_eng; 1 for the best; strictly decreasing.
      const double best = *s.best_cost();
      for (const auto& [path, cost] : colony) deposit(path, 1.0 / (1.0 + cost - best));
      deposit(s.best_path(), 1.0);
    }
    for (auto& row : tau) {
      for (double& value : row) value = std::min(kTauMax, std::max(kTauMin, value));
    }
  }
  return s.Finish(ElapsedMs(start));
}

Candidate GeneticAlgorithm(const ResearchGraph& graph, const std::string& source, const std::string& destination,
                           const NormalizationStatistics& statistics, const GeneticParameters& p) {
  const auto start = std::chrono::steady_clock::now();
  EngineCostSearch s(graph, source, destination, statistics, PoolAlgorithm::Genetic, p.seed);
  if (!s.ready()) return s.result();
  if (p.population < 2 || p.generations == 0 || p.tournament_size == 0 || !(p.crossover_rate >= 0.0 && p.crossover_rate <= 1.0) ||
      !(p.mutation_rate >= 0.0 && p.mutation_rate <= 1.0)) {
    s.result().status = CandidateStatus::InvalidInput;
    s.result().message = "invalid GA parameters";
    return s.result();
  }
  const fwd::ProblemView& view = s.view();
  const double inf = std::numeric_limits<double>::infinity();
  const auto fitness = [&](const std::vector<int>& path) { return s.Score(path).value_or(inf); };

  std::vector<std::vector<int>> population;
  std::vector<double> fit;
  for (std::size_t i = 0; i < p.population; ++i) {
    population.push_back(alg::RandomPath(view, s.rng()));
    fit.push_back(fitness(population.back()));
  }
  // Tournament: best of tournament_size uniform draws (production: 3).
  const auto select = [&]() -> const std::vector<int>& {
    std::size_t best = s.rng().Index(population.size());
    for (std::size_t k = 1; k < p.tournament_size; ++k) {
      const std::size_t other = s.rng().Index(population.size());
      if (fit[other] < fit[best]) best = other;
    }
    return population[best];
  };
  const auto mutate = [&](std::vector<int> path) {
    if (path.size() < 2) return path;
    const std::size_t cut = s.rng().Index(path.size() - 1);
    std::vector<char> blocked(view.node_ids.size(), 0);
    for (std::size_t i = 0; i < cut; ++i) blocked[static_cast<std::size_t>(path[i])] = 1;
    std::vector<int> tail = alg::RandomPath(view, s.rng(), path[cut], &blocked);
    if (tail.empty()) return path;
    path.resize(cut);
    path.insert(path.end(), tail.begin(), tail.end());
    return path;
  };

  for (std::size_t generation = 0; generation < p.generations; ++generation) {
    ++s.result().iterations;
    const std::size_t elite = static_cast<std::size_t>(std::min_element(fit.begin(), fit.end()) - fit.begin());
    std::vector<std::vector<int>> next{population[elite]};
    std::vector<double> next_fit{fit[elite]};
    while (next.size() < population.size()) {
      const std::vector<int> parent_a = select();
      const std::vector<int> parent_b = select();
      std::vector<int> a = parent_a, b = parent_b;
      if (s.rng().Uniform() < p.crossover_rate) {
        std::vector<std::pair<std::size_t, std::size_t>> common;
        for (std::size_t i = 1; i + 1 < a.size(); ++i) {
          for (std::size_t j = 1; j + 1 < b.size(); ++j) {
            if (a[i] == b[j]) common.push_back({i, j});
          }
        }
        if (!common.empty()) {
          const auto [i, j] = common[s.rng().Index(common.size())];
          std::vector<int> child_a(a.begin(), a.begin() + static_cast<long>(i));
          child_a.insert(child_a.end(), b.begin() + static_cast<long>(j), b.end());
          std::vector<int> child_b(b.begin(), b.begin() + static_cast<long>(j));
          child_b.insert(child_b.end(), a.begin() + static_cast<long>(i), a.end());
          a = RemoveLoops(child_a);
          b = RemoveLoops(child_b);
        }
      }
      for (auto [child, parent] : {std::pair{&a, &parent_a}, std::pair{&b, &parent_b}}) {
        if (next.size() >= population.size()) break;
        if (s.rng().Uniform() < p.mutation_rate) *child = mutate(*child);
        if (!s.Valid(*child)) {  // Never expected; rejected and replaced by its parent.
          ++s.result().rejected_paths;
          *child = *parent;
        }
        next_fit.push_back(fitness(*child));
        next.push_back(std::move(*child));
      }
    }
    population = std::move(next);
    fit = std::move(next_fit);
  }
  return s.Finish(ElapsedMs(start));
}

std::vector<Candidate> RunPool(const ResearchGraph& graph, const std::string& source, const std::string& destination,
                               const NormalizationStatistics& statistics) {
  return {StaticDelayDijkstra(graph, source, destination), PredictedWeightAStar(graph, source, destination),
          AntColonyOptimization(graph, source, destination, statistics),
          GeneticAlgorithm(graph, source, destination, statistics)};
}

std::optional<std::vector<LinkObservation>> PathLinks(const ResearchGraph& graph, const std::vector<std::string>& path) {
  std::vector<LinkObservation> links;
  for (std::size_t i = 0; i + 1 < path.size(); ++i) {
    const auto it = std::find_if(graph.links.begin(), graph.links.end(), [&](const ResearchLink& l) {
      return l.observation.from == path[i] && l.observation.to == path[i + 1];
    });
    if (it == graph.links.end() || !it->available) return std::nullopt;
    links.push_back(it->observation);
  }
  return links;
}

PathEvaluation EvaluatePath(const ResearchGraph& graph, const std::vector<std::string>& path,
                            const NormalizationStatistics& statistics) {
  PathEvaluation e;
  const auto links = PathLinks(graph, path);
  if (!links || path.size() < 2) {
    e.quality.unavailable_reason = e.engine_cost.unavailable_reason = "path is not on the graph's available links";
    return e;
  }
  e.on_graph = true;
  e.metrics = ComputePathMetrics(*links);
  e.quality = QualityScore(e.metrics);
  e.engine_cost = EngineCost(e.metrics, statistics);
  return e;
}

CandidateArms DistinctChallengers(const std::vector<Candidate>& candidates, const std::vector<std::string>& installed) {
  CandidateArms arms;
  for (const Candidate& c : candidates) {
    if (c.status != CandidateStatus::PathFound) continue;
    if (c.path == installed) {
      arms.installed_algorithms.push_back(c.algorithm);
      continue;
    }
    auto it = std::find_if(arms.challengers.begin(), arms.challengers.end(), [&](const Arm& a) { return a.path == c.path; });
    if (it == arms.challengers.end()) arms.challengers.push_back(Arm{c.path, {c.algorithm}});
    else it->algorithms.push_back(c.algorithm);
  }
  return arms;
}

const char* ToString(PoolAlgorithm algorithm) {
  switch (algorithm) {
    case PoolAlgorithm::Dijkstra: return "dijkstra";
    case PoolAlgorithm::AStar: return "astar";
    case PoolAlgorithm::AntColony: return "aco";
    case PoolAlgorithm::Genetic: return "ga";
  }
  return "unknown";
}

const char* ToString(CandidateStatus status) {
  switch (status) {
    case CandidateStatus::PathFound: return "path_found";
    case CandidateStatus::NoPath: return "no_path";
    case CandidateStatus::MissingCost: return "missing_cost";
    case CandidateStatus::InvalidCost: return "invalid_cost";
    case CandidateStatus::InvalidInput: return "invalid_input";
    case CandidateStatus::NoScoredPath: return "no_scored_path";
  }
  return "unknown";
}

const char* ToString(CostKind kind) {
  switch (kind) {
    case CostKind::StartupDelayMs: return "startup_delay_ms";
    case CostKind::PredictedWeight: return "predicted_weight";
    case CostKind::EngineCost: return "engine_cost";
  }
  return "unknown";
}

}  // namespace aafr::research
