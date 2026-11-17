#pragma once

// Multi-metric forwarding performance. Plain C++17.
//
// Two kinds of numbers exist and are never mixed:
//  1. The V0.5 objective J(P) (ForwardingObjective v1): a model estimate
//     computed from the graph's configured or simulated edge values. Two
//     J(P) values are comparable only for the same problem, graph version
//     and objective version (CompatibleObjectives).
//  2. Measured forwarding metrics from probe or outcome observations
//     (MeasuredMetricsVersion "aafr.measured_forwarding.v1"):
//       latency      median / p90 / mean of successful request durations, ms, lower is better
//       jitter       mean absolute difference of consecutive successful durations, ms, lower is better
//       failure      share of requests not delivered over a confirmed path, ratio 0..1, lower is better
//       goodput      application payload bytes delivered per second of request time, B/s, higher is better
//                    (loopback goodput of this request pattern, not link capacity)
//     Missing values stay empty (no samples -> no latency), never zero.
//
// Hard constraints (checked, never weighted): the problem's latency budget
// against the measured median latency, and the failure budget against the
// Wilson lower bound of the failure ratio. No weighted composite of
// measured metrics is formed; arbitration compares latency and reliability
// separately, and computation and switching overheads are reported beside
// them.

#include <optional>
#include <string>
#include <vector>

#include "routing/topology/forwarding_problem.h"
#include "runtime/adaptive/controlled_probe_engine.h"

namespace aafr::runtime {

inline constexpr const char* kMeasuredMetricsVersion = "aafr.measured_forwarding.v1";

struct RequestSample {
  bool success{false};
  bool timeout{false};
  double duration_ms{0.0};
  std::size_t payload_bytes{0};
};

struct ForwardingPerformance {
  std::string metrics_version{kMeasuredMetricsVersion};
  std::size_t requests{0};
  std::size_t successes{0};
  std::size_t failures{0};
  std::size_t timeouts{0};
  std::optional<double> latency_median_ms;
  std::optional<double> latency_p90_ms;
  std::optional<double> latency_mean_ms;
  std::optional<double> jitter_ms;
  std::optional<double> failure_rate;
  std::optional<double> failure_wilson_low;
  std::optional<double> failure_wilson_high;
  std::optional<double> goodput_bytes_per_s;
  std::vector<std::string> missing;  // Metric names that could not be computed, with why.
};

struct ConstraintCheck {
  std::string constraint;
  bool known{false};
  bool satisfied{true};
  std::string detail;
};

struct ObjectiveRef {
  std::string problem_id;
  std::uint64_t graph_version{0};
  int objective_version{forwarding::ForwardingObjective::kVersion};
  std::optional<double> value;
};

class ForwardingPerformanceEvaluator {
 public:
  explicit ForwardingPerformanceEvaluator(double failure_budget = 0.05) : failure_budget_(failure_budget) {}

  ForwardingPerformance Summarize(const std::vector<RequestSample>& samples) const;
  ForwardingPerformance SummarizeArm(const ProbeCampaignResult& campaign, const std::string& arm_id) const;

  // Hard constraints on measured performance for a problem.
  std::vector<ConstraintCheck> CheckConstraints(const ForwardingPerformance& performance,
                                                const forwarding::ForwardingProblem& problem) const;
  bool Satisfies(const std::vector<ConstraintCheck>& checks) const;

  // Whether two J(P) values may be compared; `why` explains a refusal.
  static bool CompatibleObjectives(const ObjectiveRef& a, const ObjectiveRef& b, std::string* why = nullptr);
  double failure_budget() const { return failure_budget_; }

 private:
  double failure_budget_;
};

}  // namespace aafr::runtime
