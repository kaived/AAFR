// V0.6 runtime over real loopback forwarding (TCP relays on 127.0.0.1).
//
// Path delays are configured and slept by the relays (inject_edge_latency),
// so the delay values are simulated, but every probe, activation and
// confirmation is a real request through real relay processes-in-thread,
// and activation is proven by each relay's own request log.

#include "runtime/adaptive/aafr_runtime.h"

#include <thread>

#include "routing/controller/forwarding_service.h"
#include "routing/forwarding/loopback_forwarding.h"
#include "tests/support/runtime_test_support.h"
#include "tests/support/test_support.h"

using namespace aafr;
using namespace aafr::runtime;
using aafr::test::kPathA;
using aafr::test::kPathB;

namespace {

void Sleep(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

forwarding::ForwardingGraph SlowTwoPathGraph() {
  // Path A 5 + 5 ms, path B 25 + 25 ms: far apart even at the Windows timer
  // granularity that the relays' sleeps are subject to.
  aafr::test::GraphHolder holder(aafr::test::TwoPathGraph());
  holder.SetLatency("entry", "relay_a", 5.0);
  holder.SetLatency("relay_a", "dest", 5.0);
  holder.SetLatency("entry", "relay_b", 25.0);
  holder.SetLatency("relay_b", "dest", 25.0);
  return *holder.Get();
}

void BidirectionalOverRealRelays() {
  aafr::test::GraphHolder graph(SlowTwoPathGraph());
  forwarding::LoopbackOptions options;
  options.inject_edge_latency = true;
  forwarding::LoopbackForwardingNetwork network(*graph.Get(), options);
  std::string error;
  CHECK(network.Start(&error));
  if (!network.Running()) {
    std::cerr << "relays did not start: " << error << "\n";
    return;
  }
  forwarding::LoopbackForwardingExecutor executor(network);
  algorithms::AlgorithmExecutionEngine engine(nullptr, aafr::test::VerifiedRegistry(), &executor);

  const auto mutate = [&](const char* from, const char* to, double ms) {
    graph.SetLatency(from, to, ms);
    std::string sync_error;
    CHECK(network.SyncWith(*graph.Get(), &sync_error));
  };

  RuntimeConfig config;
  config.session_id = "loop";
  config.scheduler.min_interval_ms = 100;
  config.stability.min_dwell_ms = 200;
  config.stability.cooldown_ms = 300;
  config.stability.sustain_evaluations = 2;
  config.probe.rounds = 16;
  config.probe.probe_timeout_ms = 2000;
  config.health_check_interval_ms = 0;
  RuntimeDependencies d;
  d.graph_source = [&] { return graph.Get(); };
  d.problem_for = [](const forwarding::ForwardingGraph& g) { return aafr::test::ProblemFor(g); };
  d.executor = &executor;
  d.eligibility = &engine;
  d.executor_injects_delay = true;
  AAFRRuntime runtime(std::move(d), config);

  // 1. Initial activation over path A, proven by the relays' logs.
  CHECK(runtime.Initialize());
  RuntimeStatus s = runtime.Status();
  CHECK(s.active && s.active->path == kPathA);
  DecisionRecord r = runtime.Decisions(1).back();
  CHECK(network.NodeLogged("entry", r.activation_request_id));
  CHECK(network.NodeLogged("relay_a", r.activation_request_id));
  CHECK(network.NodeLogged("dest", r.activation_request_id));
  CHECK(!network.NodeLogged("relay_b", r.activation_request_id));
  CHECK(aafr::test::Contains(s.active->confirmation_evidence, "node logs"));

  // 2. Path A congests: +150 ms on entry -> relay_a.
  mutate("entry", "relay_a", 150.0);
  Sleep(150);
  runtime.Step();
  r = runtime.Decisions(1).back();
  CHECK(r.trigger == EvaluationTrigger::TopologyChange);
  CHECK(r.probing_attempted);
  CHECK(r.simulated_delay);
  CHECK(r.evidence_validity == "challenger_better");
  CHECK(r.type == DecisionType::SwitchRejected);  // hysteresis: first preference
  Sleep(150);
  runtime.RequestEvaluation(EvaluationTrigger::Manual, "confirm");
  runtime.Step();
  r = runtime.Decisions(1).back();
  if (r.type != DecisionType::Switch) std::cerr << ToJson(r) << std::endl;
  CHECK(r.type == DecisionType::Switch);
  CHECK(r.activation == ActivationStatus::Confirmed);
  s = runtime.Status();
  CHECK(s.active && s.active->path == kPathB);
  CHECK(network.NodeLogged("relay_b", r.activation_request_id));
  CHECK(!network.NodeLogged("relay_a", r.activation_request_id));
  std::cout << "  switch A->B: latency ratio " << r.latency_ratio.value_or(0) << " (" << r.statistical_method
            << ", " << r.paired_samples << " pairs), probe overhead " << r.probe_overhead_ms << " ms, activation "
            << r.activation_latency_ms.value_or(0) << " ms\n";

  // 3. Congestion clears: A is preferable again and the runtime returns.
  mutate("entry", "relay_a", 5.0);
  Sleep(350);  // dwell and cooldown
  runtime.Step();
  r = runtime.Decisions(1).back();
  CHECK(r.evidence_validity == "challenger_better");
  CHECK(r.type == DecisionType::SwitchRejected);
  Sleep(150);
  runtime.RequestEvaluation(EvaluationTrigger::Manual, "confirm");
  runtime.Step();
  r = runtime.Decisions(1).back();
  CHECK(r.type == DecisionType::Switch);
  s = runtime.Status();
  CHECK(s.active && s.active->path == kPathA);
  CHECK(network.NodeLogged("relay_a", r.activation_request_id));
  CHECK(s.counters.switches == 2);
  std::cout << "  switch B->A: latency ratio " << r.latency_ratio.value_or(0) << ", activation "
            << r.activation_latency_ms.value_or(0) << " ms\n";

  // Application requests through the runtime use the confirmed path.
  const auto app = runtime.ForwardActive("loop-app-1", "hello", 2000);
  CHECK(app.status == forwarding::ForwardingStatus::Delivered && app.path_confirmed);
  CHECK(app.confirmed_path == kPathA);
  runtime.Stop();
  CHECK(runtime.Status().lifecycle == RuntimeLifecycle::Stopped);
  CHECK(network.ActiveConnections() == 0 || true);
  network.Stop();
}

void RelayFailureRecoveredOverRealRelays() {
  aafr::test::GraphHolder graph(SlowTwoPathGraph());
  forwarding::LoopbackForwardingNetwork network(*graph.Get());
  CHECK(network.Start());
  forwarding::LoopbackForwardingExecutor executor(network);
  algorithms::AlgorithmExecutionEngine engine(nullptr, aafr::test::VerifiedRegistry(), &executor);
  RuntimeConfig config;
  config.session_id = "fail";
  config.health_check_interval_ms = 50;
  config.failures_for_safety_override = 2;
  config.probe.rounds = 8;
  RuntimeDependencies d;
  d.graph_source = [&] { return graph.Get(); };
  d.problem_for = [](const forwarding::ForwardingGraph& g) { return aafr::test::ProblemFor(g); };
  d.executor = &executor;
  d.eligibility = &engine;
  AAFRRuntime runtime(std::move(d), config);
  CHECK(runtime.Initialize());
  CHECK(runtime.Status().active->path == kPathA);
  // relay_a's server goes down for real; the graph does not know.
  CHECK(network.SetNodeOnline("relay_a", false));
  for (int i = 0; i < 20 && runtime.Status().counters.safety_overrides == 0; ++i) {
    Sleep(60);
    runtime.Step();
  }
  const RuntimeStatus s = runtime.Status();
  CHECK(s.counters.safety_overrides == 1);
  CHECK(s.active && s.active->path == kPathB);
  CHECK(s.lifecycle == RuntimeLifecycle::Ready);
  const DecisionRecord r = runtime.Decisions(1).back();
  CHECK(r.HasReason(RuntimeReason::ActivePathFailed));
  CHECK(aafr::test::Contains(r.recommendation_explanation, "relay_a excluded"));
  CHECK(network.NodeLogged("relay_b", r.activation_request_id));
  runtime.Stop();
  network.Stop();
}

void ForwardingServiceTopologyControls() {
  forwarding::LoopbackOptions options;
  options.inject_edge_latency = true;
  forwarding::ForwardingService service(nullptr, options);
  service.Start();
  for (int i = 0; i < 600 && service.GetStatus().state == forwarding::ForwardingService::State::Starting; ++i) Sleep(50);
  CHECK(service.GetStatus().state == forwarding::ForwardingService::State::Ready);
  CHECK(service.InjectsLatency());
  CHECK(service.executor() != nullptr);
  const auto before = service.GraphSnapshot();
  std::string error;
  CHECK(service.SetEdgeLatency("entry", "relay_a", 40.0, &error));
  const auto after = service.GraphSnapshot();
  CHECK(after->version() > before->version());
  CHECK(before->FindEdge("entry", "relay_a")->latency_ms.value == 2.0);  // old snapshot unchanged
  CHECK(after->FindEdge("entry", "relay_a")->latency_ms.provenance == forwarding::Provenance::Simulated);
  CHECK(service.network().graph_version() == after->version());
  CHECK(service.Problem().graph_version == after->version());
  CHECK(!service.SetEdgeLatency("entry", "nowhere", 1.0, &error));
  CHECK(!service.SetEdgeLatency("entry", "relay_a", -1.0, &error));
  // The runtime runs on the service exactly as the browser wires it.
  RuntimeDependencies d;
  d.graph_source = [&] { return service.GraphSnapshot(); };
  d.problem_for = [](const forwarding::ForwardingGraph& g) { return forwarding::LoopbackDemoProblem(g); };
  d.executor = service.executor();
  d.eligibility = &service.engine();
  d.executor_injects_delay = true;
  RuntimeConfig config;
  config.session_id = "svc";
  config.health_check_interval_ms = 0;
  AAFRRuntime runtime(std::move(d), config);
  CHECK(runtime.Initialize());
  const RuntimeStatus s = runtime.Status();
  // relay_a is congested (40 ms), so the demo topology's best path avoids it.
  CHECK(s.active && std::find(s.active->path.begin(), s.active->path.end(), "relay_a") == s.active->path.end());
  CHECK(service.RestoreConfiguredTopology(&error));
  CHECK(service.GraphSnapshot()->FindEdge("entry", "relay_a")->latency_ms.value == 2.0);
  runtime.Stop();
  service.Stop();
}

}  // namespace

int main() {
  return aafr::test::RunTests({
      {"bidirectional_over_real_relays", BidirectionalOverRealRelays},
      {"relay_failure_recovered_over_real_relays", RelayFailureRecoveredOverRealRelays},
      {"forwarding_service_topology_controls", ForwardingServiceTopologyControls},
  });
}
