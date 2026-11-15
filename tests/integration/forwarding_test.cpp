#include "routing/forwarding/forwarding_executor.h"
#include "tests/support/test_support.h"

#include <QCoreApplication>

using namespace aafr::forwarding;

void GraphAndPaths() {
  auto p = LoopbackProblem();
  CHECK(ValidateProblem(p).empty());
  ForwardingPath path{p.graph.id(), p.graph.version(), {"entry", "relay-a", "destination"}};
  CHECK(EvaluatePath(p, path).score == 4);
  p.constraints.max_latency_ms = 3;
  CHECK(!EvaluatePath(p, path).score);
  p.constraints.max_latency_ms.reset();
  auto missing = path;
  missing.nodes = {"entry", "destination"};
  CHECK(!EvaluatePath(p, missing).score);
  p.graph.SetEdgeAvailable("entry", "relay-a", false);
  CHECK(!EvaluatePath(p, path).score);
  path.graph_version = p.graph.version();
  CHECK(!EvaluatePath(p, path).score);
  p.graph.SetEdgeLatency("entry", "relay-b", Measurement::Of(-1, Provenance::Configured));
  CHECK(!ValidateProblem(p).empty());
}

void Loopback() {
  const auto p = LoopbackProblem();
  std::atomic<bool> cancelled{false};
  ForwardingRequest req{"test", {p.graph.id(), p.graph.version(), {"entry", "relay-a", "destination"}}};
  for (const char* relay : {"relay-a", "relay-b"}) {
    req.path.nodes[1] = relay;
    const auto result = LoopbackExecutor{}.Execute(p, req, cancelled);
    if (result.status != ForwardingStatus::Success) std::cerr << result.reason << '\n';
    CHECK(result.status == ForwardingStatus::Success);
    CHECK(result.provenance == Provenance::Measured);
    CHECK(result.confirmed == req.path.nodes);
    CHECK(result.response_status == 200);
    CHECK(!result.response.empty());
  }
  const auto unavailable = LoopbackExecutor({"relay-b"}).Execute(p, req, cancelled);
  CHECK(unavailable.status == ForwardingStatus::Unavailable);
  CHECK(unavailable.unavailable_node == "relay-b");
  req.path.nodes[1] = "unknown";
  CHECK(LoopbackExecutor{}.Execute(p, req, cancelled).status == ForwardingStatus::InvalidPath);
  req.path.nodes[1] = "relay-b";
  req.timeout = std::chrono::milliseconds(1);
  CHECK(SimulatedExecutor(42).Execute(p, req, cancelled).status == ForwardingStatus::Timeout);
  cancelled = true;
  CHECK(LoopbackExecutor{}.Execute(p, req, cancelled).status == ForwardingStatus::Cancelled);
}

int main(int argc, char** argv) {
  QCoreApplication app(argc, argv);
  return aafr::test::RunTests({{"graph and paths", GraphAndPaths}, {"loopback", Loopback}});
}
