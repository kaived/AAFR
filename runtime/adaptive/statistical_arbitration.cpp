#include "runtime/adaptive/statistical_arbitration.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <numeric>
#include <sstream>

#include "algorithms/common/path_algorithm.h"

namespace aafr::runtime {

namespace stats {

double Mean(const std::vector<double>& v) {
  if (v.empty()) return std::numeric_limits<double>::quiet_NaN();
  return std::accumulate(v.begin(), v.end(), 0.0) / static_cast<double>(v.size());
}

double SampleVariance(const std::vector<double>& v) {
  if (v.size() < 2) return 0.0;
  const double m = Mean(v);
  double s = 0.0;
  for (double x : v) s += (x - m) * (x - m);
  return s / static_cast<double>(v.size() - 1);
}

double Median(std::vector<double> v) {
  if (v.empty()) return std::numeric_limits<double>::quiet_NaN();
  std::sort(v.begin(), v.end());
  const std::size_t n = v.size();
  return n % 2 ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
}

std::optional<double> Lag1Autocorrelation(const std::vector<double>& v) {
  if (v.size() < 3) return std::nullopt;
  const double m = Mean(v);
  double num = 0.0, den = 0.0;
  for (std::size_t i = 0; i < v.size(); ++i) {
    den += (v[i] - m) * (v[i] - m);
    if (i + 1 < v.size()) num += (v[i] - m) * (v[i + 1] - m);
  }
  if (den <= 0.0) return 0.0;  // Constant series: no serial dependence to measure.
  return num / den;
}

namespace {
std::optional<double> CentralMomentRatio(const std::vector<double>& v, int order) {
  if (v.size() < 4) return std::nullopt;
  const double m = Mean(v);
  double m2 = 0.0, mk = 0.0;
  for (double x : v) {
    const double d = x - m;
    m2 += d * d;
    mk += std::pow(d, order);
  }
  const double n = static_cast<double>(v.size());
  m2 /= n;
  mk /= n;
  if (m2 <= 0.0) return 0.0;
  return mk / std::pow(m2, order / 2.0);
}
}  // namespace

std::optional<double> Skewness(const std::vector<double>& v) { return CentralMomentRatio(v, 3); }

std::optional<double> ExcessKurtosis(const std::vector<double>& v) {
  const auto k = CentralMomentRatio(v, 4);
  if (!k) return std::nullopt;
  return *k - 3.0;
}

namespace {
// Continued fraction for I_x(a, b), modified Lentz's method (DLMF 8.17.22).
double BetaContinuedFraction(double a, double b, double x) {
  constexpr int kMaxIterations = 500;
  constexpr double kEpsilon = 1e-15;
  constexpr double kTiny = 1e-300;
  double c = 1.0;
  double d = 1.0 - (a + b) * x / (a + 1.0);
  if (std::fabs(d) < kTiny) d = kTiny;
  d = 1.0 / d;
  double f = d;
  for (int m = 1; m <= kMaxIterations; ++m) {
    const double m2 = 2.0 * m;
    // Even step.
    double numerator = m * (b - m) * x / ((a + m2 - 1.0) * (a + m2));
    d = 1.0 + numerator * d;
    if (std::fabs(d) < kTiny) d = kTiny;
    c = 1.0 + numerator / c;
    if (std::fabs(c) < kTiny) c = kTiny;
    d = 1.0 / d;
    f *= d * c;
    // Odd step.
    numerator = -(a + m) * (a + b + m) * x / ((a + m2) * (a + m2 + 1.0));
    d = 1.0 + numerator * d;
    if (std::fabs(d) < kTiny) d = kTiny;
    c = 1.0 + numerator / c;
    if (std::fabs(c) < kTiny) c = kTiny;
    d = 1.0 / d;
    const double delta = d * c;
    f *= delta;
    if (std::fabs(delta - 1.0) < kEpsilon) break;
  }
  return f;
}
}  // namespace

double IncompleteBeta(double a, double b, double x) {
  if (x <= 0.0) return 0.0;
  if (x >= 1.0) return 1.0;
  const double log_front = std::lgamma(a + b) - std::lgamma(a) - std::lgamma(b) + a * std::log(x) + b * std::log1p(-x);
  const double front = std::exp(log_front);
  // The continued fraction converges fast for x < (a + 1) / (a + b + 2);
  // otherwise use the symmetry I_x(a, b) = 1 - I_{1-x}(b, a).
  if (x < (a + 1.0) / (a + b + 2.0)) return front * BetaContinuedFraction(a, b, x) / a;
  return 1.0 - front * BetaContinuedFraction(b, a, 1.0 - x) / b;
}

double TwoSidedTPValue(double t, double df) {
  if (!std::isfinite(t)) return 0.0;
  if (!(df > 0.0)) return 1.0;
  return IncompleteBeta(0.5 * df, 0.5, df / (df + t * t));
}

double TQuantileTwoSided(double level, double df) {
  const double target = 1.0 - level;  // Two-sided tail mass.
  double lo = 0.0, hi = 1e6;
  for (int i = 0; i < 300; ++i) {
    const double mid = 0.5 * (lo + hi);
    if (TwoSidedTPValue(mid, df) > target) lo = mid;
    else hi = mid;
  }
  return 0.5 * (lo + hi);
}

double NormalQuantile(double p) {
  if (p <= 0.0) return -std::numeric_limits<double>::infinity();
  if (p >= 1.0) return std::numeric_limits<double>::infinity();
  double lo = -40.0, hi = 40.0;
  for (int i = 0; i < 200; ++i) {
    const double mid = 0.5 * (lo + hi);
    const double cdf = 0.5 * std::erfc(-mid / std::sqrt(2.0));
    if (cdf < p) lo = mid;
    else hi = mid;
  }
  return 0.5 * (lo + hi);
}

void Wilson(std::size_t k, std::size_t n, double level, double& low, double& high) {
  if (n == 0) {
    low = 0.0;
    high = 1.0;
    return;
  }
  const double z = NormalQuantile(1.0 - (1.0 - level) / 2.0);
  const double p = static_cast<double>(k) / static_cast<double>(n);
  const double nn = static_cast<double>(n);
  const double denominator = 1.0 + z * z / nn;
  const double centre = (p + z * z / (2.0 * nn)) / denominator;
  const double half = z * std::sqrt(p * (1.0 - p) / nn + z * z / (4.0 * nn * nn)) / denominator;
  low = std::max(0.0, centre - half);
  high = std::min(1.0, centre + half);
}

std::vector<double> HolmAdjust(const std::vector<double>& p) {
  const std::size_t m = p.size();
  std::vector<std::size_t> order(m);
  std::iota(order.begin(), order.end(), std::size_t{0});
  std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) { return p[a] < p[b]; });
  std::vector<double> adjusted(m, 1.0);
  double running = 0.0;
  for (std::size_t rank = 0; rank < m; ++rank) {
    const double value = std::min(1.0, static_cast<double>(m - rank) * p[order[rank]]);
    running = std::max(running, value);
    adjusted[order[rank]] = running;
  }
  return adjusted;
}

}  // namespace stats

