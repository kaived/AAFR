#include "research/path_algorithms.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <map>
#include <queue>
#include <set>
#include <tuple>

namespace aafr::research {
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

Candidate AntColonyOptimization(const ResearchGraph&, const std::string&, const std::string&,
                                const NormalizationStatistics&, const AntColonyParameters&) {
  return Refuse(PoolAlgorithm::AntColony, CostKind::EngineCost, CandidateStatus::NotImplemented,
                "ACO search operators (transition rule, edge desirability, pheromone deposit for a signed J_eng, "
                "evaporation, initialization, bounds) are not defined by the specification");
}

Candidate GeneticAlgorithm(const ResearchGraph&, const std::string&, const std::string&,
                           const NormalizationStatistics&, const GeneticParameters&) {
  return Refuse(PoolAlgorithm::Genetic, CostKind::EngineCost, CandidateStatus::NotImplemented,
                "GA operators (encoding, initialization, selection for a signed J_eng, crossover, mutation, "
                "repair, elitism) are not defined by the specification");
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
    case CandidateStatus::NotImplemented: return "not_implemented";
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
