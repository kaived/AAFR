// V0.7.1 controlled probing regressions: timeout, cancellation, partial
// failure, deadline, stale measurements, cleanup, executability and
// isolation from application traffic.
//
// The reference repository steers ICMP probes with SDN rules and needed an
// RAII guard to remove them; AAFR probes are ordinary per-request forwarding
// requests over validated paths, so "cleanup" means: no request is left in
// flight and nothing persistent was installed. These tests check exactly
// that on AAFR's own executor contract.

#include "runtime/adaptive/controlled_probe_engine.h"

#include <atomic>
#include <set>

#include "runtime/adaptive/statistical_arbitration.h"
#include "tests/support/runtime_test_support.h"
#include "tests/support/test_support.h"

using namespace aafr;
using namespace aafr::runtime;
using aafr::test::kPathA;
using aafr::test::kPathB;
using algorithms::AlgorithmId;

namespace {

struct Bench {
  aafr::test::GraphHolder graph{aafr::test::TwoPathGraph()};
  aafr::test::TestExecutor executor{graph};
  ManualClock clock;
  ControlledProbeEngine engine{&executor, &clock};
  std::vector<ProbeArm> arms{{"incumbent", AlgorithmId::Dijkstra, kPathA, true},
                             {"challenger1", AlgorithmId::BellmanFord, kPathB, false}};
  ProbeConfig config;
  Bench() { config.rounds = 10; }
  ProbeCampaignResult Run(const std::atomic<bool>* cancel = nullptr) {
    const auto g = graph.Get();
    return engine.Run("probe-test", *g, aafr::test::ProblemFor(*g), arms, config, cancel);
  }
};

void BalancedInterleavedRounds() {
  Bench b;
  const ProbeCampaignResult r = b.Run();
  CHECK(r.ran);
  CHECK(r.rounds_completed == 10);
  CHECK(r.observations.size() == 20);
  // Every round holds every arm exactly once (balanced), and the order
  // varies across rounds (interleaved, not blocked).
  std::set<std::string> first_arms;
  for (std::size_t round = 0; round < 10; ++round) {
    std::set<std::string> seen;
    for (const auto& o : r.observations) {
      if (o.round != round) continue;
      seen.insert(o.arm_id);
      if (o.order_in_round == 0) first_arms.insert(o.arm_id);
    }
    CHECK(seen.size() == 2);
  }
  CHECK(first_arms.size() == 2);
  // Provenance on every observation.
  for (const auto& o : r.observations) {
    CHECK(o.graph_version == r.graph_version);
    CHECK(o.executor == "test_simulated");
    CHECK(o.payload_bytes == b.config.payload_bytes);
    CHECK(o.probe_id.find("probe-test") == 0);
  }
  // Same seed, same order: reproducible.
  const ProbeCampaignResult again = b.Run();
  for (std::size_t i = 0; i < r.observations.size(); ++i) CHECK(r.observations[i].arm_id == again.observations[i].arm_id);
}

void ProbesAreTaggedAndIsolated() {
  Bench b;
  std::set<std::string> scenarios;
  std::set<forwarding::Path> paths;
  bool payload_tagged = true;
  b.executor.on_request = [&](const forwarding::ForwardingRequest& req, const forwarding::ForwardingExecutionContext& ctx) {
    scenarios.insert(ctx.scenario_id);
    paths.insert(req.path);
    payload_tagged = payload_tagged && req.payload.rfind("aafr-probe:", 0) == 0;
  };
  b.Run();
  // Only the runtime_probe scenario, only the two arm paths, every payload
  // tagged: probes never carry or alter application traffic.
  CHECK(scenarios.size() == 1 && *scenarios.begin() == "runtime_probe");
  CHECK(paths == (std::set<forwarding::Path>{kPathA, kPathB}));
  CHECK(payload_tagged);
}

void UnexecutablePathRefused() {
  Bench b;
  b.arms[1].path = {"entry", "dest"};  // no such edge: computed is not executable
  int requests_before = b.executor.requests.load();
  const ProbeCampaignResult r = b.Run();
  CHECK(!r.ran);
  CHECK(aafr::test::Contains(r.unavailable_reason, "does not validate"));
  CHECK(b.executor.requests.load() == requests_before);
  // Duplicate arm paths, a single arm and an unready executor are refused too.
  Bench dup;
  dup.arms[1].path = kPathA;
  CHECK(!dup.Run().ran);
  Bench single;
  single.arms.pop_back();
  CHECK(!single.Run().ran);
  Bench unready;
  unready.executor.ready = false;
  CHECK(aafr::test::Contains(unready.Run().unavailable_reason, "not ready"));
}

void ProbeTimeoutRecorded() {
  Bench b;
  b.executor.timeout_paths.insert(kPathB);
  const ProbeCampaignResult r = b.Run();
  CHECK(r.ran);
  CHECK(r.timeouts == 10);
  for (const auto& o : r.ForArm("challenger1")) CHECK(o->timeout && !o->success);
  // The arbitration counts timeouts as failures: the challenger is never better.
  const ArbitrationResult a = StatisticalArbitrationEngine().Evaluate(r);
  CHECK(a.comparisons.size() == 1);
  CHECK(a.comparisons[0].challenger.timeouts == 10);
  CHECK(a.comparisons[0].verdict == ArbitrationVerdict::ChallengerNotBetter);
  CHECK(!a.best);
}

void CancellationStopsAndCleansUp() {
  Bench b;
  std::atomic<bool> cancel{false};
  int sent = 0;
  b.executor.on_request = [&](const forwarding::ForwardingRequest&, const forwarding::ForwardingExecutionContext&) {
    if (++sent == 7) cancel = true;
  };
  const ProbeCampaignResult r = b.Run(&cancel);
  CHECK(r.cancelled);
  CHECK(r.rounds_completed < 10);
  CHECK(r.observations.size() == 7);
  CHECK(b.executor.in_flight.load() == 0);  // nothing left running
  CHECK(b.executor.max_in_flight.load() == 1);  // probes are sequential
  // A cancelled campaign never produces a better challenger out of partial data.
  const ArbitrationResult a = StatisticalArbitrationEngine().Evaluate(r);
  CHECK(!a.best);
}

void DeadlineBoundsTheCampaign() {
  Bench b;
  b.executor.sleep_ms = 20.0;
  b.config.campaign_deadline_ms = 100.0;
  const ProbeCampaignResult r = b.Run();
  CHECK(r.deadline_reached);
  CHECK(r.observations.size() < 20);
  CHECK(r.total_ms < 1000.0);
  CHECK(b.executor.in_flight.load() == 0);
}

void PartialFailureDropsPairs() {
  Bench b;
  b.executor.loss_ratio[kPathB] = 0.3;
  b.config.rounds = 24;
  const ProbeCampaignResult r = b.Run();
  std::size_t failed = 0;
  for (const auto* o : r.ForArm("challenger1")) failed += o->success ? 0 : 1;
  CHECK(failed > 0 && failed < 24);
  const ArbitrationResult a = StatisticalArbitrationEngine().Evaluate(r);
  const ComparisonResult& c = a.comparisons[0];
  CHECK(c.pairs_dropped == failed);
  CHECK(c.pairs_used == 24 - failed);
  CHECK(c.challenger.failures == failed);
}

void StaleMeasurementsInvalid() {
  Bench b;
  ProbeCampaignResult r = b.Run();
  // One observation from another graph version: the campaign no longer
  // describes one network state, so the evidence is invalid.
  r.observations[3].graph_version = r.graph_version + 1;
  const ArbitrationResult a = StatisticalArbitrationEngine().Evaluate(r);
  CHECK(a.comparisons[0].verdict == ArbitrationVerdict::InvalidEvidence);
  CHECK(!a.best);
}

void InvalidConfigurationRefused() {
  Bench b;
  b.config.rounds = 1;
  const ProbeCampaignResult r = b.Run();
  CHECK(!r.ran);
  CHECK(aafr::test::Contains(r.unavailable_reason, "invalid probe configuration"));
  b.config.rounds = ProbeConfig::kMaxRounds + 1;
  CHECK(!b.Run().ran);
}

}  // namespace

int main() {
  return aafr::test::RunTests({
      {"balanced_interleaved_rounds", BalancedInterleavedRounds},
      {"probes_are_tagged_and_isolated", ProbesAreTaggedAndIsolated},
      {"unexecutable_path_refused", UnexecutablePathRefused},
      {"probe_timeout_recorded", ProbeTimeoutRecorded},
      {"cancellation_stops_and_cleans_up", CancellationStopsAndCleansUp},
      {"deadline_bounds_the_campaign", DeadlineBoundsTheCampaign},
      {"partial_failure_drops_pairs", PartialFailureDropsPairs},
      {"stale_measurements_invalid", StaleMeasurementsInvalid},
      {"invalid_configuration_refused", InvalidConfigurationRefused},
  });
}
