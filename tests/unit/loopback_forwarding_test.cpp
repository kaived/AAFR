// V0.5: real forwarding over loopback TCP relays. Uses 127.0.0.1 only; no
// Internet access.

#include <chrono>
#include <thread>

#include "algorithms/execution/implementation_verifier.h"
#include "routing/forwarding/loopback_forwarding.h"
#include "routing/topology/scenario_catalog.h"
#include "tests/support/test_support.h"

using namespace aafr::forwarding;

namespace {

ForwardingRequest Request(const std::string& id, Path path, double timeout_ms = 3000.0) {
  ForwardingRequest request;
  request.request_id = id;
  request.path = std::move(path);
  request.payload = "hello " + id;
  request.timeout_ms = timeout_ms;
  return request;
}

void TestSelectedRelayHandlesRequest() {
  const ForwardingGraph graph = LoopbackDemoGraph();
  LoopbackForwardingNetwork network(graph);
  std::string error;
  CHECK(network.Start(&error));
  CHECK(network.PortOf("entry") != 0 && network.PortOf("relay_a") != network.PortOf("relay_b"));
  LoopbackForwardingExecutor executor(network);
  CHECK(executor.Ready() && executor.Capability().real_network && executor.Capability().loopback_only);

  // The path chosen decides which relay handles the request.
  const auto via_a = executor.Execute(Request("req-a", {"entry", "relay_a", "dest"}), {});
  CHECK(via_a.status == ForwardingStatus::Delivered);
  CHECK(via_a.path_confirmed);
  CHECK((via_a.confirmed_path == Path{"entry", "relay_a", "dest"}));
  CHECK(via_a.response_status == "OK");
  CHECK(aafr::test::Contains(via_a.response_body, "served-by=dest"));
  CHECK(aafr::test::Contains(via_a.response_body, "hello req-a"));
  CHECK(via_a.provenance == Provenance::Measured && via_a.end_to_end_ms > 0.0);
  CHECK(network.HandledCount("relay_a") == 1 && network.HandledCount("relay_b") == 0);
  CHECK(network.NodeLogged("relay_a", "req-a") && !network.NodeLogged("relay_b", "req-a"));

  const auto via_b = executor.Execute(Request("req-b", {"entry", "relay_b", "dest"}), {});
  CHECK(via_b.path_confirmed);
  CHECK(network.HandledCount("relay_a") == 1 && network.HandledCount("relay_b") == 1);

  const auto both = executor.Execute(Request("req-ab", {"entry", "relay_a", "relay_b", "dest"}), {});
  CHECK(both.path_confirmed && both.confirmed_path.size() == 4);
  CHECK(network.HandledCount("dest") == 3 && network.HandledCount("entry") == 3);
  network.Stop();
}

void TestInvalidPaths() {
  const ForwardingGraph graph = LoopbackDemoGraph();
  LoopbackForwardingNetwork network(graph);
  CHECK(network.Start());
  LoopbackForwardingExecutor executor(network);
  // No configured edge entry -> dest: the entry refuses to act as an open proxy.
  const auto direct = executor.Execute(Request("bad-1", {"entry", "dest"}), {});
  CHECK(direct.status == ForwardingStatus::Failed);
  CHECK(direct.response_status == "NOT_A_NEIGHBOUR");
  CHECK(!direct.path_confirmed);
  CHECK((direct.confirmed_path == Path{"entry"}));
  // Loops are refused before anything is sent.
  const auto loop = executor.Execute(Request("bad-2", {"entry", "relay_a", "entry", "dest"}), {});
  CHECK(loop.status == ForwardingStatus::RequestRejected);
  // Unknown node: nothing is listening for it.
  const auto unknown = executor.Execute(Request("bad-3", {"entry", "relay_x", "dest"}), {});
  CHECK(unknown.status == ForwardingStatus::Failed && unknown.response_status == "NOT_A_NEIGHBOUR");
  // Destination does not forward further.
  const auto beyond = executor.Execute(Request("bad-4", {"entry", "relay_a", "dest", "relay_b"}), {});
  CHECK(beyond.status == ForwardingStatus::Failed && beyond.response_status == "BAD_ROUTE");
  // Malformed ids and payloads are refused.
  CHECK(executor.Execute(Request("bad id", {"entry", "relay_a", "dest"}), {}).status == ForwardingStatus::RequestRejected);
  auto newline = Request("bad-5", {"entry", "relay_a", "dest"});
  newline.payload = "a\nAAFR1 REQ";
  CHECK(executor.Execute(newline, {}).status == ForwardingStatus::RequestRejected);
  // A disabled link is a real refusal at the relay.
  CHECK(network.SetEdgeEnabled("relay_a", "dest", false));
  const auto down = executor.Execute(Request("bad-6", {"entry", "relay_a", "dest"}), {});
  CHECK(down.status == ForwardingStatus::RelayUnavailable && down.response_status == "LINK_DOWN");
  network.Stop();
}

void TestRelayUnavailable() {
  const ForwardingGraph graph = LoopbackDemoGraph();
  LoopbackForwardingNetwork network(graph);
  CHECK(network.Start());
  LoopbackForwardingExecutor executor(network);
  CHECK(network.SetNodeOnline("relay_b", false));
  CHECK(network.PortOf("relay_b") == 0);
  const auto result = executor.Execute(Request("down-1", {"entry", "relay_b", "dest"}), {});
  CHECK(result.status == ForwardingStatus::RelayUnavailable);
  CHECK(result.failed_at == "relay_b");
  CHECK(!result.path_confirmed);
  // The other relay still works, and relay_b comes back on a new port.
  CHECK(executor.Execute(Request("down-2", {"entry", "relay_a", "dest"}), {}).path_confirmed);
  CHECK(network.SetNodeOnline("relay_b", true));
  CHECK(executor.Execute(Request("down-3", {"entry", "relay_b", "dest"}), {}).path_confirmed);
  // Entry offline: nothing to connect to.
  network.SetNodeOnline("entry", false);
  CHECK(executor.Execute(Request("down-4", {"entry", "relay_b", "dest"}), {}).status == ForwardingStatus::RelayUnavailable);
  network.Stop();
}

void TestTimeoutAndCancellation() {
  ForwardingGraph graph = LoopbackDemoGraph();
  graph.SetEdgeLatency("relay_a", "dest", Measurement::Of(400.0, Provenance::Configured));
  LoopbackOptions options;
  options.inject_edge_latency = true;
  LoopbackForwardingNetwork network(graph, options);
  CHECK(network.Start());
  LoopbackForwardingExecutor executor(network);

  const auto start = std::chrono::steady_clock::now();
  const auto slow = executor.Execute(Request("slow-1", {"entry", "relay_a", "dest"}, 150.0), {});
  const double waited = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
  CHECK(slow.status == ForwardingStatus::Timeout);
  CHECK(waited < 1000.0);

  std::atomic<bool> cancel{false};
  std::thread canceller([&cancel] {
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    cancel = true;
  });
  const auto cancelled = executor.Execute(Request("slow-2", {"entry", "relay_a", "dest"}, 5000.0), {&cancel, "", ""});
  canceller.join();
  CHECK(cancelled.status == ForwardingStatus::Cancelled);
  CHECK(cancelled.end_to_end_ms < 1000.0);

  // With enough time the injected delay is observed and labelled.
  const auto patient = executor.Execute(Request("slow-3", {"entry", "relay_a", "dest"}, 3000.0), {});
  CHECK(patient.path_confirmed);
  CHECK(patient.end_to_end_ms >= 300.0);
  CHECK(aafr::test::Contains(patient.evidence, "injected"));
  network.Stop();
}

void TestShutdownAndRestart() {
  const ForwardingGraph graph = LoopbackDemoGraph();
  LoopbackForwardingNetwork network(graph);
  CHECK(network.Start());
  LoopbackForwardingExecutor executor(network);
  // Concurrent requests share the relays safely.
  std::vector<std::thread> clients;
  std::atomic<int> confirmed{0};
  for (int i = 0; i < 8; ++i) {
    clients.emplace_back([&executor, &confirmed, i] {
      const Path path = i % 2 ? Path{"entry", "relay_a", "dest"} : Path{"entry", "relay_b", "dest"};
      if (executor.Execute(Request("par-" + std::to_string(i), path), {}).path_confirmed) ++confirmed;
    });
  }
  for (auto& client : clients) client.join();
  CHECK(confirmed == 8);
  network.Stop();
  CHECK(!network.Running());
  CHECK(network.PortOf("entry") == 0);
  CHECK(network.ActiveConnections() == 0);
  const auto refused = executor.Execute(Request("after-stop", {"entry", "relay_a", "dest"}), {});
  CHECK(refused.status == ForwardingStatus::RequestRejected && refused.response_status == "NOT_STARTED");
  CHECK(network.Start());
  CHECK(executor.Execute(Request("restarted", {"entry", "relay_a", "dest"}), {}).path_confirmed);
  // Graph version pinning: a request built for another graph version is refused.
  auto stale = Request("stale", {"entry", "relay_a", "dest"});
  stale.graph_version = network.graph_version() + 5;
  CHECK(executor.Execute(stale, {}).response_status == "STALE_GRAPH");
}  // Destructor stops the network again.

void TestVerificationWithRealForwarding() {
  const ForwardingGraph graph = LoopbackDemoGraph();
  LoopbackForwardingNetwork network(graph);
  CHECK(network.Start());
  LoopbackForwardingExecutor executor(network);
  const auto report = aafr::algorithms::VerifyImplementations(&executor);
  CHECK(report.forwarding_real_network);
  CHECK(report.CountVerifiedExecutable() == 10);
  const auto registry = aafr::algorithms::ApplyVerification(aafr::algorithms::CapabilityRegistry::BuiltIn(), report);
  CHECK(registry.ExecutableAlgorithms().size() == 10);
  for (const auto& capability : registry.All()) {
    CHECK(aafr::test::Contains(capability.implementation.verification_note, "real loopback traffic"));
    CHECK(capability.implementation.execution_environments.size() == 2);
  }
  // Each algorithm's request really crossed the relays.
  CHECK(network.HandledCount("dest") == 10);
}

}  // namespace

int main() {
  return aafr::test::RunTests({
      {"selected_relay_handles_request", TestSelectedRelayHandlesRequest},
      {"invalid_paths", TestInvalidPaths},
      {"relay_unavailable", TestRelayUnavailable},
      {"timeout_and_cancellation", TestTimeoutAndCancellation},
      {"shutdown_and_restart", TestShutdownAndRestart},
      {"verification_with_real_forwarding", TestVerificationWithRealForwarding},
  });
}
