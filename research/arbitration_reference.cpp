#include "research/arbitration_reference.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "runtime/adaptive/statistical_arbitration.h"

namespace aafr::research {

namespace stats = aafr::runtime::stats;

double FSurvival(double f, double d1, double d2) {
  if (std::isnan(f) || !(d1 > 0.0) || !(d2 > 0.0)) return std::numeric_limits<double>::quiet_NaN();
  if (f <= 0.0) return 1.0;
  if (std::isinf(f)) return 0.0;
  return stats::IncompleteBeta(0.5 * d2, 0.5 * d1, d2 / (d2 + d1 * f));
}

PairedAnova RepeatedMeasuresAnova(const std::vector<double>& inc, const std::vector<double>& chal, double ci_level) {
  PairedAnova a;
  a.ci_level = ci_level;
  const std::size_t n = inc.size();
  a.rounds = n;
  if (chal.size() != n) {
    a.unavailable_reason = "the arms have different numbers of rounds";
    return a;
  }
  if (n < 2) {
    a.unavailable_reason = "fewer than two rounds";
    return a;
  }
  if (!(ci_level > 0.0 && ci_level < 1.0)) {
    a.unavailable_reason = "confidence level must lie within (0, 1)";
    return a;
  }
  for (std::size_t r = 0; r < n; ++r) {
    if (!std::isfinite(inc[r]) || !std::isfinite(chal[r])) {
      a.unavailable_reason = "a round value is not finite";
      return a;
    }
  }
  const double nn = static_cast<double>(n);
  double mean_inc = 0.0, mean_chal = 0.0;
  for (std::size_t r = 0; r < n; ++r) {
    mean_inc += inc[r];
    mean_chal += chal[r];
  }
  mean_inc /= nn;
  mean_chal /= nn;
  const double grand = 0.5 * (mean_inc + mean_chal);
  a.ss_arm = nn * ((mean_inc - grand) * (mean_inc - grand) + (mean_chal - grand) * (mean_chal - grand));
  double d_sum = 0.0;
  for (std::size_t r = 0; r < n; ++r) {
    const double round_mean = 0.5 * (inc[r] + chal[r]);
    a.ss_round += 2.0 * (round_mean - grand) * (round_mean - grand);
    const double e_inc = inc[r] - mean_inc - round_mean + grand;
    const double e_chal = chal[r] - mean_chal - round_mean + grand;
    a.ss_error += e_inc * e_inc + e_chal * e_chal;
    d_sum += chal[r] - inc[r];
  }
  a.df_effect = 1.0;
  a.df_error = nn - 1.0;
  a.mean_difference_log10 = d_sum / nn;
  double d_ss = 0.0;
  for (std::size_t r = 0; r < n; ++r) {
    const double dev = (chal[r] - inc[r]) - a.mean_difference_log10;
    d_ss += dev * dev;
  }
  const double sd_d = std::sqrt(d_ss / (nn - 1.0));

  if (a.ss_error <= 0.0) {
    if (a.ss_arm <= 0.0) {
      a.unavailable_reason = "no variation between or within arms";
      return a;
    }
    a.f = std::numeric_limits<double>::infinity();
    a.p_value = 0.0;
    a.partial_eta_squared = 1.0;
  } else {
    a.f = a.ss_arm / (a.ss_error / a.df_error);
    a.p_value = FSurvival(a.f, a.df_effect, a.df_error);
    a.partial_eta_squared = a.ss_arm / (a.ss_arm + a.ss_error);
  }
  const double half = stats::TQuantileTwoSided(ci_level, a.df_error) * sd_d / std::sqrt(nn);
  a.ci_low_log10 = a.mean_difference_log10 - half;
  a.ci_high_log10 = a.mean_difference_log10 + half;
  a.rtt_ratio = std::pow(10.0, a.mean_difference_log10);
  a.ratio_low = std::pow(10.0, a.ci_low_log10);
  a.ratio_high = std::pow(10.0, a.ci_high_log10);
  a.computed = true;
  return a;
}

std::vector<double> HolmAdjusted(const std::vector<double>& p_values) { return stats::HolmAdjust(p_values); }

ArmFeasibility EvaluateFeasibility(std::size_t sent, std::size_t lost, std::optional<double> mean_rtt_ms,
                                   const FeasibilityBudget& budget) {
  ArmFeasibility f;
  f.sent = sent;
  f.lost = std::min(lost, sent);
  f.mean_rtt_ms = mean_rtt_ms;
  stats::Wilson(f.lost, sent, budget.wilson_level, f.loss_low, f.loss_high);
  const bool rtt_known = mean_rtt_ms.has_value() && std::isfinite(*mean_rtt_ms);
  const bool rtt_over = !rtt_known || *mean_rtt_ms > budget.mean_rtt_ms;
  f.infeasible = sent == 0 || f.loss_low > budget.loss_ratio || rtt_over;
  f.within_budget = sent > 0 && f.loss_high <= budget.loss_ratio && !rtt_over;
  return f;
}

ConfidenceResult ComputeConfidence(const ConfidenceInputs& in, const ConfidenceWeights& w) {
  ConfidenceResult c;
  if (!in.delta_goodness || !std::isfinite(*in.delta_goodness)) {
    c.unavailable_reason = "goodness difference unavailable";
    return c;
  }
  if (!(in.partial_eta_squared >= 0.0 && in.partial_eta_squared <= 1.0)) {
    c.unavailable_reason = "partial eta squared outside [0, 1]";
    return c;
  }
  if (!std::isfinite(in.ci_low_log10) || !std::isfinite(in.ci_high_log10) || in.ci_low_log10 > in.ci_high_log10) {
    c.unavailable_reason = "invalid confidence interval";
    return c;
  }
  c.s_eta = in.partial_eta_squared;
  if (in.challenger.infeasible) {
    c.s_ci = -1.0;
    c.challenger_infeasible = true;
  } else if (in.incumbent.infeasible && in.challenger.within_budget) {
    c.s_ci = 1.0;
    c.repair = true;
  } else if (in.ci_high_log10 < 0.0) {
    c.s_ci = std::min(1.0, -in.ci_high_log10 / w.ci_scale);
  } else if (in.ci_low_log10 > 0.0) {
    c.s_ci = -std::min(1.0, in.ci_low_log10 / w.ci_scale);
  } else {
    c.s_ci = 0.0;
  }
  c.s_g = std::clamp(*in.delta_goodness / w.goodness_scale, -1.0, 1.0);
  c.confidence = w.eta * c.s_eta + w.ci * c.s_ci + w.goodness * c.s_g;
  c.computed = true;
  return c;
}

ChallengerDecision Decide(const ConfidenceResult& confidence, std::optional<double> p_holm,
                          std::optional<double> seconds_since_last_switch, const DecisionRule& rule) {
  ChallengerDecision d;
  d.confidence_ok = confidence.computed && confidence.confidence > rule.confidence_threshold;
  d.significance_ok = p_holm.has_value() && *p_holm < rule.alpha;
  d.dwell_ok = !seconds_since_last_switch || *seconds_since_last_switch >= rule.dwell_s;
  d.qualifies = d.confidence_ok && d.significance_ok && d.dwell_ok;
  return d;
}

ArbitrationOutcome ArbitrateTournament(const ProbeTournament& t,
                                       const std::map<std::string, std::optional<double>>& delta_goodness,
                                       std::optional<double> seconds_since_last_switch, const DecisionRule& rule,
                                       const FeasibilityBudget& budget, const ConfidenceWeights& weights) {
  ArbitrationOutcome out;
  out.issues = ValidateTournament(t);
  if (!out.issues.empty()) {
    out.reason = "invalid tournament";
    return out;
  }
  const ArmSummary* incumbent = nullptr;
  for (const ProbeArm& arm : t.arms) out.arms.push_back(SummarizeArm(arm));
  for (const ArmSummary& s : out.arms) {
    if (s.incumbent) incumbent = &s;
  }
  out.incumbent_feasibility = EvaluateFeasibility(incumbent->sent, incumbent->lost, incumbent->mean_rtt_ms, budget);

  // Per challenger, pair only the rounds where both frames are valid.
  for (const ArmSummary& s : out.arms) {
    if (s.incumbent) continue;
    ChallengerEvaluation e;
    e.arm_id = s.arm_id;
    e.feasibility = EvaluateFeasibility(s.sent, s.lost, s.mean_rtt_ms, budget);
    std::vector<double> inc_values, chal_values;
    for (std::size_t r = 0; r < kFrames; ++r) {
      const auto& fi = incumbent->frames[r].value_log10;
      const auto& fc = s.frames[r].value_log10;
      if (!fi || !fc) continue;
      inc_values.push_back(*fi);
      chal_values.push_back(*fc);
    }
    e.paired_rounds = inc_values.size();
    if (e.paired_rounds < kMinPairedRounds) {
      e.anova.rounds = e.paired_rounds;
      e.anova.unavailable_reason = "fewer than " + std::to_string(kMinPairedRounds) +
                                   " rounds with a valid frame on both arms (" + std::to_string(e.paired_rounds) + ")";
    } else {
      e.anova = RepeatedMeasuresAnova(inc_values, chal_values);
    }
    out.challengers.push_back(e);
  }
  // Holm over every challenger; a test that could not be computed enters with
  // p = 1, the research engine's default result.
  std::vector<double> p;
  for (const auto& e : out.challengers) p.push_back(e.anova.computed ? e.anova.p_value : 1.0);
  const std::vector<double> adjusted = HolmAdjusted(p);
  bool missing_goodness = false;
  for (std::size_t i = 0; i < out.challengers.size(); ++i) {
    ChallengerEvaluation& e = out.challengers[i];
    e.p_holm = adjusted[i];
    if (!e.anova.computed) {
      e.confidence.unavailable_reason = "no paired test: " + e.anova.unavailable_reason;
      e.decision = Decide(e.confidence, e.p_holm, seconds_since_last_switch, rule);  // Cannot qualify.
      continue;
    }
    ConfidenceInputs in;
    in.partial_eta_squared = e.anova.partial_eta_squared;
    in.ci_low_log10 = e.anova.ci_low_log10;
    in.ci_high_log10 = e.anova.ci_high_log10;
    in.incumbent = out.incumbent_feasibility;
    in.challenger = e.feasibility;
    const auto it = delta_goodness.find(e.arm_id);
    if (it != delta_goodness.end()) in.delta_goodness = it->second;
    e.confidence = ComputeConfidence(in, weights);
    missing_goodness = missing_goodness || !e.confidence.computed;
    e.decision = Decide(e.confidence, e.p_holm, seconds_since_last_switch, rule);
    if (e.decision.qualifies) out.qualifying_arms.push_back(e.arm_id);
  }
  if (missing_goodness) {
    out.qualifying_arms.clear();
    out.reason = "a challenger with a paired test has no confidence (goodness difference unavailable)";
    return out;
  }
  if (out.qualifying_arms.empty()) {
    out.verdict = ArbitrationVerdict::Keep;
    out.reason = "no challenger meets C, Holm p and dwell";
    return out;
  }
  // Highest C; strict '>' keeps the earliest challenger on an exact tie.
  const ChallengerEvaluation* best = nullptr;
  for (const auto& e : out.challengers) {
    if (e.decision.qualifies && (!best || e.confidence.confidence > best->confidence.confidence)) best = &e;
  }
  for (const auto& e : out.challengers) {
    if (&e != best && e.decision.qualifies && e.confidence.confidence == best->confidence.confidence) {
      out.tied_arms.push_back(e.arm_id);
    }
  }
  out.verdict = ArbitrationVerdict::Switch;
  out.selected_arm = best->arm_id;
  out.reason = out.qualifying_arms.size() == 1
                   ? "one challenger meets C, Holm p and dwell"
                   : std::to_string(out.qualifying_arms.size()) + " challengers meet C, Holm p and dwell; the highest C is selected";
  if (!out.tied_arms.empty()) out.reason += " (exact tie on C: the earliest arm in arm order is kept)";
  return out;
}

const char* ToString(ArbitrationVerdict verdict) {
  switch (verdict) {
    case ArbitrationVerdict::Keep: return "keep";
    case ArbitrationVerdict::Switch: return "switch";
    case ArbitrationVerdict::Undetermined: return "undetermined";
  }
  return "unknown";
}

}  // namespace aafr::research
