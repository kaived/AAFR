// V0.5 integration:
//   NetworkState -> eligibility -> algorithm execution -> path validation
//     -> forwarding execution (real loopback relays) -> outcome telemetry
// No Internet access is needed.

#include <chrono>
#include <thread>

#include "algorithms/execution/algorithm_execution_engine.h"
#include "algorithms/execution/implementation_verifier.h"
#include "routing/forwarding/loopback_forwarding.h"
#include "routing/topology/scenario_catalog.h"
#include "tests/support/test_support.h"

using namespace aafr::algorithms;
namespace fwd = aafr::forwarding;
namespace telemetry = aafr::telemetry;

namespace {

struct Fixture {
  fwd::ForwardingGraph graph = fwd::LoopbackDemoGraph();
  fwd::LoopbackForwardingNetwork network{graph};
  fwd::LoopbackForwardingExecutor executor{network};
  telemetry::TelemetryPipeline pipeline;
  CapabilityRegistry registry;

  Fixture() {
    network.Start();
    registry = ApplyVerification(CapabilityRegistry::BuiltIn(), VerifyImplementations(&executor));
  }

  ExecutionRequest Request(AlgorithmId id) const {
    ExecutionRequest request;
    request.algorithm = id;
    request.graph = std::make_shared<const fwd::ForwardingGraph>(graph);
    request.problem = fwd::LoopbackDemoProblem(graph);
    request.budget.max_iterations = 40;
    request.network_state = aafr::test::FreshCaptureState();
    request.scenario_id = "integration";
    return request;
  }

