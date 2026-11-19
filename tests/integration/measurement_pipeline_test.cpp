// V0.7.4 end-to-end measurement pipeline over real loopback relays:
//
//   proxy client (browser-style requests) -> AAFR gateway -> relays -> origin
//     -> ForwardingMeasurementCollector -> ForwardingObservationStore
//     -> NetworkStateService (NetworkState::forwarding)
//     -> ModelFeatureAdapter -> compatibility -> AIRecommendationProvider
//     -> AAFRRuntime (eligibility, arbitration, gates, confirmed activation)
//
// Experiments (V0.7.4 section 17): A normal forwarding, B a hidden relay
// impairment, C its removal, D a disabled measurement source, E the model
// adapter on measured snapshots, F deterministic adaptation while the model
// abstains. Plus measurement overhead. Everything is local (127.0.0.1).
// The Qt WebEngine version of A/E runs in browser_forwarding_test
// (realPageLoadsReachNetworkState).

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>
#include <memory>
#include <thread>

#include "network/network_state/forwarding_observations.h"
#include "network/network_state/network_state_service.h"
#include "network/telemetry/telemetry_pipeline.h"
#include "routing/forwarding/loopback_forwarding.h"
#include "routing/gateway/browser_forwarding_gateway.h"
#include "routing/gateway/forwarding_measurement_collector.h"
#include "routing/gateway/runtime_gateway_binding.h"
#include "runtime/adaptive/aafr_runtime.h"
#include "runtime/model/ai_recommendation_provider.h"
#include "runtime/model/model_compatibility.h"
#include "tests/support/http_test_server.h"
#include "tests/support/model_test_support.h"
#include "tests/support/runtime_test_support.h"
#include "tests/support/test_support.h"

using namespace aafr;
using namespace aafr::gateway;
using aafr::test::Contains;
using aafr::test::kPathA;
using aafr::test::kPathB;
namespace ns = aafr::netstate;
namespace net = aafr::forwarding::net;
namespace mdl = aafr::model;

