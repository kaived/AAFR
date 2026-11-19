// Phase 5B.1: the research pipeline attached to the AAFR runtime through the
// non-actuating ResearchRuntimeAdapter (research/runtime_adapter). Uses a
// real AAFRRuntime with the deterministic test executor and a manual clock
// (as adaptive_runtime_test) and controlled telemetry / seeded synthetic
// probes, all labelled as such. Verifies the lifecycle, the input contract,
// deterministic records, the dwell time read from the runtime's applied
// switches, and that no evaluation changes the runtime or sends a request.
// Needs the XGBoost library (fails without it). No network access.

#include <cmath>
#include <filesystem>
#include <functional>
#include <iostream>
#include <thread>

#include "algorithms/common/path_algorithm.h"
#include "research/runtime_adapter.h"
#include "tests/fixtures/research/reference_values.h"
#include "tests/support/runtime_test_support.h"
#include "tests/support/test_support.h"

using namespace aafr;
using namespace aafr::research;
using runtime::AAFRRuntime;
using runtime::EvaluationTrigger;
using runtime::ManualClock;
using runtime::RuntimeConfig;
using runtime::RuntimeDependencies;
namespace rf = aafr::research_fixture;
namespace alg = aafr::algorithms;
using Path = std::vector<std::string>;

namespace {

ResearchRuntimeAdapter::Options AdapterOptions() {
  ResearchRuntimeAdapter::Options o;
  o.model_dir = AAFR_TEST_XGBOOST_MODEL_DIR;
  o.xgboost_library = AAFR_TEST_XGBOOST_LIBRARY;
  o.history = 4;
  return o;
}

// ---- Controlled telemetry and synthetic probes (labelled) ----

TelemetrySnapshot Congested(std::optional<double> since_switch = std::nullopt) {
  TelemetrySnapshot t;
  t.origin = DataOrigin::Controlled;
  t.description = "controlled: six-switch testbed, s1-s2 at 22 % loss and 99 % utilization";
  t.graph.nodes = {"s1", "s2", "s3", "s4", "s5", "s6"};
  for (const auto& l : rf::kTestbedLinks) {
    for (const auto& [from, to] : {std::pair{l.a, l.b}, std::pair{l.b, l.a}}) {
      ResearchLink link;
      const bool s1s2 = (std::string(from) == "s1" && std::string(to) == "s2") ||
                        (std::string(from) == "s2" && std::string(to) == "s1");
      link.observation = LinkObservation{from, to, l.delay_ms, s1s2 ? 0.22 : 0.0, l.capacity_mbps, s1s2 ? 0.99 : 0.0};
      link.startup_delay_ms = l.delay_ms;
      t.graph.links.push_back(link);
    }
  }
  t.source = "s1";
  t.destination = "s6";
  t.installed_path = {"s1", "s2", "s6"};
  t.seconds_since_last_switch = since_switch;
  return t;
}

ProbeObservations Synthetic(const TournamentPlan& plan, std::uint64_t seed) {
  alg::Rng rng(seed);
  std::vector<double> drift(kFrames);
  for (double& d : drift) d = std::exp(0.03 * rng.Normal());
  const Path clean = plan.candidates[2].candidate.path;  // ACO's J_eng choice.
  ProbeObservations o;
  o.origin = DataOrigin::Synthetic;
  o.description = "synthetic: seeded log-normal echoes, seed " + std::to_string(seed);
  for (std::size_t a = 0; a < plan.arms.size(); ++a) {
    const auto& pa = plan.arms[a];
    const double base = pa.incumbent ? 81.5 : (pa.path == clean ? 20.4 : 60.0);
    const std::size_t replies = pa.incumbent ? 3 : (pa.path == clean ? 8 : 4);
    ProbeArm arm;
    arm.arm_id = pa.arm_id;
    arm.incumbent = pa.incumbent;
    arm.path = pa.path;
    for (const auto alg_id : pa.algorithms) arm.algorithms.push_back(ToString(alg_id));
    for (std::size_t f = 0; f < kFrames; ++f) {
      ProbeFrame frame;
      frame.round = f;
      frame.position_in_round = (a + f) % plan.arms.size();
      for (std::size_t b = 0; b < kBlocks; ++b) {
        ProbeBlock block;
        for (std::size_t e = 0; e < kEchoes; ++e) {
          const double rtt = base * drift[f] * std::exp(0.02 * rng.Normal());
          block.echoes.push_back(e < replies ? std::optional<double>(rtt) : std::nullopt);
        }
        frame.blocks.push_back(block);
      }
      arm.frames.push_back(frame);
    }
    o.tournament.arms.push_back(arm);
  }
  return o;
}

// ---- A real runtime (as in adaptive_runtime_test) ----

RuntimeConfig RuntimeTestConfig() {
  RuntimeConfig c;
  c.session_id = "r";
  c.scheduler.min_interval_ms = 1000;
  c.stability.min_dwell_ms = 1000;
  c.stability.cooldown_ms = 2000;
  c.stability.sustain_evaluations = 2;
  c.probe.rounds = 12;
  c.health_check_interval_ms = 0;
  c.activation_attempts = 1;
  return c;
}

struct RuntimeFixture {
  aafr::test::GraphHolder graph{aafr::test::TwoPathGraph()};
  aafr::test::TestExecutor executor{graph};
  ManualClock clock;
  alg::AlgorithmExecutionEngine engine{nullptr, aafr::test::VerifiedRegistry(), &executor};
  std::unique_ptr<AAFRRuntime> runtime;
  RuntimeFixture() {
    RuntimeDependencies d;
    d.graph_source = [this] { return graph.Get(); };
    d.problem_for = [](const forwarding::ForwardingGraph& g) { return aafr::test::ProblemFor(g); };
    d.executor = &executor;
    d.eligibility = &engine;
    d.clock = &clock;
    runtime = std::make_unique<AAFRRuntime>(std::move(d), RuntimeTestConfig());
  }
};

struct RuntimeFingerprint {
  std::uint64_t config_version{0};
  Path path;
  std::uint64_t evaluations{0}, switches{0}, probe_requests{0};
  std::size_t decisions{0}, events{0};
  int executor_requests{0};
  bool operator==(const RuntimeFingerprint& o) const {
    return config_version == o.config_version && path == o.path && evaluations == o.evaluations &&
           switches == o.switches && probe_requests == o.probe_requests && decisions == o.decisions &&
           events == o.events && executor_requests == o.executor_requests;
  }
};

RuntimeFingerprint Fingerprint(const RuntimeFixture& f) {
  const auto s = f.runtime->Status();
  RuntimeFingerprint p;
  if (s.active) {
    p.config_version = s.active->config_version;
    p.path = s.active->path;
  }
  p.evaluations = s.counters.evaluations;
  p.switches = s.counters.switches;
  p.probe_requests = s.counters.probe_requests;
  p.decisions = f.runtime->Decisions(100000).size();
  p.events = f.runtime->Events(100000).size();
  p.executor_requests = f.executor.requests.load();
  return p;
}

// ---- Tests ----

void UnavailableWithoutTheModel() {
  auto o = AdapterOptions();
  o.xgboost_library = "no_such_xgboost.dll";
  ResearchRuntimeAdapter adapter(o);
  CHECK(adapter.Status().state == ResearchRuntimeState::Unavailable);
  CHECK(aafr::test::Contains(adapter.Status().state_reason, "unavailable"));
  const auto e = adapter.Evaluate(Congested(), std::nullopt);
  CHECK(!e.produced && !e.recommendation && !e.errors.empty() && e.state == ResearchRuntimeState::Unavailable);
  CHECK(adapter.Status().counters.refused_unavailable == 1 && adapter.History(10).empty());
  std::vector<std::string> errors;
  CHECK(!adapter.Plan(Congested(), &errors) && !errors.empty());
  auto o2 = AdapterOptions();
  o2.model_dir += "/missing";
  CHECK(ResearchRuntimeAdapter(o2).Status().state == ResearchRuntimeState::Unavailable);
}

void LifecycleAndRecommendation() {
  ResearchRuntimeAdapter adapter(AdapterOptions());
  ResearchRuntimeStatus s = adapter.Status();
  CHECK(s.state == ResearchRuntimeState::Idle && !s.last && s.counters.requests == 0);
  CHECK(s.model_source_commit == "6b6c008fc484bcbadfb8d8447acf36fe5f39e10d");
  CHECK(!ResearchRuntimeStatus::kActuationEnabled);
  // Plan changes no state.
  const auto plan = adapter.Plan(Congested());
  CHECK(plan && plan->tournament_needed);
  CHECK(adapter.Status().state == ResearchRuntimeState::Idle && adapter.Status().counters.requests == 0);
  // Evaluate: switch recommendation, not applied.
  const auto obs = Synthetic(*plan, 11);
  const auto e = adapter.Evaluate(Congested(), obs);
  CHECK(e.produced && e.state == ResearchRuntimeState::Decided);
  const ResearchRecommendation& r = *e.recommendation;
  CHECK(r.verdict == ArbitrationVerdict::Switch && !r.applied() && !ResearchRecommendation::kApplied);
  CHECK(r.recommended_path == plan->candidates[2].candidate.path && (r.installed_path == Path{"s1", "s2", "s6"}));
  CHECK(r.kind == "research_recommendation" && r.sequence == 1);
  CHECK(r.telemetry_origin == DataOrigin::Controlled && r.probe_origin == DataOrigin::Synthetic);
  CHECK(r.model_source_commit == "6b6c008fc484bcbadfb8d8447acf36fe5f39e10d");
  s = adapter.Status();
  CHECK(s.state == ResearchRuntimeState::Decided && s.last && s.last->sequence == 1);
  CHECK(aafr::test::Contains(s.state_reason, "not applied"));
  CHECK(s.counters.requests == 1 && s.counters.recommendations == 1 && s.counters.switch_recommended == 1);
  // The record is exactly the pipeline's record for the same inputs.
  auto loaded = LinkWeightModel::Load(AAFR_TEST_XGBOOST_MODEL_DIR, AAFR_TEST_XGBOOST_LIBRARY);
  const auto stats = PathObjectiveStatistics(AAFR_TEST_XGBOOST_MODEL_DIR);
  const auto direct = DecideTournament(Congested(), PlanTournament(Congested(), *loaded.model, *stats), obs,
                                       *loaded.model, *stats);
  CHECK(r.record_json == ToJson(direct));
  CHECK(r.record_json.find("\"action\":\"none: decision only, no forwarding change\"") != std::string::npos);
}

void RecordsAreDeterministicAndBounded() {
  ResearchRuntimeAdapter a(AdapterOptions()), b(AdapterOptions());
  const auto plan = a.Plan(Congested());
  const auto obs = Synthetic(*plan, 21);
  const auto ra = a.Evaluate(Congested(), obs);
  const auto rb = b.Evaluate(Congested(), obs);
  CHECK(ra.recommendation->record_json == rb.recommendation->record_json);
  for (int i = 0; i < 6; ++i) a.Evaluate(Congested(), obs);
  const auto history = a.History(100);
  CHECK(history.size() == 4);  // Options::history.
  CHECK(history.back().sequence == 7 && history.front().sequence == 4);
  for (const auto& h : history) CHECK(h.record_json == ra.recommendation->record_json);
  CHECK(a.History(2).size() == 2 && a.History(2).back().sequence == 7);
}

void InvalidAndMissingTelemetry() {
  ResearchRuntimeAdapter adapter(AdapterOptions());
  // Contract violations: no recommendation, state Error with the reasons.
  TelemetrySnapshot unlabelled = Congested();
  unlabelled.description.clear();
  auto e = adapter.Evaluate(unlabelled, std::nullopt);
  CHECK(!e.produced && e.state == ResearchRuntimeState::Error && !e.errors.empty());
  CHECK(adapter.Status().state == ResearchRuntimeState::Error && !adapter.Status().last_errors.empty());
  TelemetrySnapshot off_graph = Congested();
  off_graph.installed_path = {"s1", "s5", "s6"};
  CHECK(!adapter.Evaluate(off_graph, std::nullopt).produced);
  TelemetrySnapshot negative = Congested(-1.0);
  CHECK(!adapter.Evaluate(negative, std::nullopt).produced);
  auto obs = Synthetic(*adapter.Plan(Congested()), 3);
  obs.description.clear();
  CHECK(!adapter.Evaluate(Congested(), obs).produced);
  CHECK(adapter.Status().counters.contract_errors == 4 && adapter.History(10).empty());
  // Missing measurement (utilization on s3-s5): a recommendation is
  // produced; A*, ACO and GA refuse, so no challenger and keep.
  TelemetrySnapshot missing = Congested();
  for (auto& l : missing.graph.links) {
    if ((l.observation.from == "s3" && l.observation.to == "s5") || (l.observation.from == "s5" && l.observation.to == "s3")) {
      l.observation.utilization.reset();
    }
  }
  e = adapter.Evaluate(missing, std::nullopt);
  CHECK(e.produced && e.recommendation->verdict == ArbitrationVerdict::Keep);
  CHECK(e.recommendation->record.plan.candidates[1].candidate.status == CandidateStatus::MissingCost);
  CHECK(adapter.Status().state == ResearchRuntimeState::Decided && adapter.Status().last_errors.empty());
  // A tournament is needed but no observations: undetermined, recorded.
  e = adapter.Evaluate(Congested(), std::nullopt);
  CHECK(e.produced && e.recommendation->verdict == ArbitrationVerdict::Undetermined);
  CHECK(aafr::test::Contains(e.recommendation->record.reason, "no probe observations"));
  CHECK(e.recommendation->recommended_path.empty());
  const auto c = adapter.Status().counters;
  CHECK(c.keep == 1 && c.undetermined == 1 && c.recommendations == 2 && c.requests == 6);
}

void EvaluationsAreSerialized() {
  ResearchRuntimeAdapter adapter(AdapterOptions());
  const auto obs = Synthetic(*adapter.Plan(Congested()), 5);
  std::vector<std::thread> threads;
  for (int t = 0; t < 4; ++t) {
    threads.emplace_back([&] {
      for (int i = 0; i < 3; ++i) adapter.Evaluate(Congested(), obs);
    });
  }
  for (auto& t : threads) t.join();
  const auto s = adapter.Status();
  CHECK(s.counters.requests == 12 && s.counters.recommendations == 12 && s.counters.switch_recommended == 12);
  CHECK(s.state == ResearchRuntimeState::Decided && s.last->sequence == 12);
}

void RuntimeIsNeverActuated() {
  RuntimeFixture f;
  CHECK(f.runtime->Initialize());
  ResearchRuntimeAdapter adapter(AdapterOptions());
  const RuntimeFingerprint before = Fingerprint(f);
  // 1. A snapshot built from the runtime itself: its forwarding graph and
  //    active path. Utilization is not carried by that graph, so A*, ACO and
  //    GA refuse and only Dijkstra runs.
  const auto graph = f.graph.Get();
  const StartupDelays startup = CaptureStartupDelays(*graph);
  const RuntimeView view = ReadRuntime(*f.runtime, f.clock.NowNs());
  CHECK(view.has_active && view.installed_path == aafr::test::kPathA && view.config_version == 1);
  CHECK(!view.seconds_since_applied_switch);  // Initial activation is not a switch.
  const TelemetrySnapshot t = SnapshotFromRuntimeGraph(*graph, startup, view, "runtime two-path graph");
  CHECK(ValidateTelemetry(t).empty() && t.origin == DataOrigin::Controlled);
  CHECK(aafr::test::Contains(t.description, "utilization not measured"));
  for (const auto& l : t.graph.links) CHECK(!l.observation.utilization && l.startup_delay_ms);
  auto e = adapter.Evaluate(t, std::nullopt);
  CHECK(e.produced && e.recommendation->verdict == ArbitrationVerdict::Keep);
  for (std::size_t i = 1; i < 4; ++i) CHECK(e.recommendation->record.plan.candidates[i].candidate.status == CandidateStatus::MissingCost);
  // 2. A switch recommendation on controlled testbed telemetry.
  const auto plan = adapter.Plan(Congested());
  e = adapter.Evaluate(Congested(), Synthetic(*plan, 11));
  CHECK(e.recommendation->verdict == ArbitrationVerdict::Switch && !e.recommendation->applied());
  // Nothing in the runtime changed and no request reached the executor.
  CHECK(Fingerprint(f) == before);
  CHECK(f.runtime->Status().active->path == aafr::test::kPathA);
  // The runtime still works normally afterwards.
  f.clock.AdvanceMs(1500);
  f.runtime->Step();
  CHECK(f.runtime->Status().active.has_value());
}

void DwellComesFromAppliedSwitches() {
  RuntimeFixture f;
  CHECK(f.runtime->Initialize());
  // A real switch applied by the runtime itself (as in adaptive_runtime_test).
  f.graph.SetLatency("entry", "relay_a", 100.0);
  f.clock.AdvanceMs(1500);
  f.runtime->Step();
  f.clock.AdvanceMs(1100);
  f.runtime->RequestEvaluation(EvaluationTrigger::Manual, "test");
  f.runtime->Step();
  CHECK(f.runtime->Status().active->config_version == 2);
  ResearchRuntimeAdapter adapter(AdapterOptions());
  const auto plan = adapter.Plan(Congested());
  const auto obs = Synthetic(*plan, 11);
  // 10 s after the applied switch: the research dwell (30 s) blocks.
  f.clock.AdvanceMs(10'000);
  RuntimeView view = ReadRuntime(*f.runtime, f.clock.NowNs());
  CHECK(view.seconds_since_applied_switch && std::fabs(*view.seconds_since_applied_switch - 10.0) < 1e-9);
  auto e = adapter.Evaluate(Congested(view.seconds_since_applied_switch), obs);
  CHECK(e.recommendation->verdict == ArbitrationVerdict::Keep);
  for (const auto& c : e.recommendation->record.challengers) CHECK(!c.evaluation.decision.dwell_ok);
  // A research recommendation is not a switch: it does not restart the dwell.
  f.clock.AdvanceMs(20'000);
  view = ReadRuntime(*f.runtime, f.clock.NowNs());
  CHECK(std::fabs(*view.seconds_since_applied_switch - 30.0) < 1e-9);
  e = adapter.Evaluate(Congested(view.seconds_since_applied_switch), obs);
  CHECK(e.recommendation->verdict == ArbitrationVerdict::Switch && !e.recommendation->applied());
  CHECK(f.runtime->Status().active->config_version == 2);  // Still the runtime's own last switch.
}

}  // namespace

int main() {
  const std::string library = AAFR_TEST_XGBOOST_LIBRARY;
  if (library.empty() || !std::filesystem::exists(library)) {
    std::cout << "FAIL: XGBoost library not available (AAFR_XGBOOST_LIBRARY='" << library << "')\n";
    return 1;
  }
  return aafr::test::RunTests({
      {"unavailable_without_the_model", UnavailableWithoutTheModel},
      {"lifecycle_and_recommendation", LifecycleAndRecommendation},
      {"records_are_deterministic_and_bounded", RecordsAreDeterministicAndBounded},
      {"invalid_and_missing_telemetry", InvalidAndMissingTelemetry},
      {"evaluations_are_serialized", EvaluationsAreSerialized},
      {"runtime_is_never_actuated", RuntimeIsNeverActuated},
      {"dwell_comes_from_applied_switches", DwellComesFromAppliedSwitches},
  });
}