std::vector<std::string> ArbitrationConfig::Validate() const {
  std::vector<std::string> issues;
  if (!(alpha > 0.0 && alpha < 0.5)) issues.push_back("alpha must be within (0, 0.5)");
  if (min_pairs < 4) issues.push_back("min_pairs must be at least 4");
  if (!(failure_budget >= 0.0 && failure_budget < 1.0)) issues.push_back("failure_budget must be within [0, 1)");
  if (bootstrap_resamples < 200 || bootstrap_resamples > 100'000) issues.push_back("bootstrap_resamples must be within 200..100000");
  if (!(autocorrelation_limit > 0.0 && autocorrelation_limit < 1.0)) issues.push_back("autocorrelation_limit must be within (0, 1)");
  if (!(min_effective_pairs >= 3.0) || min_effective_pairs > static_cast<double>(min_pairs)) {
    issues.push_back("min_effective_pairs must be within 3..min_pairs");
  }
  return issues;
}

namespace {

std::string Fixed(double v, int precision = 4) {
  std::ostringstream out;
  out.setf(std::ios::fixed);
  out.precision(precision);
  out << v;
  return out.str();
}

ArmReliability Reliability(const std::vector<const ProbeObservation*>& obs, double level) {
  ArmReliability r;
  r.probes = obs.size();
  for (const ProbeObservation* o : obs) {
    if (!o->success) ++r.failures;
    if (o->timeout) ++r.timeouts;
  }
  r.failure_rate = r.probes ? static_cast<double>(r.failures) / static_cast<double>(r.probes) : 0.0;
  stats::Wilson(r.failures, r.probes, level, r.wilson_low, r.wilson_high);
  return r;
}

struct LatencyTest {
  std::string method{"none"};
  std::optional<double> estimate, low, high, p;
};

// Circular moving-block bootstrap of the median (block length 1 is the
// ordinary bootstrap). Deterministic for the seed.
LatencyTest BootstrapMedian(const std::vector<double>& d, std::size_t block, std::size_t resamples,
                            std::uint64_t seed, double level) {
  LatencyTest t;
  t.method = block > 1 ? "block_bootstrap_median_log_latency" : "bootstrap_median_log_latency";
  t.estimate = stats::Median(d);
  algorithms::Rng rng(seed);
  const std::size_t n = d.size();
  std::vector<double> medians;
  medians.reserve(resamples);
  std::vector<double> sample(n);
  std::size_t at_or_below = 0, at_or_above = 0;
  for (std::size_t b = 0; b < resamples; ++b) {
    std::size_t filled = 0;
    while (filled < n) {
      const std::size_t start = rng.Index(n);
      for (std::size_t k = 0; k < block && filled < n; ++k) sample[filled++] = d[(start + k) % n];
    }
    const double m = stats::Median(sample);
    medians.push_back(m);
    if (m <= 0.0) ++at_or_below;
    if (m >= 0.0) ++at_or_above;
  }
  std::sort(medians.begin(), medians.end());
  const double tail = (1.0 - level) / 2.0;
  const auto at = [&](double q) {
    const double pos = q * static_cast<double>(medians.size() - 1);
    const std::size_t i = static_cast<std::size_t>(std::floor(pos));
    const double frac = pos - static_cast<double>(i);
    return i + 1 < medians.size() ? medians[i] * (1.0 - frac) + medians[i + 1] * frac : medians[i];
  };
  t.low = at(tail);
  t.high = at(1.0 - tail);
  // Two-sided bootstrap p-value for "median difference is zero", with the
  // +1 correction so it is never reported as exactly zero.
  const double side = static_cast<double>(std::min(at_or_below, at_or_above));
  t.p = std::min(1.0, 2.0 * (side + 1.0) / (static_cast<double>(resamples) + 1.0));
  return t;
}

LatencyTest PairedT(const std::vector<double>& d, double effective_n, double level) {
  LatencyTest t;
  t.method = "paired_t_log_latency";
  const double mean = stats::Mean(d);
  const double sd = std::sqrt(stats::SampleVariance(d));
  t.estimate = mean;
  const double df = effective_n - 1.0;
  if (sd <= 1e-12) {
    // Every pair differs by the same amount: the direction is certain when
    // that amount is not zero, and there is no spread to put in an interval.
    t.low = mean;
    t.high = mean;
    t.p = std::fabs(mean) > 1e-12 ? 0.0 : 1.0;
    return t;
  }
  const double se = sd / std::sqrt(effective_n);
  const double q = stats::TQuantileTwoSided(level, df);
  t.low = mean - q * se;
  t.high = mean + q * se;
  t.p = stats::TwoSidedTPValue(mean / se, df);
  return t;
}

}  // namespace

ArbitrationResult StatisticalArbitrationEngine::Evaluate(const ProbeCampaignResult& campaign) const {
  ArbitrationResult result;
  result.scenario_id = campaign.scenario_id;
  const ProbeArm* incumbent = nullptr;
  std::vector<const ProbeArm*> challengers;
  for (const ProbeArm& arm : campaign.arms) {
    if (arm.incumbent && !incumbent) incumbent = &arm;
    else challengers.push_back(&arm);
  }
  if (!incumbent) return result;
  result.incumbent_arm = incumbent->arm_id;
  const std::size_t m = std::max<std::size_t>(1, challengers.size());
  const double level = 1.0 - config_.alpha / static_cast<double>(m);
  const auto config_issues = config_.Validate();

  // Index observations by (arm, round).
  std::map<std::pair<std::string, std::size_t>, const ProbeObservation*> by_round;
  std::map<std::string, std::size_t> payload_sizes;
  // Send positions per arm, to recognise a blocked (non-interleaved) design.
  std::map<std::string, std::pair<std::size_t, std::size_t>> span;  // first, last
  for (std::size_t i = 0; i < campaign.observations.size(); ++i) {
    const auto& id = campaign.observations[i].arm_id;
    auto found = span.find(id);
    if (found == span.end()) span[id] = {i, i};
    else found->second.second = i;
  }
  bool same_campaign = campaign.ran && config_issues.empty();
  for (const ProbeObservation& o : campaign.observations) {
    by_round[{o.arm_id, o.round}] = &o;
    if (o.graph_version != campaign.graph_version || o.executor != campaign.executor) same_campaign = false;
    payload_sizes[o.arm_id] = o.payload_bytes;
  }
  std::size_t payload = 0;
  for (const auto& entry : payload_sizes) {
    if (payload == 0) payload = entry.second;
    else if (entry.second != payload) same_campaign = false;
  }

  const ArmReliability inc_rel = Reliability(campaign.ForArm(incumbent->arm_id), level);
  std::vector<ComparisonResult> comparisons;
  std::vector<std::size_t> tested;
  std::vector<double> p_values;

  for (const ProbeArm* arm : challengers) {
    ComparisonResult c;
    c.challenger_arm = arm->arm_id;
    c.challenger_algorithm = arm->algorithm;
    c.ci_level = level;
    c.incumbent = inc_rel;
    c.challenger = Reliability(campaign.ForArm(arm->arm_id), level);
    AssumptionChecks& a = c.assumptions;
    a.same_campaign = same_campaign;

    // Paired log-latency differences, in round order.
    std::vector<double> d, inc_ms, chal_ms;
    // Interleaved: neither arm's probes were all sent before the other's.
    bool interleaved = true;
    {
      const auto si = span.find(incumbent->arm_id), sc = span.find(arm->arm_id);
      if (si != span.end() && sc != span.end() && campaign.rounds_requested >= 2) {
        if (si->second.second < sc->second.first || sc->second.second < si->second.first) interleaved = false;
      }
    }
    for (std::size_t r = 0; r < campaign.rounds_requested; ++r) {
      const auto ri = by_round.find({incumbent->arm_id, r});
      const auto rc = by_round.find({arm->arm_id, r});
      if (ri == by_round.end() && rc == by_round.end()) continue;
      if (ri == by_round.end() || rc == by_round.end()) {
        ++c.pairs_dropped;
        continue;
      }
      if (!ri->second->success || !rc->second->success) {
        ++c.pairs_dropped;
        continue;
      }
      // A zero duration (sub-resolution timing) is floored at 1 microsecond
      // so the logarithm stays finite.
      const double li = std::max(ri->second->duration_ms, 1e-3);
      const double lc = std::max(rc->second->duration_ms, 1e-3);
      inc_ms.push_back(li);
      chal_ms.push_back(lc);
      d.push_back(std::log(lc) - std::log(li));
    }
    c.pairs_used = d.size();
    a.interleaved = interleaved && campaign.rounds_completed >= 1;
    if (!inc_ms.empty()) c.incumbent_median_ms = stats::Median(inc_ms);
    if (!chal_ms.empty()) c.challenger_median_ms = stats::Median(chal_ms);

    // ---- Reliability: feasibility against the failure budget ----
    const bool challenger_over_budget = c.challenger.wilson_low > config_.failure_budget;
    const bool incumbent_over_budget = c.incumbent.wilson_low > config_.failure_budget;
    const bool challenger_less_reliable = c.challenger.wilson_low > c.incumbent.wilson_high;
    // Significantly more reliable: the intervals do not overlap. Proving
    // "within a 5% budget" would need >= 73 failure-free probes per arm, more
    // than a bounded campaign sends, so the repair asks for separation.
    const bool challenger_more_reliable = c.challenger.wilson_high < c.incumbent.wilson_low;

    if (!a.same_campaign || !a.interleaved) {
      c.verdict = ArbitrationVerdict::InvalidEvidence;
      c.reasons = {RuntimeReason::StatisticalEvidenceInvalid};
      a.summary = !config_issues.empty() ? "invalid arbitration configuration: " + config_issues.front()
                  : !a.same_campaign     ? "arms do not come from one campaign (graph version, executor or payload differ)"
                                         : "observations are not interleaved by round";
      c.explanation = a.summary;
      comparisons.push_back(std::move(c));
      continue;
    }
    if (challenger_over_budget || challenger_less_reliable) {
      c.method = "wilson_failure_rate";
      c.verdict = ArbitrationVerdict::ChallengerNotBetter;
      c.reasons = {RuntimeReason::ChallengerNotBetter};
      c.explanation = "challenger failure rate " + Fixed(c.challenger.failure_rate, 3) + " (Wilson " +
                      Fixed(c.challenger.wilson_low, 3) + ".." + Fixed(c.challenger.wilson_high, 3) + ")" +
                      (challenger_over_budget ? " exceeds the budget " + Fixed(config_.failure_budget, 3)
                                              : " is above the incumbent's interval");
      a.summary = "reliability decided the comparison";
      comparisons.push_back(std::move(c));
      continue;
    }
    if (incumbent_over_budget && challenger_more_reliable) {
      c.method = "wilson_failure_rate";
      c.reliability_repair = true;
      c.verdict = ArbitrationVerdict::ChallengerBetter;
      c.reasons = {RuntimeReason::ImprovementConfirmed};
      c.explanation = "incumbent failure rate " + Fixed(c.incumbent.failure_rate, 3) + " (Wilson lower " +
                      Fixed(c.incumbent.wilson_low, 3) + ") exceeds the budget " + Fixed(config_.failure_budget, 3) +
                      " and the challenger's (Wilson upper " + Fixed(c.challenger.wilson_high, 3) +
                      ") lies below the incumbent's interval; latency of surviving probes cannot show loss";
      a.summary = "reliability repair";
      // Latency is still reported when there are pairs, for the record.
      if (c.pairs_used >= 2) {
        const LatencyTest lt = PairedT(d, static_cast<double>(d.size()), level);
        c.effect_log = lt.estimate;
        c.ci_low_log = lt.low;
        c.ci_high_log = lt.high;
      }
      comparisons.push_back(std::move(c));
      continue;
    }

    // ---- Latency ----
    a.enough_pairs = c.pairs_used >= config_.min_pairs;
    if (!a.enough_pairs) {
      c.verdict = ArbitrationVerdict::InsufficientEvidence;
      c.reasons = {RuntimeReason::InsufficientSamples};
      a.summary = std::to_string(c.pairs_used) + " valid pair(s), " + std::to_string(config_.min_pairs) + " required";
      c.explanation = a.summary;
      comparisons.push_back(std::move(c));
      continue;
    }
    a.lag1_autocorrelation = stats::Lag1Autocorrelation(d);
    const double r1 = a.lag1_autocorrelation.value_or(0.0);
    const double n = static_cast<double>(d.size());
    a.effective_pairs = r1 > config_.autocorrelation_limit ? n * (1.0 - r1) / (1.0 + r1) : n;
    a.autocorrelation_ok = a.effective_pairs >= config_.min_effective_pairs;
    a.skewness = stats::Skewness(d);
    a.excess_kurtosis = stats::ExcessKurtosis(d);
    a.normality_ok = a.skewness && a.excess_kurtosis && std::fabs(*a.skewness) <= config_.skewness_limit &&
                     *a.excess_kurtosis <= config_.excess_kurtosis_limit;
    std::ostringstream summary;
    summary << c.pairs_used << " pairs, lag-1 r=" << Fixed(r1, 3) << ", n_eff=" << Fixed(a.effective_pairs, 1)
            << ", skew=" << Fixed(a.skewness.value_or(0.0), 2) << ", excess kurtosis="
            << Fixed(a.excess_kurtosis.value_or(0.0), 2);
    if (!a.autocorrelation_ok) {
      summary << "; serial correlation leaves too few effective pairs";
      a.summary = summary.str();
      c.verdict = ArbitrationVerdict::InvalidEvidence;
      c.reasons = {RuntimeReason::StatisticalEvidenceInvalid};
      c.explanation = a.summary;
      comparisons.push_back(std::move(c));
      continue;
    }
    LatencyTest lt;
    if (a.normality_ok) {
      lt = PairedT(d, a.effective_pairs, level);
      summary << "; differences close to normal: paired t";
    } else {
      const std::size_t block =
          r1 > config_.autocorrelation_limit ? static_cast<std::size_t>(std::ceil(n / a.effective_pairs)) : 1;
      lt = BootstrapMedian(d, block, config_.bootstrap_resamples, config_.bootstrap_seed, level);
      summary << "; heavy tails or skew: bootstrap of the median";
    }
    a.summary = summary.str();
    c.method = lt.method;
    c.effect_log = lt.estimate;
    c.ci_low_log = lt.low;
    c.ci_high_log = lt.high;
    c.p_value = lt.p;
    c.latency_ratio = std::exp(*lt.estimate);
    c.ratio_low = std::exp(*lt.low);
    c.ratio_high = std::exp(*lt.high);
    c.improvement_relative = 1.0 - *c.latency_ratio;
    c.improvement_relative_lower = 1.0 - *c.ratio_high;
    tested.push_back(comparisons.size());
    p_values.push_back(*lt.p);
    comparisons.push_back(std::move(c));
  }

  // Holm across the latency tests that ran, then the verdicts.
  const std::vector<double> adjusted = stats::HolmAdjust(p_values);
  for (std::size_t i = 0; i < tested.size(); ++i) {
    ComparisonResult& c = comparisons[tested[i]];
    c.p_holm = adjusted[i];
    const bool significant = *c.p_holm < config_.alpha;
    std::ostringstream text;
    text << "latency ratio " << Fixed(*c.latency_ratio, 3) << " (" << Fixed(c.ci_level * 100.0, 1) << "% CI "
         << Fixed(*c.ratio_low, 3) << ".." << Fixed(*c.ratio_high, 3) << "), p=" << Fixed(*c.p_value, 4)
         << ", Holm p=" << Fixed(*c.p_holm, 4) << " via " << c.method;
    if (significant && *c.ci_high_log < 0.0) {
      c.verdict = ArbitrationVerdict::ChallengerBetter;
      c.reasons = {RuntimeReason::ImprovementConfirmed};
      text << "; challenger faster";
    } else if (significant && *c.ci_low_log > 0.0) {
      c.verdict = ArbitrationVerdict::ChallengerNotBetter;
      c.reasons = {RuntimeReason::ChallengerNotBetter};
      text << "; challenger slower";
    } else {
      c.verdict = ArbitrationVerdict::NoSignificantDifference;
      c.reasons = {RuntimeReason::StatisticalEvidenceInsufficient};
      text << "; no significant difference";
    }
    c.explanation = text.str();
  }

  // Best challenger: a reliability repair first, then the largest lower
  // bound of the latency improvement.
  for (std::size_t i = 0; i < comparisons.size(); ++i) {
    const ComparisonResult& c = comparisons[i];
    if (c.verdict != ArbitrationVerdict::ChallengerBetter) continue;
    if (!result.best) {
      result.best = i;
      continue;
    }
    const ComparisonResult& b = comparisons[*result.best];
    if (c.reliability_repair != b.reliability_repair) {
      if (c.reliability_repair) result.best = i;
      continue;
    }
    if (c.improvement_relative_lower.value_or(-1.0) > b.improvement_relative_lower.value_or(-1.0)) result.best = i;
  }
  result.comparisons = std::move(comparisons);
  return result;
}

const char* ToString(ArbitrationVerdict value) {
  switch (value) {
    case ArbitrationVerdict::ChallengerBetter: return "challenger_better";
    case ArbitrationVerdict::ChallengerNotBetter: return "challenger_not_better";
    case ArbitrationVerdict::NoSignificantDifference: return "no_significant_difference";
    case ArbitrationVerdict::InsufficientEvidence: return "insufficient_evidence";
    case ArbitrationVerdict::InvalidEvidence: return "invalid_evidence";
  }
  return "unknown";
}

}  // namespace aafr::runtime