  std::vector<telemetry::EventType> EngineEvents(const std::string& execution_id) const {
    std::vector<telemetry::EventType> types;
    for (const auto& event : pipeline.EventsAfter(0, 5000)) {
      const std::string* id = event.FindAttribute("execution_id");
      if (event.source == telemetry::Source::ForwardingEngine && id && *id == execution_id) types.push_back(event.type);
    }
    return types;
  }
};

void TestEndToEnd() {
  Fixture f;
  AlgorithmExecutionEngine engine(&f.pipeline, f.registry, &f.executor);
  for (const AlgorithmId id : kAllAlgorithmIds) {
    const ExecutionRequest request = f.Request(id);
    const EligibilityResult eligibility = engine.Eligibility(id, request);
    if (eligibility.status != EligibilityStatus::Eligible) std::cerr << eligibility.explanation << "\n";
    CHECK(eligibility.status == EligibilityStatus::Eligible);

    const ExecutionRecord record = engine.RunSync(request);
    CHECK(record.stage == ExecutionStage::Completed);
    CHECK(record.outcome == "COMPLETED");
    CHECK(record.network_state_id == request.network_state->state_id);
    CHECK(record.graph_version == f.graph.version() && record.graph_id == "loopback_demo");
    CHECK(record.computation && record.computation->status == AlgorithmStatus::PathFound);
    CHECK(record.candidate && record.candidate->valid && record.candidate->feasible);
    CHECK(record.forwarding && record.forwarding->path_confirmed);
    if (!record.forwarding || !record.computation) continue;
    CHECK(record.forwarding->confirmed_path == record.computation->path);
    CHECK(record.forwarding->provenance == fwd::Provenance::Measured);
    CHECK(record.forwarding_request_id == record.execution_id + "-fwd");
    CHECK(f.network.NodeLogged("dest", record.forwarding_request_id));
    const auto events = f.EngineEvents(record.execution_id);
    const std::vector<telemetry::EventType> expected = {
        telemetry::EventType::AlgorithmExecutionStarted, telemetry::EventType::AlgorithmExecutionCompleted,
        telemetry::EventType::ForwardingExecutionStarted, telemetry::EventType::ForwardingExecutionCompleted};
    CHECK(events == expected);
  }
  // The engine never ran anything it was not asked to.
  CHECK(engine.Recent().size() == 10);
}

void TestIneligibleIsRejected() {
  Fixture f;
  // Registry without verification: nothing is implemented in its view.
  AlgorithmExecutionEngine unverified(&f.pipeline, CapabilityRegistry::BuiltIn(), &f.executor);
  const ExecutionRecord record = unverified.RunSync(f.Request(AlgorithmId::Dijkstra));
  CHECK(record.stage == ExecutionStage::Rejected && record.outcome == "INELIGIBLE");
  CHECK(!record.computation.has_value() && !record.forwarding.has_value());
  CHECK((f.EngineEvents(record.execution_id) == std::vector<telemetry::EventType>{
                                                    telemetry::EventType::AlgorithmExecutionFailed}));

  // No usable NetworkState: the host-state checks fail and block the run...
  AlgorithmExecutionEngine engine(&f.pipeline, f.registry, &f.executor);
  ExecutionRequest request = f.Request(AlgorithmId::Dijkstra);
  request.network_state.reset();
  const ExecutionRecord blocked = engine.RunSync(request);
  CHECK(blocked.outcome == "INELIGIBLE");
  CHECK(blocked.eligibility && blocked.eligibility->HasReason(ReasonCode::NetworkStateUnusable));
  // ...unless explicitly overridden, which the record states.
  request.enforce_host_state_checks = false;
  const ExecutionRecord overridden = engine.RunSync(request);
  CHECK(overridden.stage == ExecutionStage::Completed);
  CHECK(overridden.overridden_checks.size() == 2);
  // The override never covers other checks: no forwarding executor means no run.
  AlgorithmExecutionEngine no_forwarding(&f.pipeline, f.registry, nullptr);
  const ExecutionRecord without = no_forwarding.RunSync(request);
  CHECK(without.outcome == "INELIGIBLE");
  CHECK(without.eligibility && without.eligibility->HasReason(ReasonCode::ForwardingCapabilityUnavailable));
  // Invalid parameters are rejected before eligibility.
  ExecutionRequest bad = f.Request(AlgorithmId::GeneticAlgorithm);
  bad.parameters = {{"population_size", "-4"}};
  CHECK(engine.RunSync(bad).outcome == "INVALID_REQUEST");
}

void TestCandidateRejected() {
  Fixture f;
  AlgorithmExecutionEngine engine(&f.pipeline, f.registry, &f.executor);
  ExecutionRequest request = f.Request(AlgorithmId::Dijkstra);
  request.problem.constraints.latency_budget_ms = 2.0;  // Every path takes at least 4 ms.
  const ExecutionRecord record = engine.RunSync(request);
  CHECK(record.stage == ExecutionStage::Failed);
  CHECK(record.outcome == "CANDIDATE_REJECTED");
  CHECK(record.candidate && !record.candidate->feasible);
  CHECK(!record.forwarding.has_value());  // Nothing infeasible is forwarded.
  const auto events = f.EngineEvents(record.execution_id);
  CHECK(std::find(events.begin(), events.end(), telemetry::EventType::CandidatePathRejected) != events.end());
  CHECK(std::find(events.begin(), events.end(), telemetry::EventType::ForwardingExecutionStarted) == events.end());
}

void TestForwardingFailure() {
  Fixture f;
  AlgorithmExecutionEngine engine(&f.pipeline, f.registry, &f.executor);
  // The graph still lists relay_a, but its relay process is down.
  f.network.SetNodeOnline("relay_a", false);
  const ExecutionRecord record = engine.RunSync(f.Request(AlgorithmId::Dijkstra));
  CHECK(record.stage == ExecutionStage::Failed);
  CHECK(record.outcome == "FORWARDING_RELAY_UNAVAILABLE");
  CHECK(record.forwarding && !record.forwarding->path_confirmed && record.forwarding->failed_at == "relay_a");
  const auto events = f.EngineEvents(record.execution_id);
  CHECK(!events.empty() && events.back() == telemetry::EventType::ForwardingExecutionFailed);
}

void TestAsyncAndCancellation() {
  Fixture f;
  AlgorithmExecutionEngine engine(&f.pipeline, f.registry, &f.executor);
  const std::string id = engine.Submit(f.Request(AlgorithmId::AntColonyOptimization));
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
  while (!engine.Find(id)->Finished() && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  CHECK(engine.Find(id)->stage == ExecutionStage::Completed);

  // A long computation is cancelled mid-run; a queued one never starts.
  ExecutionRequest slow = f.Request(AlgorithmId::FireflyAlgorithm);
  slow.budget.max_iterations = 100000000;
  slow.budget.deadline_ms = 60000.0;
  slow.problem.constraints.computation_deadline_ms.reset();
  slow.enforce_host_state_checks = false;
  const std::string running = engine.Submit(slow);
  const std::string queued = engine.Submit(f.Request(AlgorithmId::Dijkstra));
  while (engine.Find(running)->stage != ExecutionStage::Computing && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  CHECK(engine.Cancel(queued));
  CHECK(engine.Cancel(running));
  while ((!engine.Find(running)->Finished() || !engine.Find(queued)->Finished()) &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  CHECK(engine.Find(running)->stage == ExecutionStage::Cancelled);
  CHECK(engine.Find(queued)->stage == ExecutionStage::Cancelled);
  CHECK(!engine.Cancel(running));  // Already finished.
}

void TestQueueBound() {
  Fixture f;
  AlgorithmExecutionEngine::Config config;
  config.max_pending = 1;
  AlgorithmExecutionEngine engine(&f.pipeline, f.registry, &f.executor, config);
  ExecutionRequest slow = f.Request(AlgorithmId::FireflyAlgorithm);
  slow.budget.max_iterations = 100000000;
  slow.budget.deadline_ms = 60000.0;
  slow.problem.constraints.computation_deadline_ms.reset();
  slow.enforce_host_state_checks = false;
  std::vector<std::string> ids;
  for (int i = 0; i < 4; ++i) ids.push_back(engine.Submit(slow));
  bool rejected = false;
  for (const auto& id : ids) rejected = rejected || engine.Find(id)->outcome == "QUEUE_FULL";
  CHECK(rejected);
  engine.CancelAll();
}  // Destructor cancels and joins.

}  // namespace

int main() {
  return aafr::test::RunTests({
      {"end_to_end_all_algorithms", TestEndToEnd},
      {"ineligible_is_rejected", TestIneligibleIsRejected},
      {"candidate_rejected", TestCandidateRejected},
      {"forwarding_failure", TestForwardingFailure},
      {"async_and_cancellation", TestAsyncAndCancellation},
      {"queue_bound", TestQueueBound},
  });
}
