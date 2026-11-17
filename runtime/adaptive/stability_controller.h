#pragma once

// Switching stability and anti-flapping gates. Plain C++17, deterministic:
// time is passed in (monotonic ns), never read from a clock.
//
// Ordinary switch: every gate must pass.
//  dwell       the active configuration has been active (since its
//              confirmed activation) for at least min_dwell_ms.
//  cooldown    at least cooldown_ms since the last committed switch, and
//              failed_activation_cooldown_ms since the last failed activation.
//  backoff     the candidate is not in its retry backoff after a failed
//              activation (initial backoff doubling up to a maximum; it
//              always expires, so no algorithm is blacklisted for good).
//  evidence    the evidence is valid (fresh, correctly designed, enough data).
//  margin      measured evidence: relative latency improvement >= margin,
//              its confidence bound > 0, and >= min_improvement_ms per
//              request; reliability repair: passes on the failure-rate
//              evidence; estimate only (no probing): estimated relative
//              J(P) improvement >= the larger estimate margin.
//  hysteresis  the same candidate path must be preferred in
//              sustain_evaluations consecutive evaluations. The streak resets
//              whenever the preferred candidate changes or no challenger is
//              preferred, so a fluctuation cannot accumulate wins.
//
// The rules are symmetric: returning to an earlier algorithm passes the same
// gates as leaving it, so recovery is never locked out.
//
// Safety override (active path failed, active algorithm ineligible, active
// configuration invalid, hard constraint violated): dwell, cooldown, margin
// and hysteresis are bypassed; evidence validity and the candidate backoff
// still apply, and the runtime still validates and confirms the replacement.

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "algorithms/algorithm_id.h"
#include "routing/topology/forwarding_problem.h"
#include "runtime/adaptive/reason_codes.h"

namespace aafr::runtime {

struct StabilityConfig {
  double min_dwell_ms{10'000.0};
  double cooldown_ms{15'000.0};
  double failed_activation_cooldown_ms{5'000.0};
  double candidate_backoff_initial_ms{5'000.0};
  double candidate_backoff_max_ms{60'000.0};
  double improvement_margin_relative{0.10};
  double min_improvement_ms{0.5};
  double estimate_margin_relative{0.20};
  int sustain_evaluations{2};

  std::vector<std::string> Validate() const;
  // Every gate disabled: for the isolated "no stability controls"
  // experiment only. Never used by the browser.
  static StabilityConfig Disabled();
};

enum class EvidenceKind {
  None,
  EstimateOnly,       // J(P) on the forwarding graph; nothing measured.
  Measured,           // Arbitration found the challenger faster.
  ReliabilityRepair,  // Arbitration found the incumbent over the failure budget, the challenger within it.
};

struct SwitchProposal {
  algorithms::AlgorithmId candidate{algorithms::AlgorithmId::Dijkstra};
  forwarding::Path path;
  EvidenceKind evidence{EvidenceKind::None};
  bool evidence_valid{false};
  std::string evidence_note;
  std::optional<double> measured_improvement_relative;
  std::optional<double> measured_improvement_lower;
  std::optional<double> measured_improvement_ms;
  std::optional<double> estimated_improvement_relative;
  // Set when the active configuration must be left for a safety reason.
  std::optional<RuntimeReason> safety_reason;
};

struct GateResult {
  std::string gate;
  bool applicable{true};
  bool passed{false};
  std::string detail;
  std::optional<double> remaining_ms;
};

struct StabilityAssessment {
  bool allowed{false};
  bool safety_override{false};
  GateResult dwell{"min_dwell"};
  GateResult cooldown{"cooldown"};
  GateResult backoff{"candidate_backoff"};
  GateResult evidence{"evidence_quality"};
  GateResult margin{"improvement_margin"};
  GateResult hysteresis{"hysteresis"};
  std::vector<RuntimeReason> blocking;
  std::string explanation;

  std::vector<GateResult> Gates() const { return {dwell, cooldown, backoff, evidence, margin, hysteresis}; }
};

class SwitchingStabilityController {
 public:
  explicit SwitchingStabilityController(StabilityConfig config = {}) : config_(config) {}

  // Assesses one proposal and advances the hysteresis streak. Call once per
  // evaluation that produced a challenger.
  StabilityAssessment Assess(const SwitchProposal& proposal, std::int64_t now_ns);
  // The evaluation preferred no challenger: the hysteresis streak resets.
  void NoChallenger();

  void OnInitialActivation(std::int64_t now_ns);
  void OnCommitted(std::int64_t now_ns);
  void OnActivationFailed(const forwarding::Path& candidate_path, std::int64_t now_ns);

  double DwellRemainingMs(std::int64_t now_ns) const;
  double CooldownRemainingMs(std::int64_t now_ns) const;
  double BackoffRemainingMs(const forwarding::Path& path, std::int64_t now_ns) const;
  int streak() const { return streak_; }
  const StabilityConfig& config() const { return config_; }

 private:
  struct Backoff {
    std::int64_t until_ns{0};
    double next_ms{0.0};
  };

  StabilityConfig config_;
  std::optional<std::int64_t> active_since_ns_;
  std::optional<std::int64_t> last_switch_ns_;
  std::optional<std::int64_t> last_failure_ns_;
  std::map<forwarding::Path, Backoff> backoff_;
  std::optional<forwarding::Path> streak_path_;
  int streak_{0};
};

const char* ToString(EvidenceKind value);

}  // namespace aafr::runtime
