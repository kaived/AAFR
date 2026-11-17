#pragma once

// Statistical arbitration of incumbent vs challenger probe evidence.
// Plain C++17, deterministic (the bootstrap uses a seeded portable RNG).
//
// It answers, per challenger: is there valid evidence that the challenger
// forwards faster or more reliably than the incumbent, how large is the
// effect, and how certain is its direction? It never switches anything and
// produces no "confidence probability".
//
// Method (chosen from the design of the measurements, see README):
//  - Pairing: probes are sent in rounds with every arm once per round in a
//    randomized order (ControlledProbeEngine), so round r of the incumbent
//    and of a challenger form a pair; drift shared within a round cancels.
//  - Latency effect: d_r = ln(challenger_r) - ln(incumbent_r) over rounds
//    where both probes succeeded. A negative d means the challenger is
//    faster; exp(d) is the latency ratio. Logs make multiplicative effects
//    additive and tame the right skew of latency.
//  - Independence: lag-1 autocorrelation r1 of d in round order. When
//    r1 > autocorrelation_limit the sample size is deflated to
//    n_eff = n (1 - r1) / (1 + r1) (standard errors and degrees of freedom
//    use n_eff); evidence with n_eff < min_effective_pairs is invalid.
//  - Distribution: sample skewness and excess kurtosis of d. Within limits:
//    paired t on the mean of d (t distribution, df = n_eff - 1). Otherwise a
//    seeded percentile bootstrap of the median of d (robust to heavy tails).
//  - Multiple challengers: Holm step-down adjustment of the p-values;
//    confidence intervals use the Bonferroni level 1 - alpha / m so the
//    direction claim holds simultaneously for all m challengers.
//  - Reliability: failure (non-delivered or unconfirmed) rate per arm over
//    all its probes, with Wilson score intervals. Latency of the surviving
//    probes cannot show loss, so when the incumbent's interval lies entirely
//    above the failure budget and the challenger's lies entirely below the
//    incumbent's, the challenger is better on reliability even at equal
//    latency (a "reliability repair"); a challenger whose interval lies
//    entirely above the budget, or entirely above the incumbent's, is never
//    better.
//  - Validity: every arm must come from the same campaign (same graph
//    version, payload size and executor) with interleaved rounds.
//
// ANOVA is not used: with one incumbent and one challenger per comparison a
// repeated-measures ANOVA is the paired t test (F = t^2), and its sphericity
// and normality assumptions add nothing the checks above do not cover.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "runtime/adaptive/controlled_probe_engine.h"
#include "runtime/adaptive/reason_codes.h"

namespace aafr::runtime {

struct ArbitrationConfig {
  double alpha{0.05};
  std::size_t min_pairs{8};
  double failure_budget{0.05};  // Highest acceptable failure ratio.
  std::size_t bootstrap_resamples{2000};
  std::uint64_t bootstrap_seed{7};
  double autocorrelation_limit{0.3};
  // Floor for the autocorrelation-deflated sample size; below it the serial
  // dependence leaves too little independent information to test.
  double min_effective_pairs{5.0};
  double skewness_limit{1.0};
  double excess_kurtosis_limit{3.0};

  std::vector<std::string> Validate() const;
};

enum class ArbitrationVerdict {
  ChallengerBetter,         // Significantly faster, or a reliability repair.
  ChallengerNotBetter,      // Significantly slower, or less reliable / over budget.
  NoSignificantDifference,  // Valid evidence that does not separate the arms.
  InsufficientEvidence,     // Too few valid pairs.
  InvalidEvidence,          // Assumptions or campaign design violated.
};

struct AssumptionChecks {
  bool same_campaign{false};
  bool interleaved{false};
  bool enough_pairs{false};
  std::optional<double> lag1_autocorrelation;
  bool autocorrelation_ok{false};
  double effective_pairs{0.0};
  std::optional<double> skewness;
  std::optional<double> excess_kurtosis;
  bool normality_ok{false};
  std::string summary;
};

struct ArmReliability {
  std::size_t probes{0};
  std::size_t failures{0};
  std::size_t timeouts{0};
  double failure_rate{0.0};
  double wilson_low{0.0};
  double wilson_high{1.0};
};

struct ComparisonResult {
  std::string challenger_arm;
  algorithms::AlgorithmId challenger_algorithm{algorithms::AlgorithmId::Dijkstra};
  ArmReliability incumbent;
  ArmReliability challenger;
  std::size_t pairs_used{0};
  std::size_t pairs_dropped{0};  // Rounds where either probe failed or is missing.
  std::string method;            // "paired_t_log_latency", "bootstrap_median_log_latency", "wilson_failure_rate", "none".
  AssumptionChecks assumptions;
  // Challenger minus incumbent, natural-log latency (negative: faster).
  std::optional<double> effect_log;
  std::optional<double> ci_low_log;
  std::optional<double> ci_high_log;
  double ci_level{0.95};
  std::optional<double> latency_ratio;  // exp(effect_log)
  std::optional<double> ratio_low;
  std::optional<double> ratio_high;
  // 1 - latency_ratio: share of incumbent latency saved (negative: slower).
  std::optional<double> improvement_relative;
  std::optional<double> improvement_relative_lower;  // From the CI bound nearest zero.
  std::optional<double> incumbent_median_ms;
  std::optional<double> challenger_median_ms;
  std::optional<double> p_value;
  std::optional<double> p_holm;
  bool reliability_repair{false};
  ArbitrationVerdict verdict{ArbitrationVerdict::InsufficientEvidence};
  std::vector<RuntimeReason> reasons;
  std::string explanation;

  bool EvidenceValid() const {
    return verdict != ArbitrationVerdict::InvalidEvidence && verdict != ArbitrationVerdict::InsufficientEvidence;
  }
};

struct ArbitrationResult {
  std::string scenario_id;
  std::string incumbent_arm;
  std::vector<ComparisonResult> comparisons;  // One per challenger arm, in arm order.
  std::string multiple_comparison{"holm"};
  // Index of the best challenger with verdict ChallengerBetter (largest
  // lower-bound improvement; a reliability repair first), if any.
  std::optional<std::size_t> best;
};

class StatisticalArbitrationEngine {
 public:
  explicit StatisticalArbitrationEngine(ArbitrationConfig config = {}) : config_(config) {}
  ArbitrationResult Evaluate(const ProbeCampaignResult& campaign) const;
  const ArbitrationConfig& config() const { return config_; }

 private:
  ArbitrationConfig config_;
};

const char* ToString(ArbitrationVerdict value);

// ---- Numerical helpers (exposed for tests) ----
namespace stats {
double Mean(const std::vector<double>& v);
double SampleVariance(const std::vector<double>& v);
double Median(std::vector<double> v);
std::optional<double> Lag1Autocorrelation(const std::vector<double>& v);
std::optional<double> Skewness(const std::vector<double>& v);
std::optional<double> ExcessKurtosis(const std::vector<double>& v);
// Regularized incomplete beta I_x(a, b).
double IncompleteBeta(double a, double b, double x);
// P(|T| >= |t|) for Student's t with df degrees of freedom (df may be fractional).
double TwoSidedTPValue(double t, double df);
// t such that P(|T| <= t) = level.
double TQuantileTwoSided(double level, double df);
// Wilson score interval for k successes in n trials at two-sided level.
void Wilson(std::size_t k, std::size_t n, double level, double& low, double& high);
double NormalQuantile(double p);
// Holm step-down adjusted p-values, same order as the input.
std::vector<double> HolmAdjust(const std::vector<double>& p);
}  // namespace stats

}  // namespace aafr::runtime
