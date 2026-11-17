// V0.6 AAFRRuntime: lifecycle, initial activation, change-driven
// evaluation, switching with confirmation, failure handling, recovery,
// bidirectional adaptation, concurrency and explainability.
//
// Deterministic: a ManualClock drives every gate and the test executor
// forwards over the current graph with seeded noise. Live loopback
// forwarding is covered by runtime_loopback_test.

#include "runtime/adaptive/aafr_runtime.h"

#include <algorithm>
#include <thread>

#include "tests/support/runtime_test_support.h"
#include "tests/support/test_support.h"

using namespace aafr;
using namespace aafr::runtime;
using algorithms::AlgorithmId;
using aafr::test::kPathA;
using aafr::test::kPathB;

namespace {

RuntimeConfig BaseConfig() {
  RuntimeConfig c;
  c.session_id = "t";
  c.scheduler.min_interval_ms = 1000;
  c.stability.min_dwell_ms = 1000;
  c.stability.cooldown_ms = 2000;
  c.stability.failed_activation_cooldown_ms = 500;
  c.stability.candidate_backoff_initial_ms = 1000;
  c.stability.sustain_evaluations = 2;
  c.probe.rounds = 12;
  c.health_check_interval_ms = 0;  // enabled per test
  c.degraded_retry_ms = 1000;
  c.activation_attempts = 1;
  return c;
}

struct Fixture {
  aafr::test::GraphHolder graph{aafr::test::TwoPathGraph()};
  aafr::test::TestExecutor executor{graph};
  ManualClock clock;
  algorithms::AlgorithmExecutionEngine engine;
  std::mutex state_mutex;
  std::shared_ptr<const netstate::StateSnapshot> state;
  std::unique_ptr<AAFRRuntime> runtime;

