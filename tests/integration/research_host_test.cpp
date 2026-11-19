// Phase 5B.2: ResearchHost, the in-process owner of the research runtime
// adapter (research/research_host): worker-thread loading, explicit requests
// only, coalescing, skipped and refused requests, an unavailable model, clean
// stop and destruction, the read-only observability snapshot, and no change
// to a real AAFRRuntime or its executor. Needs the XGBoost library for the
// available cases (fails without it). No network access.

#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <iostream>
#include <mutex>

#include "research/research_host.h"
#include "tests/support/runtime_test_support.h"
#include "tests/support/test_support.h"

using namespace aafr;
using namespace aafr::research;
using runtime::AAFRRuntime;
using runtime::ManualClock;
using runtime::RuntimeConfig;
using runtime::RuntimeDependencies;

namespace {

ResearchRuntimeAdapter::Options Options(const std::string& library = AAFR_TEST_XGBOOST_LIBRARY) {
  ResearchRuntimeAdapter::Options o;
  o.model_dir = AAFR_TEST_XGBOOST_MODEL_DIR;
  o.xgboost_library = library;
  return o;
}

struct RuntimeFixture {
  aafr::test::GraphHolder graph{aafr::test::TwoPathGraph()};
  aafr::test::TestExecutor executor{graph};
  ManualClock clock;
  algorithms::AlgorithmExecutionEngine engine{nullptr, aafr::test::VerifiedRegistry(), &executor};
  std::unique_ptr<AAFRRuntime> runtime;
  RuntimeFixture() {
    RuntimeConfig c;
    c.session_id = "h";
    c.health_check_interval_ms = 0;
    c.activation_attempts = 1;
    RuntimeDependencies d;
    d.graph_source = [this] { return graph.Get(); };
    d.problem_for = [](const forwarding::ForwardingGraph& g) { return aafr::test::ProblemFor(g); };
    d.executor = &executor;
    d.eligibility = &engine;
    d.clock = &clock;
    runtime = std::make_unique<AAFRRuntime>(std::move(d), c);
  }
  // The browser's snapshot source, against this runtime.
  ResearchHost::SnapshotSource Source() {
    auto startup = std::make_shared<std::optional<StartupDelays>>();
    return [this, startup](std::string* why) -> std::optional<TelemetrySnapshot> {
      const auto g = graph.Get();
      if (!*startup) *startup = CaptureStartupDelays(*g);
      const RuntimeView view = ReadRuntime(*runtime, clock.NowNs());
      if (!view.has_active) {
        if (why) *why = "the runtime has no confirmed configuration yet";
        return std::nullopt;
      }
      return SnapshotFromRuntimeGraph(*g, **startup, view, "test runtime two-path graph");
    };
  }
};

void LifecycleAndExplicitRequestsOnly() {
  RuntimeFixture f;
  CHECK(f.runtime->Initialize());
  ResearchHost host(Options(), f.Source());
  CHECK(host.ResearchSnapshot(8).host_state == ResearchHostState::NotStarted);
  host.RequestEvaluation("before start");  // Refused: not running.
  host.Start();
  host.WaitIdle();
  ResearchObservabilitySnapshot s = host.ResearchSnapshot(8);
  CHECK(s.host_state == ResearchHostState::Running && s.adapter_present);
  CHECK(s.adapter.state == ResearchRuntimeState::Idle && s.recent.empty());
  CHECK(!ResearchObservabilitySnapshot::kActuationEnabled);
  CHECK(s.requests.size() == 1 && aafr::test::Contains(s.requests[0].outcome, "refused"));
  // Nothing evaluates on its own.
  f.clock.AdvanceMs(60'000);
  CHECK(host.ResearchSnapshot(8).recent.empty());
  host.RequestEvaluation("test");
  host.WaitIdle();
  s = host.ResearchSnapshot(8);
  CHECK(s.recent.size() == 1 && s.adapter.state == ResearchRuntimeState::Decided);
  CHECK(s.recent[0].verdict == ArbitrationVerdict::Keep && !s.recent[0].applied());  // Utilization absent.
  CHECK(s.recent[0].telemetry_origin == DataOrigin::Controlled && !s.recent[0].probe_origin);
  CHECK(aafr::test::Contains(s.recent[0].record.telemetry_description, "utilization not measured"));
  CHECK(aafr::test::Contains(s.requests.back().outcome, "evaluated: recommendation #1 keep (not applied)"));
  host.Stop();
  s = host.ResearchSnapshot(8);
  CHECK(s.host_state == ResearchHostState::Stopped);
  host.RequestEvaluation("after stop");
  CHECK(aafr::test::Contains(host.ResearchSnapshot(8).requests.back().outcome, "refused"));
  host.Stop();  // Idempotent.
}

void SkippedWithoutAConfiguration() {
  RuntimeFixture f;  // Not initialized: no active configuration.
  ResearchHost host(Options(), f.Source());
  host.Start();
  host.RequestEvaluation("no configuration");
  host.WaitIdle();
  const auto s = host.ResearchSnapshot(8);
  CHECK(s.requests_skipped == 1 && s.recent.empty());
  CHECK(aafr::test::Contains(s.requests.back().outcome, "skipped: the runtime has no confirmed configuration"));
  CHECK(s.adapter.counters.requests == 0);  // The adapter was not called.
}

void RequestsAreCoalesced() {
  RuntimeFixture f;
  CHECK(f.runtime->Initialize());
  // A source that blocks until released keeps the worker busy.
  std::mutex m;
  std::condition_variable cv;
  bool released = false;
  std::atomic<int> calls{0};
  auto inner = f.Source();
  ResearchHost host(Options(), [&](std::string* why) {
    ++calls;
    std::unique_lock<std::mutex> lock(m);
    cv.wait(lock, [&] { return released; });
    return inner(why);
  });
  host.Start();
  host.RequestEvaluation("first");  // Taken by the worker, which blocks.
  while (calls.load() == 0) std::this_thread::yield();
  host.RequestEvaluation("second");  // Pending.
  host.RequestEvaluation("third");   // Coalesced.
  host.RequestEvaluation("fourth");  // Coalesced.
  CHECK(host.ResearchSnapshot(8).evaluation_pending);
  {
    std::lock_guard<std::mutex> lock(m);
    released = true;
  }
  cv.notify_all();
  host.WaitIdle();
  const auto s = host.ResearchSnapshot(8);
  CHECK(calls.load() == 2 && s.recent.size() == 2);
  CHECK(s.requests_received == 4 && s.requests_coalesced == 2);
}

void UnavailableModelDoesNotFail() {
  RuntimeFixture f;
  CHECK(f.runtime->Initialize());
  ResearchHost host(Options("no_such_xgboost.dll"), f.Source());
  host.Start();
  host.WaitIdle();
  auto s = host.ResearchSnapshot(8);
  CHECK(s.host_state == ResearchHostState::Running && s.adapter.state == ResearchRuntimeState::Unavailable);
  host.RequestEvaluation("test");
  host.WaitIdle();
  s = host.ResearchSnapshot(8);
  CHECK(s.recent.empty() && aafr::test::Contains(s.requests.back().outcome, "refused"));
  CHECK(s.adapter.counters.refused_unavailable == 1);
}

void DestructionWithoutStopIsClean() {
  RuntimeFixture f;
  CHECK(f.runtime->Initialize());
  {
    ResearchHost host(Options(), f.Source());
    host.Start();
    host.RequestEvaluation("pending at destruction");
  }  // Destructor stops, joins and releases the adapter.
  {
    ResearchHost never_started(Options(), f.Source());
  }
  CHECK(true);
}

void NoForwardingChangeAndDeterministicRecords() {
  RuntimeFixture f;
  CHECK(f.runtime->Initialize());
  const auto before = f.runtime->Status();
  const int requests_before = f.executor.requests.load();
  const std::size_t decisions_before = f.runtime->Decisions(100000).size();
  ResearchHost a(Options(), f.Source()), b(Options(), f.Source());
  a.Start();
  b.Start();
  for (int i = 0; i < 3; ++i) {
    a.RequestEvaluation("a");
    a.WaitIdle();
  }
  b.RequestEvaluation("b");
  b.WaitIdle();
  const auto sa = a.ResearchSnapshot(8);
  const auto sb = b.ResearchSnapshot(8);
  CHECK(sa.recent.size() == 3 && sb.recent.size() == 1);
  CHECK(sa.recent[0].record_json == sb.recent[0].record_json);
  CHECK(sa.recent[2].record_json == sb.recent[0].record_json);
  const auto after = f.runtime->Status();
  CHECK(after.active->config_version == before.active->config_version && after.active->path == before.active->path);
  CHECK(after.counters.evaluations == before.counters.evaluations && after.counters.switches == before.counters.switches);
  CHECK(f.executor.requests.load() == requests_before);
  CHECK(f.runtime->Decisions(100000).size() == decisions_before);
}

}  // namespace

int main() {
  const std::string library = AAFR_TEST_XGBOOST_LIBRARY;
  if (library.empty() || !std::filesystem::exists(library)) {
    std::cout << "FAIL: XGBoost library not available (AAFR_XGBOOST_LIBRARY='" << library << "')\n";
    return 1;
  }
  return aafr::test::RunTests({
      {"lifecycle_and_explicit_requests_only", LifecycleAndExplicitRequestsOnly},
      {"skipped_without_a_configuration", SkippedWithoutAConfiguration},
      {"requests_are_coalesced", RequestsAreCoalesced},
      {"unavailable_model_does_not_fail", UnavailableModelDoesNotFail},
      {"destruction_without_stop_is_clean", DestructionWithoutStopIsClean},
      {"no_forwarding_change_and_deterministic_records", NoForwardingChangeAndDeterministicRecords},
  });
}
