#include "algorithms/common/path_algorithm.h"

#include <algorithm>
#include <cmath>
#include <exception>
#include <limits>
#include <numeric>

namespace aafr::algorithms {
namespace {

using Clock = std::chrono::steady_clock;

double MsSince(Clock::time_point start) {
  return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

std::uint64_t SplitMix(std::uint64_t& x) {
  std::uint64_t z = (x += 0x9E3779B97F4A7C15ull);
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
  return z ^ (z >> 31);
}

std::uint64_t Rotl(std::uint64_t x, int k) { return (x << k) | (x >> (64 - k)); }

const forwarding::UsableEdge* FindUsable(const forwarding::ProblemView& view, int from, int to) {
  for (const auto& edge : view.out[static_cast<std::size_t>(from)]) {
    if (edge.to == to) return &edge;
  }
  return nullptr;
}

// Depth-first walk that orders the neighbours of each node with `order`.
template <typename Order>
std::vector<int> Walk(const forwarding::ProblemView& view, int from, const std::vector<char>* blocked, Order order) {
  const std::size_t n = view.node_ids.size();
  if (from < 0 || view.destination < 0) return {};
  std::vector<char> visited(n, 0);
  if (blocked) {
    for (std::size_t i = 0; i < n && i < blocked->size(); ++i) visited[i] = (*blocked)[i];
  }
  struct Frame {
    int node;
    std::vector<int> next;
    std::size_t cursor;
  };
  std::vector<Frame> stack;
  visited[static_cast<std::size_t>(from)] = 1;
  stack.push_back({from, order(from), 0});
  while (!stack.empty()) {
    Frame& top = stack.back();
    if (top.node == view.destination) {
      std::vector<int> path;
      for (const Frame& frame : stack) path.push_back(frame.node);
      return path;
    }
    if (top.cursor >= top.next.size()) {
      // Dead end. The node stays marked: DFS with permanent marks still finds
      // a path whenever one exists, in O(V + E).
      stack.pop_back();
      continue;
    }
    const int next = top.next[top.cursor++];
    if (visited[static_cast<std::size_t>(next)]) continue;
    visited[static_cast<std::size_t>(next)] = 1;
    stack.push_back({next, order(next), 0});
  }
  return {};
}

}  // namespace

// ---- IPathAlgorithm::Run ----

AlgorithmResult IPathAlgorithm::Run(const AlgorithmRequest& original) const {
  const auto start = Clock::now();
  AlgorithmResult result;
  result.algorithm = id();
  result.algorithm_version = version();
  result.seed = original.seed;
  result.termination = Termination::Error;
  const auto fail = [&](AlgorithmStatus status, std::string message) {
    result.status = status;
    result.message = std::move(message);
    result.termination = status == AlgorithmStatus::Cancelled ? Termination::Cancelled : Termination::Error;
    result.duration_ms = MsSince(start);
    return result;
  };
  if (!original.graph || !original.problem) return fail(AlgorithmStatus::InvalidInput, "no graph or problem");
  result.graph_version = original.graph->version();

  AlgorithmParameters parameters = DefaultParameters();
  std::vector<std::string> issues;
  for (const auto& entry : original.parameters) {
    if (parameters.count(entry.first) == 0) issues.push_back("unknown parameter '" + entry.first + "'");
    else parameters[entry.first] = entry.second;
  }
  result.parameters = parameters;
  for (const std::string& issue : ValidateParameters(parameters)) issues.push_back(issue);
  for (const std::string& issue : forwarding::ValidateProblem(*original.graph, *original.problem)) {
    issues.push_back(issue);
  }
  if (original.budget.max_iterations == 0) issues.push_back("max_iterations must be positive");
  if (!issues.empty()) {
    std::string message;
    for (const std::string& issue : issues) message += (message.empty() ? "" : "; ") + issue;
    return fail(AlgorithmStatus::InvalidInput, message);
  }

  // The problem's own computation limits tighten the request budget.
  AlgorithmRequest request = original;
  const auto& c = original.problem->constraints;
  if (c.computation_deadline_ms) {
    request.budget.deadline_ms = request.budget.deadline_ms
                                     ? std::min(*request.budget.deadline_ms, *c.computation_deadline_ms)
                                     : *c.computation_deadline_ms;
  }
  if (c.max_objective_evaluations) {
    request.budget.max_evaluations = request.budget.max_evaluations
                                         ? std::min(*request.budget.max_evaluations, *c.max_objective_evaluations)
                                         : *c.max_objective_evaluations;
  }

  try {
    const forwarding::ProblemView view = forwarding::BuildView(*request.graph, *request.problem);
    result.status = AlgorithmStatus::Error;
    Solve(request, view, parameters, result);
  } catch (const std::exception& error) {
    return fail(AlgorithmStatus::Error, std::string("exception: ") + error.what());
  } catch (...) {
    return fail(AlgorithmStatus::Error, "unknown exception");
  }

  if (!result.path.empty()) {
    const auto validation = forwarding::ValidatePath(*request.graph, *request.problem, result.path);
    result.objective = validation.objective;
    result.feasible = validation.feasible;
    if (!validation.valid) {
      result.status = AlgorithmStatus::Error;
      result.message = "the algorithm produced a structurally invalid path: " +
                       (validation.details.empty() ? std::string() : validation.details.front());
      result.feasible = false;
    } else if (!validation.feasible && result.status == AlgorithmStatus::PathFound) {
      // Keep the best candidate for diagnostics, but say it breaks a limit.
      result.status = AlgorithmStatus::NoFeasiblePath;
      if (result.message.empty()) result.message = validation.details.back();
    }
  }
  if (result.status != AlgorithmStatus::PathFound) result.optimality_claimed = false;
  result.duration_ms = MsSince(start);
  return result;
}

const char* ToString(AlgorithmStatus value) {
  switch (value) {
    case AlgorithmStatus::PathFound: return "path_found";
    case AlgorithmStatus::NoPath: return "no_path";
    case AlgorithmStatus::NoFeasiblePath: return "no_feasible_path";
    case AlgorithmStatus::NegativeCycle: return "negative_cycle";
    case AlgorithmStatus::InvalidInput: return "invalid_input";
    case AlgorithmStatus::Cancelled: return "cancelled";
    case AlgorithmStatus::Error: return "error";
  }
  return "unknown";
}

const char* ToString(Termination value) {
  switch (value) {
    case Termination::Completed: return "completed";
    case Termination::IterationBudget: return "iteration_budget";
    case Termination::EvaluationBudget: return "evaluation_budget";
    case Termination::Deadline: return "deadline";
    case Termination::Cancelled: return "cancelled";
    case Termination::Error: return "error";
  }
  return "unknown";
}

// ---- Rng: xoshiro256** seeded through splitmix64 ----

Rng::Rng(std::uint64_t seed) {
  std::uint64_t x = seed;
  for (auto& word : state_) word = SplitMix(x);
}

std::uint64_t Rng::Next() {
  const std::uint64_t result = Rotl(state_[1] * 5, 7) * 9;
  const std::uint64_t t = state_[1] << 17;
  state_[2] ^= state_[0];
  state_[3] ^= state_[1];
  state_[1] ^= state_[2];
  state_[0] ^= state_[3];
  state_[2] ^= t;
  state_[3] = Rotl(state_[3], 45);
  return result;
}

double Rng::Uniform() { return static_cast<double>(Next() >> 11) * 0x1.0p-53; }
double Rng::Uniform(double low, double high) { return low + (high - low) * Uniform(); }
std::size_t Rng::Index(std::size_t size) { return size == 0 ? 0 : static_cast<std::size_t>(Next() % size); }

double Rng::Normal() {
  double u1 = Uniform();
  while (u1 <= 0.0) u1 = Uniform();
  const double u2 = Uniform();
  return std::sqrt(-2.0 * std::log(u1)) * std::cos(6.283185307179586 * u2);
}

// ---- BudgetTracker ----

BudgetTracker::BudgetTracker(const AlgorithmRequest& request) : request_(request), start_(Clock::now()) {}

bool BudgetTracker::Exhausted() {
  if (request_.cancel && request_.cancel->load(std::memory_order_relaxed)) {
    reason_ = Termination::Cancelled;
    return true;
  }
  if (request_.budget.deadline_ms && MsSince(start_) >= *request_.budget.deadline_ms) {
    reason_ = Termination::Deadline;
    return true;
  }
  if (iterations_ >= request_.budget.max_iterations) {
    reason_ = Termination::IterationBudget;
    return true;
  }
  if (!CanEvaluate()) return true;
  return false;
}

bool BudgetTracker::CanEvaluate() {
  if (request_.budget.max_evaluations && evaluations_ >= *request_.budget.max_evaluations) {
    reason_ = Termination::EvaluationBudget;
    return false;
  }
  return true;
}

// ---- FitnessEvaluator ----

FitnessEvaluator::FitnessEvaluator(const forwarding::ProblemView& view, const forwarding::ForwardingProblem& problem)
    : view_(view), problem_(problem) {
  // S bounds |J(P)| for every simple path, since a simple path uses each
  // edge at most once. With penalty 2S + 1 per unit of (1 + violation), any
  // infeasible path scores worse than any feasible one.
  double sum = 0.0;
  for (const auto& edges : view.out) {
    for (const auto& edge : edges) sum += std::fabs(edge.cost);
  }
  penalty_scale_ = 2.0 * sum + 1.0;
}

double FitnessEvaluator::Objective(const std::vector<int>& path) const {
  if (path.size() < 2 || path.front() != view_.source || path.back() != view_.destination) {
    return std::numeric_limits<double>::infinity();
  }
  double cost = 0.0;
  for (std::size_t i = 0; i + 1 < path.size(); ++i) {
    const auto* edge = FindUsable(view_, path[i], path[i + 1]);
    if (!edge) return std::numeric_limits<double>::infinity();
    cost += edge->cost;
  }
  return cost;
}

double FitnessEvaluator::Violation(const std::vector<int>& path) const {
  const auto& c = problem_.constraints;
  double violation = 0.0;
  if (c.latency_budget_ms) {
    double latency = 0.0;
    for (std::size_t i = 0; i + 1 < path.size(); ++i) {
      if (const auto* edge = FindUsable(view_, path[i], path[i + 1])) latency += edge->latency_ms;
    }
    violation += std::max(0.0, latency - *c.latency_budget_ms) / *c.latency_budget_ms;
  }
  if (c.max_hops && path.size() - 1 > *c.max_hops) {
    violation += static_cast<double>(path.size() - 1 - *c.max_hops) / static_cast<double>(*c.max_hops);
  }
  return violation;
}

bool FitnessEvaluator::Feasible(const std::vector<int>& path) const {
  return std::isfinite(Objective(path)) && Violation(path) == 0.0;
}

double FitnessEvaluator::Fitness(const std::vector<int>& path) const {
  const double objective = Objective(path);
  if (!std::isfinite(objective)) return objective;
  const double violation = Violation(path);
  return violation > 0.0 ? objective + penalty_scale_ * (1.0 + violation) : objective;
}

// ---- Decoding and paths ----

std::vector<int> DecodePriorities(const forwarding::ProblemView& view, const std::vector<double>& keys) {
  return Walk(view, view.source, nullptr, [&view, &keys](int node) {
    std::vector<int> next;
    for (const auto& edge : view.out[static_cast<std::size_t>(node)]) next.push_back(edge.to);
    // Highest key first; ties broken by node index so decoding is total.
    std::stable_sort(next.begin(), next.end(), [&keys](int a, int b) {
      return keys[static_cast<std::size_t>(a)] > keys[static_cast<std::size_t>(b)];
    });
    return next;
  });
}

std::vector<int> RandomPath(const forwarding::ProblemView& view, Rng& rng, int from, const std::vector<char>* blocked) {
  return Walk(view, from < 0 ? view.source : from, blocked, [&view, &rng](int node) {
    std::vector<int> next;
    for (const auto& edge : view.out[static_cast<std::size_t>(node)]) next.push_back(edge.to);
    for (std::size_t i = next.size(); i > 1; --i) std::swap(next[i - 1], next[rng.Index(i)]);
    return next;
  });
}

bool Reachable(const forwarding::ProblemView& view) {
  if (view.source < 0 || view.destination < 0) return false;
  std::vector<char> seen(view.node_ids.size(), 0);
  std::vector<int> stack{view.source};
  seen[static_cast<std::size_t>(view.source)] = 1;
  while (!stack.empty()) {
    const int node = stack.back();
    stack.pop_back();
    if (node == view.destination) return true;
    for (const auto& edge : view.out[static_cast<std::size_t>(node)]) {
      if (!seen[static_cast<std::size_t>(edge.to)]) {
        seen[static_cast<std::size_t>(edge.to)] = 1;
        stack.push_back(edge.to);
      }
    }
  }
  return false;
}

forwarding::Path ToNames(const forwarding::ProblemView& view, const std::vector<int>& path) {
  forwarding::Path names;
  for (const int node : path) names.push_back(view.node_ids[static_cast<std::size_t>(node)]);
  return names;
}

double ParamDouble(const AlgorithmParameters& parameters, const std::string& name) {
  const auto found = parameters.find(name);
  if (found == parameters.end()) return std::numeric_limits<double>::quiet_NaN();
  try {
    std::size_t used = 0;
    const double value = std::stod(found->second, &used);
    return used == found->second.size() ? value : std::numeric_limits<double>::quiet_NaN();
  } catch (...) {
    return std::numeric_limits<double>::quiet_NaN();
  }
}

std::uint64_t ParamUInt(const AlgorithmParameters& parameters, const std::string& name) {
  const double value = ParamDouble(parameters, name);
  return std::isfinite(value) && value >= 0.0 ? static_cast<std::uint64_t>(value) : 0;
}

void RequireRange(const AlgorithmParameters& parameters, const std::string& name, double low, double high,
                  std::vector<std::string>& issues, bool integer) {
  const double value = ParamDouble(parameters, name);
  if (!std::isfinite(value)) {
    issues.push_back("parameter '" + name + "' is not a number");
  } else if (value < low || value > high) {
    issues.push_back("parameter '" + name + "' must be in [" + std::to_string(low) + ", " +
                     std::to_string(high) + "]");
  } else if (integer && std::floor(value) != value) {
    issues.push_back("parameter '" + name + "' must be an integer");
  }
}

}  // namespace aafr::algorithms