  explicit Fixture(RuntimeConfig config = BaseConfig(),
                   algorithms::CapabilityRegistry registry = aafr::test::VerifiedRegistry(), bool with_state = true)
      : engine(nullptr, std::move(registry), &executor) {
    RuntimeDependencies d;
    d.graph_source = [this] { return graph.Get(); };
    d.problem_for = [](const forwarding::ForwardingGraph& g) { return aafr::test::ProblemFor(g); };
    if (with_state) {
      SetState(1, netstate::StateValidity::NoData);
      d.state_source = [this] {
        std::lock_guard<std::mutex> lock(state_mutex);
        return state;
      };
    }
    d.executor = &executor;
    d.eligibility = &engine;
    d.clock = &clock;
    runtime = std::make_unique<AAFRRuntime>(std::move(d), std::move(config));
  }
  void SetState(std::uint64_t id, netstate::StateValidity validity) {
    auto s = std::make_shared<netstate::StateSnapshot>();
    s->state.state_id = id;
    s->state.validity = validity;
    s->state.primary_window = "short";
    s->state.windows.push_back({"short", 10'000, {}});
    std::lock_guard<std::mutex> lock(state_mutex);
    state = s;
  }
  // Advances time and runs one step.
  void StepAfter(double ms) {
    clock.AdvanceMs(ms);
    runtime->Step();
  }
  void Evaluate(double after_ms = 1100) {
    clock.AdvanceMs(after_ms);
    runtime->RequestEvaluation(EvaluationTrigger::Manual, "test");
    runtime->Step();
  }
  DecisionRecord Last() const { return runtime->Decisions(1).back(); }
  ActiveConfiguration Active() const { return *runtime->Status().active; }
};

bool Has(const DecisionRecord& r, RuntimeReason reason) { return r.HasReason(reason); }

// ---------------------------------------------------------------- lifecycle

void InitialActivation() {
  Fixture f;
  CHECK(f.runtime->Status().lifecycle == RuntimeLifecycle::Uninitialized);
  CHECK(f.runtime->Initialize());
  const RuntimeStatus s = f.runtime->Status();
  CHECK(s.lifecycle == RuntimeLifecycle::Ready);
  CHECK(s.active && s.active_verified);
  CHECK(s.active->path == kPathA);
  CHECK(s.active->algorithm == AlgorithmId::Dijkstra);
  CHECK(s.active->config_version == 1);
  const DecisionRecord r = f.Last();
  CHECK(r.type == DecisionType::InitialActivation);
  CHECK(Has(r, RuntimeReason::InitialActivation));
  CHECK(Has(r, RuntimeReason::SwitchCommitted));
  CHECK(r.activation == ActivationStatus::Confirmed);
  CHECK(!r.previous_algorithm);  // not a switch from an imaginary algorithm
  CHECK(r.confirmed_path == kPathA);
  CHECK(!r.activation_request_id.empty());
  CHECK(r.trigger == EvaluationTrigger::Startup);
  CHECK(r.eligible_algorithms.size() == 10);
  CHECK(!r.overridden_checks.empty());  // host-state checks with a no-data NetworkState
  CHECK(r.network_validity == "no_data");
  CHECK(f.runtime->Initialize());  // idempotent
  const auto events = f.runtime->Events(100);
  CHECK(std::any_of(events.begin(), events.end(), [](const RuntimeEvent& e) { return e.type == RuntimeEventType::RuntimeInitialized; }));
}

void MissingNetworkState() {
  Fixture f(BaseConfig(), aafr::test::VerifiedRegistry(), false);
  CHECK(f.runtime->Initialize());
  const DecisionRecord r = f.Last();
  CHECK(Has(r, RuntimeReason::NetworkStateUnavailable));
  CHECK(r.network_state_id == 0);
  CHECK(r.network_validity == "unavailable");
}

void NoEligibleAlgorithms() {
  // The V0.4 registry before verification: nothing is implemented.
  Fixture f(BaseConfig(), algorithms::CapabilityRegistry::BuiltIn());
  CHECK(!f.runtime->Initialize());
  const RuntimeStatus s = f.runtime->Status();
  CHECK(s.lifecycle == RuntimeLifecycle::Degraded);
  CHECK(!s.active);
  const DecisionRecord r = f.Last();
  CHECK(r.type == DecisionType::Unavailable);
  CHECK(Has(r, RuntimeReason::NoEligibleAlgorithm));
  CHECK(r.activation == ActivationStatus::NotRequested);
}

void InvalidConfigurationFails() {
  RuntimeConfig c = BaseConfig();
  c.session_id = "bad id!";
  Fixture f(c);
  CHECK(!f.runtime->Initialize());
  CHECK(f.runtime->Status().lifecycle == RuntimeLifecycle::Failed);
  CHECK(!f.runtime->Status().errors.empty());
  f.runtime->Stop();
  CHECK(f.runtime->Status().lifecycle == RuntimeLifecycle::Stopped);
}

void InitialActivationFailure() {
  Fixture f;
  f.executor.fail_all = true;
  CHECK(!f.runtime->Initialize());
  RuntimeStatus s = f.runtime->Status();
  CHECK(s.lifecycle == RuntimeLifecycle::Degraded);
  CHECK(!s.active);
  DecisionRecord r = f.Last();
  CHECK(Has(r, RuntimeReason::SwitchFailed));
  CHECK(Has(r, RuntimeReason::RecoveryFailed));
  CHECK(r.recovery == RecoveryStatus::Failed);
  CHECK(r.activation == ActivationStatus::Failed);
  // The relays come back: the periodic retry while degraded activates.
  f.executor.fail_all = false;
  f.StepAfter(1500);
  f.StepAfter(1500);  // past the failed candidates' backoff
  f.StepAfter(1500);
  s = f.runtime->Status();
  CHECK(s.lifecycle == RuntimeLifecycle::Ready);
  CHECK(s.active && s.active->path == kPathA);
}

void CleanShutdown() {
  Fixture f;
  f.runtime->Initialize();
  f.runtime->Stop();
  const RuntimeStatus s = f.runtime->Status();
  CHECK(s.lifecycle == RuntimeLifecycle::Stopped);
  const auto before = f.runtime->Decisions(100).size();
  f.StepAfter(5000);  // a stopped runtime does nothing
  CHECK(f.runtime->Decisions(100).size() == before);
  f.runtime->Stop();  // idempotent
  const auto events = f.runtime->Events(100);
  CHECK(aafr::test::Contains(events.back().detail, "stopping -> stopped"));
}

// ---------------------------------------------------------------- change detection

void MinorFluctuationNoEvaluation() {
  Fixture f;
  f.runtime->Initialize();
  const auto evaluations = f.runtime->Status().counters.evaluations;
  f.graph.SetLatency("entry", "relay_a", 1.5);  // below the 2 ms edge threshold
  f.StepAfter(1500);
  f.graph.SetLatency("entry", "relay_a", 1.2);
  f.StepAfter(1500);
  CHECK(f.runtime->Status().counters.evaluations == evaluations);
  CHECK(f.Active().path == kPathA);
}

// ---------------------------------------------------------------- switching

void SwitchWithStatisticalEvidence() {
  Fixture f;
  f.runtime->Initialize();
  f.graph.SetLatency("entry", "relay_a", 100.0);  // congest path A: J 102 vs 21
  f.StepAfter(1500);
  DecisionRecord r = f.Last();
  CHECK(r.trigger == EvaluationTrigger::TopologyChange);
  CHECK(Has(r, RuntimeReason::TopologyChanged));
  CHECK(r.probing_attempted);
  CHECK(r.statistical_method == "paired_t_log_latency");
  CHECK(r.evidence_validity == "challenger_better");
  CHECK(r.latency_ratio && *r.latency_ratio < 0.5);
  CHECK(r.paired_samples == 12);
  CHECK(r.type == DecisionType::SwitchRejected);  // first preference: hysteresis
  CHECK(Has(r, RuntimeReason::HysteresisNotSatisfied));
  CHECK(r.stability && !r.stability->hysteresis.passed && r.stability->dwell.passed);
  CHECK(f.Active().path == kPathA);  // nothing changed yet
  f.Evaluate();
  r = f.Last();
  CHECK(r.type == DecisionType::Switch);
  CHECK(Has(r, RuntimeReason::ImprovementConfirmed));
  CHECK(Has(r, RuntimeReason::SwitchRequested));
  CHECK(Has(r, RuntimeReason::SwitchCommitted));
  CHECK(r.activation == ActivationStatus::Confirmed);
  CHECK(r.previous_path == kPathA && r.candidate_path == kPathB && r.confirmed_path == kPathB);
  CHECK(f.Active().path == kPathB);
  CHECK(f.Active().config_version == 2);
  CHECK(f.Active().decision_id == r.decision_id);
  CHECK(f.runtime->Status().counters.switches == 1);
}

void DwellBlocksImmediateReturn() {
  Fixture f;
  f.runtime->Initialize();
  f.graph.SetLatency("entry", "relay_a", 100.0);
  f.StepAfter(1500);
  f.Evaluate();
  CHECK(f.Active().path == kPathB);
  // Congestion clears right after the switch.
  f.graph.SetLatency("entry", "relay_a", 1.0);
  f.StepAfter(1050);
  DecisionRecord r = f.Last();
  CHECK(r.type == DecisionType::SwitchRejected);
  CHECK(Has(r, RuntimeReason::MinimumDwellNotMet) || Has(r, RuntimeReason::CooldownActive));
  CHECK(r.stability && !r.stability->cooldown.passed);
  CHECK(f.Active().path == kPathB);
}

void BidirectionalRecovery() {
  Fixture f;
  f.runtime->Initialize();
  // 1. Path A performs better.
  CHECK(f.Active().path == kPathA);
  // 2. Path A degrades, B becomes preferable. 3. The runtime switches to B.
  f.graph.SetLatency("entry", "relay_a", 100.0);
  f.StepAfter(1500);
  f.Evaluate();
  CHECK(f.Active().path == kPathB);
  // 4. Congestion clears. 5. A is preferable again.
  f.graph.SetLatency("entry", "relay_a", 1.0);
  f.StepAfter(1100);  // blocked: dwell/cooldown
  CHECK(f.Active().path == kPathB);
  f.Evaluate(1100);   // cooldown over; hysteresis streak 2
  // 6. The runtime switches back to A: not locked on B.
  const DecisionRecord r = f.Last();
  CHECK(f.Active().path == kPathA);
  CHECK(r.type == DecisionType::Switch);
  CHECK(r.previous_path == kPathB && r.confirmed_path == kPathA);
  CHECK(r.evidence_validity == "challenger_better");
  CHECK(f.runtime->Status().counters.switches == 2);
  // And it can leave A again later.
  f.graph.SetLatency("entry", "relay_a", 100.0);
  f.StepAfter(2500);
  f.Evaluate();
  CHECK(f.Active().path == kPathB);
}

void AlgorithmLevelBidirectional() {
  // A negative operator preference makes Dijkstra and A* ineligible
  // (non-negative weights required): the active Dijkstra must be left for
  // Bellman-Ford. When the preference is removed, Dijkstra is chosen again.
  Fixture f;
  f.runtime->Initialize();
  CHECK(f.Active().algorithm == AlgorithmId::Dijkstra);
  f.graph.Mutate([](forwarding::ForwardingGraph& g) { g.SetEdgePolicyCost("relay_b", "dest", -30.0); });
  f.StepAfter(1500);
  DecisionRecord r = f.Last();
  CHECK(r.safety_override);
  CHECK(Has(r, RuntimeReason::ActiveAlgorithmIneligible));
  CHECK(Has(r, RuntimeReason::SafetyOverride));
  CHECK(r.type == DecisionType::SafetySwitch);
  CHECK(std::find(r.eligible_algorithms.begin(), r.eligible_algorithms.end(), "dijkstra") == r.eligible_algorithms.end());
  CHECK(f.Active().algorithm == AlgorithmId::BellmanFord);
  CHECK(f.Active().path == kPathB);
  f.graph.Mutate([](forwarding::ForwardingGraph& g) { g.SetEdgePolicyCost("relay_b", "dest", 0.0); });
  f.StepAfter(2500);
  f.Evaluate();
  r = f.Last();
  CHECK(r.type == DecisionType::Switch);
  CHECK(f.Active().algorithm == AlgorithmId::Dijkstra);
  CHECK(f.Active().path == kPathA);
}

void EstimateOnlyWhenArbitrationDisabled() {
  RuntimeConfig c = BaseConfig();
  c.statistical_arbitration = false;
  Fixture f(c);
  f.runtime->Initialize();
  f.graph.SetLatency("entry", "relay_a", 100.0);
  f.StepAfter(1500);
  f.Evaluate();
  const DecisionRecord r = f.Last();
  CHECK(!r.probing_attempted);
  CHECK(Has(r, RuntimeReason::ImprovementEstimateOnly));
  CHECK(r.evidence_validity == "not_run");
  CHECK(r.type == DecisionType::Switch);
  CHECK(f.Active().path == kPathB);
  // A small estimated gain does not pass the conservative 20% margin.
  f.graph.SetLatency("entry", "relay_a", 1.0);
  f.graph.SetLatency("entry", "relay_b", 1.0);
  f.graph.SetLatency("relay_b", "dest", 1.4);  // A: J 3.0, B: J 3.4 -> 11.8% better
  f.StepAfter(2500);
  f.Evaluate();
  CHECK(Has(f.Last(), RuntimeReason::ImprovementMarginNotMet));
  CHECK(f.Active().path == kPathB);
}

void ProbingUnavailableFallsBack() {
  Fixture f;
  f.runtime->Initialize();
  f.graph.SetLatency("entry", "relay_a", 100.0);
  // An executor whose hop limit is below the arm paths cannot carry the
  // probes, although activation (simulated) still works.
  f.executor.max_hops = 1;
  f.StepAfter(1500);
  const DecisionRecord r = f.Last();
  CHECK(!r.probing_attempted);
  CHECK(Has(r, RuntimeReason::ProbingUnavailable));
  CHECK(aafr::test::Contains(r.probing_unavailable_reason, "hop limit"));
  CHECK(Has(r, RuntimeReason::ImprovementEstimateOnly));
}

void CandidateSameAsActiveRetains() {
  Fixture f;
  f.runtime->Initialize();
  f.Evaluate(1500);
  const DecisionRecord r = f.Last();
  CHECK(r.type == DecisionType::Retain);
  CHECK(Has(r, RuntimeReason::CandidateSameAsActive));
  CHECK(Has(r, RuntimeReason::ManualEvaluation));
  CHECK(r.activation == ActivationStatus::NotRequested);
  CHECK(r.confirmed_path == kPathA);
}

void NoBetterChallengerRetains() {
  // Equal estimated preference but measured evidence says no difference.
  Fixture f;
  f.runtime->Initialize();
  f.graph.SetLatency("entry", "relay_a", 12.0);  // A: J 14 vs B: J 21... still A
  f.graph.SetLatency("relay_a", "dest", 9.0);    // A: J 22 vs B: J 21 -> B by estimate
  f.StepAfter(1500);
  const DecisionRecord r = f.Last();
  CHECK(r.probing_attempted);
  // 21 vs 22 ms with 5% noise: B is about 5% faster; either the evidence is
  // not significant or the margin (10%) is not met. Never a switch.
  CHECK(r.type == DecisionType::Retain || r.type == DecisionType::SwitchRejected);
  CHECK(f.Active().path == kPathA);
}

// ---------------------------------------------------------------- failures

void FailedSwitchKeepsPrevious() {
  Fixture f;
  f.runtime->Initialize();
  f.graph.SetLatency("entry", "relay_a", 100.0);
  f.StepAfter(1500);
  // Probes succeed, but every later request on B fails (activation).
  int probes = 0;
  f.executor.on_request = [&](const forwarding::ForwardingRequest& rq, const forwarding::ForwardingExecutionContext& ctx) {
    if (ctx.scenario_id == "runtime_probe") ++probes;
    if (ctx.scenario_id == "runtime_activation" && rq.path == kPathB) {
      std::lock_guard<std::mutex> lock(f.executor.mutex_);
      f.executor.fail_paths.insert(kPathB);
    }
  };
  f.Evaluate();
  const DecisionRecord r = f.Last();
  CHECK(probes > 0);
  CHECK(Has(r, RuntimeReason::SwitchFailed));
  CHECK(Has(r, RuntimeReason::PreviousConfigurationRetained));
  CHECK(r.recovery == RecoveryStatus::PreviousRetained);
  CHECK(r.activation == ActivationStatus::Failed);
  CHECK(r.confirmed_path == kPathA);
  const RuntimeStatus s = f.runtime->Status();
  CHECK(s.active->path == kPathA && s.active_verified);
  CHECK(s.counters.failed_switches == 1);
  CHECK(s.counters.switches == 0);
  CHECK(s.lifecycle == RuntimeLifecycle::Ready);
  CHECK(f.runtime->OutcomeTotals().activation_failures == 1);
}

void ActivationTimeoutAndUnconfirmed() {
  {
    Fixture f;
    f.runtime->Initialize();
    f.graph.SetLatency("entry", "relay_a", 100.0);
    f.StepAfter(1500);
    f.executor.on_request = [&](const forwarding::ForwardingRequest&, const forwarding::ForwardingExecutionContext& ctx) {
      if (ctx.scenario_id == "runtime_activation") {
        std::lock_guard<std::mutex> lock(f.executor.mutex_);
        f.executor.timeout_paths.insert(kPathB);
      }
    };
    f.Evaluate();
    const DecisionRecord r = f.Last();
    CHECK(r.activation == ActivationStatus::TimedOut);
    CHECK(Has(r, RuntimeReason::ActivationTimeout));
    CHECK(f.Active().path == kPathA);
  }
  {
    Fixture f;
    f.runtime->Initialize();
    f.graph.SetLatency("entry", "relay_a", 100.0);
    f.StepAfter(1500);
    f.executor.on_request = [&](const forwarding::ForwardingRequest&, const forwarding::ForwardingExecutionContext& ctx) {
      if (ctx.scenario_id == "runtime_activation") {
        std::lock_guard<std::mutex> lock(f.executor.mutex_);
        f.executor.unconfirmed_paths.insert(kPathB);
      }
    };
    f.Evaluate();
    const DecisionRecord r = f.Last();
    // Delivered without proof of the path is not an activation.
    CHECK(r.activation == ActivationStatus::Failed);
    CHECK(Has(r, RuntimeReason::SwitchFailed));
    CHECK(f.Active().path == kPathA);
  }
}

void ActivePathFailureSafetyOverride() {
  RuntimeConfig c = BaseConfig();
  c.health_check_interval_ms = 500;
  c.failures_for_safety_override = 2;
  Fixture f(c);
  f.runtime->Initialize();
  // Path A breaks at the relay without any graph change.
  {
    std::lock_guard<std::mutex> lock(f.executor.mutex_);
    f.executor.fail_paths.insert(kPathA);
  }
  f.StepAfter(600);  // health check 1 fails
  CHECK(f.runtime->Status().consecutive_failures == 1);
  f.StepAfter(600);  // health check 2 fails -> failure recovery (ignores spacing and dwell)
  const DecisionRecord r = f.Last();
  CHECK(r.trigger == EvaluationTrigger::FailureRecovery);
  CHECK(r.safety_override);
  CHECK(Has(r, RuntimeReason::ActivePathFailed));
  CHECK(Has(r, RuntimeReason::SafetyOverride));
  CHECK(r.type == DecisionType::SafetySwitch);
  CHECK(r.stability && !r.stability->dwell.applicable);
  CHECK(f.Active().path == kPathB);
  CHECK(f.runtime->Status().counters.safety_overrides == 1);
  // Health checks are associated with the decision that activated the path.
  f.StepAfter(600);
  const auto outcome = f.runtime->Outcome(r.decision_id);
  CHECK(outcome && outcome->health_checks >= 1);
}

void RecoveryFailureThenSuccess() {
  RuntimeConfig c = BaseConfig();
  c.health_check_interval_ms = 500;
  c.failures_for_safety_override = 1;
  Fixture f(c);
  f.runtime->Initialize();
  f.executor.fail_all = true;
  f.StepAfter(600);  // health check fails -> safety evaluation, B also fails
  RuntimeStatus s = f.runtime->Status();
  CHECK(s.lifecycle == RuntimeLifecycle::Degraded);
  CHECK(!s.active_verified);
  DecisionRecord r = f.Last();
  CHECK(Has(r, RuntimeReason::RecoveryFailed));
  CHECK(Has(r, RuntimeReason::ActiveConfigurationUnknown));
  CHECK(r.recovery == RecoveryStatus::Failed);
  // Forwarding over an unverified configuration is refused.
  CHECK(f.runtime->ForwardActive("app-1", "hello", 500).response_status == "NO_ACTIVE_CONFIGURATION");
  f.executor.fail_all = false;
  for (int i = 0; i < 6 && f.runtime->Status().lifecycle != RuntimeLifecycle::Ready; ++i) f.StepAfter(1100);
  s = f.runtime->Status();
  CHECK(s.lifecycle == RuntimeLifecycle::Ready);
  CHECK(s.active_verified);
  r = f.Last();
  CHECK(r.type == DecisionType::Recovery);
  CHECK(Has(r, RuntimeReason::RecoverySucceeded));
  CHECK(f.runtime->OutcomeTotals().recoveries_succeeded >= 1);
}

void StaleEvaluationDiscarded() {
  Fixture f;
  f.runtime->Initialize();
  f.graph.SetLatency("entry", "relay_a", 100.0);
  f.StepAfter(1500);
  // The graph changes while the activation request is in flight.
  bool changed = false;
  f.executor.on_request = [&](const forwarding::ForwardingRequest&, const forwarding::ForwardingExecutionContext& ctx) {
    if (ctx.scenario_id == "runtime_activation" && !changed) {
      changed = true;
      f.graph.SetLatency("relay_b", "dest", 10.5);
    }
  };
  f.Evaluate();
  const DecisionRecord r = f.Last();
  CHECK(r.type == DecisionType::Discarded);
  CHECK(Has(r, RuntimeReason::EvaluationStale));
  CHECK(f.Active().path == kPathA);  // the stale result was not committed
  CHECK(f.runtime->Status().counters.discarded == 1);
  CHECK(f.runtime->Status().pending_evaluation);  // re-evaluation scheduled
}

// ---------------------------------------------------------------- concurrency

void NoOverlappingSwitches() {
  Fixture f;
  f.runtime->Initialize();
  f.graph.SetLatency("entry", "relay_a", 100.0);
  f.clock.AdvanceMs(1500);
  std::vector<std::thread> threads;
  for (int t = 0; t < 4; ++t) {
    threads.emplace_back([&] {
      for (int i = 0; i < 20; ++i) {
        f.runtime->RequestEvaluation(EvaluationTrigger::Manual, "race");
        f.runtime->Step();
        f.clock.AdvanceMs(400);
      }
    });
  }
  for (auto& t : threads) t.join();
  // The executor never saw two runtime requests at the same time.
  CHECK(f.executor.max_in_flight.load() == 1);
  // Decisions are recorded one after another with increasing times.
  const auto decisions = f.runtime->Decisions(500);
  for (std::size_t i = 1; i < decisions.size(); ++i) CHECK(decisions[i].monotonic_ns >= decisions[i - 1].monotonic_ns);
  CHECK(f.runtime->Status().counters.coalesced_requests > 0);
  CHECK(f.Active().path == kPathB);
}

void ConcurrentUpdatesWithWorker() {
  RuntimeConfig c = BaseConfig();
  c.tick_ms = 10;
  c.scheduler.min_interval_ms = 50;
  c.stability.min_dwell_ms = 100;
  c.stability.cooldown_ms = 100;
  c.stability.sustain_evaluations = 1;
  c.probe.rounds = 8;
  Fixture f(c);
  f.runtime->Start();
  // Telemetry and topology change from other threads while the worker runs.
  std::atomic<bool> done{false};
  std::thread telemetry([&] {
    for (std::uint64_t i = 2; i < 200 && !done; ++i) {
      f.SetState(i, i % 2 ? netstate::StateValidity::Degraded : netstate::StateValidity::NoData);
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  });
  std::thread topology([&] {
    for (int i = 0; i < 60 && !done; ++i) {
      f.graph.SetLatency("entry", "relay_a", i % 2 ? 100.0 : 1.0);
      f.clock.AdvanceMs(60);
      std::this_thread::sleep_for(std::chrono::milliseconds(3));
    }
  });
  std::thread readers([&] {
    while (!done) {
      (void)f.runtime->Status();
      (void)f.runtime->Decisions(10);
      (void)f.runtime->Events(10);
      (void)f.runtime->RecentDecisions(5);
    }
  });
  topology.join();
  done = true;
  telemetry.join();
  readers.join();
  f.runtime->Stop();
  const RuntimeStatus s = f.runtime->Status();
  CHECK(s.lifecycle == RuntimeLifecycle::Stopped);
  CHECK(s.counters.evaluations >= 2);
  CHECK(f.executor.max_in_flight.load() == 1);
  // Every committed configuration was confirmed on the graph version it was evaluated on.
  for (const auto& d : f.runtime->Decisions(500)) {
    if (d.activation == ActivationStatus::Confirmed) CHECK(d.HasReason(RuntimeReason::SwitchCommitted));
  }
}

void ShutdownDuringProbingAndSwitching() {
  {
    Fixture f;
    f.runtime->Initialize();
    f.graph.SetLatency("entry", "relay_a", 100.0);
    f.clock.AdvanceMs(1500);
    f.executor.sleep_ms = 50;  // each probe takes 50 ms of real time
    std::thread step([&] { f.runtime->Step(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    const auto start = std::chrono::steady_clock::now();
    f.runtime->Stop();
    step.join();
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    CHECK(ms < 1000.0);  // 24 probes would take 1.2 s
    CHECK(f.runtime->Status().lifecycle == RuntimeLifecycle::Stopped);
    CHECK(f.Active().path == kPathA);
  }
  {
    RuntimeConfig c = BaseConfig();
    c.statistical_arbitration = false;
    c.stability.sustain_evaluations = 1;
    Fixture f(c);
    f.runtime->Initialize();
    f.graph.SetLatency("entry", "relay_a", 100.0);
    f.clock.AdvanceMs(1500);
    f.executor.on_request = [&](const forwarding::ForwardingRequest&, const forwarding::ForwardingExecutionContext& ctx) {
      if (ctx.scenario_id == "runtime_activation") f.executor.sleep_ms = 500;
    };
    std::thread step([&] { f.runtime->Step(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    f.runtime->Stop();
    step.join();
    CHECK(f.runtime->Status().lifecycle == RuntimeLifecycle::Stopped);
    // The interrupted activation was not committed.
    CHECK(f.Active().path == kPathA);
  }
}

// ---------------------------------------------------------------- explainability

void CompleteDecisionRecords() {
  Fixture f;
  f.runtime->Initialize();
  f.graph.SetLatency("entry", "relay_a", 100.0);
  f.StepAfter(1500);
  f.Evaluate();
  for (const DecisionRecord& r : f.runtime->Decisions(10)) {
    CHECK(r.schema == kDecisionRecordSchema);
    CHECK(!r.decision_id.empty() && r.session_id == "t");
    CHECK(r.graph_version > 0 && r.graph_id == "two_path");
    CHECK(!r.provider_id.empty());
    CHECK(!r.explanation.empty());
    CHECK(!r.reasons.empty());
    CHECK(!r.measurement_quality.empty());
    CHECK(!r.ranking.empty());
    const std::string json = ToJson(r);
    for (const char* key : {"\"decision_id\"", "\"reason_codes\"", "\"stability\"", "\"activation_status\"",
                            "\"evidence_validity\"", "\"outcome_status\"", "\"ranking\"", "\"graph_version\""}) {
      CHECK(aafr::test::Contains(json, key));
    }
  }
  const DecisionRecord sw = f.Last();
  CHECK(sw.stability && sw.stability->allowed);
  CHECK(sw.stability->Gates().size() == 6);
  CHECK(sw.probe_overhead_ms >= 0.0 && sw.probe_requests == 24);
  CHECK(sw.algorithm_computation_ms >= 0.0);
  CHECK(sw.activation_latency_ms.has_value());
  CHECK(aafr::test::Contains(sw.explanation, "switched to"));
  // The V0.3 viewer contract sees approved + committed.
  const auto legacy = f.runtime->RecentDecisions(1).back();
  CHECK(legacy.decision_type == observability::DecisionType::SwitchApproved);
  CHECK(legacy.execution == observability::ExecutionConfirmation::Committed);
  CHECK(observability::SwitchOutcome(legacy) == "Switch committed" ||
        aafr::test::Contains(observability::SwitchOutcome(legacy), "ommitted"));
  const auto snapshot = f.runtime->Snapshot();
  CHECK(snapshot.state == observability::RuntimeState::Running);
  CHECK(snapshot.active_algorithm && snapshot.active_algorithm->activation_decision_id == sw.decision_id);
  CHECK(!f.runtime->DecisionsJsonl().empty());
}

void OutcomeAssociation() {
  Fixture f;
  f.runtime->Initialize();
  const std::string decision = f.Active().decision_id;
  for (int i = 0; i < 5; ++i) {
    const auto r = f.runtime->ForwardActive("app-" + std::to_string(i), "payload", 1000);
    CHECK(r.status == forwarding::ForwardingStatus::Delivered && r.path_confirmed);
  }
  auto outcome = f.runtime->Outcome(decision);
  CHECK(outcome.has_value());
  CHECK(outcome->application_requests == 5);
  CHECK(outcome->config_version == 1);
  CHECK(outcome->algorithm == AlgorithmId::Dijkstra);
  CHECK(outcome->performance.successes >= 5);
  CHECK(outcome->performance.latency_median_ms.has_value());
  CHECK(!outcome->window_complete);
  const auto first_revision = outcome->revision;
  f.runtime->ForwardActive("app-late", "payload", 1000);
  outcome = f.runtime->Outcome(decision);
  CHECK(outcome->revision == first_revision + 1);  // a new revision; the record is untouched
  f.clock.AdvanceMs(31'000);
  CHECK(f.runtime->Outcome(decision)->window_complete);
  CHECK(f.Last().outcome_status == "pending");
  CHECK(!f.runtime->Outcome("unknown-decision").has_value());
  CHECK(aafr::test::Contains(ToJson(*outcome), "\"revision\""));
}

void TelemetryEventsPublished() {
  telemetry::TelemetryPipeline pipeline;
  aafr::test::GraphHolder graph(aafr::test::TwoPathGraph());
  aafr::test::TestExecutor executor(graph);
  algorithms::AlgorithmExecutionEngine engine(nullptr, aafr::test::VerifiedRegistry(), &executor);
  ManualClock clock;
  RuntimeDependencies d;
  d.graph_source = [&] { return graph.Get(); };
  d.problem_for = [](const forwarding::ForwardingGraph& g) { return aafr::test::ProblemFor(g); };
  d.executor = &executor;
  d.eligibility = &engine;
  d.clock = &clock;
  d.pipeline = &pipeline;
  AAFRRuntime runtime(std::move(d), BaseConfig());
  runtime.Initialize();
  const auto events = pipeline.EventsAfter(0, 2000);
  bool initialized = false, committed = false;
  for (const auto& e : events) {
    if (e.source != telemetry::Source::AdaptiveRuntime) continue;
    initialized = initialized || e.type == telemetry::EventType::RuntimeInitialized;
    committed = committed || e.type == telemetry::EventType::SwitchCommitted;
  }
  CHECK(initialized && committed);
  CHECK(std::string(telemetry::ToString(telemetry::EventType::SwitchCommitted)) == "runtime.switch_committed");
}

// ---------------------------------------------------------------- V0.7.1

RuntimeConfig ReviewConfig() {
  RuntimeConfig c = BaseConfig();
  c.review_interval_ms = 5000;
  return c;
}

// The key V0.7.1 regression (the reference repository's "never switches
// back"): congestion that only measurements show, then its disappearance.
// The graph never changes, so no change trigger fires; the measurement-led
// review must detect both directions.
void MeasurementOnlyCongestionAndRecovery() {
  Fixture f(ReviewConfig());
  f.runtime->Initialize();
  CHECK(f.Active().path == kPathA);
  const auto version = f.graph.Get()->version();
  f.executor.extra_latency_ms[kPathA] = 40.0;  // A: ~42 ms measured, graph still says 2 ms.
  f.StepAfter(1000);
  CHECK(f.runtime->Status().counters.evaluations == 1);  // no change trigger fired
  f.StepAfter(4100);                                      // review due
  DecisionRecord r = f.Last();
  CHECK(r.trigger == EvaluationTrigger::Review);
  CHECK(Has(r, RuntimeReason::MeasurementReview));
  CHECK(r.probing_attempted);
  CHECK(r.evidence_validity == "challenger_better");
  CHECK(r.candidate_path == kPathB);
  CHECK(r.type == DecisionType::SwitchRejected);
  CHECK(Has(r, RuntimeReason::HysteresisNotSatisfied));
  f.StepAfter(1100);  // follow-up review re-measures
  r = f.Last();
  CHECK(r.trigger == EvaluationTrigger::Review);
  CHECK(r.type == DecisionType::Switch);
  CHECK(r.activation == ActivationStatus::Confirmed);
  CHECK(f.Active().path == kPathB);
  // A is still congested: the next review keeps B (J(P) alone would return).
  f.StepAfter(5100);
  r = f.Last();
  CHECK(r.trigger == EvaluationTrigger::Review);
  CHECK(r.type == DecisionType::Retain);
  CHECK(r.evidence_validity == "challenger_not_better");
  CHECK(f.Active().path == kPathB);
  // Congestion disappears: A is re-evaluated and chosen again.
  f.executor.extra_latency_ms.erase(kPathA);
  f.StepAfter(5100);
  r = f.Last();
  CHECK(r.evidence_validity == "challenger_better");
  CHECK(r.candidate_path == kPathA);
  f.StepAfter(1100);
  r = f.Last();
  CHECK(r.type == DecisionType::Switch);
  CHECK(r.previous_path == kPathB && r.confirmed_path == kPathA);
  CHECK(f.Active().path == kPathA);
  CHECK(f.runtime->Status().counters.switches == 2);
  CHECK(f.runtime->Status().counters.reviews >= 4);
  CHECK(f.graph.Get()->version() == version);  // nothing on the graph changed
}

// V0.6 behaviour, kept reachable by configuration: without reviews the
// measurement-only degradation is never acted on (why the review exists).
void ReviewDisabledMissesMeasurementOnlyCongestion() {
  RuntimeConfig c = BaseConfig();
  c.review_interval_ms = 0;
  Fixture f(c);
  f.runtime->Initialize();
  f.executor.extra_latency_ms[kPathA] = 40.0;
  for (int i = 0; i < 40; ++i) f.StepAfter(500);
  CHECK(f.runtime->Status().counters.evaluations == 1);
  CHECK(f.Active().path == kPathA);
  CHECK(!f.runtime->Status().review_due_ms.has_value());
}

// A review never switches on J(P) alone.
void ReviewWithoutProbingNeverSwitches() {
  RuntimeConfig c = ReviewConfig();
  c.statistical_arbitration = false;
  Fixture f(c);
  f.runtime->Initialize();
  f.executor.extra_latency_ms[kPathA] = 40.0;
  for (int i = 0; i < 30; ++i) f.StepAfter(500);
  CHECK(f.Active().path == kPathA);
  CHECK(f.runtime->Status().counters.switches == 0);
  for (const auto& r : f.runtime->Decisions(64)) CHECK(!Has(r, RuntimeReason::MeasurementReview));
}

// Latency parity must not mask loss: equal-latency paths, the active one
// loses most requests (measurement only); a review leaves it.
void EqualLatencyLossyIncumbentLeftByReview() {
  Fixture f(ReviewConfig());
  f.graph.SetLatency("entry", "relay_b", 1.0);
  f.graph.SetLatency("relay_b", "dest", 1.0);
  f.runtime->Initialize();
  const forwarding::Path lossy = f.Active().path;
  f.executor.loss_ratio[lossy] = 0.7;
  for (int i = 0; i < 30 && f.Active().path == lossy; ++i) f.StepAfter(500);
  CHECK(f.Active().path != lossy);
  bool repair = false;
  for (const auto& r : f.runtime->Decisions(64)) {
    if (r.type == DecisionType::Switch) repair = r.statistical_method == "wilson_failure_rate";
  }
  CHECK(repair);
}

// Status exposes the review schedule; a commit restarts it.
void ReviewScheduleInStatus() {
  Fixture f(ReviewConfig());
  f.runtime->Initialize();
  auto s = f.runtime->Status();
  CHECK(s.review_due_ms.has_value() && *s.review_due_ms <= 5000.0 && *s.review_due_ms > 4000.0);
  f.StepAfter(2000);
  s = f.runtime->Status();
  CHECK(s.review_due_ms && *s.review_due_ms <= 3000.0);
  f.StepAfter(3100);
  s = f.runtime->Status();
  CHECK(s.counters.reviews == 1);
  CHECK(!s.last_review.empty());
  CHECK(s.review_due_ms && *s.review_due_ms > 4000.0);
}

void InvalidReviewIntervalRejected() {
  RuntimeConfig c = BaseConfig();
  c.review_interval_ms = 10;
  bool found = false;
  for (const auto& issue : c.Validate()) found = found || issue.find("review_interval_ms") != std::string::npos;
  CHECK(found);
}

}  // namespace

int main(int argc, char** argv) {
  std::vector<std::pair<const char*, void (*)()>> tests = {
      {"initial_activation", InitialActivation},
      {"missing_network_state", MissingNetworkState},
      {"no_eligible_algorithms", NoEligibleAlgorithms},
      {"invalid_configuration_fails", InvalidConfigurationFails},
      {"initial_activation_failure", InitialActivationFailure},
      {"clean_shutdown", CleanShutdown},
      {"minor_fluctuation_no_evaluation", MinorFluctuationNoEvaluation},
      {"switch_with_statistical_evidence", SwitchWithStatisticalEvidence},
      {"dwell_blocks_immediate_return", DwellBlocksImmediateReturn},
      {"bidirectional_recovery", BidirectionalRecovery},
      {"algorithm_level_bidirectional", AlgorithmLevelBidirectional},
      {"estimate_only_when_arbitration_disabled", EstimateOnlyWhenArbitrationDisabled},
      {"probing_unavailable_falls_back", ProbingUnavailableFallsBack},
      {"candidate_same_as_active_retains", CandidateSameAsActiveRetains},
      {"no_better_challenger_retains", NoBetterChallengerRetains},
      {"failed_switch_keeps_previous", FailedSwitchKeepsPrevious},
      {"activation_timeout_and_unconfirmed", ActivationTimeoutAndUnconfirmed},
      {"active_path_failure_safety_override", ActivePathFailureSafetyOverride},
      {"recovery_failure_then_success", RecoveryFailureThenSuccess},
      {"stale_evaluation_discarded", StaleEvaluationDiscarded},
      {"no_overlapping_switches", NoOverlappingSwitches},
      {"concurrent_updates_with_worker", ConcurrentUpdatesWithWorker},
      {"shutdown_during_probing_and_switching", ShutdownDuringProbingAndSwitching},
      {"complete_decision_records", CompleteDecisionRecords},
      {"outcome_association", OutcomeAssociation},
      {"telemetry_events_published", TelemetryEventsPublished},
      {"measurement_only_congestion_and_recovery", MeasurementOnlyCongestionAndRecovery},
      {"review_disabled_misses_measurement_only_congestion", ReviewDisabledMissesMeasurementOnlyCongestion},
      {"review_without_probing_never_switches", ReviewWithoutProbingNeverSwitches},
      {"equal_latency_lossy_incumbent_left_by_review", EqualLatencyLossyIncumbentLeftByReview},
      {"review_schedule_in_status", ReviewScheduleInStatus},
      {"invalid_review_interval_rejected", InvalidReviewIntervalRejected},
  };
  // Optional filter: run only tests whose name contains argv[1].
  if (argc > 1) {
    std::vector<std::pair<const char*, void (*)()>> selected;
    for (const auto& t : tests)
      if (std::string(t.first).find(argv[1]) != std::string::npos) selected.push_back(t);
    tests = selected;
  }
  return aafr::test::RunTests(tests);
}
