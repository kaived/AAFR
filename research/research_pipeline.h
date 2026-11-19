#pragma once

// Phase 5A: the isolated research pipeline. Plain C++17, deterministic for
// fixed inputs and seeds. Decision only: it measures nothing, sends no
// probe, installs no rule and switches no traffic. Not used by the live
// runtime, the gateway or the browser.
//
//   Stage 1  PlanTournament
//     telemetry snapshot (controlled / recorded / synthetic, never "live")
//       -> XGBoost link weights (AssignPredictedWeights)
//       -> Dijkstra, A*, ACO, GA (RunPool on the predicted graph)
//       -> candidate evaluation (J, J_eng) and deduplication into arms
//       -> the arms to probe: the installed path and each distinct challenger
//   Stage 2  DecideTournament
//     the plan + probe observations supplied by the caller (recorded or
//     synthetic, 10 frames x 5 blocks x 8 echoes per arm, same arms as the plan)
//       -> per-arm measured outcome and goodness G, dG per challenger
//       -> reference arbitration (RM-ANOVA, Holm, Wilson feasibility, C,
//          C > 0.70, Holm p < 0.05, dwell >= 30 s)
//       -> ResearchDecisionRecord (keep / switch / undetermined, with reasons)
//
// Goodness of an arm uses its path's hop count, its available bottleneck
// bandwidth B(P) from the telemetry snapshot, and the mean RTT and delivery
// ratio of its own probes. Invalid blocks, incomplete frames, unpaired rounds
// and several qualifying challengers follow the research engine (see
// arbitration_reference.h); a missing dG stays undetermined.

#include <optional>
#include <string>
#include <vector>

#include "research/arbitration_reference.h"
#include "research/link_weight_model.h"
#include "research/normalization.h"
#include "research/path_algorithms.h"
#include "research/probe_tournament.h"

namespace aafr::research {

inline constexpr const char* kDecisionRecordSchema = "aafr.research.decision_record.v1";

// Where pipeline inputs come from. There is deliberately no "live" value:
// Phase 5A accepts no live measurement.
enum class DataOrigin {
  Controlled,  // Set up explicitly by a test or experiment (e.g. a configured topology state).
  Recorded,    // Replayed from a recorded run.
  Synthetic,   // Generated (e.g. seeded probe echoes).
};

struct TelemetrySnapshot {
  DataOrigin origin{DataOrigin::Controlled};
  std::string description;  // Required: what the snapshot is.
  ResearchGraph graph;      // Live link observations here are the snapshot's values.
  std::string source;
  std::string destination;
  std::vector<std::string> installed_path;            // The incumbent; must be on the graph.
  std::optional<double> seconds_since_last_switch;    // nullopt: no switch yet.
};

std::vector<std::string> ValidateTelemetry(const TelemetrySnapshot& telemetry);

struct PipelineConfig {
  AntColonyParameters aco;
  GeneticParameters ga;
  DecisionRule rule;
  FeasibilityBudget budget;
  ConfidenceWeights weights;
};

struct CandidateRecord {
  Candidate candidate;
  PathEvaluation evaluation;  // J and J_eng of the returned path on the snapshot (when a path was returned).
};

struct PlannedArm {
  std::string arm_id;  // "incumbent", "challenger-1", ...
  bool incumbent{false};
  std::vector<std::string> path;
  std::vector<PoolAlgorithm> algorithms;  // Algorithms that returned this path (empty for an unproposed incumbent).
};

struct TournamentPlan {
  std::vector<std::string> issues;               // Non-empty: the plan is invalid.
  std::vector<LinkWeightPrediction> predictions; // One per graph link, in link order.
  ResearchGraph predicted_graph;                 // The snapshot graph with predicted_weight set.
  std::vector<CandidateRecord> candidates;       // The four algorithms, in pool order.
  std::vector<PlannedArm> arms;                  // Incumbent first; empty when the plan is invalid.
  bool tournament_needed{false};                 // At least one challenger arm.
};

TournamentPlan PlanTournament(const TelemetrySnapshot& telemetry, const LinkWeightModel& model,
                              const NormalizationStatistics& statistics, const PipelineConfig& config = {});

struct ProbeObservations {
  DataOrigin origin{DataOrigin::Synthetic};
  std::string description;  // Required.
  ProbeTournament tournament;
};

struct ArmRecord {
  std::string arm_id;
  bool incumbent{false};
  std::vector<std::string> path;
  std::vector<PoolAlgorithm> algorithms;
  std::size_t sent{0}, returned{0};
  std::optional<double> mean_rtt_ms;
  double delivery_ratio{0.0};
  std::size_t complete_frames{0};  // All 5 blocks valid.
  std::size_t valid_frames{0};     // At least 1 valid block (used by the paired test).
  std::optional<double> bandwidth_mbps;  // B(P) from the snapshot.
  Score goodness;                        // G from the measured outcome.
};

struct ChallengerRecord {
  std::string arm_id;
  Score delta_goodness;
  ChallengerEvaluation evaluation;  // ANOVA, Holm p, feasibility, confidence, decision flags.
};

struct ResearchDecisionRecord {
  std::string schema{kDecisionRecordSchema};
  DataOrigin telemetry_origin{DataOrigin::Controlled};
  std::string telemetry_description;
  std::optional<DataOrigin> probe_origin;  // Absent when no tournament was needed or run.
  std::string probe_description;
  std::string model_id, model_source_commit, statistics_source;
  std::string source, destination;
  std::vector<std::string> installed_path;
  std::optional<double> seconds_since_last_switch;
  TournamentPlan plan;
  std::vector<std::string> observation_issues;
  std::vector<ArmRecord> arms;
  std::vector<ChallengerRecord> challengers;
  ArbitrationVerdict verdict{ArbitrationVerdict::Undetermined};
  std::optional<std::string> selected_arm;
  std::vector<std::string> selected_path;
  std::vector<std::string> qualifying_arms;
  std::vector<std::string> tied_arms;
  std::string reason;
  // The pipeline never acts; this states it in every record.
  std::string action{"none: decision only, no forwarding change"};
};

// Runs stage 2. With a plan that needs no tournament, `observations` is
// ignored and the verdict is keep.
ResearchDecisionRecord DecideTournament(const TelemetrySnapshot& telemetry, const TournamentPlan& plan,
                                        const std::optional<ProbeObservations>& observations,
                                        const LinkWeightModel& model, const NormalizationStatistics& statistics,
                                        const PipelineConfig& config = {});

// Deterministic JSON of a record. Timing fields (run times) are written only
// when include_timing is true, so records of identical inputs compare equal.
std::string ToJson(const ResearchDecisionRecord& record, bool include_timing = false);

const char* ToString(DataOrigin origin);

}  // namespace aafr::research
