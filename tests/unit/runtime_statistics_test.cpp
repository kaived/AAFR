// V0.6 statistical arbitration and multi-metric performance evaluation.

#include "runtime/adaptive/statistical_arbitration.h"

#include <cmath>

#include "runtime/adaptive/performance_evaluator.h"
#include "tests/support/runtime_test_support.h"
#include "tests/support/test_support.h"

using namespace aafr;
using namespace aafr::runtime;
using aafr::test::ArmSpec;
using aafr::test::Jitter;
using aafr::test::kPathA;
using aafr::test::kPathB;
using aafr::test::MakeCampaign;
using algorithms::AlgorithmId;

namespace {

const forwarding::Path kPathC = {"entry", "relay_a", "relay_b", "dest"};

ArmSpec Arm(const std::string& id, bool incumbent, const forwarding::Path& path,
            std::function<std::optional<double>(std::size_t)> d) {
  return {id, incumbent ? AlgorithmId::Dijkstra : AlgorithmId::BellmanFord, path, incumbent, std::move(d)};
}

// Multiplicative noise around `base` (about +/-5%) from a fixed seed.
std::function<std::optional<double>(std::size_t)> Around(double base, std::uint64_t seed, double spread = 0.05) {
  return [=](std::size_t r) -> std::optional<double> { return base * (1.0 + spread * Jitter(seed, r)); };
}

void NumericalHelpers() {
  CHECK_NEAR(stats::Mean({1, 2, 3, 4}), 2.5, 1e-12);
  CHECK_NEAR(stats::SampleVariance({1, 2, 3, 4}), 1.6666666667, 1e-9);
  CHECK_NEAR(stats::Median({5, 1, 3}), 3.0, 1e-12);
  CHECK_NEAR(stats::Median({4, 1, 3, 2}), 2.5, 1e-12);
  CHECK_NEAR(stats::IncompleteBeta(2.0, 2.0, 0.5), 0.5, 1e-12);
  CHECK_NEAR(stats::IncompleteBeta(1.0, 1.0, 0.3), 0.3, 1e-12);
  // Student t critical values (two-sided 95%): df 10 -> 2.2281, df 30 -> 2.0423.
  CHECK_NEAR(stats::TQuantileTwoSided(0.95, 10), 2.228139, 1e-5);
  CHECK_NEAR(stats::TQuantileTwoSided(0.95, 30), 2.042272, 1e-5);
  CHECK_NEAR(stats::TwoSidedTPValue(2.228139, 10), 0.05, 1e-5);
  CHECK_NEAR(stats::NormalQuantile(0.975), 1.959964, 1e-5);
  // Wilson 95% for 0/10: [0, 0.2775].
  double lo = 0, hi = 0;
  stats::Wilson(0, 10, 0.95, lo, hi);
  CHECK_NEAR(lo, 0.0, 1e-12);
  CHECK_NEAR(hi, 0.27753, 1e-4);
  stats::Wilson(5, 10, 0.95, lo, hi);
  CHECK_NEAR(lo, 0.23659, 1e-4);
  CHECK_NEAR(hi, 0.76341, 1e-4);
  // Holm: [0.01, 0.04, 0.03] -> [0.03, 0.06, 0.06].
  const auto holm = stats::HolmAdjust({0.01, 0.04, 0.03});
  CHECK_NEAR(holm[0], 0.03, 1e-12);
  CHECK_NEAR(holm[1], 0.06, 1e-12);
  CHECK_NEAR(holm[2], 0.06, 1e-12);
  CHECK(stats::Lag1Autocorrelation({1, 2, 3, 4, 5, 6}).value() > 0.4);
  CHECK(stats::Lag1Autocorrelation({1, -1, 1, -1, 1, -1}).value() < -0.5);
  CHECK(!stats::Skewness({1, 2}).has_value());
}

void ValidPairedComparisonFasterChallenger() {
  // Challenger 20% faster with independent 5% noise.
  const auto campaign = MakeCampaign({Arm("incumbent", true, kPathA, Around(50.0, 1)),
                                      Arm("challenger1", false, kPathB, Around(40.0, 2))},
                                     16);
  const ArbitrationResult r = StatisticalArbitrationEngine().Evaluate(campaign);
  CHECK(r.comparisons.size() == 1);
  const ComparisonResult& c = r.comparisons[0];
  CHECK(c.verdict == ArbitrationVerdict::ChallengerBetter);
  CHECK(c.method == "paired_t_log_latency");
  CHECK(c.pairs_used == 16);
  CHECK(c.assumptions.normality_ok);
  CHECK(c.assumptions.interleaved);
  // Direction: challenger faster means a negative log effect, ratio < 1.
  CHECK(*c.effect_log < 0.0);
  CHECK(*c.latency_ratio < 1.0);
  // The CI covers the true ratio 0.8 and excludes 1.
  CHECK(*c.ratio_low <= 0.8 + 0.02 && *c.ratio_high >= 0.8 - 0.02);
  CHECK(*c.ratio_high < 1.0);
  CHECK(*c.improvement_relative > 0.15 && *c.improvement_relative < 0.25);
  CHECK(*c.improvement_relative_lower > 0.0);
  CHECK(r.best && *r.best == 0);
  CHECK(std::find(c.reasons.begin(), c.reasons.end(), RuntimeReason::ImprovementConfirmed) != c.reasons.end());
}

void ConfidenceIntervalMatchesHandCalculation() {
  // Constant +-1% alternation: d_r = ln(40.4/50) or ln(39.6/50) etc.
  std::vector<double> inc, chal;
  for (std::size_t r = 0; r < 10; ++r) {
    inc.push_back(50.0 * (1.0 + 0.02 * Jitter(3, r)));
    chal.push_back(45.0 * (1.0 + 0.02 * Jitter(4, r)));
  }
  const auto campaign = MakeCampaign({Arm("incumbent", true, kPathA, [&](std::size_t r) { return inc[r]; }),
                                      Arm("challenger1", false, kPathB, [&](std::size_t r) { return chal[r]; })},
                                     10);
  const ComparisonResult c = StatisticalArbitrationEngine().Evaluate(campaign).comparisons[0];
  std::vector<double> d;
  for (std::size_t r = 0; r < 10; ++r) d.push_back(std::log(chal[r]) - std::log(inc[r]));
  const double mean = stats::Mean(d);
  const double se = std::sqrt(stats::SampleVariance(d) / 10.0);
  const double q = stats::TQuantileTwoSided(0.95, 9.0);
  CHECK(c.method == "paired_t_log_latency");
  CHECK_NEAR(*c.effect_log, mean, 1e-12);
  CHECK_NEAR(*c.ci_low_log, mean - q * se, 1e-9);
  CHECK_NEAR(*c.ci_high_log, mean + q * se, 1e-9);
  CHECK_NEAR(c.ci_level, 0.95, 1e-12);
}

void InsufficientSamples() {
  const auto campaign = MakeCampaign({Arm("incumbent", true, kPathA, Around(50.0, 1)),
                                      Arm("challenger1", false, kPathB, Around(25.0, 2))},
                                     5);
  const ComparisonResult c = StatisticalArbitrationEngine().Evaluate(campaign).comparisons[0];
  CHECK(c.verdict == ArbitrationVerdict::InsufficientEvidence);
  CHECK(!c.EvidenceValid());
  CHECK(std::find(c.reasons.begin(), c.reasons.end(), RuntimeReason::InsufficientSamples) != c.reasons.end());
  CHECK(!c.p_value.has_value());
}

void EqualLatencyDifferentLoss() {
  // Same latency; the incumbent loses every third probe (33%), the challenger none.
  const auto campaign = MakeCampaign(
      {Arm("incumbent", true, kPathA,
           [](std::size_t r) -> std::optional<double> {
             if (r % 3 == 0) return std::nullopt;
             return 30.0 * (1.0 + 0.03 * Jitter(5, r));
           }),
       Arm("challenger1", false, kPathB, Around(30.0, 6, 0.03))},
      30);
  const ComparisonResult c = StatisticalArbitrationEngine().Evaluate(campaign).comparisons[0];
  CHECK(c.reliability_repair);
  CHECK(c.verdict == ArbitrationVerdict::ChallengerBetter);
  CHECK(c.method == "wilson_failure_rate");
  CHECK(c.incumbent.failures == 10);
  CHECK(c.incumbent.wilson_low > 0.05);
  CHECK(c.challenger.failures == 0);
  CHECK(c.challenger.wilson_high < c.incumbent.wilson_low);
  CHECK(c.pairs_dropped == 10);
}

void DifferentLatencyEqualLoss() {
  // No loss anywhere; the challenger is 30% slower.
  const auto campaign = MakeCampaign({Arm("incumbent", true, kPathA, Around(20.0, 7)),
                                      Arm("challenger1", false, kPathB, Around(26.0, 8))},
                                     12);
  const ComparisonResult c = StatisticalArbitrationEngine().Evaluate(campaign).comparisons[0];
  CHECK(!c.reliability_repair);
  CHECK(c.verdict == ArbitrationVerdict::ChallengerNotBetter);
  CHECK(*c.effect_log > 0.0);
  CHECK(*c.improvement_relative < 0.0);
}

void NoSignificantDifference() {
  const auto campaign = MakeCampaign({Arm("incumbent", true, kPathA, Around(20.0, 9, 0.1)),
                                      Arm("challenger1", false, kPathB, Around(20.0, 10, 0.1))},
                                     12);
  const ComparisonResult c = StatisticalArbitrationEngine().Evaluate(campaign).comparisons[0];
  CHECK(c.verdict == ArbitrationVerdict::NoSignificantDifference);
  CHECK(c.EvidenceValid());
  CHECK(*c.ci_low_log < 0.0 && *c.ci_high_log > 0.0);
}

void HeavyTailedUsesBootstrap() {
  // Occasional 8x outliers on the incumbent only: heavy-tailed differences.
  const auto campaign = MakeCampaign(
      {Arm("incumbent", true, kPathA,
           [](std::size_t r) -> std::optional<double> {
             const double base = 40.0 * (1.0 + 0.03 * Jitter(11, r));
             return r % 7 == 3 ? base * 8.0 : base;
           }),
       Arm("challenger1", false, kPathB, Around(30.0, 12, 0.03))},
      21);
  const ComparisonResult c = StatisticalArbitrationEngine().Evaluate(campaign).comparisons[0];
  CHECK(!c.assumptions.normality_ok);
  CHECK(c.method == "bootstrap_median_log_latency");
  CHECK(c.verdict == ArbitrationVerdict::ChallengerBetter);
  // The median-based effect is near ln(0.75), not dragged by the outliers.
  CHECK_NEAR(*c.latency_ratio, 0.75, 0.05);
  // Deterministic: the same campaign gives the same bootstrap interval.
  const ComparisonResult again = StatisticalArbitrationEngine().Evaluate(campaign).comparisons[0];
  CHECK(*again.ci_low_log == *c.ci_low_log && *again.ci_high_log == *c.ci_high_log);
}

void MultipleComparisonsUseHolm() {
  const auto campaign = MakeCampaign({Arm("incumbent", true, kPathA, Around(50.0, 13, 0.08)),
                                      Arm("challenger1", false, kPathB, Around(45.0, 14, 0.08)),
                                      Arm("challenger2", false, kPathC, Around(30.0, 15, 0.08))},
                                     16);
  const ArbitrationResult r = StatisticalArbitrationEngine().Evaluate(campaign);
  CHECK(r.comparisons.size() == 2);
  for (const auto& c : r.comparisons) {
    CHECK(c.p_holm.has_value());
    CHECK(*c.p_holm >= *c.p_value - 1e-15);
    // Simultaneous (Bonferroni) interval level for m = 2.
    CHECK_NEAR(c.ci_level, 0.975, 1e-12);
  }
  CHECK(r.best && r.comparisons[*r.best].challenger_arm == "challenger2");
}

void InvalidAssumptions() {
  // Blocked design: every incumbent probe before any challenger probe.
  auto blocked = MakeCampaign({Arm("incumbent", true, kPathA, Around(50.0, 16)),
                               Arm("challenger1", false, kPathB, Around(30.0, 17))},
                              12, true);
  ComparisonResult c = StatisticalArbitrationEngine().Evaluate(blocked).comparisons[0];
  CHECK(c.verdict == ArbitrationVerdict::InvalidEvidence);
  CHECK(!c.assumptions.interleaved);
  // Strong serial correlation (a drifting difference) leaves too few effective pairs.
  auto drifting = MakeCampaign({Arm("incumbent", true, kPathA, [](std::size_t) -> std::optional<double> { return 50.0; }),
                                Arm("challenger1", false, kPathB,
                                    [](std::size_t r) -> std::optional<double> { return 30.0 + 2.0 * r; })},
                               12);
  c = StatisticalArbitrationEngine().Evaluate(drifting).comparisons[0];
  CHECK(c.assumptions.lag1_autocorrelation.value() > 0.3);
  CHECK(c.verdict == ArbitrationVerdict::InvalidEvidence);
  CHECK(std::find(c.reasons.begin(), c.reasons.end(), RuntimeReason::StatisticalEvidenceInvalid) != c.reasons.end());
  // Arms from different campaigns (graph versions differ).
  auto mixed = MakeCampaign({Arm("incumbent", true, kPathA, Around(50.0, 18)),
                             Arm("challenger1", false, kPathB, Around(30.0, 19))},
                            12);
  mixed.observations[3].graph_version = 2;
  c = StatisticalArbitrationEngine().Evaluate(mixed).comparisons[0];
  CHECK(c.verdict == ArbitrationVerdict::InvalidEvidence);
  CHECK(!c.assumptions.same_campaign);
}

void MissingDataAndUnreliableChallenger() {
  // Missing observations (removed probes) are dropped from pairing.
  auto campaign = MakeCampaign({Arm("incumbent", true, kPathA, Around(50.0, 20)),
                                Arm("challenger1", false, kPathB, Around(35.0, 21))},
                               14);
  campaign.observations.erase(campaign.observations.begin() + 5);
  ComparisonResult c = StatisticalArbitrationEngine().Evaluate(campaign).comparisons[0];
  CHECK(c.pairs_dropped == 1);
  CHECK(c.pairs_used == 13);
  CHECK(c.verdict == ArbitrationVerdict::ChallengerBetter);
  // A challenger that fails often is never better, however fast it is.
  auto lossy = MakeCampaign({Arm("incumbent", true, kPathA, Around(50.0, 22)),
                             Arm("challenger1", false, kPathB,
                                 [](std::size_t r) -> std::optional<double> {
                                   if (r % 2 == 0) return std::nullopt;
                                   return 10.0;
                                 })},
                            20);
  c = StatisticalArbitrationEngine().Evaluate(lossy).comparisons[0];
  CHECK(c.verdict == ArbitrationVerdict::ChallengerNotBetter);
  CHECK(c.challenger.wilson_low > 0.05);
}

void PerformanceEvaluator() {
  ForwardingPerformanceEvaluator e(0.05);
  ForwardingPerformance p = e.Summarize({});
  CHECK(!p.latency_median_ms && !p.failure_rate);
  CHECK(p.missing.size() == 4);
  p = e.Summarize({{true, false, 10.0, 64}, {true, false, 14.0, 64}, {false, true, 100.0, 64}, {true, false, 12.0, 64}});
  CHECK(p.requests == 4 && p.successes == 3 && p.failures == 1 && p.timeouts == 1);
  CHECK_NEAR(*p.latency_median_ms, 12.0, 1e-12);
  CHECK_NEAR(*p.jitter_ms, 3.0, 1e-12);  // |14-10| and |12-14|
  CHECK_NEAR(*p.failure_rate, 0.25, 1e-12);
  CHECK_NEAR(*p.goodput_bytes_per_s, 3.0 * 128.0 / (136.0 / 1000.0), 1e-6);
  forwarding::ForwardingProblem problem;
  problem.constraints.latency_budget_ms = 11.0;
  const auto checks = e.CheckConstraints(p, problem);
  CHECK(checks.size() == 2);
  CHECK(!e.Satisfies(checks));  // median 12 > 11 and the failure lower bound is 0.046 < 0.05
  std::string why;
  CHECK(ForwardingPerformanceEvaluator::CompatibleObjectives({"p", 3, 1, 5.0}, {"p", 3, 1, 6.0}, &why));
  CHECK(!ForwardingPerformanceEvaluator::CompatibleObjectives({"p", 3, 1, 5.0}, {"p", 4, 1, 6.0}, &why));
  CHECK(why == "graph versions differ");
  CHECK(!ForwardingPerformanceEvaluator::CompatibleObjectives({"p", 3, 1, 5.0}, {"p", 3, 2, 6.0}, &why));
  CHECK(!ForwardingPerformanceEvaluator::CompatibleObjectives({"p", 3, 1, std::nullopt}, {"p", 3, 1, 6.0}, &why));
}

}  // namespace

int main() {
  return aafr::test::RunTests({
      {"numerical_helpers", NumericalHelpers},
      {"valid_paired_comparison", ValidPairedComparisonFasterChallenger},
      {"confidence_interval_matches_hand_calculation", ConfidenceIntervalMatchesHandCalculation},
      {"insufficient_samples", InsufficientSamples},
      {"equal_latency_different_loss", EqualLatencyDifferentLoss},
      {"different_latency_equal_loss", DifferentLatencyEqualLoss},
      {"no_significant_difference", NoSignificantDifference},
      {"heavy_tailed_uses_bootstrap", HeavyTailedUsesBootstrap},
      {"multiple_comparisons_use_holm", MultipleComparisonsUseHolm},
      {"invalid_assumptions", InvalidAssumptions},
      {"missing_data_and_unreliable_challenger", MissingDataAndUnreliableChallenger},
      {"performance_evaluator", PerformanceEvaluator},
  });
}
