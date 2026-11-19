// Research alignment, Phase 5A: the isolated research pipeline
// (research/research_pipeline). Controlled telemetry on the six-switch
// testbed and seeded synthetic probe echoes (both labelled as such), plus
// the seven recorded research tournaments as an independent numerical
// reference for the statistics layer and the goodness score. Needs the
// XGBoost library (fails without it). Deterministic; no network access; no
// probing, steering or forwarding.

#include <cmath>
#include <filesystem>
#include <functional>
#include <iostream>
#include <limits>
#include <set>
#include <string>

#include "algorithms/common/path_algorithm.h"
#include "research/json_reader.h"
#include "research/research_pipeline.h"
#include "routing/topology/forwarding_problem.h"
#include "tests/fixtures/research/recorded_tournaments.h"
#include "tests/fixtures/research/reference_values.h"
#include "tests/fixtures/research/xgboost_reference_values.h"
#include "tests/support/test_support.h"

using namespace aafr::research;
namespace rf = aafr::research_fixture;
namespace rec = aafr::recorded_tournaments;
namespace xr = aafr::xgboost_reference;
namespace alg = aafr::algorithms;
using Path = std::vector<std::string>;

namespace {

const std::string kModelDir = AAFR_TEST_XGBOOST_MODEL_DIR;
const std::string kLibrary = AAFR_TEST_XGBOOST_LIBRARY;
std::unique_ptr<LinkWeightModel> g_model;
NormalizationStatistics g_stats;

bool Near(double a, double b, double rel, double abs = 0.0) { return std::fabs(a - b) <= rel * std::fabs(b) + abs; }

// ---- Controlled telemetry ----

ResearchGraph TestbedGraph() {
  ResearchGraph g;
  g.nodes = {"s1", "s2", "s3", "s4", "s5", "s6"};
  for (const auto& l : rf::kTestbedLinks) {
    for (const auto& [from, to] : {std::pair{l.a, l.b}, std::pair{l.b, l.a}}) {
      ResearchLink link;
      link.observation = LinkObservation{from, to, l.delay_ms, 0.0, l.capacity_mbps, 0.0};
      link.startup_delay_ms = l.delay_ms;
      g.links.push_back(link);
    }
  }
  return g;
}

void SetBoth(ResearchGraph& g, const std::string& a, const std::string& b, const std::function<void(ResearchLink&)>& f) {
  for (auto& l : g.links) {
    if ((l.observation.from == a && l.observation.to == b) || (l.observation.from == b && l.observation.to == a)) f(l);
  }
}

TelemetrySnapshot Idle() {
  TelemetrySnapshot t;
  t.origin = DataOrigin::Controlled;
  t.description = "controlled: six-switch testbed, configured delays and capacities, idle";
  t.graph = TestbedGraph();
  t.source = "s1";
  t.destination = "s6";
  t.installed_path = {"s1", "s2", "s6"};
  return t;
}

// The congestion state of the recorded first congestion tournament: s1-s2
// lossy (22 %) and almost fully used; everything else idle.
TelemetrySnapshot Congested() {
  TelemetrySnapshot t = Idle();
  t.description = "controlled: six-switch testbed, s1-s2 at 22 % loss and 99 % utilization";
  SetBoth(t.graph, "s1", "s2", [](ResearchLink& l) {
    l.observation.loss_ratio = 0.22;
    l.observation.utilization = 0.99;
  });
  return t;
}

// ---- Synthetic probe observations ----

struct Profile {
  double base_ms;
  std::size_t replies_per_block;  // Of 8.
};

// Seeded log-normal echoes with a drift shared by all arms in a round; the
// first `replies_per_block` echoes of each block return, the rest are lost.
ProbeObservations Synthetic(const TournamentPlan& plan, const std::function<Profile(const PlannedArm&)>& profile,
                            std::uint64_t seed, std::function<void(ProbeTournament&)> tweak = {}) {
  alg::Rng rng(seed);
  std::vector<double> drift(kFrames);
  for (double& d : drift) d = std::exp(0.03 * rng.Normal());
  ProbeObservations o;
  o.origin = DataOrigin::Synthetic;
  o.description = "synthetic: seeded log-normal echoes, seed " + std::to_string(seed);
  for (std::size_t a = 0; a < plan.arms.size(); ++a) {
    const Profile p = profile(plan.arms[a]);
    ProbeArm arm;
    arm.arm_id = plan.arms[a].arm_id;
    arm.incumbent = plan.arms[a].incumbent;
    arm.path = plan.arms[a].path;
    for (std::size_t f = 0; f < kFrames; ++f) {
      ProbeFrame frame;
      frame.round = f;
      frame.position_in_round = (a + f) % plan.arms.size();
      for (std::size_t b = 0; b < kBlocks; ++b) {
        ProbeBlock block;
        for (std::size_t e = 0; e < kEchoes; ++e) {
          const double rtt = p.base_ms * drift[f] * std::exp(0.02 * rng.Normal());
          block.echoes.push_back(e < p.replies_per_block ? std::optional<double>(rtt) : std::nullopt);
        }
        frame.blocks.push_back(block);
      }
      arm.frames.push_back(frame);
    }
    o.tournament.arms.push_back(arm);
  }
  if (tweak) tweak(o.tournament);
  return o;
}

bool Contains(const Path& p, const std::string& a, const std::string& b) {
  for (std::size_t i = 0; i + 1 < p.size(); ++i) {
    if (p[i] == a && p[i + 1] == b) return true;
  }
  return false;
}

std::vector<Path> AllSimplePaths(const ResearchGraph& g, const std::string& s, const std::string& t) {
  std::vector<Path> out;
  Path path{s};
  std::set<std::string> seen{s};
  std::function<void(const std::string&)> visit = [&](const std::string& u) {
    if (u == t) {
      out.push_back(path);
      return;
    }
    for (const auto& l : g.links) {
      if (!l.available || l.observation.from != u || seen.count(l.observation.to)) continue;
      seen.insert(l.observation.to);
      path.push_back(l.observation.to);
      visit(l.observation.to);
      path.pop_back();
      seen.erase(l.observation.to);
    }
  };
  visit(s);
  return out;
}

void PrintRecord(const char* label, const ResearchDecisionRecord& r) {
  std::cout << "RESULT " << label << ": verdict=" << ToString(r.verdict);
  for (const auto& a : r.plan.arms) {
    std::cout << " | " << a.arm_id << "=" << aafr::forwarding::PathText(a.path) << " [";
    for (std::size_t i = 0; i < a.algorithms.size(); ++i) std::cout << (i ? "+" : "") << ToString(a.algorithms[i]);
    std::cout << "]";
  }
  for (const auto& c : r.challengers) {
    if (c.evaluation.confidence.computed) {
      std::cout << " | " << c.arm_id << " C=" << c.evaluation.confidence.confidence << " dG=" << *c.delta_goodness.value
                << (c.evaluation.confidence.repair ? " repair" : "");
    }
  }
  std::cout << " | reason=" << r.reason << "\n";
}

// ---- Telemetry and plan ----

void InvalidTelemetryIsRefused() {
  TelemetrySnapshot t = Idle();
  t.description.clear();
  CHECK(!ValidateTelemetry(t).empty());
  t = Idle();
  t.installed_path = {"s1", "s5", "s6"};  // Not on the graph.
  CHECK(!ValidateTelemetry(t).empty());
  t = Idle();
  t.installed_path = {"s2", "s6"};  // Wrong start.
  CHECK(!ValidateTelemetry(t).empty());
  t = Idle();
  t.seconds_since_last_switch = -1.0;
  CHECK(!ValidateTelemetry(t).empty());
  const TournamentPlan plan = PlanTournament(t, *g_model, g_stats);
  CHECK(!plan.issues.empty() && plan.arms.empty());
  const auto record = DecideTournament(t, plan, std::nullopt, *g_model, g_stats);
  CHECK(record.verdict == ArbitrationVerdict::Undetermined && aafr::test::Contains(record.reason, "invalid plan"));
  CHECK(ValidateTelemetry(Idle()).empty());
}

void PlanUsesModelWeightsAndFourAlgorithms() {
  const TelemetrySnapshot t = Idle();
  const TournamentPlan plan = PlanTournament(t, *g_model, g_stats);
  CHECK(plan.issues.empty());
  CHECK(plan.predictions.size() == t.graph.links.size());
  // Link weights are the model's, equal to Python XGBoost's for these inputs.
  for (std::size_t i = 0; i < plan.predicted_graph.links.size(); ++i) {
    const auto& l = plan.predicted_graph.links[i];
    CHECK(plan.predictions[i].status == PredictionStatus::Ok && l.predicted_weight == plan.predictions[i].weight);
    for (const auto& c : xr::kCases) {
      const std::string a = "testbed_" + l.observation.from + "_" + l.observation.to;
      const std::string b = "testbed_" + l.observation.to + "_" + l.observation.from;
      if (a == c.label || b == c.label) CHECK(*plan.predictions[i].raw == c.raw);
    }
  }
  CHECK(plan.candidates.size() == 4);
  const PoolAlgorithm order[] = {PoolAlgorithm::Dijkstra, PoolAlgorithm::AStar, PoolAlgorithm::AntColony, PoolAlgorithm::Genetic};
  for (std::size_t i = 0; i < 4; ++i) {
    CHECK(plan.candidates[i].candidate.algorithm == order[i]);
    CHECK(plan.candidates[i].candidate.status == CandidateStatus::PathFound);
    CHECK(plan.candidates[i].evaluation.quality.value && plan.candidates[i].evaluation.engine_cost.value);
  }
  // A* optimum on the predicted weights and the J_eng optimum, by brute force.
  double w_best = std::numeric_limits<double>::infinity(), j_best = std::numeric_limits<double>::infinity();
  for (const Path& p : AllSimplePaths(plan.predicted_graph, "s1", "s6")) {
    double w = 0.0;
    for (std::size_t k = 0; k + 1 < p.size(); ++k) {
      for (const auto& l : plan.predicted_graph.links) {
        if (l.observation.from == p[k] && l.observation.to == p[k + 1]) w += *l.predicted_weight;
      }
    }
    w_best = std::min(w_best, w);
    j_best = std::min(j_best, *EvaluatePath(plan.predicted_graph, p, g_stats).engine_cost.value);
  }
  CHECK(*plan.candidates[1].candidate.optimization_cost == w_best);
  CHECK(*plan.candidates[2].candidate.optimization_cost >= j_best - 1e-9);
  CHECK(*plan.candidates[3].candidate.optimization_cost >= j_best - 1e-9);
  // Arms: incumbent first, then one arm per distinct non-installed path.
  CHECK(!plan.arms.empty() && plan.arms[0].incumbent && plan.arms[0].path == t.installed_path);
  std::set<Path> distinct;
  for (const auto& c : plan.candidates) {
    if (c.candidate.path != t.installed_path) distinct.insert(c.candidate.path);
  }
  CHECK(plan.arms.size() == 1 + distinct.size());
  CHECK(plan.tournament_needed == !distinct.empty());
}

void NoChallengerMeansKeepWithoutTournament() {
  // Installed path = the path every algorithm returns? Use the idle plan and
  // install the path that the most algorithms chose; if all agree, no
  // tournament is needed.
  TelemetrySnapshot t = Idle();
  // Make every algorithm agree: idle testbed with only s1-s3-s6 available.
  for (auto& l : t.graph.links) l.available = false;
  SetBoth(t.graph, "s1", "s3", [](ResearchLink& l) { l.available = true; });
  SetBoth(t.graph, "s3", "s6", [](ResearchLink& l) { l.available = true; });
  t.installed_path = {"s1", "s3", "s6"};
  const TournamentPlan plan = PlanTournament(t, *g_model, g_stats);
  CHECK(plan.issues.empty() && !plan.tournament_needed && plan.arms.size() == 1);
  CHECK(plan.arms[0].algorithms.size() == 4);  // All four returned the installed path.
  const auto r = DecideTournament(t, plan, std::nullopt, *g_model, g_stats);
  CHECK(r.verdict == ArbitrationVerdict::Keep && !r.probe_origin && r.challengers.empty());
}

// ---- Decisions ----

void CongestionRepairSwitch() {
  TelemetrySnapshot t = Congested();
  const TournamentPlan plan = PlanTournament(t, *g_model, g_stats);
  CHECK(plan.issues.empty() && plan.tournament_needed);
  // Dijkstra stays on its start-up path; A* and the J_eng search leave s1-s2.
  CHECK((plan.candidates[0].candidate.path == Path{"s1", "s2", "s6"}));
  for (std::size_t i = 1; i < 4; ++i) CHECK(!Contains(plan.candidates[i].candidate.path, "s1", "s2"));
  // Synthetic probes shaped like the recorded tournament: the incumbent
  // returns 3 of 8 echoes per block at ~81.5 ms; one challenger is clean at
  // ~20.4 ms; any other challenger is lossy (2.5 % ... 50 %) and slow.
  const Path best_path = plan.candidates[2].candidate.path;  // ACO's J_eng choice.
  const auto obs = Synthetic(plan, [&](const PlannedArm& a) {
    if (a.incumbent) return Profile{81.5, 3};
    if (a.path == best_path) return Profile{20.4, 8};
    return Profile{60.0, 4};
  }, 11);
  const auto r = DecideTournament(t, plan, obs, *g_model, g_stats);
  PrintRecord("congestion_repair", r);
  CHECK(r.verdict == ArbitrationVerdict::Switch && r.selected_path == best_path);
  CHECK(r.arms[0].sent == 400 && r.arms[0].returned == 150);
  for (const auto& c : r.challengers) {
    if (c.arm_id == *r.selected_arm) {
      CHECK(c.evaluation.confidence.repair && c.evaluation.confidence.confidence > 0.70);
      CHECK(*c.evaluation.p_holm < 0.05);
    } else {
      CHECK(!c.evaluation.decision.qualifies);
    }
  }
  // The pipeline's arbitration equals the reference arbitration called
  // directly with the same tournament and dG.
  std::map<std::string, std::optional<double>> dg;
  for (const auto& c : r.challengers) dg[c.arm_id] = c.delta_goodness.value;
  const ArbitrationOutcome direct = ArbitrateTournament(obs.tournament, dg, std::nullopt);
  CHECK(direct.verdict == r.verdict && direct.selected_arm == r.selected_arm);
  for (std::size_t i = 0; i < direct.challengers.size(); ++i) {
    CHECK(direct.challengers[i].confidence.confidence == r.challengers[i].evaluation.confidence.confidence);
  }
  // dG from G of the measured outcomes (path hops, snapshot B(P), probe RTT
  // and delivery) under the frozen statistics.
  const ArmRecord& inc = r.arms[0];
  const Score g_inc = Goodness(ArmOutcome{inc.path.size() - 1, inc.bandwidth_mbps, inc.mean_rtt_ms, inc.delivery_ratio}, g_stats);
  CHECK(g_inc.value && *g_inc.value == *inc.goodness.value);
  // Same evidence inside the dwell time: keep.
  t.seconds_since_last_switch = 10.0;
  const auto dwell = DecideTournament(t, plan, obs, *g_model, g_stats);
  CHECK(dwell.verdict == ArbitrationVerdict::Keep);
  for (const auto& c : dwell.challengers) CHECK(!c.evaluation.decision.dwell_ok);
}

void SeveralQualifyingChallengersSelectHighestConfidence() {
  // Formerly undetermined; now the research engine's rule: highest C wins.
  const TelemetrySnapshot t = Congested();
  const TournamentPlan plan = PlanTournament(t, *g_model, g_stats);
  CHECK(plan.arms.size() >= 3);  // At least two distinct challengers on this state.
  if (plan.arms.size() < 3) return;
  const auto obs = Synthetic(plan, [](const PlannedArm& a) { return a.incumbent ? Profile{81.5, 3} : Profile{20.4, 8}; }, 12);
  const auto r = DecideTournament(t, plan, obs, *g_model, g_stats);
  PrintRecord("several_qualify", r);
  std::size_t qualifying = 0;
  double best_c = -1e9;
  std::string best;
  for (const auto& c : r.challengers) {
    if (!c.evaluation.decision.qualifies) continue;
    ++qualifying;
    if (c.evaluation.confidence.confidence > best_c) {
      best_c = c.evaluation.confidence.confidence;
      best = c.arm_id;
    }
  }
  CHECK(qualifying >= 2 && r.qualifying_arms.size() == qualifying);
  CHECK(r.verdict == ArbitrationVerdict::Switch && r.selected_arm == best);
  CHECK(aafr::test::Contains(r.reason, "highest C"));
  for (const auto& a : plan.arms) {
    if (a.arm_id == best) CHECK(r.selected_path == a.path);
  }
}

void IncompleteFramesUseValidBlocks() {
  // Formerly undetermined. An incumbent block with 2 replies is discarded and
  // its frame still counts; a frame with no valid block drops its round.
  const TelemetrySnapshot t = Congested();
  const TournamentPlan plan = PlanTournament(t, *g_model, g_stats);
  const auto profile = [&](const PlannedArm& a) {
    if (a.incumbent) return Profile{81.5, 3};
    return a.path == plan.candidates[2].candidate.path ? Profile{20.4, 8} : Profile{60.0, 4};
  };
  auto obs = Synthetic(plan, profile, 13, [](ProbeTournament& tour) {
    tour.arms[0].frames[6].blocks[1].echoes[2].reset();  // 2 replies: invalid block.
    for (auto& b : tour.arms[0].frames[8].blocks) b.echoes[2].reset();  // Whole frame invalid.
  });
  const auto r = DecideTournament(t, plan, obs, *g_model, g_stats);
  PrintRecord("incomplete_frames", r);
  CHECK(r.arms[0].complete_frames == 8 && r.arms[0].valid_frames == 9);
  for (const auto& c : r.challengers) {
    CHECK(c.evaluation.paired_rounds == 9 && c.evaluation.anova.computed && c.evaluation.anova.df_error == 8.0);
  }
  CHECK(r.verdict == ArbitrationVerdict::Switch && r.selected_path == plan.candidates[2].candidate.path);
  // Too few paired rounds on every challenger: no test, keep.
  auto sparse = Synthetic(plan, profile, 13, [](ProbeTournament& tour) {
    for (std::size_t f = 1; f < kFrames; ++f) {
      for (auto& b : tour.arms[0].frames[f].blocks) b.echoes[2].reset();
    }
  });
  const auto k = DecideTournament(t, plan, sparse, *g_model, g_stats);
  CHECK(k.verdict == ArbitrationVerdict::Keep);
  for (const auto& c : k.challengers) CHECK(c.evaluation.paired_rounds == 1 && !c.evaluation.anova.computed);
}

void ObservationsMustMatchThePlan() {
  const TelemetrySnapshot t = Congested();
  const TournamentPlan plan = PlanTournament(t, *g_model, g_stats);
  const auto profile = [](const PlannedArm& a) { return a.incumbent ? Profile{81.5, 3} : Profile{20.4, 8}; };
  // Wrong path for a challenger.
  auto wrong = Synthetic(plan, profile, 14, [](ProbeTournament& tour) { tour.arms[1].path = {"s1", "s4", "s5", "s6"}; });
  auto r = DecideTournament(t, plan, wrong, *g_model, g_stats);
  CHECK(r.verdict == ArbitrationVerdict::Undetermined && !r.observation_issues.empty());
  // Missing arm.
  auto missing = Synthetic(plan, profile, 14, [](ProbeTournament& tour) { tour.arms.pop_back(); });
  r = DecideTournament(t, plan, missing, *g_model, g_stats);
  CHECK(r.verdict == ArbitrationVerdict::Undetermined && !r.observation_issues.empty());
  // No observations at all.
  r = DecideTournament(t, plan, std::nullopt, *g_model, g_stats);
  CHECK(r.verdict == ArbitrationVerdict::Undetermined && aafr::test::Contains(r.reason, "no probe observations"));
  // Unlabelled observations.
  auto unlabelled = Synthetic(plan, profile, 14);
  unlabelled.description.clear();
  CHECK(DecideTournament(t, plan, unlabelled, *g_model, g_stats).verdict == ArbitrationVerdict::Undetermined);
}

void MissingMeasurementsLimitTheCandidates() {
  TelemetrySnapshot t = Congested();
  SetBoth(t.graph, "s3", "s5", [](ResearchLink& l) { l.observation.utilization.reset(); });
  const TournamentPlan plan = PlanTournament(t, *g_model, g_stats);
  CHECK(plan.issues.empty());
  // No prediction for s3-s5: A* refuses; ACO and GA refuse (J_eng needs the
  // measurement); Dijkstra routes on start-up delays and still runs.
  CHECK(plan.candidates[0].candidate.status == CandidateStatus::PathFound);
  CHECK(plan.candidates[1].candidate.status == CandidateStatus::MissingCost);
  CHECK(plan.candidates[2].candidate.status == CandidateStatus::MissingCost);
  CHECK(plan.candidates[3].candidate.status == CandidateStatus::MissingCost);
  CHECK(!plan.tournament_needed);  // Dijkstra returns the installed path.
  const auto r = DecideTournament(t, plan, std::nullopt, *g_model, g_stats);
  CHECK(r.verdict == ArbitrationVerdict::Keep);
}

// ---- Record ----

void RecordIsDeterministicAndLabelled() {
  const TelemetrySnapshot t = Congested();
  const auto run = [&]() {
    const TournamentPlan plan = PlanTournament(t, *g_model, g_stats);
    const auto obs = Synthetic(plan, [&](const PlannedArm& a) {
      if (a.incumbent) return Profile{81.5, 3};
      return a.path == plan.candidates[2].candidate.path ? Profile{20.4, 8} : Profile{60.0, 4};
    }, 11);
    return DecideTournament(t, plan, obs, *g_model, g_stats);
  };
  const std::string a = ToJson(run());
  const std::string b = ToJson(run());
  CHECK(a == b);
  std::string error;
  const auto parsed = ParseJson(a, &error);
  CHECK(parsed.has_value() && error.empty());
  if (!parsed) return;
  CHECK(*parsed->String("schema") == kDecisionRecordSchema);
  CHECK(*parsed->Find("telemetry")->String("origin") == "controlled");
  CHECK(*parsed->Find("probes")->String("origin") == "synthetic");
  CHECK(*parsed->String("verdict") == "switch");
  CHECK(aafr::test::Contains(*parsed->String("action"), "decision only"));
  CHECK(*parsed->Find("model")->String("source_commit") == "6b6c008fc484bcbadfb8d8447acf36fe5f39e10d");
  CHECK(parsed->Find("link_weights")->array.size() == 18 && parsed->Find("candidates")->array.size() == 4);
  CHECK(a.find("\"live\"") == std::string::npos);  // No input is labelled live.
  CHECK(ToJson(run(), true).find("duration_ms") != std::string::npos);
  std::cout << "RESULT record_bytes=" << a.size() << "\n";
}

// ---- Recorded research tournaments: independent numerical reference ----

void RecordedTournamentsReproduce() {
  std::size_t tests = 0;
  double max_f_rel = 0.0, max_p_rel = 0.0, max_eta = 0.0, max_ci = 0.0, max_c = 0.0, max_g = 0.0;
  double previous_swap_t = -1e9;
  std::string current_run;
  for (const auto& tour : rec::kTournaments) {
    aafr::test::current_test = std::string("recorded_") + tour.run + "_" + std::to_string(tour.id);
    if (tour.run != current_run) {  // Each run is a separate experiment: no earlier switch.
      current_run = tour.run;
      previous_swap_t = -1e9;
    }
    const rec::Arm* inc = nullptr;
    for (const auto& a : tour.arms) {
      if (a.incumbent) inc = &a;
    }
    CHECK(inc != nullptr);
    // Goodness of every arm from its recorded outcome and the frozen statistics.
    for (const auto& a : tour.arms) {
      const Score g = Goodness(ArmOutcome{a.path.size() - 1, a.bottleneck_bw_mbps, a.mean_rtt_ms,
                                          static_cast<double>(a.recv) / a.sent}, g_stats);
      CHECK(g.value && Near(*g.value, a.goodness, 1e-12, 1e-12));
      if (g.value) max_g = std::max(max_g, std::fabs(*g.value - a.goodness));
    }
    std::vector<double> p_raw;
    for (const auto& test : tour.tests) {
      const rec::Arm* ch = nullptr;
      for (const auto& a : tour.arms) {
        if (!a.incumbent && std::string(a.label) == test.challenger) ch = &a;
      }
      CHECK(ch != nullptr);
      if (!ch) continue;
      ++tests;
      const PairedAnova an = RepeatedMeasuresAnova(inc->frame_log10_rtt, ch->frame_log10_rtt);
      CHECK(an.computed && an.df_error == test.df2);
      CHECK(Near(an.f, test.f, 1e-9));
      CHECK(Near(an.p_value, test.p, 1e-8));
      CHECK(Near(an.partial_eta_squared, test.eta2_partial, 0.0, 1e-12));
      CHECK(Near(an.mean_difference_log10, test.mean_diff_log10, 0.0, 1e-12));
      CHECK(Near(an.ci_low_log10, test.ci_low_log10, 0.0, 1e-10) && Near(an.ci_high_log10, test.ci_high_log10, 0.0, 1e-10));
      CHECK(Near(an.rtt_ratio, test.rtt_ratio, 1e-11));
      max_f_rel = std::max(max_f_rel, std::fabs(an.f - test.f) / test.f);
      max_p_rel = std::max(max_p_rel, std::fabs(an.p_value - test.p) / test.p);
      max_eta = std::max(max_eta, std::fabs(an.partial_eta_squared - test.eta2_partial));
      max_ci = std::max({max_ci, std::fabs(an.ci_low_log10 - test.ci_low_log10), std::fabs(an.ci_high_log10 - test.ci_high_log10)});
      p_raw.push_back(an.p_value);
      // Confidence from AAFR's statistics, AAFR's feasibility and the
      // recorded dG.
      CHECK(Near(test.delta_goodness, ch->goodness - inc->goodness, 0.0, 1e-12));
      ConfidenceInputs in;
      in.partial_eta_squared = an.partial_eta_squared;
      in.ci_low_log10 = an.ci_low_log10;
      in.ci_high_log10 = an.ci_high_log10;
      in.incumbent = EvaluateFeasibility(inc->sent, inc->sent - inc->recv, inc->mean_rtt_ms);
      in.challenger = EvaluateFeasibility(ch->sent, ch->sent - ch->recv, ch->mean_rtt_ms);
      in.delta_goodness = test.delta_goodness;
      const ConfidenceResult c = ComputeConfidence(in);
      CHECK(c.computed);
      CHECK(Near(c.s_eta, test.s_eta, 0.0, 1e-12) && Near(c.s_ci, test.s_ci, 0.0, 1e-9) && c.s_g == test.s_g);
      CHECK(Near(c.confidence, test.c, 0.0, 1e-9));
      CHECK(c.repair == (std::string(test.constraint) == "repair"));
      max_c = std::max(max_c, std::fabs(c.confidence - test.c));
      const double since = tour.t_s - previous_swap_t;
      const ChallengerDecision d = Decide(c, test.p_holm, since);
      CHECK(d.confidence_ok == (test.c > 0.70) && d.significance_ok == test.significant);
      CHECK(d.qualifies == tour.swap);
    }
    // Holm over this tournament's challengers equals the recorded p_holm.
    const auto holm = HolmAdjusted(p_raw);
    for (std::size_t i = 0; i < holm.size(); ++i) CHECK(Near(holm[i], tour.tests[i].p_holm, 1e-8));
    if (tour.swap) previous_swap_t = tour.t_s;
  }
  aafr::test::current_test = "recorded_tournaments_reproduce";
  CHECK(tests == 7);
  std::cout << "RESULT recorded: tournaments=" << rec::kTournaments.size() << " tests=" << tests
            << " max_rel_F=" << max_f_rel << " max_rel_p=" << max_p_rel << " max_abs_eta=" << max_eta
            << " max_abs_ci=" << max_ci << " max_abs_C=" << max_c << " max_abs_G=" << max_g << "\n";
}

}  // namespace

