// Research alignment, Phase 1: path metrics, the scores J, J_eng and G, and
// Welford / frozen normalization (research/). Independent of the live
// runtime. Reference values come from reference_values.h and from numpy via
// tests/research/scipy_reference.py. Deterministic; no network access.

#include <cmath>
#include <limits>
#include <string>
#include <tuple>

#include "research/normalization.h"
#include "research/path_metrics.h"
#include "tests/fixtures/research/reference_values.h"
#include "tests/fixtures/research/scipy_reference_values.h"
#include "tests/support/test_support.h"

using namespace aafr::research;
namespace rf = aafr::research_fixture;
namespace ref = aafr::research_reference;

namespace {

bool Near(double actual, double expected, double rel, double abs = 0.0) {
  return std::fabs(actual - expected) <= rel * std::fabs(expected) + abs;
}

LinkObservation Link(const char* from, const char* to, std::optional<double> delay, std::optional<double> loss,
                     std::optional<double> capacity, std::optional<double> utilization) {
  return LinkObservation{from, to, delay, loss, capacity, utilization};
}

std::vector<LinkObservation> ThreeLinks() {
  return {Link("a", "b", 2.0, 0.01, 100.0, 0.2), Link("b", "c", 5.0, 0.02, 50.0, 0.1),
          Link("c", "d", 1.5, 0.0, 80.0, 0.5)};
}

NormalizationStatistics Stats() {
  NormalizationStatistics s;
  s.source = "synthetic test statistics";
  const auto put = [&s](NormalizedFeature f, double mean, double sd) {
    s.features[f] = FeatureStatistics{100, mean, sd, 1};
  };
  put(NormalizedFeature::Delay, 10.0, 4.0);
  put(NormalizedFeature::Loss, 0.02, 0.01);
  put(NormalizedFeature::InverseBandwidth, 0.05, 0.02);
  put(NormalizedFeature::Hops, 4.0, 2.0);
  put(NormalizedFeature::Bandwidth, 30.0, 15.0);
  put(NormalizedFeature::Rtt, 25.0, 10.0);
  put(NormalizedFeature::Throughput, 28.0, 12.0);
  return s;
}

// ---- Path metrics ----

void AdditiveDelayAndHops() {
  const PathMetrics m = ComputePathMetrics(ThreeLinks());
  CHECK(m.path_valid);
  CHECK(m.issues.empty());
  CHECK(m.hops && *m.hops == 3);
  CHECK(m.delay_ms && Near(*m.delay_ms, 8.5, 1e-15));
}

void MultiplicativeLoss() {
  const PathMetrics m = ComputePathMetrics(ThreeLinks());
  // 1 - (0.99 * 0.98 * 1.0) = 0.0298, not the sum 0.03.
  CHECK(m.loss_ratio && Near(*m.loss_ratio, 1.0 - 0.99 * 0.98, 1e-14));
  CHECK(std::fabs(*m.loss_ratio - 0.03) > 1e-4);
  const PathMetrics lossless = ComputePathMetrics({Link("a", "b", 1.0, 0.0, 10.0, 0.0)});
  CHECK(lossless.loss_ratio && *lossless.loss_ratio == 0.0);
  const PathMetrics dead = ComputePathMetrics({Link("a", "b", 1.0, 1.0, 10.0, 0.0), Link("b", "c", 1.0, 0.0, 10.0, 0.0)});
  CHECK(dead.loss_ratio && *dead.loss_ratio == 1.0);
  CHECK(dead.throughput_mbps && *dead.throughput_mbps == 0.0);
}

void BottleneckBandwidthAndThroughput() {
  const PathMetrics m = ComputePathMetrics(ThreeLinks());
  // min(100 * 0.8, 50 * 0.9, 80 * 0.5) = min(80, 45, 40) = 40.
  CHECK(m.bandwidth_mbps && Near(*m.bandwidth_mbps, 40.0, 1e-15));
  CHECK(m.throughput_mbps && Near(*m.throughput_mbps, 40.0 * 0.99 * 0.98, 1e-14));
}

void ZeroAndUnavailableBandwidth() {
  // Fully utilized link: B = 0 is a value, and J / J_eng are undefined.
  const PathMetrics zero = ComputePathMetrics({Link("a", "b", 1.0, 0.0, 100.0, 1.0), Link("b", "c", 1.0, 0.0, 100.0, 0.0)});
  CHECK(zero.bandwidth_mbps && *zero.bandwidth_mbps == 0.0);
  CHECK(!QualityScore(zero).value);
  CHECK(aafr::test::Contains(QualityScore(zero).unavailable_reason, "zero"));
  CHECK(!EngineCost(zero, Stats()).value);
  const PathMetrics zero_capacity = ComputePathMetrics({Link("a", "b", 1.0, 0.0, 0.0, 0.0)});
  CHECK(zero_capacity.bandwidth_mbps && *zero_capacity.bandwidth_mbps == 0.0);
  // Missing capacity or utilization: B absent, never 0 and never infinite.
  const PathMetrics no_util = ComputePathMetrics({Link("a", "b", 1.0, 0.0, 100.0, std::nullopt)});
  CHECK(!no_util.bandwidth_mbps);
  CHECK(!no_util.throughput_mbps);
  CHECK(no_util.Has(PathIssue::MissingUtilization));
  CHECK(no_util.delay_ms && no_util.loss_ratio);  // Other metrics unaffected.
  const PathMetrics no_cap = ComputePathMetrics({Link("a", "b", 1.0, 0.0, std::nullopt, 0.1)});
  CHECK(!no_cap.bandwidth_mbps);
  CHECK(no_cap.Has(PathIssue::MissingCapacity));
}

void InvalidValues() {
  const double nan = std::numeric_limits<double>::quiet_NaN();
  const double inf = std::numeric_limits<double>::infinity();
  CHECK(ComputePathMetrics({Link("a", "b", 1.0, 1.2, 10.0, 0.0)}).Has(PathIssue::InvalidLoss));
  CHECK(ComputePathMetrics({Link("a", "b", 1.0, -0.1, 10.0, 0.0)}).Has(PathIssue::InvalidLoss));
  CHECK(ComputePathMetrics({Link("a", "b", 1.0, nan, 10.0, 0.0)}).Has(PathIssue::InvalidLoss));
  CHECK(ComputePathMetrics({Link("a", "b", 1.0, 0.0, 10.0, 1.5)}).Has(PathIssue::InvalidUtilization));
  CHECK(ComputePathMetrics({Link("a", "b", 1.0, 0.0, 10.0, -0.5)}).Has(PathIssue::InvalidUtilization));
  CHECK(ComputePathMetrics({Link("a", "b", -1.0, 0.0, 10.0, 0.0)}).Has(PathIssue::InvalidDelay));
  CHECK(ComputePathMetrics({Link("a", "b", inf, 0.0, 10.0, 0.0)}).Has(PathIssue::InvalidDelay));
  CHECK(ComputePathMetrics({Link("a", "b", 1.0, 0.0, -10.0, 0.0)}).Has(PathIssue::InvalidCapacity));
  const PathMetrics bad_loss = ComputePathMetrics({Link("a", "b", 1.0, 1.2, 10.0, 0.0)});
  CHECK(!bad_loss.loss_ratio && !bad_loss.throughput_mbps);
  CHECK(bad_loss.delay_ms && bad_loss.bandwidth_mbps);
}

void DisconnectedAndEmptyPaths() {
  const PathMetrics empty = ComputePathMetrics({});
  CHECK(!empty.path_valid && empty.Has(PathIssue::EmptyPath));
  const PathMetrics gap = ComputePathMetrics({Link("a", "b", 1.0, 0.0, 10.0, 0.0), Link("c", "d", 1.0, 0.0, 10.0, 0.0)});
  CHECK(!gap.path_valid && gap.Has(PathIssue::Disconnected));
  CHECK(!gap.delay_ms && !gap.hops && !gap.bandwidth_mbps);
  CHECK(!QualityScore(gap).value);
  const PathMetrics loop = ComputePathMetrics({Link("a", "b", 1.0, 0.0, 10.0, 0.0), Link("b", "a", 1.0, 0.0, 10.0, 0.0)});
  CHECK(!loop.path_valid && loop.Has(PathIssue::RepeatedNode));
}

void MissingDataIsNeverZero() {
  auto links = ThreeLinks();
  links[1].delay_ms.reset();
  links[2].loss_ratio.reset();
  const PathMetrics m = ComputePathMetrics(links);
  CHECK(m.path_valid);
  CHECK(!m.delay_ms && m.Has(PathIssue::MissingDelay));
  CHECK(!m.loss_ratio && m.Has(PathIssue::MissingLoss));
  CHECK(!m.throughput_mbps);  // Needs L.
  CHECK(m.bandwidth_mbps);
  const Score j = QualityScore(m);
  CHECK(!j.value && !j.unavailable_reason.empty());
  CHECK(!EngineCost(m, Stats()).value);
}

// ---- Scores ----

void QualityScoreWorkedCheck() {
  // 5 hops, bottleneck 598, delay 14, no loss.
  std::vector<LinkObservation> links;
  const char* nodes[] = {"n0", "n1", "n2", "n3", "n4", "n5"};
  const double delays[] = {2.0, 3.0, 3.0, 3.0, 3.0};
  const double capacities[] = {1000.0, rf::kJCheckBottleneck, 700.0, 900.0, 800.0};
  for (int i = 0; i < 5; ++i) links.push_back(Link(nodes[i], nodes[i + 1], delays[i], 0.0, capacities[i], 0.0));
  const PathMetrics m = ComputePathMetrics(links);
  CHECK(m.hops && *m.hops == static_cast<std::size_t>(rf::kJCheckHops));
  CHECK(m.delay_ms && *m.delay_ms == rf::kJCheckDelay);
  CHECK(m.bandwidth_mbps && *m.bandwidth_mbps == rf::kJCheckBottleneck);
  const Score j = QualityScore(m);
  CHECK(j.value && Near(*j.value, rf::kJCheckValue, 0.0, 5e-4));
  CHECK(j.value && Near(*j.value, 0.5 * 5 + 100.0 / 598.0 + 14.0, 1e-15));
}

void EngineCostUsesFrozenStatistics() {
  const PathMetrics m = ComputePathMetrics(ThreeLinks());
  const NormalizationStatistics s = Stats();
  const Score j = EngineCost(m, s);
  const double expected = 1.0 * (8.5 - 10.0) / 4.0 + 1.0 * (*m.loss_ratio - 0.02) / 0.01 +
                          1.0 * (1.0 / 40.0 - 0.05) / 0.02 + 0.3 * (3.0 - 4.0) / 2.0;
  CHECK(j.value && Near(*j.value, expected, 1e-13));
  // Weights are the specified ones and distinct from J.
  CHECK(EngineCostWeights{}.hop == rf::kJengHopWeight && EngineCostWeights{}.delay == rf::kJengDelayWeight);
  // Without the needed statistics there is no cost (no default normalization).
  NormalizationStatistics partial = s;
  partial.features.erase(NormalizedFeature::InverseBandwidth);
  const Score missing = EngineCost(m, partial);
  CHECK(!missing.value && aafr::test::Contains(missing.unavailable_reason, "inverse_bandwidth"));
  // J and J_eng are different quantities on the same path.
  CHECK(QualityScore(m).value && *QualityScore(m).value != *j.value);
}

void GoodnessAndDifference() {
  const NormalizationStatistics s = Stats();
  const ArmOutcome inc{2, 10.0, 81.5, 150.0 / 400.0};
  const ArmOutcome chal{2, 100.0, 20.4, 1.0};
  const Score gi = Goodness(inc, s);
  const Score gc = Goodness(chal, s);
  const auto g = [](double h, double b, double rtt, double t) {
    return -0.5 * (h - 4.0) / 2.0 + (b - 30.0) / 15.0 - (rtt - 25.0) / 10.0 + (t - 28.0) / 12.0;
  };
  CHECK(gi.value && Near(*gi.value, g(2, 10.0, 81.5, 10.0 * 150.0 / 400.0), 1e-13));
  CHECK(gc.value && Near(*gc.value, g(2, 100.0, 20.4, 100.0), 1e-13));
  const Score dg = GoodnessDifference(gc, gi);
  CHECK(dg.value && Near(*dg.value, *gc.value - *gi.value, 1e-15));
  CHECK(*dg.value > 0.0);
  CHECK(GoodnessWeights{}.hop == rf::kGHopWeight && GoodnessWeights{}.latency == rf::kGLatencyWeight);
  // Measured throughput uses the measured delivery ratio, not a predicted loss.
  ArmOutcome no_delivery = inc;
  no_delivery.delivery_ratio.reset();
  CHECK(!Goodness(no_delivery, s).value);
  CHECK(!GoodnessDifference(gc, Goodness(no_delivery, s)).value);
  ArmOutcome bad_rtt = inc;
  bad_rtt.mean_rtt_ms = 0.0;
  CHECK(!Goodness(bad_rtt, s).value);
}

// ---- Welford and frozen normalization ----

void WelfordMatchesNumpy() {
  for (const auto& [data, mean, var0, var1, rel] :
       {std::tuple{&ref::kWelfordLognormal, ref::kWelfordLognormalMean, ref::kWelfordLognormalVar0,
                   ref::kWelfordLognormalVar1, 1e-12},
        // Values near 1e9 with unit spread. Doubles there are spaced ~1.2e-7
        // apart, so each deviation x - mean already carries ~1e-7 relative
        // rounding; Welford agrees with numpy's two-pass variance to 1.4e-8
        // relative on this data (the naive E[x^2] - E[x]^2 has relative error 1).
        std::tuple{&ref::kWelfordOffset, ref::kWelfordOffsetMean, ref::kWelfordOffsetVar0,
                   ref::kWelfordOffsetVar1, 1e-6}}) {
    WelfordAccumulator acc;
    for (double x : *data) CHECK(acc.Add(x));
    CHECK(acc.count() == data->size());
    CHECK(Near(acc.mean(), mean, 1e-15));
    CHECK(acc.Variance(0) && Near(*acc.Variance(0), var0, rel));
    CHECK(acc.Variance(1) && Near(*acc.Variance(1), var1, rel));
  }
}

void WelfordIsStableWhereNaiveFails() {
  // The one-pass naive formula E[x^2] - E[x]^2 loses every significant digit
  // on the offset data; Welford does not.
  double sum = 0.0, sum_sq = 0.0;
  for (double x : ref::kWelfordOffset) {
    sum += x;
    sum_sq += x * x;
  }
  const double n = static_cast<double>(ref::kWelfordOffset.size());
  const double naive = sum_sq / n - (sum / n) * (sum / n);
  CHECK(!Near(naive, ref::kWelfordOffsetVar0, 1e-2));
  WelfordAccumulator acc;
  for (double x : ref::kWelfordOffset) acc.Add(x);
  CHECK(Near(*acc.Variance(0), ref::kWelfordOffsetVar0, 1e-6));
}

void WelfordEdgeCases() {
  WelfordAccumulator acc;
  CHECK(!acc.Variance(0) && !acc.Variance(1));
  CHECK(!acc.Add(std::numeric_limits<double>::quiet_NaN()));
  CHECK(!acc.Add(std::numeric_limits<double>::infinity()));
  CHECK(acc.count() == 0);
  CHECK(acc.Add(5.0));
  CHECK(acc.Variance(0) && *acc.Variance(0) == 0.0);
  CHECK(!acc.Variance(1));
  CHECK(!acc.Variance(-1));
}

void FrozenNormalizationContract() {
  WelfordAccumulator acc;
  for (double x : ref::kWelfordLognormal) acc.Add(x);
  CHECK(!Freeze(acc, 2));
  const auto frozen = Freeze(acc, 1);
  CHECK(frozen && frozen->ddof == 1 && frozen->count == ref::kWelfordLognormal.size());
  CHECK(Near(frozen->standard_deviation, std::sqrt(ref::kWelfordLognormalVar1), 1e-12));

  NormalizationStatistics s;
  s.source = "lognormal test data";
  s.features[NormalizedFeature::Rtt] = *frozen;
  CHECK(s.Validate().empty());
  CHECK(std::string(s.contract) == kNormalizationContract);
  const auto z = ZScore(s, NormalizedFeature::Rtt, ref::kWelfordLognormalMean + frozen->standard_deviation);
  CHECK(z && Near(*z, 1.0, 1e-12));
  // Frozen: z-scoring live values never changes the statistics.
  ZScore(s, NormalizedFeature::Rtt, 1e6);
  CHECK(s.features.at(NormalizedFeature::Rtt).mean == frozen->mean);
  // Refusals.
  CHECK(!ZScore(s, NormalizedFeature::Delay, 1.0));
  CHECK(!ZScore(s, NormalizedFeature::Rtt, std::numeric_limits<double>::quiet_NaN()));
  NormalizationStatistics undeclared = s;
  undeclared.features[NormalizedFeature::Rtt].ddof = -1;
  CHECK(!undeclared.Validate().empty());
  CHECK(!ZScore(undeclared, NormalizedFeature::Rtt, 1.0));
  NormalizationStatistics flat = s;
  flat.features[NormalizedFeature::Rtt].standard_deviation = 0.0;
  CHECK(!flat.Validate().empty());
  CHECK(!ZScore(flat, NormalizedFeature::Rtt, 1.0));
  NormalizationStatistics no_source = s;
  no_source.source.clear();
  CHECK(!no_source.Validate().empty());
  NormalizationStatistics wrong = s;
  wrong.contract = "other.v0";
  CHECK(!wrong.Validate().empty());
  CHECK(!ZScore(wrong, NormalizedFeature::Rtt, 1.0));
}

}  // namespace

int main() {
  return aafr::test::RunTests({
      {"additive_delay_and_hops", AdditiveDelayAndHops},
      {"multiplicative_loss", MultiplicativeLoss},
      {"bottleneck_bandwidth_and_throughput", BottleneckBandwidthAndThroughput},
      {"zero_and_unavailable_bandwidth", ZeroAndUnavailableBandwidth},
      {"invalid_values", InvalidValues},
      {"disconnected_and_empty_paths", DisconnectedAndEmptyPaths},
      {"missing_data_is_never_zero", MissingDataIsNeverZero},
      {"quality_score_worked_check", QualityScoreWorkedCheck},
      {"engine_cost_uses_frozen_statistics", EngineCostUsesFrozenStatistics},
      {"goodness_and_difference", GoodnessAndDifference},
      {"welford_matches_numpy", WelfordMatchesNumpy},
      {"welford_is_stable_where_naive_fails", WelfordIsStableWhereNaiveFails},
      {"welford_edge_cases", WelfordEdgeCases},
      {"frozen_normalization_contract", FrozenNormalizationContract},
  });
}
