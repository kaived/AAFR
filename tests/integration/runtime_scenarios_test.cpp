// V0.7.1 controlled runtime scenarios.
//
// Ten versioned, seeded scenarios drive the AAFRRuntime on the two-path
// controlled graph with a ManualClock and the deterministic test executor
// (simulated forwarding: this is runtime-logic evidence, not live
// forwarding; runtime_loopback_test covers real relays). Every scenario
// writes its switching trace to runtime_scenarios.jsonl in the working
// directory (one complete line per scenario, flushed and closed before the
// assertions run) so runs can be compared and replayed.

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <atomic>
#include <fstream>
#include <functional>
#include <sstream>

#include "runtime/adaptive/aafr_runtime.h"
#include "tests/support/runtime_test_support.h"
#include "tests/support/test_support.h"

using namespace aafr;
using namespace aafr::runtime;
using algorithms::AlgorithmId;
using aafr::test::kPathA;
using aafr::test::kPathB;

namespace {

constexpr const char* kScenarioVersion = "aafr.runtime_scenarios.v1";
constexpr double kStepMs = 250.0;

RuntimeConfig ScenarioConfig() {
  RuntimeConfig c;
  c.session_id = "scn";
  c.scheduler.min_interval_ms = 1000;
  c.stability.min_dwell_ms = 2000;
  c.stability.cooldown_ms = 3000;
  c.stability.failed_activation_cooldown_ms = 1000;
  c.stability.candidate_backoff_initial_ms = 2000;
  c.stability.sustain_evaluations = 2;
  c.probe.rounds = 12;
  c.health_check_interval_ms = 0;
  c.degraded_retry_ms = 1000;
  c.activation_attempts = 1;
  c.review_interval_ms = 5000;
  return c;
}

struct Commit {
  double t_ms;
  std::string decision_id;
  std::string type;
  std::string trigger;
  std::string algorithm;
  forwarding::Path from;
  forwarding::Path to;
};

struct ScenarioRun {
  std::string name;
  std::uint64_t seed{0};
  double duration_ms{0};
  std::vector<Commit> commits;
  RuntimeCounters counters;
  forwarding::Path final_path;
  std::uint64_t evaluations{0};
  std::uint64_t discarded{0};
  std::size_t failed_activations{0};
};

struct World {
  aafr::test::GraphHolder graph{aafr::test::TwoPathGraph()};
  aafr::test::TestExecutor executor;
  ManualClock clock;
  algorithms::AlgorithmExecutionEngine engine;
  std::atomic<double> demand_mbps{0.0};
  std::atomic<bool> forbid_relay_a{false};
  std::unique_ptr<AAFRRuntime> runtime;

