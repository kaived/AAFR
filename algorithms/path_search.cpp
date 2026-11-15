#include "algorithms/path_search_internal.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <queue>

namespace aafr::forwarding {
namespace detail {

bool SearchContext::Step() {
  if (cancelled) result.status = SearchStatus::Cancelled;
  else if (Clock::now() >= stop) result.status = SearchStatus::Deadline;
  else if (result.work >= config.work_limit) result.status = SearchStatus::Budget;
  else {
    ++result.work;
    return true;
  }
  return false;
}

ForwardingPath SearchContext::Path(const Route& route) const {
  ForwardingPath path{problem.graph.id(), problem.graph.version(), {}};
  for (int node : route) path.nodes.push_back(problem.graph.nodes()[node].id);
  return path;
}

double SearchContext::Score(const Route& route) const {
  return EvaluatePath(problem, Path(route)).score.value_or(kInfinity);
}

}  // namespace detail
namespace {

using algorithms::AlgorithmId;
using namespace detail;

std::vector<double> Heuristic(SearchContext& c) {
  std::vector<double> zero(c.outgoing.size());
  if (c.result.algorithm != AlgorithmId::AStar || c.config.heuristic.empty()) return zero;
  auto estimate = zero;
  for (const auto& [id, value] : c.config.heuristic) {
    const int index = c.problem.graph.IndexOf(id);
    if (index < 0 || !std::isfinite(value) || value < 0) return zero;
    estimate[index] = value;
  }
  if (estimate[c.destination] != 0) return zero;
  for (std::size_t from = 0; from < c.outgoing.size(); ++from) {
    for (const auto& arc : c.outgoing[from]) {
      if (estimate[from] > arc.cost + estimate[arc.to]) return zero;
    }
  }
  c.result.heuristic_used = true;
  return estimate;
}

bool Reachable(SearchContext& c) {
  Route queue{c.source};
  std::vector<bool> seen(c.outgoing.size());
  seen[c.source] = true;
  for (std::size_t i = 0; i < queue.size(); ++i) {
    if (!c.Step()) return false;
    if (queue[i] == c.destination) return true;
    for (const auto& arc : c.outgoing[queue[i]]) {
      if (!c.Step()) return false;
      if (seen[arc.to]) continue;
      seen[arc.to] = true;
      queue.push_back(arc.to);
    }
  }
  c.result.status = SearchStatus::Infeasible;
  c.result.reason = "destination unreachable on enabled transitions";
  return false;
}

void Save(SearchContext& c, const Route& route, bool optimal) {
  auto path = c.Path(route);
  const auto evaluation = EvaluatePath(c.problem, path);
  if (!evaluation.score) {
    c.result.reason = evaluation.reason;
    return;
  }
  c.result.path = std::move(path);
  c.result.score = evaluation.score;
  c.result.optimal = optimal;
}

Route Reconstruct(const SearchContext& c, const Route& parent) {
  Route route;
  for (int node = c.destination; node >= 0 && route.size() < parent.size(); node = parent[node]) {
    route.push_back(node);
    if (node == c.source) {
      std::reverse(route.begin(), route.end());
      return route;
    }
  }
  return {};
}

void BestFirst(SearchContext& c) {
  const auto estimate = Heuristic(c);
  using QueueEntry = std::tuple<double, double, int>;
  std::priority_queue<QueueEntry, std::vector<QueueEntry>, std::greater<QueueEntry>> queue;
  Route parent(c.outgoing.size(), -1);
  std::vector<double> distance(c.outgoing.size(), kInfinity);
  distance[c.source] = 0;
  queue.emplace(estimate[c.source], 0, c.source);
  while (!queue.empty() && c.Step()) {
    const auto [priority, cost, node] = queue.top();
    queue.pop();
    if (cost != distance[node]) continue;
    ++c.result.iterations;
    if (node == c.destination) {
      Save(c, Reconstruct(c, parent), true);
      return;
    }
    for (const auto& arc : c.outgoing[node]) {
      if (!c.Step()) return;
      const auto next = cost + arc.cost;
      if (next >= distance[arc.to]) continue;
      distance[arc.to] = next;
      parent[arc.to] = node;
      queue.emplace(next + estimate[arc.to], next, arc.to);
    }
  }
}

// Returns false on cancellation, exhausted budget or a reachable negative cycle.
bool BellmanFord(SearchContext& c, Route& parent) {
  std::vector<double> distance(c.outgoing.size(), kInfinity);
  distance[c.source] = 0;
  for (std::size_t pass = 0; pass < c.outgoing.size(); ++pass) {
    ++c.result.iterations;
    bool changed = false;
    for (std::size_t from = 0; from < distance.size(); ++from) {
      if (!c.Step()) return false;
      if (distance[from] == kInfinity) continue;
      for (const auto& arc : c.outgoing[from]) {
        if (!c.Step()) return false;
        const auto next = distance[from] + arc.cost;
        if (next >= distance[arc.to]) continue;
        if (pass + 1 == distance.size()) {
          c.result.status = SearchStatus::NegativeCycle;
          return false;
        }
        distance[arc.to] = next;
        parent[arc.to] = static_cast<int>(from);
        changed = true;
      }
    }
    if (!changed) break;
  }
  return true;
}

// A hard sum-of-latency limit needs a resource-constrained search. Bounded
// enumeration preserves alternative prefixes and prevents cyclic candidates.
void Constrained(SearchContext& c) {
  double best = kInfinity;
  Route path{c.source};
  std::vector<bool> visited(c.outgoing.size());
  visited[c.source] = true;
  const double max_latency = *c.problem.constraints.max_latency_ms;
  const auto visit = [&](auto&& self, int node, double cost, double latency) -> bool {
    if (!c.Step()) return false;
    if (node == c.destination) {
      ++c.result.iterations;
      if (cost < best) {
        Save(c, path, false);
        best = cost;
      }
      return true;
    }
    for (const auto& arc : c.outgoing[node]) {
      if (!c.Step()) return false;
      if (visited[arc.to] || latency + arc.latency > max_latency ||
          (!c.negative && cost + arc.cost >= best)) continue;
      visited[arc.to] = true;
      path.push_back(arc.to);
      if (!self(self, arc.to, cost + arc.cost, latency + arc.latency)) return false;
      path.pop_back();
      visited[arc.to] = false;
    }
    return true;
  };
  if (visit(visit, c.source, 0, 0)) c.result.optimal = c.result.score.has_value();
}

void RunSearch(SearchContext& c) {
  if (!c.Step()) return;
  if (!IsMetaheuristic(c.result.algorithm) && c.negative &&
      c.result.algorithm != AlgorithmId::BellmanFord) {
    c.result.status = SearchStatus::InvalidInput;
    c.result.reason = "negative enabled edge cost requires a compatible algorithm";
    return;
  }
  Route parent(c.outgoing.size(), -1);
  if (c.result.algorithm == AlgorithmId::BellmanFord && !BellmanFord(c, parent)) return;
  if (!Reachable(c)) return;
  if (IsMetaheuristic(c.result.algorithm)) Metaheuristic(c);
  else if (c.problem.constraints.max_latency_ms) Constrained(c);
  else if (c.result.algorithm == AlgorithmId::BellmanFord) Save(c, Reconstruct(c, parent), true);
  else BestFirst(c);
  if (c.result.status == SearchStatus::NoCandidate && c.result.score)
    c.result.status = SearchStatus::Success;
  else if (c.result.status == SearchStatus::NoCandidate && !IsMetaheuristic(c.result.algorithm))
    c.result.status = SearchStatus::Infeasible;
}

}  // namespace

bool IsMetaheuristic(AlgorithmId id) { return id >= AlgorithmId::AntColonyOptimization; }

std::string ValidateSearchConfig(const SearchConfig& config) {
  if (config.population < 4 || config.population > 64 || config.iterations == 0 ||
      config.iterations > 10000 || config.work_limit == 0 || config.work_limit > 10000000 ||
      config.deadline.count() <= 0 || config.deadline > std::chrono::seconds(10) ||
      config.heuristic.size() > kMaxNodes)
    return "algorithm configuration outside supported bounds";
  return {};
}

SearchResult Search(AlgorithmId id, const ForwardingProblem& problem, const SearchConfig& config,
                    const std::atomic<bool>& cancelled) {
  const auto start = SearchContext::Clock::now();
  SearchResult result;
  result.algorithm = id;
  result.seed = config.seed;
  const auto finish = [&] {
    result.computation_ms = std::chrono::duration<double, std::milli>(
        SearchContext::Clock::now() - start).count();
    if (result.reason.empty()) result.reason = ToString(result.status);
    return result;
  };
  if (std::find(algorithms::kAllAlgorithmIds.begin(), algorithms::kAllAlgorithmIds.end(), id) ==
      algorithms::kAllAlgorithmIds.end()) {
    result.reason = "unknown algorithm identifier";
    return finish();
  }
  result.reason = ValidateSearchConfig(config);
  if (!result.reason.empty()) return finish();
  // Conservative per-run workspace admission bound, including validation,
  // queue entries, paths, population vectors and allocator overhead.
  const auto nodes = problem.graph.nodes().size();
  const auto edges = problem.graph.edges().size();
  if (nodes > kMaxNodes || edges > kMaxEdges) {
    result.reason = "graph exceeds supported size";
    return finish();
  }
  if (config.memory_budget_bytes < 65536 + 512 * edges + (1024 + 64 * config.population) * nodes) {
    result.status = SearchStatus::Budget;
    result.reason = "insufficient workspace budget";
    return finish();
  }
  result.reason = ValidateProblem(problem);
  if (!result.reason.empty()) return finish();
  result.status = SearchStatus::NoCandidate;
  SearchContext context{problem, config, cancelled, result, start + config.deadline,
                        std::vector<std::vector<Arc>>(nodes), problem.graph.IndexOf(problem.source),
                        problem.graph.IndexOf(problem.destination)};
  for (const auto& e : problem.graph.edges()) {
    if (!context.Step()) return finish();
    if (!EdgeAllowed(problem, e)) continue;
    const auto cost = EdgeCost(problem.objective, e);
    context.outgoing[problem.graph.IndexOf(e.from)].push_back(
        {problem.graph.IndexOf(e.to), cost, e.latency_ms.value.value_or(0), context.edge_count++});
    context.negative |= cost < 0;
  }
  RunSearch(context);
  if (result.status == SearchStatus::Success && !context.Step()) result.optimal = false;
  return finish();
}

const char* ToString(SearchStatus status) {
  switch (status) {
    case SearchStatus::Success: return "success";
    case SearchStatus::InvalidInput: return "invalid_input";
    case SearchStatus::Infeasible: return "infeasible";
    case SearchStatus::NoCandidate: return "no_feasible_candidate_found";
    case SearchStatus::NegativeCycle: return "reachable_negative_cycle";
    case SearchStatus::Deadline: return "computation_deadline";
    case SearchStatus::Budget: return "resource_budget";
    case SearchStatus::Cancelled: return "cancelled";
    case SearchStatus::Ineligible: return "ineligible";
  }
  return "unknown";
}

}  // namespace aafr::forwarding
