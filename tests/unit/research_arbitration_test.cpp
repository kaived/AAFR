// Research alignment, Phase 2: probe tournament data model and the reference
// statistical arbitration (research/). Independent of the live runtime and
// of its arbitration in runtime/adaptive. Statistical values are compared
// with scipy (tests/research/scipy_reference.py) on seeded synthetic
// tournaments; the recorded tournaments of reference_values.h are checked at
// the level of their recorded summary statistics (their per-frame probe data
// is not available and is not reconstructed). Deterministic; no network.

#include <cmath>
#include <limits>
#include <string>

#include "research/arbitration_reference.h"
#include "research/probe_tournament.h"
#include "tests/fixtures/research/reference_values.h"
#include "tests/fixtures/research/scipy_reference_values.h"
#include "tests/support/test_support.h"

using namespace aafr::research;
namespace rf = aafr::research_fixture;
namespace ref = aafr::research_reference;

namespace {

using RawArm = std::vector<std::vector<std::vector<std::optional<double>>>>;

bool Near(double actual, double expected, double rel, double abs = 0.0) {
  return std::fabs(actual - expected) <= rel * std::fabs(expected) + abs;
}

// Tolerances. Sums and means of identical inputs: the C++ and numpy
// summation orders differ, so agreement is to a few ulp of the operands
// (1e-12 relative leaves margin). p-values go through a continued fraction
// (C++) and a different scipy algorithm; 1e-9 relative is far tighter than
// the 1e-6 any decision needs. The t quantile is found by bisection.
constexpr double kArithmetic = 1e-12;
constexpr double kDistribution = 1e-9;

ProbeArm MakeArm(const std::string& id, bool incumbent, std::vector<std::string> path, const RawArm& raw,
                 std::size_t arm_index, std::size_t arm_count, double scale = 1.0) {
  ProbeArm arm;
  arm.arm_id = id;
  arm.incumbent = incumbent;
  arm.path = std::move(path);
  for (std::size_t f = 0; f < raw.size(); ++f) {
    ProbeFrame frame;
    frame.round = f;
    frame.position_in_round = (arm_index + f) % arm_count;  // Rotated order.
    for (const auto& echoes : raw[f]) {
      ProbeBlock block;
      for (const auto& e : echoes) block.echoes.push_back(e ? std::optional<double>(*e * scale) : std::nullopt);
      frame.blocks.push_back(block);
    }
    arm.frames.push_back(frame);
  }
  return arm;
}

ProbeTournament TwoArms(const RawArm& inc, const RawArm& chal) {
  ProbeTournament t;
  t.arms.push_back(MakeArm("inc", true, {"s1", "s2", "s6"}, inc, 0, 2));
  t.arms.push_back(MakeArm("chal", false, {"s1", "s3", "s6"}, chal, 1, 2));
  return t;
}

void CheckAnova(const PairedAnova& a, const ref::AnovaCase& r) {
  CHECK(a.computed);
  CHECK(a.rounds == r.rounds);
  CHECK(a.df_effect == 1.0 && a.df_error == static_cast<double>(r.rounds) - 1.0);
  CHECK(Near(a.f, r.f, 1e-9));
  CHECK(Near(a.p_value, r.p, kDistribution));
  CHECK(Near(a.p_value, r.p_from_f, kDistribution));
  CHECK(Near(a.partial_eta_squared, r.eta, kArithmetic, 1e-15));
  CHECK(Near(a.mean_difference_log10, r.mean_d, kArithmetic, 1e-15));
  CHECK(Near(a.ci_low_log10, r.ci_low, 1e-10, 1e-14));
  CHECK(Near(a.ci_high_log10, r.ci_high, 1e-10, 1e-14));
  CHECK(Near(a.rtt_ratio, std::pow(10.0, r.mean_d), 1e-12));
}

// ---- Data model ----

void TournamentDimensions() {
  CHECK(kFrames == rf::kFramesPerArm && kBlocks == rf::kBlocksPerFrame && kEchoes == rf::kEchoesPerBlock);
  CHECK(kMinReplies == rf::kMinRepliesPerBlock && kProbesPerArm == 400);
  const ProbeTournament good = TwoArms(ref::kT1Incumbent, ref::kT1Challenger);
  CHECK(ValidateTournament(good).empty());

  ProbeTournament t = good;
  t.arms[1].frames.pop_back();
  CHECK(!ValidateTournament(t).empty());
  t = good;
  t.arms[0].frames[3].blocks.pop_back();
  CHECK(!ValidateTournament(t).empty());
  t = good;
  t.arms[0].frames[3].blocks[1].echoes.push_back(5.0);
  CHECK(!ValidateTournament(t).empty());
  t = good;
  t.arms[1].incumbent = true;
  CHECK(!ValidateTournament(t).empty());
  t = good;
  t.arms[1].path = t.arms[0].path;  // Identical paths are one arm.
  CHECK(!ValidateTournament(t).empty());
  t = good;
  t.arms[1].arm_id = "inc";
  CHECK(!ValidateTournament(t).empty());
  t = good;
  t.arms[1].frames[2].round = 3;
  CHECK(!ValidateTournament(t).empty());
  t = good;
  t.arms[1].frames[5].position_in_round = t.arms[0].frames[5].position_in_round;  // Not interleaved.
  CHECK(!ValidateTournament(t).empty());
  t = good;
  t.arms[0].frames[0].blocks[0].echoes[0] = 0.0;  // A returned RTT must be positive.
  CHECK(!ValidateTournament(t).empty());
  t = good;
  t.arms[0].frames[0].blocks[0].echoes[0] = std::numeric_limits<double>::quiet_NaN();
  CHECK(!ValidateTournament(t).empty());
  t = good;
  t.arms.pop_back();
  CHECK(!ValidateTournament(t).empty());
}

void BlocksAndMissingEchoes() {
  ProbeArm arm = MakeArm("a", true, {"x", "y"}, ref::kT1Incumbent, 0, 1);
  auto& echoes = arm.frames[0].blocks[0].echoes;
  for (std::size_t i = 0; i < 5; ++i) echoes[i].reset();  // 3 replies: still valid.
  auto& invalid = arm.frames[1].blocks[0].echoes;
  for (std::size_t i = 0; i < 6; ++i) invalid[i].reset();  // 2 replies: invalid.
  const ArmSummary s = SummarizeArm(arm);
  CHECK(s.frames[0].blocks[0].replies == 3 && s.frames[0].blocks[0].valid);
  CHECK(s.frames[0].complete && s.frames[0].value_log10);
  CHECK(s.frames[1].blocks[0].replies == 2 && !s.frames[1].blocks[0].valid);
  CHECK(!s.frames[1].blocks[0].mean_log10);
  CHECK(s.frames[1].valid_blocks == 4 && !s.frames[1].complete);
  // Research-engine policy (replaces the former "no value"): the invalid
  // block is discarded and the frame value is the mean of the 4 valid block means.
  CHECK(s.frames[1].valid && s.frames[1].value_log10);
  double sum = 0.0;
  for (std::size_t b = 1; b < kBlocks; ++b) sum += *s.frames[1].blocks[b].mean_log10;
  CHECK(Near(*s.frames[1].value_log10, sum / 4.0, 1e-15));
  CHECK(!s.all_frames_complete && s.valid_frames == kFrames);
  CHECK(s.sent == 400 && s.lost == 11 && s.returned == 389);
  CHECK(Near(s.loss_ratio, 11.0 / 400.0, 0.0, 1e-15));  // 1 - 389/400: absolute error ~1 ulp.
  // A frame without any valid block has no value.
  ProbeArm empty = MakeArm("e", true, {"x", "y"}, ref::kT1Incumbent, 0, 1);
  for (auto& block : empty.frames[3].blocks) {
    for (std::size_t i = 0; i < 6; ++i) block.echoes[i].reset();
  }
  const ArmSummary se = SummarizeArm(empty);
  CHECK(se.frames[3].valid_blocks == 0 && !se.frames[3].valid && !se.frames[3].value_log10);
  CHECK(se.valid_frames == kFrames - 1);
  // One valid block: a value, no block-mean variance (needs two).
  ProbeArm one = MakeArm("o", true, {"x", "y"}, ref::kT1Incumbent, 0, 1);
  for (std::size_t b = 1; b < kBlocks; ++b) {
    for (std::size_t i = 0; i < 6; ++i) one.frames[5].blocks[b].echoes[i].reset();
  }
  const ArmSummary so = SummarizeArm(one);
  CHECK(so.frames[5].valid && so.frames[5].value_log10 == so.frames[5].blocks[0].mean_log10);
  CHECK(!so.frames[5].block_mean_variance);
}

void LogTransformAndVariances() {
  const ArmSummary s = SummarizeArm(MakeArm("a", true, {"x", "y"}, ref::kT2Incumbent, 0, 1));
  CHECK(s.lost == ref::kT2IncumbentLost);
  CHECK(s.mean_rtt_ms && Near(*s.mean_rtt_ms, ref::kT2IncumbentMeanRtt, kArithmetic));
  // Block mean is the mean of log10 RTT over returned echoes only (not the
  // log of the mean RTT).
  const auto& first = ref::kT2Incumbent[0][0];
  double sum = 0.0, rtt_sum = 0.0;
  std::size_t returned = 0;
  for (const auto& e : first) {
    if (!e) continue;
    sum += std::log10(*e);
    rtt_sum += *e;
    ++returned;
  }
  CHECK(s.frames[0].blocks[0].replies == returned);
  CHECK(s.frames[0].blocks[0].mean_log10 &&
        Near(*s.frames[0].blocks[0].mean_log10, sum / static_cast<double>(returned), kArithmetic));
  CHECK(std::fabs(*s.frames[0].blocks[0].mean_log10 - std::log10(rtt_sum / static_cast<double>(returned))) > 1e-6);
  CHECK(s.frames[0].blocks[0].variance_log10 &&
        Near(*s.frames[0].blocks[0].variance_log10, ref::kT2IncumbentBlock00Variance, kArithmetic));
  CHECK(s.frames[0].block_mean_variance &&
        Near(*s.frames[0].block_mean_variance, ref::kT2IncumbentFrame0BlockMeanVariance, kArithmetic));
  for (std::size_t f = 0; f < kFrames; ++f) {
    CHECK(s.frames[f].value_log10 && Near(*s.frames[f].value_log10, ref::kT2IncumbentFrameValues[f], kArithmetic));
  }
}

// ---- RM-ANOVA ----

std::vector<double> FrameValues(const RawArm& raw) {
  const ArmSummary s = SummarizeArm(MakeArm("a", true, {"x", "y"}, raw, 0, 1));
  std::vector<double> v;
  for (const auto& f : s.frames) v.push_back(*f.value_log10);
  return v;
}

void AnovaMatchesScipy() {
  CheckAnova(RepeatedMeasuresAnova(FrameValues(ref::kT1Incumbent), FrameValues(ref::kT1Challenger)), ref::kT1Anova);
  CheckAnova(RepeatedMeasuresAnova(FrameValues(ref::kT2Incumbent), FrameValues(ref::kT2Fast)), ref::kT2FastAnova);
  CheckAnova(RepeatedMeasuresAnova(FrameValues(ref::kT2Incumbent), FrameValues(ref::kT2Same)), ref::kT2SameAnova);
  CheckAnova(RepeatedMeasuresAnova(FrameValues(ref::kT4Incumbent), FrameValues(ref::kT4Challenger)), ref::kT4Anova);
}

void AnovaDecomposition() {
  const auto inc = FrameValues(ref::kT2Incumbent);
  const auto chal = FrameValues(ref::kT2Same);
  const PairedAnova a = RepeatedMeasuresAnova(inc, chal);
  double grand = 0.0;
  for (std::size_t r = 0; r < inc.size(); ++r) grand += inc[r] + chal[r];
  grand /= 2.0 * static_cast<double>(inc.size());
  double total = 0.0;
  for (std::size_t r = 0; r < inc.size(); ++r) {
    total += (inc[r] - grand) * (inc[r] - grand) + (chal[r] - grand) * (chal[r] - grand);
  }
  CHECK(Near(a.ss_arm + a.ss_round + a.ss_error, total, 1e-12));
  // The round factor removes shared drift: it carries most of the variance.
  CHECK(a.ss_round > a.ss_error);
  // Two arms: F = t^2 of the paired test, partial eta^2 = F / (F + df).
  CHECK(Near(a.partial_eta_squared, a.f / (a.f + a.df_error), 1e-12));
  // The CI spans zero: no significant difference.
  CHECK(a.ci_low_log10 < 0.0 && a.ci_high_log10 > 0.0);
  CHECK(a.ratio_low < 1.0 && a.ratio_high > 1.0);
}

void AnovaDegenerateInputs() {
  const std::vector<double> x(10, 1.0);
  CHECK(!RepeatedMeasuresAnova(x, x).computed);  // No variation at all.
  std::vector<double> shifted = x;
  for (double& v : shifted) v -= 0.25;  // Constant difference, no error variance.
  const PairedAnova a = RepeatedMeasuresAnova(x, shifted);
  CHECK(a.computed && std::isinf(a.f) && a.p_value == 0.0 && a.partial_eta_squared == 1.0);
  CHECK(Near(a.ci_low_log10, -0.25, 1e-15) && Near(a.ci_high_log10, -0.25, 1e-15));
  CHECK(!RepeatedMeasuresAnova(x, std::vector<double>(9, 1.0)).computed);
  CHECK(!RepeatedMeasuresAnova({1.0}, {2.0}).computed);
  std::vector<double> nan = x;
  nan[3] = std::numeric_limits<double>::quiet_NaN();
  CHECK(!RepeatedMeasuresAnova(x, nan).computed);
}

void FSurvivalMatchesScipy() {
  for (const auto& c : ref::kFSurvival) CHECK(Near(FSurvival(c.f, c.d1, c.d2), c.sf, kDistribution));
  CHECK(FSurvival(0.0, 1, 9) == 1.0);
  CHECK(FSurvival(std::numeric_limits<double>::infinity(), 1, 9) == 0.0);
}

// ---- Holm ----

void HolmMatchesReference() {
  for (const auto& c : ref::kHolm) {
    const auto adjusted = HolmAdjusted(c.p);
    CHECK(adjusted.size() == c.adjusted.size());
    for (std::size_t i = 0; i < adjusted.size(); ++i) CHECK(Near(adjusted[i], c.adjusted[i], 1e-15));
  }
  // Single challenger: unchanged. Monotone in rank, capped at 1.
  CHECK(HolmAdjusted({0.03})[0] == 0.03);
  const auto capped = HolmAdjusted({0.5, 0.6});
  CHECK(capped[0] == 1.0 && capped[1] == 1.0);
  CHECK(HolmAdjusted({}).empty());
}

// ---- Feasibility and confidence ----

void WilsonFeasibility() {
  for (const auto& c : ref::kWilson) {
    const ArmFeasibility f = EvaluateFeasibility(c.n, c.k, 10.0);
    CHECK(Near(f.loss_low, c.low, 1e-12, 1e-15));
    CHECK(Near(f.loss_high, c.high, 1e-12, 1e-15));
  }
  // 0 of 400 lost: upper bound ~0.0095, within the 3 % budget.
  CHECK(EvaluateFeasibility(400, 0, 20.0).within_budget);
  // 5 of 400 (1.25 %): upper bound ~0.0289, still within the budget.
  CHECK(EvaluateFeasibility(400, 5, 20.0).within_budget);
  // 8 of 400 (2 %): bounds ~[0.010, 0.039] straddle the budget, so the arm
  // is neither infeasible nor provably within budget.
  const ArmFeasibility grey = EvaluateFeasibility(400, 8, 20.0);
  CHECK(!grey.infeasible && !grey.within_budget);
  CHECK(EvaluateFeasibility(400, 106, 9.2).infeasible);
  // Mean RTT budget.
  CHECK(EvaluateFeasibility(400, 0, 150.0).within_budget);
  CHECK(EvaluateFeasibility(400, 0, 150.5).infeasible);
  CHECK(EvaluateFeasibility(400, 400, std::nullopt).infeasible);
  CHECK(EvaluateFeasibility(0, 0, std::nullopt).infeasible);
}

ConfidenceInputs Inputs(double eta, double ci_lo, double ci_hi, ArmFeasibility inc, ArmFeasibility chal,
                        std::optional<double> dg) {
  ConfidenceInputs in;
  in.partial_eta_squared = eta;
  in.ci_low_log10 = ci_lo;
  in.ci_high_log10 = ci_hi;
  in.incumbent = inc;
  in.challenger = chal;
  in.delta_goodness = dg;
  return in;
}

void ConfidenceScoreComponents() {
  const ArmFeasibility ok = EvaluateFeasibility(400, 0, 20.0);
  const ArmFeasibility bad = EvaluateFeasibility(400, 100, 20.0);
  // CI below zero, scaled by 0.05 and capped at 1.
  ConfidenceResult c = ComputeConfidence(Inputs(0.5, -0.03, -0.02, ok, ok, 0.4));
  CHECK(c.computed && Near(c.s_ci, 0.4, 1e-12) && c.s_eta == 0.5 && c.s_g == 0.4);
  CHECK(Near(c.confidence, 0.40 * 0.5 + 0.35 * 0.4 + 0.25 * 0.4, 1e-15));
  CHECK(ComputeConfidence(Inputs(0.5, -0.3, -0.2, ok, ok, 0.0)).s_ci == 1.0);
  CHECK(Near(ComputeConfidence(Inputs(0.5, 0.01, 0.02, ok, ok, 0.0)).s_ci, -0.2, 1e-12));
  CHECK(ComputeConfidence(Inputs(0.5, -0.01, 0.02, ok, ok, 0.0)).s_ci == 0.0);
  // Clipping of s_g.
  CHECK(ComputeConfidence(Inputs(0.5, -0.1, -0.05, ok, ok, 7.0)).s_g == 1.0);
  CHECK(ComputeConfidence(Inputs(0.5, -0.1, -0.05, ok, ok, -7.0)).s_g == -1.0);
  // Infeasible challenger never wins; repair overrides a slower CI.
  c = ComputeConfidence(Inputs(1.0, -0.5, -0.4, ok, bad, 1.0));
  CHECK(c.s_ci == -1.0 && c.challenger_infeasible);
  c = ComputeConfidence(Inputs(0.99, 0.3, 0.4, bad, ok, 1.0));
  CHECK(c.s_ci == 1.0 && c.repair);
  // Missing dG: no confidence at all (no default).
  CHECK(!ComputeConfidence(Inputs(1.0, -0.5, -0.4, ok, ok, std::nullopt)).computed);
  CHECK(!ComputeConfidence(Inputs(1.2, -0.5, -0.4, ok, ok, 0.0)).computed);
  CHECK(!ComputeConfidence(Inputs(1.0, 0.1, -0.1, ok, ok, 0.0)).computed);
}

void ThresholdAndDwellBoundaries() {
  ConfidenceResult c;
  c.computed = true;
  c.confidence = 0.70;
  CHECK(!Decide(c, 0.001, std::nullopt).confidence_ok);  // C must exceed 0.70.
  c.confidence = std::nextafter(0.70, 1.0);
  CHECK(Decide(c, 0.001, std::nullopt).qualifies);
  CHECK(!Decide(c, 0.05, std::nullopt).significance_ok);  // Holm p must be below 0.05.
  CHECK(Decide(c, std::nextafter(0.05, 0.0), std::nullopt).significance_ok);
  CHECK(!Decide(c, std::nullopt, std::nullopt).qualifies);
  CHECK(!Decide(c, 0.001, 29.999).dwell_ok);
  CHECK(Decide(c, 0.001, 30.0).dwell_ok);
  CHECK(Decide(c, 0.001, std::nullopt).dwell_ok);  // No switch yet.
  ConfidenceResult none;
  CHECK(!Decide(none, 0.001, std::nullopt).qualifies);
}

void RecordedTournamentDecisions() {
  // Recorded summary statistics: partial eta^2, the 95 % CI of the RTT ratio,
  // probes delivered, mean RTT and dG. The recorded Holm p-values were all
  // far below 0.05 (as small as 1e-16) and are not tabulated individually;
  // 1e-15 stands in for them here. Tournaments were more than 30 s apart.
  for (const auto& t : rf::kRecordedTournaments) {
    aafr::test::current_test = std::string("recorded_") + t.run + "_" + std::to_string(t.number);
    const ArmFeasibility inc = EvaluateFeasibility(400, 400 - t.incumbent_delivered, t.incumbent_mean_rtt_ms);
    const ArmFeasibility chal = EvaluateFeasibility(400, 400 - t.challenger_delivered, t.challenger_mean_rtt_ms);
    const ConfidenceResult c = ComputeConfidence(Inputs(t.partial_eta_squared, std::log10(t.ratio_ci_low),
                                                        std::log10(t.ratio_ci_high), inc, chal, t.delta_goodness));
    CHECK(c.computed && Near(c.confidence, t.confidence, 0.0, 6e-4));
    CHECK(Decide(c, 1e-15, 60.0).qualifies == (t.decision == rf::Decision::Swap));
  }
  aafr::test::current_test = "recorded_tournament_decisions";
  // Loss 1 swaps only through the repair rule.
  const auto& loss1 = rf::kRecordedTournaments[4];
  const ConfidenceResult c = ComputeConfidence(
      Inputs(loss1.partial_eta_squared, std::log10(loss1.ratio_ci_low), std::log10(loss1.ratio_ci_high),
             EvaluateFeasibility(400, 400 - loss1.incumbent_delivered, loss1.incumbent_mean_rtt_ms),
             EvaluateFeasibility(400, 400 - loss1.challenger_delivered, loss1.challenger_mean_rtt_ms),
             loss1.delta_goodness));
  CHECK(c.repair && c.s_ci == 1.0);
}

void OneSidedLimitationIsReproduced() {
  const ArmFeasibility ok = EvaluateFeasibility(400, 0, 10.0);
  // Strongest possible evidence for a faster challenger, dG at the limit.
  ConfidenceResult c = ComputeConfidence(Inputs(1.0, -1.0, -0.5, ok, ok, rf::kOneSidedGoodnessLimit));
  CHECK(Near(c.confidence, 0.70, 1e-15));
  CHECK(!Decide(c, 1e-15, std::nullopt).qualifies);
  c = ComputeConfidence(Inputs(1.0, -1.0, -0.5, ok, ok, -4.46));
  CHECK(Near(c.confidence, 0.50, 1e-15));
  CHECK(!Decide(c, 1e-15, std::nullopt).qualifies);
}

// ---- Full tournament arbitration ----

void ArbitrationEndToEnd() {
  // T1: faster, loss-free challenger.
  ArbitrationOutcome o = ArbitrateTournament(TwoArms(ref::kT1Incumbent, ref::kT1Challenger),
                                             {{"chal", ref::kT1DeltaGoodness}}, std::nullopt);
  CHECK(o.issues.empty());
  CHECK(o.verdict == ArbitrationVerdict::Switch && o.selected_arm == std::string("chal"));
  CheckAnova(o.challengers[0].anova, ref::kT1Anova);
  CHECK(o.challengers[0].p_holm && Near(*o.challengers[0].p_holm, ref::kT1Anova.p, kDistribution));
  CHECK(Near(o.challengers[0].confidence.confidence, ref::kT1Confidence, 1e-12));
  // Same evidence inside the dwell time: keep.
  o = ArbitrateTournament(TwoArms(ref::kT1Incumbent, ref::kT1Challenger), {{"chal", ref::kT1DeltaGoodness}}, 12.0);
  CHECK(o.verdict == ArbitrationVerdict::Keep && !o.selected_arm);

  // T2: three arms; Holm over two challengers.
  ProbeTournament t2;
  t2.arms.push_back(MakeArm("inc", true, {"s1", "s2", "s6"}, ref::kT2Incumbent, 0, 3));
  t2.arms.push_back(MakeArm("fast", false, {"s1", "s3", "s6"}, ref::kT2Fast, 1, 3));
  t2.arms.push_back(MakeArm("same", false, {"s1", "s4", "s5", "s6"}, ref::kT2Same, 2, 3));
  o = ArbitrateTournament(t2, {{"fast", ref::kT2FastDeltaGoodness}, {"same", ref::kT2SameDeltaGoodness}}, std::nullopt);
  CHECK(o.issues.empty() && o.challengers.size() == 2);
  CHECK(o.challengers[0].p_holm && Near(*o.challengers[0].p_holm, ref::kT2FastPHolm, kDistribution));
  CHECK(o.challengers[1].p_holm && Near(*o.challengers[1].p_holm, ref::kT2SamePHolm, kDistribution));
  CHECK(Near(o.challengers[0].confidence.confidence, ref::kT2FastConfidence, 1e-12));
  CHECK(Near(o.challengers[1].confidence.confidence, ref::kT2SameConfidence, 1e-12, 1e-15));
  CHECK(o.challengers[0].confidence.challenger_infeasible);  // ~18 % loss.
  CHECK(o.verdict == ArbitrationVerdict::Keep);

  // T4: repair of a fast, lossy incumbent by a slower loss-free challenger.
  o = ArbitrateTournament(TwoArms(ref::kT4Incumbent, ref::kT4Challenger), {{"chal", ref::kT4DeltaGoodness}},
                          std::nullopt);
  CHECK(o.incumbent_feasibility.infeasible);
  CHECK(o.challengers[0].anova.rtt_ratio > 2.0);
  CHECK(o.challengers[0].confidence.repair);
  CHECK(Near(o.challengers[0].confidence.confidence, ref::kT4Confidence, 1e-12));
  CHECK(o.verdict == ArbitrationVerdict::Switch);
}

void IncompleteFramesFollowTheResearchEngine() {
  // T3: one incumbent block with 2 replies. The frame keeps its 4 valid block
  // means, all 10 rounds pair, and the result matches scipy on the
  // engine-policy frame values (formerly: undetermined).
  ArbitrationOutcome o = ArbitrateTournament(TwoArms(ref::kT3Incumbent, ref::kT3Challenger), {{"chal", 1.0}},
                                             std::nullopt);
  CHECK(o.issues.empty() && o.challengers[0].paired_rounds == 10);
  CheckAnova(o.challengers[0].anova, ref::kT3Anova);
  CHECK(o.verdict == ArbitrationVerdict::Switch && o.selected_arm == std::string("chal"));
  for (std::size_t f = 0; f < kFrames; ++f) {
    const auto s = SummarizeArm(MakeArm("i", true, {"x", "y"}, ref::kT3Incumbent, 0, 1));
    CHECK(s.frames[f].value_log10 && Near(*s.frames[f].value_log10, ref::kT3IncumbentFrameValues[f], kArithmetic));
  }
  // T5: incumbent frame 7 has no valid block (round dropped: 9 paired rounds,
  // F(1, 8)); frame 2 keeps 3 of 5 blocks.
  o = ArbitrateTournament(TwoArms(ref::kT5Incumbent, ref::kT5Challenger), {{"chal", 1.0}}, std::nullopt);
  CHECK(o.challengers[0].paired_rounds == 9 && ref::kT5Anova.rounds == 9);
  CheckAnova(o.challengers[0].anova, ref::kT5Anova);
  CHECK(o.challengers[0].anova.df_error == 8.0);
  const auto s5 = SummarizeArm(MakeArm("i", true, {"x", "y"}, ref::kT5Incumbent, 0, 1));
  CHECK(!s5.frames[7].valid && s5.frames[2].valid_blocks == 3 && s5.valid_frames == 9);
  CHECK(std::isnan(ref::kT5IncumbentFrameValues[7]));
  CHECK(Near(*s5.frames[2].value_log10, ref::kT5IncumbentFrameValues[2], kArithmetic));
}

// Removes every valid block from the listed frames of an arm.
void BlankFrames(ProbeArm& arm, const std::vector<std::size_t>& frames) {
  for (const std::size_t f : frames) {
    for (auto& block : arm.frames[f].blocks) {
      for (auto& e : block.echoes) e.reset();
    }
  }
}

void InsufficientPairedRounds() {
  // The incumbent has a valid frame in round 0 only: one paired round, no
  // test, Holm p = 1, the challenger cannot qualify, keep.
  ProbeTournament t = TwoArms(ref::kT1Incumbent, ref::kT1Challenger);
  BlankFrames(t.arms[0], {1, 2, 3, 4, 5, 6, 7, 8, 9});
  ArbitrationOutcome o = ArbitrateTournament(t, {{"chal", 1.0}}, std::nullopt);
  CHECK(o.issues.empty() && o.challengers[0].paired_rounds == 1);
  CHECK(!o.challengers[0].anova.computed && aafr::test::Contains(o.challengers[0].anova.unavailable_reason, "fewer than 2"));
  CHECK(o.challengers[0].p_holm == 1.0 && !o.challengers[0].decision.qualifies);
  CHECK(o.verdict == ArbitrationVerdict::Keep);
  // Rounds that are valid on one arm only do not pair: incumbent valid in
  // rounds 0-4, challenger in rounds 4-9 -> one paired round (4).
  t = TwoArms(ref::kT1Incumbent, ref::kT1Challenger);
  BlankFrames(t.arms[0], {5, 6, 7, 8, 9});
  BlankFrames(t.arms[1], {0, 1, 2, 3});
  o = ArbitrateTournament(t, {{"chal", 1.0}}, std::nullopt);
  CHECK(o.challengers[0].paired_rounds == 1 && o.verdict == ArbitrationVerdict::Keep);
  // Exactly two paired rounds: computed with F(1, 1) and a t(1) interval.
  t = TwoArms(ref::kT1Incumbent, ref::kT1Challenger);
  BlankFrames(t.arms[0], {2, 3, 4, 5, 6, 7, 8, 9});
  o = ArbitrateTournament(t, {{"chal", 1.0}}, std::nullopt);
  CHECK(o.challengers[0].paired_rounds == 2 && o.challengers[0].anova.computed);
  CHECK(o.challengers[0].anova.df_error == 1.0);
  const auto inc = FrameValues(ref::kT1Incumbent);
  const auto chal = FrameValues(ref::kT1Challenger);
  const PairedAnova direct = RepeatedMeasuresAnova({inc[0], inc[1]}, {chal[0], chal[1]});
  CHECK(o.challengers[0].anova.f == direct.f && o.challengers[0].anova.ci_low_log10 == direct.ci_low_log10);
  // A challenger without a test still counts in the Holm family (m = 2).
  ProbeTournament mixed;
  mixed.arms.push_back(MakeArm("inc", true, {"s1", "s2", "s6"}, ref::kT1Incumbent, 0, 3));
  mixed.arms.push_back(MakeArm("a", false, {"s1", "s3", "s6"}, ref::kT1Challenger, 1, 3));
  mixed.arms.push_back(MakeArm("b", false, {"s1", "s4", "s5", "s6"}, ref::kT1Challenger, 2, 3, 0.9));
  BlankFrames(mixed.arms[2], {0, 1, 2, 3, 4, 5, 6, 7, 8});
  o = ArbitrateTournament(mixed, {{"a", 0.5}, {"b", 0.5}}, std::nullopt);
  CHECK(o.challengers[1].paired_rounds == 1 && !o.challengers[1].anova.computed && o.challengers[1].p_holm == 1.0);
  CHECK(Near(*o.challengers[0].p_holm, std::min(1.0, 2.0 * o.challengers[0].anova.p_value), 1e-15));
  CHECK(o.verdict == ArbitrationVerdict::Switch && o.selected_arm == std::string("a"));
}

void HighestConfidenceIsSelected() {
  // Both challengers qualify; the higher C wins (formerly: undetermined).
  ProbeTournament two;
  two.arms.push_back(MakeArm("inc", true, {"s1", "s2", "s6"}, ref::kT1Incumbent, 0, 3));
  two.arms.push_back(MakeArm("a", false, {"s1", "s3", "s6"}, ref::kT1Challenger, 1, 3));
  two.arms.push_back(MakeArm("b", false, {"s1", "s4", "s5", "s6"}, ref::kT1Challenger, 2, 3, 0.9));
  ArbitrationOutcome o = ArbitrateTournament(two, {{"a", 0.5}, {"b", 0.9}}, std::nullopt);
  CHECK(o.challengers[0].decision.qualifies && o.challengers[1].decision.qualifies);
  CHECK(o.qualifying_arms.size() == 2 && o.tied_arms.empty());
  CHECK(o.challengers[1].confidence.confidence > o.challengers[0].confidence.confidence);
  CHECK(o.verdict == ArbitrationVerdict::Switch && o.selected_arm == std::string("b"));
  CHECK(aafr::test::Contains(o.reason, "highest C"));
  // Order does not matter: the higher C is chosen wherever it sits.
  o = ArbitrateTournament(two, {{"a", 0.9}, {"b", 0.1}}, std::nullopt);
  const std::string expected = o.challengers[0].confidence.confidence > o.challengers[1].confidence.confidence ? "a" : "b";
  CHECK(o.selected_arm == expected);
  // A qualifying challenger is preferred over a higher-C one that fails Holm,
  // feasibility or dwell: only qualifying arms are ranked.
  ProbeTournament lossy = two;
  for (auto& f : lossy.arms[2].frames) {
    for (auto& b : f.blocks) b.echoes[0].reset();  // 12.5 % loss: infeasible.
  }
  o = ArbitrateTournament(lossy, {{"a", 0.5}, {"b", 0.9}}, std::nullopt);
  CHECK(!o.challengers[1].decision.qualifies && o.selected_arm == std::string("a"));
}

void ExactTiesKeepTheEarliestArm() {
  // Identical probe data on two different paths gives exactly equal C. The
  // research engine compares with a strict '>', so the earliest challenger in
  // arm order is kept; AAFR does the same and records the tie.
  ProbeTournament tie;
  tie.arms.push_back(MakeArm("inc", true, {"s1", "s2", "s6"}, ref::kT1Incumbent, 0, 3));
  tie.arms.push_back(MakeArm("a", false, {"s1", "s3", "s6"}, ref::kT1Challenger, 1, 3));
  tie.arms.push_back(MakeArm("b", false, {"s1", "s4", "s5", "s6"}, ref::kT1Challenger, 2, 3));
  ArbitrationOutcome o = ArbitrateTournament(tie, {{"a", 0.5}, {"b", 0.5}}, std::nullopt);
  CHECK(o.challengers[0].confidence.confidence == o.challengers[1].confidence.confidence);
  CHECK(o.verdict == ArbitrationVerdict::Switch && o.selected_arm == std::string("a"));
  CHECK((o.tied_arms == std::vector<std::string>{"b"}));
  CHECK(aafr::test::Contains(o.reason, "exact tie"));
  // Swapping the arms' order swaps the choice: the rule is positional, not
  // a property of the paths.
  std::swap(tie.arms[1], tie.arms[2]);
  o = ArbitrateTournament(tie, {{"a", 0.5}, {"b", 0.5}}, std::nullopt);
  CHECK(o.selected_arm == std::string("b") && (o.tied_arms == std::vector<std::string>{"a"}));
  // Deterministic.
  CHECK(ArbitrateTournament(tie, {{"a", 0.5}, {"b", 0.5}}, std::nullopt).selected_arm == o.selected_arm);
}

void ArbitrationUndeterminedCases() {
  // Missing dG stays undetermined (the research engine would use s_g = 0).
  ArbitrationOutcome o = ArbitrateTournament(TwoArms(ref::kT1Incumbent, ref::kT1Challenger), {}, std::nullopt);
  CHECK(o.verdict == ArbitrationVerdict::Undetermined);
  // Invalid structure.
  ProbeTournament bad = TwoArms(ref::kT1Incumbent, ref::kT1Challenger);
  bad.arms[1].incumbent = true;
  o = ArbitrateTournament(bad, {}, std::nullopt);
  CHECK(!o.issues.empty() && o.verdict == ArbitrationVerdict::Undetermined);
}

}  // namespace

