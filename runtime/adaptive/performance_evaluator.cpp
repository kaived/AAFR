#include "runtime/adaptive/performance_evaluator.h"

#include <algorithm>
#include <cmath>
#include <sstream>

#include "runtime/adaptive/statistical_arbitration.h"

namespace aafr::runtime {

namespace {
double Percentile(std::vector<double> v, double q) {
  std::sort(v.begin(), v.end());
  const double pos = q * static_cast<double>(v.size() - 1);
  const std::size_t i = static_cast<std::size_t>(std::floor(pos));
  const double frac = pos - static_cast<double>(i);
  return i + 1 < v.size() ? v[i] * (1.0 - frac) + v[i + 1] * frac : v[i];
}
std::string Fixed(double v) {
  std::ostringstream out;
  out.setf(std::ios::fixed);
  out.precision(3);
  out << v;
  return out.str();
}
}  // namespace

ForwardingPerformance ForwardingPerformanceEvaluator::Summarize(const std::vector<RequestSample>& samples) const {
  ForwardingPerformance p;
  p.requests = samples.size();
  std::vector<double> durations;
  double total_ms = 0.0;
  std::size_t delivered_bytes = 0;
  for (const RequestSample& s : samples) {
    if (s.timeout) ++p.timeouts;
    total_ms += s.duration_ms;
    if (s.success) {
      ++p.successes;
      durations.push_back(s.duration_ms);
      // The payload travels to the destination and its echo comes back.
      delivered_bytes += 2 * s.payload_bytes;
    } else {
      ++p.failures;
    }
  }
  if (p.requests == 0) {
    p.missing = {"latency: no requests", "jitter: no requests", "failure_rate: no requests", "goodput: no requests"};
    return p;
  }
  p.failure_rate = static_cast<double>(p.failures) / static_cast<double>(p.requests);
  double lo = 0.0, hi = 1.0;
  stats::Wilson(p.failures, p.requests, 0.95, lo, hi);
  p.failure_wilson_low = lo;
  p.failure_wilson_high = hi;
  if (durations.empty()) {
    p.missing.push_back("latency: no successful request");
    p.missing.push_back("jitter: no successful request");
  } else {
    p.latency_median_ms = Percentile(durations, 0.5);
    p.latency_p90_ms = Percentile(durations, 0.9);
    p.latency_mean_ms = stats::Mean(durations);
    if (durations.size() >= 2) {
      double sum = 0.0;
      for (std::size_t i = 1; i < durations.size(); ++i) sum += std::fabs(durations[i] - durations[i - 1]);
      p.jitter_ms = sum / static_cast<double>(durations.size() - 1);
    } else {
      p.missing.push_back("jitter: fewer than two successful requests");
    }
  }
  if (total_ms > 0.0 && delivered_bytes > 0) {
    p.goodput_bytes_per_s = static_cast<double>(delivered_bytes) / (total_ms / 1000.0);
  } else {
    p.missing.push_back("goodput: no delivered payload or zero request time");
  }
  return p;
}

ForwardingPerformance ForwardingPerformanceEvaluator::SummarizeArm(const ProbeCampaignResult& campaign,
                                                                  const std::string& arm_id) const {
  std::vector<RequestSample> samples;
  for (const ProbeObservation* o : campaign.ForArm(arm_id)) {
    samples.push_back({o->success, o->timeout, o->duration_ms, o->payload_bytes});
  }
  return Summarize(samples);
}

std::vector<ConstraintCheck> ForwardingPerformanceEvaluator::CheckConstraints(
    const ForwardingPerformance& performance, const forwarding::ForwardingProblem& problem) const {
  std::vector<ConstraintCheck> checks;
  ConstraintCheck failure{"failure_budget"};
  if (performance.failure_wilson_low) {
    failure.known = true;
    failure.satisfied = *performance.failure_wilson_low <= failure_budget_;
    failure.detail = "failure Wilson lower bound " + Fixed(*performance.failure_wilson_low) + " vs budget " +
                     Fixed(failure_budget_);
  } else {
    failure.detail = "unknown: no requests";
  }
  checks.push_back(failure);
  if (problem.constraints.latency_budget_ms) {
    ConstraintCheck latency{"latency_budget_ms"};
    if (performance.latency_median_ms) {
      latency.known = true;
      latency.satisfied = *performance.latency_median_ms <= *problem.constraints.latency_budget_ms;
      latency.detail = "median " + Fixed(*performance.latency_median_ms) + " ms vs budget " +
                       Fixed(*problem.constraints.latency_budget_ms) + " ms";
    } else {
      latency.detail = "unknown: no successful request";
    }
    checks.push_back(latency);
  }
  return checks;
}

bool ForwardingPerformanceEvaluator::Satisfies(const std::vector<ConstraintCheck>& checks) const {
  for (const ConstraintCheck& c : checks) {
    if (c.known && !c.satisfied) return false;
  }
  return true;
}

bool ForwardingPerformanceEvaluator::CompatibleObjectives(const ObjectiveRef& a, const ObjectiveRef& b,
                                                          std::string* why) {
  const auto refuse = [why](const std::string& reason) {
    if (why) *why = reason;
    return false;
  };
  if (!a.value || !b.value) return refuse("an objective value is missing");
  if (a.objective_version != b.objective_version) return refuse("objective versions differ");
  if (a.problem_id != b.problem_id) return refuse("problems differ");
  if (a.graph_version != b.graph_version) return refuse("graph versions differ");
  return true;
}

}  // namespace aafr::runtime
