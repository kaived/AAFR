// Research alignment, Phase 0: checks that the reference values
// (tests/fixtures/research/reference_values.h) are internally consistent and
// that the specified equations reproduce the recorded results. These are
// consistency checks of the reference data. They do not test an AAFR
// implementation of the method (none exists yet); the only product code
// exercised is the existing Dijkstra and the existing Wilson interval, on the
// reference inputs. Deterministic; no network access.

#include <algorithm>
#include <cmath>
#include <string>

#include "algorithms/common/path_algorithm.h"
#include "runtime/adaptive/statistical_arbitration.h"
#include "tests/fixtures/research/reference_values.h"
#include "tests/support/test_support.h"

namespace fwd = aafr::forwarding;
namespace rf = aafr::research_fixture;
using aafr::algorithms::AlgorithmId;
using aafr::algorithms::AlgorithmRequest;
using aafr::algorithms::AlgorithmStatus;
using aafr::algorithms::CreateAlgorithm;

namespace {

// The testbed topology as a forwarding graph: s1 is the entry (h1's switch),
// s6 the destination (h2's switch). Links are bidirectional; nothing leaves s6
// or enters s1, since those directions cannot lie on an s1 -> s6 simple path.
fwd::ForwardingGraph TestbedGraph() {
  fwd::ForwardingGraph graph("research_testbed");
  for (const char* id : {"s1", "s2", "s3", "s4", "s5", "s6"}) {
    fwd::ForwardingNode node;
    node.id = id;
    node.role = std::string(id) == rf::kSenderSwitch     ? fwd::NodeRole::Entry
                : std::string(id) == rf::kReceiverSwitch ? fwd::NodeRole::Destination
                                                         : fwd::NodeRole::Relay;
    graph.AddNode(node);
  }
  const auto add = [&graph](const char* from, const char* to, const rf::TestbedLink& link) {
    if (std::string(to) == rf::kSenderSwitch || std::string(from) == rf::kReceiverSwitch) return;
    fwd::ForwardingEdge edge;
    edge.from = from;
    edge.to = to;
    edge.latency_ms = fwd::Measurement::Of(link.delay_ms, fwd::Provenance::Configured);
    edge.loss_ratio = fwd::Measurement::Of(0.0, fwd::Provenance::Configured);
    edge.bandwidth_mbps = fwd::Measurement::Of(link.capacity_mbps, fwd::Provenance::Configured);
    graph.AddEdge(edge);
  };
  for (const auto& link : rf::kTestbedLinks) {
    add(link.a, link.b, link);
    add(link.b, link.a, link);
  }
  return graph;
}

// Dijkstra's static cost is the start-up link delay alone, so every other
// weight of the existing objective is 0.
fwd::ForwardingProblem DelayOnlyProblem(const fwd::ForwardingGraph& graph) {
  fwd::ForwardingProblem problem;
  problem.id = "static_delay";
  problem.source = rf::kSenderSwitch;
  problem.destination = rf::kReceiverSwitch;
  problem.objective.latency_weight = 1.0;
  problem.objective.loss_weight = 0.0;
  problem.objective.hop_weight = 0.0;
  problem.objective.policy_weight = 0.0;
  problem.graph_version = graph.version();
  return problem;
}

void TestbedTopologyIsWellFormed() {
  const fwd::ForwardingGraph graph = TestbedGraph();
  CHECK(graph.Validate().empty());
  CHECK(graph.nodes().size() == 6);
  CHECK(rf::kTestbedLinks.size() == 9);
  CHECK(fwd::ValidateProblem(graph, DelayOnlyProblem(graph)).empty());
}

void StaticDelayDijkstraPicksTheShortNarrowPath() {
  const fwd::ForwardingGraph graph = TestbedGraph();
  const fwd::ForwardingProblem problem = DelayOnlyProblem(graph);
  AlgorithmRequest request;
  request.graph = &graph;
  request.problem = &problem;
  const auto result = CreateAlgorithm(AlgorithmId::Dijkstra)->Run(request);
  CHECK(result.status == AlgorithmStatus::PathFound);
  CHECK(fwd::PathText(result.path) == fwd::PathText({"s1", "s2", "s6"}));
  const auto chosen = fwd::ValidatePath(graph, problem, result.path);
  CHECK_NEAR(chosen.latency_ms, rf::kDefaultPathDelayMs, 1e-9);
  const auto detour = fwd::ValidatePath(graph, problem, {"s1", "s3", "s6"});
  CHECK(detour.valid);
  CHECK_NEAR(detour.latency_ms, rf::kDetourPathDelayMs, 1e-9);
}

void QualityScoreWorkedCheck() {
  const double j = rf::kJHopWeight * rf::kJCheckHops + rf::kJBandwidthWeight / rf::kJCheckBottleneck +
                   rf::kJDelayWeight * rf::kJCheckDelay + rf::kJLossWeight * rf::kJCheckLoss;
  CHECK_NEAR(j, rf::kJCheckValue, 5e-4);
}

void TournamentShape() {
  CHECK(rf::kProbesPerArm == 400);
  CHECK(rf::kMinRepliesPerBlock <= rf::kEchoesPerBlock);
  CHECK_NEAR(rf::kWeightEta + rf::kWeightCi + rf::kWeightGoodness, 1.0, 1e-12);
  for (const auto& t : rf::kRecordedTournaments) {
    CHECK(t.incumbent_delivered >= 0 && t.incumbent_delivered <= static_cast<int>(rf::kProbesPerArm));
    CHECK(t.challenger_delivered >= 0 && t.challenger_delivered <= static_cast<int>(rf::kProbesPerArm));
    CHECK(t.ratio_ci_low <= t.rtt_ratio && t.rtt_ratio <= t.ratio_ci_high);
    // The recorded ratio is 10^(mean paired log10 difference), a
    // geometric-mean ratio, while the RTT columns are arithmetic means, so the
    // two need not be equal (Loss 1: 20.5 / 9.2 = 2.23 against 2.294). They
    // must agree on which arm is faster.
    const double from_means = t.challenger_mean_rtt_ms / t.incumbent_mean_rtt_ms;
    CHECK((from_means < 1.0) == (t.rtt_ratio < 1.0));
  }
}

// ---- The confidence equation and the feasibility rule as specified. ----

struct ArmFeasibility {
  bool infeasible{false};     // Lower Wilson bound of loss above the budget, or mean RTT over budget.
  bool within_budget{false};  // Upper Wilson bound of loss at or below the budget.
};

ArmFeasibility Feasibility(int delivered, double mean_rtt_ms) {
  const std::size_t n = rf::kProbesPerArm;
  const std::size_t lost = n - static_cast<std::size_t>(delivered);
  double low = 0.0, high = 1.0;
  aafr::runtime::stats::Wilson(lost, n, rf::kWilsonLevel, low, high);
  ArmFeasibility f;
  f.infeasible = low > rf::kLossBudget || mean_rtt_ms > rf::kMeanRttBudgetMs;
  f.within_budget = high <= rf::kLossBudget && mean_rtt_ms <= rf::kMeanRttBudgetMs;
  return f;
}

double ScoreCi(const rf::Tournament& t) {
  const ArmFeasibility incumbent = Feasibility(t.incumbent_delivered, t.incumbent_mean_rtt_ms);
  const ArmFeasibility challenger = Feasibility(t.challenger_delivered, t.challenger_mean_rtt_ms);
  if (challenger.infeasible) return -1.0;                            // Can never win.
  if (incumbent.infeasible && challenger.within_budget) return 1.0;  // Repair.
  // CI on the log10 paired difference, challenger minus incumbent.
  const double lo = std::log10(t.ratio_ci_low);
  const double hi = std::log10(t.ratio_ci_high);
  if (hi < 0.0) return std::min(1.0, -hi / rf::kCiScale);
  if (lo > 0.0) return -std::min(1.0, lo / rf::kCiScale);
  return 0.0;
}

double Confidence(const rf::Tournament& t) {
  const double s_eta = t.partial_eta_squared;
  const double s_g = std::clamp(t.delta_goodness / rf::kGoodnessScale, -1.0, 1.0);
  return rf::kWeightEta * s_eta + rf::kWeightCi * ScoreCi(t) + rf::kWeightGoodness * s_g;
}

void RecordedConfidenceReproduces() {
  for (const auto& t : rf::kRecordedTournaments) {
    aafr::test::current_test = std::string("recorded_") + t.run + "_" + std::to_string(t.number);
    const double c = Confidence(t);
    // Recorded to three decimals.
    CHECK_NEAR(c, t.confidence, 6e-4);
    // Holm p and the dwell timer were satisfied in every tournament (p down
    // to 1e-16, tournaments more than 30 s apart), so C decides.
    const bool swap = c > rf::kConfidenceThreshold;
    CHECK(swap == (t.decision == rf::Decision::Swap));
  }
  aafr::test::current_test = "recorded_confidence_reproduces";
}

void RepairRuleDecidesTheLossSwap() {
  // Loss 1: the challenger is 2.29 times slower, so the CI alone gives
  // s_ci = -1; the swap happens only because the incumbent breaks the loss
  // budget and the challenger is within it.
  const rf::Tournament& loss1 = rf::kRecordedTournaments[4];
  CHECK(std::string(loss1.run) == "Loss" && loss1.number == 1);
  CHECK(std::log10(loss1.ratio_ci_low) > 0.0);
  CHECK(Feasibility(loss1.incumbent_delivered, loss1.incumbent_mean_rtt_ms).infeasible);
  CHECK(Feasibility(loss1.challenger_delivered, loss1.challenger_mean_rtt_ms).within_budget);
  CHECK(ScoreCi(loss1) == 1.0);
}

void OneSidedGoodnessDefect() {
  // With s_eta = 1 and s_ci = 1, C > 0.70 needs s_g > -0.2, so a
  // lower-goodness path can never win. Known defect of the reference rule;
  // preserved, not corrected.
  const double limit = (rf::kConfidenceThreshold - rf::kWeightEta - rf::kWeightCi) / rf::kWeightGoodness;
  CHECK_NEAR(limit, rf::kOneSidedGoodnessLimit, 1e-12);
  const rf::Tournament& congestion2 = rf::kRecordedTournaments[1];
  CHECK(congestion2.delta_goodness < rf::kOneSidedGoodnessLimit);
  CHECK(ScoreCi(congestion2) == 1.0);                          // Challenger clearly faster ...
  CHECK(Confidence(congestion2) <= rf::kConfidenceThreshold);  // ... and still kept.
}

}  // namespace

int main() {
  return aafr::test::RunTests({
      {"testbed_topology_is_well_formed", TestbedTopologyIsWellFormed},
      {"static_delay_dijkstra_picks_short_narrow_path", StaticDelayDijkstraPicksTheShortNarrowPath},
      {"quality_score_worked_check", QualityScoreWorkedCheck},
      {"tournament_shape", TournamentShape},
      {"recorded_confidence_reproduces", RecordedConfidenceReproduces},
      {"repair_rule_decides_loss_swap", RepairRuleDecidesTheLossSwap},
      {"one_sided_goodness_defect", OneSidedGoodnessDefect},
  });
}
