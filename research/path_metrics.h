#pragma once

// Path metrics and path scores of the research method. Plain C++17, pure
// functions, no I/O and no runtime state. Not used by the live runtime.
//
// For a path P over links e:
//   D(P) = sum d_e                    additive one-way delay (ms)
//   L(P) = 1 - prod (1 - eps_e)       end-to-end loss (ratio)
//   B(P) = min c_e (1 - u_e)          available bottleneck bandwidth (Mb/s)
//   T(P) = B(P) (1 - L(P))            estimated goodput (Mb/s)
//   H(P) = number of links
//
// Three scores, each with its own role, never interchangeable:
//   J      independent quality score, J = 0.5 H + 100 / B + D + 100 L
//          (lower is better). Evaluates tested paths only.
//   J_eng  engine cost of ACO and GA,
//          J_eng = z(D) + z(L) + z(1/B) + 0.3 z(H), z from frozen
//          training-set statistics (normalization.h).
//   G      goodness of a probed arm from measured outcomes,
//          G = -0.5 z(H) + z(B) - z(RTT) + z(T_measured), with
//          T_measured = B * measured delivery ratio. Only
//          dG = G_challenger - G_incumbent enters a decision.
//
// A value that is missing or invalid is never replaced by zero or any other
// default: the metric (and every score that needs it) is absent, with the
// reason recorded.

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include "research/normalization.h"

namespace aafr::research {

// One directed link of a path. nullopt: not available.
struct LinkObservation {
  std::string from;
  std::string to;
  std::optional<double> delay_ms;       // One-way, finite, >= 0.
  std::optional<double> loss_ratio;     // Finite, within [0, 1].
  std::optional<double> capacity_mbps;  // Finite, >= 0.
  std::optional<double> utilization;    // Finite, within [0, 1].
};

enum class PathIssue {
  EmptyPath,
  Disconnected,  // A link does not start where the previous one ended.
  RepeatedNode,  // The links do not form a simple path.
  MissingDelay,
  MissingLoss,
  MissingCapacity,
  MissingUtilization,
  InvalidDelay,
  InvalidLoss,
  InvalidCapacity,
  InvalidUtilization,
};

struct PathMetrics {
  // True when the links form one connected simple path. When false, no
  // metric is computed.
  bool path_valid{false};
  std::optional<double> delay_ms;         // D
  std::optional<double> loss_ratio;       // L
  std::optional<double> bandwidth_mbps;   // B (0 is a valid value: no headroom)
  std::optional<double> throughput_mbps;  // T
  std::optional<std::size_t> hops;        // H
  std::vector<PathIssue> issues;

  bool Has(PathIssue issue) const;
};

PathMetrics ComputePathMetrics(const std::vector<LinkObservation>& links);

// A score, or why it could not be computed.
struct Score {
  std::optional<double> value;
  std::string unavailable_reason;  // Empty when value is set.
};

struct QualityScoreWeights {
  double hop{0.5};
  double inverse_bandwidth{100.0};  // Multiplies 1 / B.
  double delay{1.0};
  double loss{100.0};
};

// J. Needs H, B > 0, D and L. B = 0 makes J undefined (not infinite).
Score QualityScore(const PathMetrics& metrics, const QualityScoreWeights& weights = {});

struct EngineCostWeights {
  double delay{1.0};
  double loss{1.0};
  double inverse_bandwidth{1.0};
  double hop{0.3};
};

// J_eng. Needs D, L, B > 0, H and frozen statistics for Delay, Loss,
// InverseBandwidth and Hops.
Score EngineCost(const PathMetrics& metrics, const NormalizationStatistics& statistics,
                 const EngineCostWeights& weights = {});

// Measured outcome of one probed arm (not a prediction).
struct ArmOutcome {
  std::optional<std::size_t> hops;
  std::optional<double> bandwidth_mbps;   // Available bottleneck bandwidth of the arm's path.
  std::optional<double> mean_rtt_ms;      // Arithmetic mean RTT of the returned probes.
  std::optional<double> delivery_ratio;   // Returned probes / sent probes, within [0, 1].
};

struct GoodnessWeights {
  double hop{-0.5};
  double bandwidth{1.0};
  double latency{-1.0};
  double throughput{1.0};
};

// G. Needs every ArmOutcome field and frozen statistics for Hops, Bandwidth,
// Rtt and Throughput.
Score Goodness(const ArmOutcome& outcome, const NormalizationStatistics& statistics,
               const GoodnessWeights& weights = {});

// dG = G_challenger - G_incumbent; absent when either is absent.
Score GoodnessDifference(const Score& challenger, const Score& incumbent);

const char* ToString(PathIssue issue);

}  // namespace aafr::research
