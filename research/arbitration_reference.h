#pragma once

// Reference statistical arbitration of a probe tournament: repeated-measures
// ANOVA on log10 RTT, Holm-Bonferroni correction, Wilson feasibility, the
// confidence score C and the switching decision. Plain C++17, pure
// functions. It decides nothing on its own and switches nothing; it is not
// used by the live runtime (whose arbitration in runtime/adaptive is a
// different, unchanged method).
//
// Per challenger against the incumbent (rounds r = 1 .. n, n = kFrames):
//   x_a,r   frame value (log10) of arm a in round r
//   RM-ANOVA, arm x round, two arms: SS_arm, SS_round, SS_error,
//   F = (SS_arm / 1) / (SS_error / (n - 1)) ~ F(1, n - 1),
//   partial eta^2 = SS_arm / (SS_arm + SS_error),
//   95 % CI on the mean paired difference d = x_challenger - x_incumbent,
//   RTT ratio 10^mean(d) with CI 10^bounds.
//   Holm step-down adjustment of the p-values across challengers.
//
// Feasibility (per arm, from its probes): loss budget 3 %, mean RTT budget
// 150 ms. Infeasible: lower Wilson 95 % bound of loss above the budget, or
// mean RTT above its budget. Within budget: upper Wilson bound at or below
// the loss budget and mean RTT within its budget.
//
// Confidence (reference rule):
//   s_eta = partial eta^2
//   s_ci  = -1 if the challenger is infeasible;
//           +1 if the incumbent is infeasible and the challenger within budget (repair);
//           otherwise +min(1, -CI_hi / 0.05) if CI_hi < 0,
//                     -min(1,  CI_lo / 0.05) if CI_lo > 0, else 0
//   s_g   = clip(dG / g_scale, -1, 1), g_scale = 1
//   C     = 0.40 s_eta + 0.35 s_ci + 0.25 s_g
// A challenger qualifies when C > 0.70, its Holm-adjusted p < 0.05 and at
// least 30 s have passed since the last switch.
//
// Known limitation, reproduced deliberately: because s_g is clipped, a
// challenger with dG <= -0.2 cannot reach C > 0.70 even with s_eta = s_ci = 1,
// so a lower-goodness path never wins.

#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "research/probe_tournament.h"

namespace aafr::research {

// P(F > f) for F(d1, d2), exact through the regularized incomplete beta.
double FSurvival(double f, double d1, double d2);

struct PairedAnova {
  bool computed{false};
  std::string unavailable_reason;
  std::size_t rounds{0};
  double df_effect{1.0};
  double df_error{0.0};
  double ss_arm{0.0};
  double ss_round{0.0};
  double ss_error{0.0};
  double mean_difference_log10{0.0};  // Challenger minus incumbent (negative: challenger faster).
  double f{0.0};
  double p_value{1.0};
  double partial_eta_squared{0.0};
  double ci_level{0.95};
  double ci_low_log10{0.0};
  double ci_high_log10{0.0};
  double rtt_ratio{1.0};  // 10^mean_difference_log10.
  double ratio_low{1.0};
  double ratio_high{1.0};
};

// Two-arm repeated-measures ANOVA over paired per-round values (log10).
// Both vectors hold one value per round in round order.
PairedAnova RepeatedMeasuresAnova(const std::vector<double>& incumbent_log10,
                                  const std::vector<double>& challenger_log10, double ci_level = 0.95);

// Holm step-down adjusted p-values, same order as the input (reuses the
// existing implementation in runtime/adaptive).
std::vector<double> HolmAdjusted(const std::vector<double>& p_values);

struct FeasibilityBudget {
  double loss_ratio{0.03};
  double mean_rtt_ms{150.0};
  double wilson_level{0.95};
};

struct ArmFeasibility {
  std::size_t sent{0};
  std::size_t lost{0};
  std::optional<double> mean_rtt_ms;
  double loss_low{0.0};   // Wilson bounds of the loss ratio.
  double loss_high{1.0};
  bool infeasible{false};
  bool within_budget{false};
};

ArmFeasibility EvaluateFeasibility(std::size_t sent, std::size_t lost, std::optional<double> mean_rtt_ms,
                                   const FeasibilityBudget& budget = {});

struct ConfidenceWeights {
  double eta{0.40};
  double ci{0.35};
  double goodness{0.25};
  double ci_scale{0.05};
  double goodness_scale{1.0};
};

struct ConfidenceInputs {
  double partial_eta_squared{0.0};
  double ci_low_log10{0.0};
  double ci_high_log10{0.0};
  ArmFeasibility incumbent;
  ArmFeasibility challenger;
  std::optional<double> delta_goodness;  // G_challenger - G_incumbent.
};

struct ConfidenceResult {
  bool computed{false};
  std::string unavailable_reason;
  double s_eta{0.0};
  double s_ci{0.0};
  double s_g{0.0};
  double confidence{0.0};
  bool repair{false};                 // s_ci forced to +1 by the repair rule.
  bool challenger_infeasible{false};  // s_ci forced to -1.
};

ConfidenceResult ComputeConfidence(const ConfidenceInputs& inputs, const ConfidenceWeights& weights = {});

struct DecisionRule {
  double confidence_threshold{0.70};  // C must exceed it.
  double alpha{0.05};                 // Holm-adjusted p must be below it.
  double dwell_s{30.0};               // Minimum time since the last switch.
};

struct ChallengerDecision {
  bool confidence_ok{false};
  bool significance_ok{false};
  bool dwell_ok{false};
  bool qualifies{false};
};

// seconds_since_last_switch: nullopt when no switch has happened yet (the
// dwell condition is then met).
ChallengerDecision Decide(const ConfidenceResult& confidence, std::optional<double> p_holm,
                          std::optional<double> seconds_since_last_switch, const DecisionRule& rule = {});

struct ChallengerEvaluation {
  std::string arm_id;
  PairedAnova anova;
  std::optional<double> p_holm;
  ArmFeasibility feasibility;
  ConfidenceResult confidence;
  ChallengerDecision decision;
};

enum class ArbitrationVerdict {
  Keep,          // No challenger qualifies.
  Switch,        // Exactly one challenger qualifies (selected).
  Undetermined,  // The method does not define the outcome (see reason).
};

struct ArbitrationOutcome {
  std::vector<std::string> issues;  // Tournament validation problems.
  std::vector<ArmSummary> arms;
  ArmFeasibility incumbent_feasibility;
  std::vector<ChallengerEvaluation> challengers;
  ArbitrationVerdict verdict{ArbitrationVerdict::Undetermined};
  std::optional<std::string> selected_arm;
  std::string reason;
};

// Full reference evaluation of a tournament. delta_goodness maps each
// challenger arm id to its dG (computed by the caller with Goodness()); a
// challenger without a dG cannot get a confidence.
ArbitrationOutcome ArbitrateTournament(const ProbeTournament& tournament,
                                       const std::map<std::string, std::optional<double>>& delta_goodness,
                                       std::optional<double> seconds_since_last_switch,
                                       const DecisionRule& rule = {}, const FeasibilityBudget& budget = {},
                                       const ConfidenceWeights& weights = {});

const char* ToString(ArbitrationVerdict verdict);

}  // namespace aafr::research