int main() {
  return aafr::test::RunTests({
      {"tournament_dimensions", TournamentDimensions},
      {"blocks_and_missing_echoes", BlocksAndMissingEchoes},
      {"log_transform_and_variances", LogTransformAndVariances},
      {"anova_matches_scipy", AnovaMatchesScipy},
      {"anova_decomposition", AnovaDecomposition},
      {"anova_degenerate_inputs", AnovaDegenerateInputs},
      {"f_survival_matches_scipy", FSurvivalMatchesScipy},
      {"holm_matches_reference", HolmMatchesReference},
      {"wilson_feasibility", WilsonFeasibility},
      {"confidence_score_components", ConfidenceScoreComponents},
      {"threshold_and_dwell_boundaries", ThresholdAndDwellBoundaries},
      {"recorded_tournament_decisions", RecordedTournamentDecisions},
      {"one_sided_limitation_is_reproduced", OneSidedLimitationIsReproduced},
      {"arbitration_end_to_end", ArbitrationEndToEnd},
      {"incomplete_frames_follow_the_research_engine", IncompleteFramesFollowTheResearchEngine},
      {"insufficient_paired_rounds", InsufficientPairedRounds},
      {"highest_confidence_is_selected", HighestConfidenceIsSelected},
      {"exact_ties_keep_the_earliest_arm", ExactTiesKeepTheEarliestArm},
      {"arbitration_undetermined_cases", ArbitrationUndeterminedCases},
  });
}