int main() {
  if (kLibrary.empty() || !std::filesystem::exists(kLibrary)) {
    std::cout << "FAIL: XGBoost library not available (AAFR_XGBOOST_LIBRARY='" << kLibrary << "')\n";
    return 1;
  }
  auto loaded = LinkWeightModel::Load(kModelDir, kLibrary);
  std::string error;
  auto stats = PathObjectiveStatistics(kModelDir, &error);
  if (!loaded.model || !stats) {
    std::cout << "FAIL: " << (loaded.model ? error : loaded.error) << "\n";
    return 1;
  }
  g_model = std::move(loaded.model);
  g_stats = *stats;
  const int rc = aafr::test::RunTests({
      {"invalid_telemetry_is_refused", InvalidTelemetryIsRefused},
      {"plan_uses_model_weights_and_four_algorithms", PlanUsesModelWeightsAndFourAlgorithms},
      {"no_challenger_means_keep_without_tournament", NoChallengerMeansKeepWithoutTournament},
      {"congestion_repair_switch", CongestionRepairSwitch},
      {"several_qualifying_challengers_select_highest_confidence", SeveralQualifyingChallengersSelectHighestConfidence},
      {"incomplete_frames_use_valid_blocks", IncompleteFramesUseValidBlocks},
      {"observations_must_match_the_plan", ObservationsMustMatchThePlan},
      {"missing_measurements_limit_the_candidates", MissingMeasurementsLimitTheCandidates},
      {"record_is_deterministic_and_labelled", RecordIsDeterministicAndLabelled},
      {"recorded_tournaments_reproduce", RecordedTournamentsReproduce},
  });
  g_model.reset();
  return rc;
}