  World(std::uint64_t seed, RuntimeConfig config)
      : executor(graph, seed), engine(nullptr, aafr::test::VerifiedRegistry(), &executor) {
    RuntimeDependencies d;
    d.graph_source = [this] { return graph.Get(); };
    d.problem_for = [this](const forwarding::ForwardingGraph& g) {
      forwarding::ForwardingProblem p = aafr::test::ProblemFor(g);
      const double demand = demand_mbps.load();
      if (demand > 0.0) p.constraints.min_bandwidth_mbps = demand;
      if (forbid_relay_a.load()) p.constraints.forbidden_nodes.insert("relay_a");
      return p;
    };
    d.executor = &executor;
    d.eligibility = &engine;
    d.clock = &clock;
    config.probe.seed = seed;
    config.algorithm_seed = seed;
    runtime = std::make_unique<AAFRRuntime>(std::move(d), std::move(config));
  }
};

std::string PathJson(const forwarding::Path& p) {
  std::string s = "[";
  for (std::size_t i = 0; i < p.size(); ++i) s += (i ? ",\"" : "\"") + p[i] + "\"";
  return s + "]";
}

void Append(const ScenarioRun& run) {
  static bool truncated = false;
  std::ofstream out("runtime_scenarios.jsonl", truncated ? std::ios::app : std::ios::trunc);
  truncated = true;
  std::ostringstream line;
  line << "{\"schema\":\"" << kScenarioVersion << "\",\"scenario\":\"" << run.name << "\",\"seed\":" << run.seed
       << ",\"duration_ms\":" << run.duration_ms << ",\"evaluations\":" << run.evaluations
       << ",\"switches\":" << run.counters.switches << ",\"rejected\":" << run.counters.rejected
       << ",\"failed_switches\":" << run.counters.failed_switches << ",\"recoveries\":" << run.counters.recoveries
       << ",\"discarded\":" << run.counters.discarded << ",\"reviews\":" << run.counters.reviews
       << ",\"probe_requests\":" << run.counters.probe_requests << ",\"final_path\":" << PathJson(run.final_path)
       << ",\"commits\":[";
  for (std::size_t i = 0; i < run.commits.size(); ++i) {
    const Commit& c = run.commits[i];
    line << (i ? "," : "") << "{\"t_ms\":" << c.t_ms << ",\"decision\":\"" << c.decision_id << "\",\"type\":\""
         << c.type << "\",\"trigger\":\"" << c.trigger << "\",\"algorithm\":\"" << c.algorithm
         << "\",\"from\":" << PathJson(c.from) << ",\"to\":" << PathJson(c.to) << "}";
  }
  line << "]}";
  out << line.str() << '\n';
  out.flush();
  CHECK(out.good());
}

// Runs `duration_ms` of steps; `event(t_ms)` changes the world before each step.
ScenarioRun Run(const std::string& name, std::uint64_t seed, double duration_ms,
                const std::function<void(World&, double)>& event, RuntimeConfig config = ScenarioConfig(),
                const std::function<void(World&)>& setup = nullptr) {
  World w(seed, config);
  if (setup) setup(w);
  const std::int64_t t0 = w.clock.NowNs();
  CHECK(w.runtime->Initialize());
  for (double t = kStepMs; t <= duration_ms; t += kStepMs) {
    w.clock.AdvanceMs(kStepMs);
    event(w, t);
    w.runtime->Step();
  }
  ScenarioRun run;
  run.name = name;
  run.seed = seed;
  run.duration_ms = duration_ms;
  const RuntimeStatus s = w.runtime->Status();
  run.counters = s.counters;
  run.evaluations = s.counters.evaluations;
  run.final_path = s.active ? s.active->path : forwarding::Path{};
  for (const DecisionRecord& r : w.runtime->Decisions(256)) {
    if (r.activation == ActivationStatus::Failed || r.activation == ActivationStatus::TimedOut) ++run.failed_activations;
    if (r.activation != ActivationStatus::Confirmed || r.type == DecisionType::InitialActivation) continue;
    if (r.previous_path == r.confirmed_path) continue;
    run.commits.push_back({NsToMs(r.monotonic_ns - t0), r.decision_id, ToString(r.type), ToString(r.trigger),
                           r.confirmed_algorithm ? algorithms::ToString(*r.confirmed_algorithm) : "",
                           r.previous_path, r.confirmed_path});
  }
  if (std::getenv("AAFR_SCENARIO_DEBUG")) {
    for (const DecisionRecord& r : w.runtime->Decisions(256)) {
      std::printf("  %s %.0f %s %s %s act=%s %s\n", r.decision_id.c_str(), NsToMs(r.monotonic_ns - t0), ToString(r.trigger),
                  ToString(r.type), ReasonsText(r.reasons).c_str(), ToString(r.activation), r.evidence_validity.c_str());
    }
  }
  Append(run);
  return run;
}

// No two committed switches closer than the cooldown (anti-flapping).
bool RespectsCooldown(const ScenarioRun& run, double cooldown_ms) {
  for (std::size_t i = 1; i < run.commits.size(); ++i) {
    if (run.commits[i].type == "safety_switch" || run.commits[i].type == "recovery") continue;
    if (run.commits[i].t_ms - run.commits[i - 1].t_ms < cooldown_ms) return false;
  }
  return true;
}

void StableNetwork() {
  const auto r = Run("stable_network", 101, 40'000, [](World&, double) {});
  CHECK(r.commits.empty());
  CHECK(r.final_path == kPathA);
  CHECK(r.counters.reviews >= 6);  // reviews ran and found nothing better
}

void GradualCongestion() {
  const auto r = Run("gradual_congestion", 102, 40'000, [](World& w, double t) {
    // relay_a latency climbs 1 -> 61 ms over 30 s, in 2 s steps.
    if (std::fmod(t, 2000.0) == 0.0 && t <= 30'000) w.graph.SetLatency("entry", "relay_a", 1.0 + t / 500.0);
  });
  CHECK(r.commits.size() == 1);
  CHECK(r.commits[0].to == kPathB);
  CHECK(r.final_path == kPathB);
}

void SuddenCongestion() {
  const auto r = Run("sudden_congestion", 103, 20'000, [](World& w, double t) {
    if (t == 5000) w.graph.SetLatency("entry", "relay_a", 100.0);
  });
  CHECK(r.commits.size() == 1);
  CHECK(r.commits[0].to == kPathB);
  CHECK(r.commits[0].t_ms - 5000 <= 5000);  // reacts within the dwell + hysteresis window
}

void CongestionRecovery() {
  const auto r = Run("congestion_recovery", 104, 45'000, [](World& w, double t) {
    if (t == 5000) w.graph.SetLatency("entry", "relay_a", 100.0);
    if (t == 20'000) w.graph.SetLatency("entry", "relay_a", 1.0);
  });
  CHECK(r.commits.size() == 2);
  CHECK(r.commits[0].to == kPathB);
  CHECK(r.commits[1].from == kPathB && r.commits[1].to == kPathA);
  CHECK(r.final_path == kPathA);
}

void IntermittentCongestion() {
  const RuntimeConfig config = ScenarioConfig();
  const auto r = Run("intermittent_congestion", 105, 40'000, [](World& w, double t) {
    // Toggles every 1.5 s: faster than hysteresis + cooldown can follow.
    const int phase = static_cast<int>(t / 1500.0);
    if (std::fmod(t, 1500.0) == 0.0) w.executor.extra_latency_ms[kPathA] = phase % 2 ? 60.0 : 0.0;
  }, config);
  CHECK(RespectsCooldown(r, config.stability.cooldown_ms));
  CHECK(r.counters.switches <= static_cast<std::uint64_t>(40'000 / config.stability.cooldown_ms));
}

void EqualLatencyUnequalLoss() {
  const auto r = Run(
      "equal_latency_unequal_loss", 106, 30'000,
      [](World& w, double t) {
        if (t == 2000) w.executor.loss_ratio[kPathA] = 0.6;  // measurement only; graph unchanged
      },
      ScenarioConfig(),
      [](World& w) {
        w.graph.SetLatency("entry", "relay_b", 1.0);
        w.graph.SetLatency("relay_b", "dest", 1.0);
        w.graph.Mutate([](forwarding::ForwardingGraph& g) { g.SetEdgePolicyCost("entry", "relay_b", 0.01); });
      });
  CHECK(r.commits.size() == 1);
  CHECK(r.commits[0].to == kPathB);
  CHECK(r.commits[0].trigger == "review");
}

void NetworkConstraintChange() {
  // An operator constraint forbids relay_a, then lifts it. The constraint
  // source requests a ConstraintChange evaluation, as the V0.4 constraint
  // pipeline does; the active path then violates a hard constraint.
  const auto r = Run("network_constraint_change", 107, 30'000, [](World& w, double t) {
    if (t == 3000) {
      w.forbid_relay_a = true;
      w.runtime->RequestEvaluation(EvaluationTrigger::ConstraintChange, "relay_a forbidden");
    }
    if (t == 15'000) {
      w.forbid_relay_a = false;
      w.runtime->RequestEvaluation(EvaluationTrigger::ConstraintChange, "relay_a allowed again");
    }
  });
  CHECK(r.commits.size() == 2);
  CHECK(r.commits[0].to == kPathB && r.commits[0].type == "safety_switch");
  CHECK(r.commits[1].to == kPathA && r.commits[1].type == "switch");
  CHECK(r.final_path == kPathA);
}

void ForwardingActivationFailure() {
  const auto r = Run("forwarding_activation_failure", 108, 40'000, [](World& w, double t) {
    if (t == 3000) {
      std::lock_guard<std::mutex> lock(w.executor.mutex_);
      w.executor.unconfirmed_activation_paths.insert(kPathB);  // B probes fine, activation unconfirmable
      w.graph.SetLatency("entry", "relay_a", 100.0);
    }
    if (t == 15'000) {
      std::lock_guard<std::mutex> lock(w.executor.mutex_);
      w.executor.unconfirmed_activation_paths.clear();
    }
  });
  CHECK(r.failed_activations >= 1);
  // Nothing was committed before confirmation; B only after it confirmed.
  CHECK(!r.commits.empty());
  for (const Commit& c : r.commits) CHECK(c.t_ms >= 15'000 - kStepMs);
  CHECK(r.final_path == kPathB);
}

void TopologyChangeDuringEvaluation() {
  std::atomic<int> mutations{0};
  const auto r = Run(
      "topology_change_during_evaluation", 109, 30'000,
      [](World& w, double t) {
        if (t == 3000) w.graph.SetLatency("entry", "relay_a", 100.0);
      },
      ScenarioConfig(),
      [&mutations](World& w) {
        w.executor.on_request = [&w, &mutations](const forwarding::ForwardingRequest& req,
                                                 const forwarding::ForwardingExecutionContext& ctx) {
          // During the first two probe campaigns after congestion, the graph
          // changes under the evaluation (a harmless relay_b tweak).
          if (ctx.scenario_id == "runtime_probe" && req.path == kPathB && mutations.load() < 2 &&
              req.request_id.find("-r0-") != std::string::npos &&
              w.graph.Get()->FindEdge("entry", "relay_a")->latency_ms.value.value_or(0) > 50) {
            ++mutations;
            w.graph.SetLatency("relay_b", "dest", 10.0 + 0.001 * mutations.load());
          }
        };
      });
  CHECK(mutations.load() == 2);
  CHECK(r.counters.discarded >= 1);
  CHECK(r.final_path == kPathB);  // still adapts once the graph is stable
}

void RapidTelemetryUpdates() {
  const RuntimeConfig config = ScenarioConfig();
  const auto r = Run("rapid_telemetry_updates", 110, 30'000, [](World& w, double t) {
    // A new graph version every step with sub-threshold jitter on A.
    w.graph.SetLatency("entry", "relay_a", 1.0 + 0.05 * std::sin(t / 300.0));
  }, config);
  CHECK(r.commits.empty());
  CHECK(r.final_path == kPathA);
  // The scheduler bounds evaluations by min_interval regardless of update rate.
  CHECK(r.evaluations <= static_cast<std::uint64_t>(30'000 / config.scheduler.min_interval_ms) + 2);
}

}  // namespace

int main(int argc, char** argv) {
  std::vector<std::pair<const char*, void (*)()>> tests = {
      {"stable_network", StableNetwork},
      {"gradual_congestion", GradualCongestion},
      {"sudden_congestion", SuddenCongestion},
      {"congestion_recovery", CongestionRecovery},
      {"intermittent_congestion", IntermittentCongestion},
      {"equal_latency_unequal_loss", EqualLatencyUnequalLoss},
      {"network_constraint_change", NetworkConstraintChange},
      {"forwarding_activation_failure", ForwardingActivationFailure},
      {"topology_change_during_evaluation", TopologyChangeDuringEvaluation},
      {"rapid_telemetry_updates", RapidTelemetryUpdates},
  };
  if (argc > 1) {
    std::vector<std::pair<const char*, void (*)()>> selected;
    for (const auto& t : tests)
      if (std::string(t.first).find(argv[1]) != std::string::npos) selected.push_back(t);
    tests = selected;
  }
  return aafr::test::RunTests(tests);
}