namespace {

void Sleep(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

template <typename F>
bool WaitFor(F condition, int timeout_ms = 5000) {
  const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (std::chrono::steady_clock::now() < until) {
    if (condition()) return true;
    Sleep(10);
  }
  return condition();
}

forwarding::ForwardingGraph Graph() {
  aafr::test::GraphHolder holder(aafr::test::TwoPathGraph());
  holder.SetLatency("entry", "relay_a", 3.0);
  holder.SetLatency("relay_a", "dest", 3.0);
  holder.SetLatency("entry", "relay_b", 15.0);
  holder.SetLatency("relay_b", "dest", 15.0);
  return *holder.Get();
}

// A short window keeps the experiments fast; the rules are the production ones.
ns::ForwardingWindowPolicy ShortWindow() {
  ns::ForwardingWindowPolicy p;
  p.window_ms = 2'500;
  p.latency_stale_after_ms = 2'500;
  return p;
}

struct Bed {
  aafr::test::GraphHolder graph{Graph()};
  telemetry::TelemetryPipeline pipeline;
  std::unique_ptr<ns::NetworkStateService> service;
  std::shared_ptr<ns::ForwardingObservationStore> store = std::make_shared<ns::ForwardingObservationStore>(ShortWindow());
  std::unique_ptr<forwarding::LoopbackForwardingNetwork> network;
  std::unique_ptr<forwarding::LoopbackForwardingExecutor> executor;
  std::unique_ptr<algorithms::AlgorithmExecutionEngine> engine;
  aafr::test::FakeModel model;
  std::unique_ptr<mdl::AIRecommendationProvider> provider;
  std::unique_ptr<runtime::AAFRRuntime> runtime;
  std::unique_ptr<ForwardingMeasurementCollector> collector;
  aafr::test::HttpTestServer origin;
  std::unique_ptr<AAFRBrowserForwardingGateway> gateway;
  bool ok{false};

  Bed() {
    service = std::make_unique<ns::NetworkStateService>(pipeline);
    forwarding::LoopbackOptions options;
    options.inject_edge_latency = true;
    network = std::make_unique<forwarding::LoopbackForwardingNetwork>(*graph.Get(), options);
    network->SetEgressPolicy(forwarding::EgressPolicy::LocalTesting());
    executor = std::make_unique<forwarding::LoopbackForwardingExecutor>(*network);
    engine = std::make_unique<algorithms::AlgorithmExecutionEngine>(nullptr, aafr::test::VerifiedRegistry(), executor.get());
    model.Always(aafr::test::FakeModel::Label("PSO"));
    mdl::AIProviderConfig pc;
    pc.mode = runtime::RecommendationMode::AiPreferred;
    pc.reuse_identical_features = false;
    pc.compatibility.profile = mdl::LoadTrainingProfile(aafr::test::Define(AAFR_TEST_TRAINING_PROFILE)).profile;
    provider = std::make_unique<mdl::AIRecommendationProvider>(&model, nullptr, pc);
    runtime::RuntimeConfig config;
    config.session_id = "meas";
    config.scheduler.min_interval_ms = 100;
    config.stability.min_dwell_ms = 200;
    config.stability.cooldown_ms = 300;
    config.stability.sustain_evaluations = 2;
    config.probe.rounds = 16;
    config.probe.probe_timeout_ms = 2000;
    config.health_check_interval_ms = 0;
    config.review_interval_ms = 0;
    runtime::RuntimeDependencies d;
    d.graph_source = [this] { return graph.Get(); };
    d.problem_for = [](const forwarding::ForwardingGraph& g) { return aafr::test::ProblemFor(g); };
    d.state_source = [this] { return service->Latest(); };
    d.executor = executor.get();
    d.eligibility = engine.get();
    d.executor_injects_delay = true;
    d.provider = provider.get();
    runtime = std::make_unique<runtime::AAFRRuntime>(std::move(d), config);
    GatewayConfig gc;
    gc.session_id = "meas";
    gc.egress = forwarding::EgressPolicy::LocalTesting();
    gc.configuration_wait_ms = 200;
    gc.establish_timeout_ms = 3000;
    gc.restrict_to_process = true;
    gateway = std::make_unique<AAFRBrowserForwardingGateway>(gc);
    CollectorSources cs;
    cs.graph_source = [this] { return graph.Get(); };
    cs.runtime = runtime.get();
    cs.gateway = gateway.get();
    cs.executor_injects_delay = true;
    collector = std::make_unique<ForwardingMeasurementCollector>(store, cs);
    service->SetForwardingObservations(store, [this] { collector->Poll(); });
    std::string error;
    ok = network->Start(&error) && origin.Start() && gateway->Start(&error);
    if (!ok) std::cerr << "bed did not start: " << error << "\n";
    RuntimeBindingSources s;
    s.runtime = runtime.get();
    s.graph_source = [this] { return graph.Get(); };
    s.problem_for = [](const forwarding::ForwardingGraph& g) { return aafr::test::ProblemFor(g); };
    s.executor = executor.get();
    s.network = network.get();
    s.collector = collector.get();
    gateway->SetBindings(MakeRuntimeBindings(s));
  }
  ~Bed() {
    service->SetForwardingObservations(nullptr);
    gateway->Detach();
    gateway->Stop();
    runtime->Stop();
    origin.Stop();
    network->Stop();
  }
  // n plain-HTTP requests and n CONNECT tunnels; waits until the collector saw them.
  void Traffic(int n) {
    const auto before = collector->Stats().connections;
    for (int i = 0; i < n; ++i) {
      CHECK(aafr::test::ProxyGet(gateway->port(), origin.Base() + "/page").status == 200);
      net::Connection tunnel;
      if (aafr::test::ProxyConnect(gateway->port(), "127.0.0.1:" + std::to_string(origin.port()), &tunnel) == 200) {
        CHECK(aafr::test::RequestOverTunnel(tunnel, "127.0.0.1", "/page", true).status == 200);
      } else {
        CHECK(false);
      }
    }
    CHECK(WaitFor([&] { return collector->Stats().connections >= before + 2 * static_cast<std::uint64_t>(n); }));
  }
  std::shared_ptr<const ns::StateSnapshot> Step() { return service->ProcessOnce(); }
  static const ns::Feature* F(const std::shared_ptr<const ns::StateSnapshot>& s, const std::string& id) {
    return s->state.FindFeature(id);
  }
  // Heartbeats for a full window so counts become valid.
  void ObserveFullWindow() {
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ShortWindow().window_ms + 200);
    while (std::chrono::steady_clock::now() < until) {
      Step();
      Sleep(250);
    }
  }
};

double P50(const std::shared_ptr<const ns::StateSnapshot>& s) {
  const ns::Feature* f = Bed::F(s, "fwd.path.establish_ms.p50");
  return f && f->value ? *f->value : -1.0;
}

void ExperimentsAThroughF() {
  Bed bed;
  if (!bed.ok) {
    CHECK(false);
    return;
  }
  CHECK(bed.runtime->Initialize());
  const auto active = bed.runtime->ConfirmedConfiguration();
  CHECK(active && active->path == kPathA);
  bed.ObserveFullWindow();

  // ---- A: normal forwarding ----
  bed.Traffic(6);
  auto s = bed.Step();
  const auto set = s->state.forwarding;
  CHECK(set && set->config_generation == active->config_version);
  CHECK(set->path == "entry>relay_a>dest" && set->impairment == ns::Impairment::EmulatedEdgeDelay);
  const ns::Feature* attempts = Bed::F(s, "fwd.path.attempts");
  CHECK(attempts && attempts->quality == ns::FeatureQuality::Valid && *attempts->value >= 12.0);
  CHECK(Bed::F(s, "fwd.path.success_ratio")->quality == ns::FeatureQuality::Valid);
  CHECK_NEAR(*Bed::F(s, "fwd.path.success_ratio")->value, 1.0, 1e-12);
  const double base_p50 = P50(s);
  CHECK(Bed::F(s, "fwd.path.establish_ms.p50")->quality == ns::FeatureQuality::Valid);
  // Path A's relays sleep 3 + 3 ms: the measured set-up includes them, yet
  // is a measurement (it is never exactly the configured sum).
  CHECK(base_p50 >= 6.0);
  CHECK(Bed::F(s, "fwd.path.first_byte_ms.mean")->quality == ns::FeatureQuality::Valid);
  // Only the edges the streams crossed.
  CHECK(set->Find("fwd.edge.entry->relay_a.hop_ms") != nullptr);
  CHECK(set->Find("fwd.edge.relay_a->dest.hop_ms") != nullptr);
  CHECK(set->Find("fwd.edge.entry->relay_b.hop_ms") == nullptr);
  CHECK(set->Find("fwd.edge.relay_b->dest.hop_ms") == nullptr);
  CHECK(set->counters.rejected.empty());
  const auto* base_hop_entry = set->Find("fwd.edge.entry->relay_a.hop_ms");
  const double base_entry_hop = base_hop_entry && base_hop_entry->feature.value ? *base_hop_entry->feature.value : 0.0;
  const auto* base_hop_dest = set->Find("fwd.edge.relay_a->dest.hop_ms");
  const double base_dest_hop = base_hop_dest && base_hop_dest->feature.value ? *base_hop_dest->feature.value : 0.0;
  std::cout << "  A: entry->relay_a hop " << base_entry_hop << " ms; p50 set-up " << base_p50 << " ms over " << *attempts->value << " attempts, edges " << set->edges.size()
            << "\n";

  // ---- B: hidden impairment on relay_a -> dest (+120 ms), unknown to the graph ----
  // Large enough to dominate set-up noise (Windows ~15 ms sleep quantization on
  // three relay hops plus origin connect): with +40 ms the 12-sample p50 shift
  // varied 13-40 ms between runs while the edge hop shift stayed ~30 ms.
  const auto graph_version = bed.graph.Get()->version();
  CHECK(bed.network->SetHiddenEdgeDelay("relay_a", "dest", 120.0));
  Sleep(ShortWindow().window_ms + 100);  // let the healthy samples leave the window
  bed.Traffic(6);
  s = bed.Step();
  const double degraded_p50 = P50(s);
  CHECK(degraded_p50 >= base_p50 + 60.0);
  CHECK(bed.graph.Get()->version() == graph_version);  // the graph was not told
  const auto* hop = s->state.forwarding->Find("fwd.edge.relay_a->dest.hop_ms");
  CHECK(hop && hop->feature.value && *hop->feature.value >= 110.0);  // localised to the impaired edge
  const auto* hop_a = s->state.forwarding->Find("fwd.edge.entry->relay_a.hop_ms");
  // Localisation: relays stamp opened_ns when the tunnel request arrives, so
  // the upstream edge cannot contain relay_a's delay. Its value may move by
  // Windows timer quantization (~15.6 ms quanta) and CPU contention, not by
  // the impairment: its change must stay under a third of the impaired edge's.
  CHECK(hop_a && hop_a->feature.value && hop &&
        (*hop_a->feature.value - base_entry_hop) < (*hop->feature.value - base_dest_hop) / 3.0);
  CHECK(hop_a && hop_a->feature.value && hop && *hop_a->feature.value < *hop->feature.value - 80.0);
  std::cout << "  B: entry->relay_a hop " << (hop_a && hop_a->feature.value ? *hop_a->feature.value : -1.0) << " ms\n";
  const double degraded_hop = hop && hop->feature.value ? *hop->feature.value : 0.0;
  std::cout << "  B: p50 set-up " << degraded_p50 << " ms; relay_a->dest hop " << *hop->feature.value << " ms\n";

  // ---- C: recovery ----
  CHECK(bed.network->SetHiddenEdgeDelay("relay_a", "dest", 0.0));
  Sleep(ShortWindow().window_ms + 100);
  bed.Traffic(6);
  s = bed.Step();
  const double recovered_p50 = P50(s);
  CHECK(recovered_p50 >= 0.0 && recovered_p50 < degraded_p50);
  // The impaired edge itself is the robust signal (whole-path set-up also
  // carries origin-connect and scheduler noise).
  const auto* hop_back = s->state.forwarding->Find("fwd.edge.relay_a->dest.hop_ms");
  CHECK(hop_back && hop_back->feature.value && *hop_back->feature.value < degraded_hop - 80.0);
  std::cout << "  C: p50 set-up " << recovered_p50 << " ms; relay_a->dest hop "
            << (hop_back && hop_back->feature.value ? *hop_back->feature.value : -1.0) << " ms\n";

  // ---- D: a measurement source disabled ----
  bed.store->SetSourceEnabled(ns::ObservationSource::Topology, false);
  s = bed.Step();
  CHECK(Bed::F(s, "fwd.topology.link_failure_events")->quality == ns::FeatureQuality::Missing);
  mdl::ModelFeatureAdapter adapter;
  {
    const auto g = bed.graph.Get();
    const auto v = adapter.Build(*g, aafr::test::ProblemFor(*g), &s->state, nullptr);
    const mdl::ModelFeatureValue* lf = nullptr;
    for (const auto& f : v.features)
      if (f.name == "link_failure_events") lf = &f;
    CHECK(lf && !lf->value && Contains(lf->issue, "disabled"));
  }
  bed.store->SetSourceEnabled(ns::ObservationSource::Topology, true);
  bed.store->SetSourceEnabled(ns::ObservationSource::Gateway, false);
  s = bed.Step();
  CHECK(Bed::F(s, "fwd.path.establish_ms.p50")->quality == ns::FeatureQuality::Missing);
  CHECK(!Bed::F(s, "fwd.path.establish_ms.p50")->value);
  bed.store->SetSourceEnabled(ns::ObservationSource::Gateway, true);

  // ---- E: the model adapter on a measured snapshot ----
  bed.ObserveFullWindow();
  bed.Traffic(3);
  s = bed.Step();
  {
    const auto g = bed.graph.Get();
    const auto t0 = std::chrono::steady_clock::now();
    const auto v = adapter.Build(*g, aafr::test::ProblemFor(*g), &s->state, nullptr);
    const double adapter_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    CHECK(v.CountStatus(mdl::FeatureStatus::Valid) == 15 && v.derived.size() == 3);
    CHECK(!v.Complete() && v.missing.size() == 20);
    CHECK(!v.estimates.empty());
    const auto profile = mdl::LoadTrainingProfile(aafr::test::Define(AAFR_TEST_TRAINING_PROFILE)).profile;
    const auto compat = mdl::AssessCompatibility(v, profile.get());
    CHECK(compat.status == mdl::CompatibilityStatus::ClearlyOutOfDistribution);
    std::cout << "  E: " << v.Summary() << "; " << compat.Summary() << "; adapter " << adapter_ms << " ms, compat "
              << compat.assess_ms << " ms\n";
  }

  // ---- F: deterministic adaptation while the model abstains ----
  const auto switches_before = bed.runtime->Status().counters.switches;
  bed.graph.Mutate([](forwarding::ForwardingGraph& g) { g.SetNodeAvailable("relay_a", false); });
  CHECK(bed.network->SetNodeOnline("relay_a", false));
  {
    std::string error;
    CHECK(bed.network->SyncWith(*bed.graph.Get(), &error));
  }
  bed.Step();
  Sleep(150);
  bed.runtime->Step();
  for (int i = 0; i < 6 && bed.runtime->Status().counters.switches == switches_before; ++i) {
    Sleep(200);
    bed.runtime->RequestEvaluation(runtime::EvaluationTrigger::Manual, "confirm");
    bed.runtime->Step();
  }
  const auto now_active = bed.runtime->ConfirmedConfiguration();
  CHECK(now_active && now_active->path == kPathB);
  const auto decision = bed.runtime->Decisions(1).back();
  CHECK(decision.model && decision.model->inference_status == "abstained");
  CHECK(decision.model->source == runtime::RecommendationSource::DeterministicFallback);
  CHECK(bed.model.calls == 0);  // never asked with measured inputs
  bed.Traffic(3);
  s = bed.Step();
  CHECK(s->state.forwarding->config_generation == now_active->config_version);
  CHECK(s->state.forwarding->path == "entry>relay_b>dest");
  CHECK(s->state.forwarding->Find("fwd.edge.entry->relay_b.hop_ms") != nullptr);
  CHECK(s->state.forwarding->Find("fwd.edge.entry->relay_a.hop_ms") == nullptr);  // old path not current
  bed.ObserveFullWindow();  // counts need the full window after the topology source restarted
  // The failure and the reroute are observed... and leave the window again.
  // (Checked on a store snapshot taken right after the change below.)
  bed.graph.Mutate([](forwarding::ForwardingGraph& g) { g.SetNodeAvailable("relay_a", true); });
  bed.network->SetNodeOnline("relay_a", true);
  {
    std::string error;
    CHECK(bed.network->SyncWith(*bed.graph.Get(), &error));
  }
  s = bed.Step();
  CHECK(*Bed::F(s, "fwd.topology.link_recovery_events")->value >= 2.0);  // entry->relay_a and relay_a->dest
  CHECK(Bed::F(s, "fwd.runtime.reroute_count")->quality == ns::FeatureQuality::Valid);
  std::cout << "  F: switched to " << forwarding::PathText(now_active->path) << " via deterministic fallback ("
            << decision.model->error_code << ")\n";

  // ---- Overhead ----
  const auto c = bed.store->Counters();
  const auto cs = bed.collector->Stats();
  const auto ss = bed.service->Stats();
  CHECK(c.max_ingest_us < 5'000.0);
  CHECK(c.stored <= c.capacity && c.dropped_capacity == 0);
  std::cout << "  overhead: ingest last " << c.last_ingest_us << " us max " << c.max_ingest_us << " us; snapshot last "
            << c.last_snapshot_us << " us max " << c.max_snapshot_us << " us; per-connection record max "
            << cs.max_record_us << " us; poll max " << cs.max_poll_us << " us; state step max " << ss.max_step_us
            << " us; stored " << c.stored << "/" << c.capacity << " (~"
            << (c.stored * (sizeof(ns::ForwardingObservation) + 160)) / 1024 << " KiB), accepted " << c.accepted
            << ", rejected " << cs.rejected << ", dropped " << c.dropped_capacity << "\n";
}

}  // namespace

int main() {
  return aafr::test::RunTests({
      {"experiments_a_through_f", ExperimentsAThroughF},
  });
}
