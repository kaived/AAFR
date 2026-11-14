// Integration tests for the V0.4 update pipeline: V0.3 telemetry (the real
// TelemetryPipeline and the real TShark field parser, fed with fixture lines)
// through NetworkState, constraints and eligibility, including the worker
// thread. No Qt and no live packet capture.

#include <atomic>
#include <chrono>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "network/network_state/network_state_service.h"
#include "network/telemetry/telemetry_pipeline.h"
#include "network/tshark/tshark_fields.h"
#include "tests/support/test_support.h"

using namespace aafr::netstate;
using namespace aafr::test;
using aafr::algorithms::AlgorithmId;
using aafr::algorithms::CapabilityRegistry;
using aafr::algorithms::EligibilityStatus;
using aafr::algorithms::ReasonCode;
using aafr::telemetry::EventType;
using aafr::telemetry::TelemetryEvent;
using aafr::telemetry::TelemetryPipeline;

namespace {

// TShark "-T fields" lines: handshake RTT 30 ms and 50 ms on two connections,
// two retransmissions, one 5.1 ms DNS response, QUIC, and one malformed line.
const char* const kCaptureLines[] = {
    "1731225600.000100\t74\teth:ethertype:ip:tcp\t\t\t\t\t\t\t",
    "1731225600.010200\t66\teth:ethertype:ip:tcp\t0.012500\t\t\t\t\t3\t0.030000",
    "1731225600.020300\t1514\teth:ethertype:ip:tcp\t0.020000\t1\t\t\t\t3\t0.030000",
    "1731225600.030400\t1514\teth:ethertype:ip:tcp\t\t1\t\t\t\t\t",
    "1731225600.040500\t80\teth:ethertype:ip:udp:dns\t\t\tFalse\t\t\t\t",
    "1731225600.045600\t120\teth:ethertype:ip:udp:dns\t\t\tTrue\t0.005100\t\t\t",
    "1731225600.050700\t1250\teth:ethertype:ipv6:udp:quic\t\t\t\t\t0x00000001\t\t",
    "Capturing on 'Wi-Fi'",
    "1731225600.070900\t60\teth:ethertype:ip:tcp\t0.008000\t\t\t\t\t4\t0.050000",
};

void PublishLifecycle(TelemetryPipeline& pipeline, EventType type) {
  TelemetryEvent event;
  event.type = type;
  event.source = aafr::telemetry::Source::TShark;
  event.scope = aafr::telemetry::MeasurementScope::CaptureInterface;
  event.attributes.push_back({"interface", "Wi-Fi"});
  pipeline.Publish(event);
}

// One capture interval through the same parser and aggregator the monitor uses.
void PublishInterval(TelemetryPipeline& pipeline) {
  aafr::tshark::IntervalAggregator aggregator;
  for (const char* line : kCaptureLines) aggregator.AddLine(line);
  for (auto& event : aafr::tshark::ToTelemetryEvents(aggregator.Flush(), 1000.0)) pipeline.Publish(event);
}

int CountType(const TelemetryPipeline& pipeline, EventType type) {
  int count = 0;
  for (const TelemetryEvent& event : pipeline.EventsAfter(0, 100000)) count += event.type == type;
  return count;
}

std::vector<TelemetryEvent> OfType(const TelemetryPipeline& pipeline, EventType type) {
  std::vector<TelemetryEvent> events;
  for (const TelemetryEvent& event : pipeline.EventsAfter(0, 100000)) {
    if (event.type == type) events.push_back(event);
  }
  return events;
}

void TestTelemetryToEligibility() {
  TelemetryPipeline pipeline;
  NetworkStateService service(pipeline);
  CHECK(service.Latest() == nullptr);

  // Before any telemetry: an honest empty state, and nothing eligible.
  auto snapshot = service.ProcessOnce();
  CHECK(snapshot != nullptr && service.Latest() == snapshot);
  CHECK(snapshot->state.validity == StateValidity::NoData);
  CHECK(snapshot->state.telemetry_session_id == pipeline.SessionId());
  CHECK(snapshot->eligibility.size() == 10 && snapshot->capabilities.size() == 10);
  CHECK(snapshot->CountWithStatus(EligibilityStatus::Eligible) == 0);
  CHECK(snapshot->CountWithStatus(EligibilityStatus::NotImplemented) == 10);

  // The first step reports the state, the constraints, the evaluation and
  // one exclusion per algorithm.
  CHECK(CountType(pipeline, EventType::NetworkStateUpdated) == 1);
  CHECK(CountType(pipeline, EventType::ConstraintEvaluated) == 1);
  CHECK(CountType(pipeline, EventType::AlgorithmEligibilityEvaluated) == 1);
  const auto excluded = OfType(pipeline, EventType::AlgorithmExcluded);
  CHECK(excluded.size() == 10);
  std::set<std::string> algorithms;
  for (const TelemetryEvent& event : excluded) {
    CHECK(event.source == aafr::telemetry::Source::StatePipeline);
    CHECK(*event.FindAttribute("status") == "not_implemented");
    CHECK(Contains(*event.FindAttribute("reason_codes"), "IMPLEMENTATION_UNAVAILABLE"));
    CHECK(Contains(*event.FindAttribute("reason_codes"), "TOPOLOGY_UNAVAILABLE"));
    algorithms.insert(*event.FindAttribute("algorithm"));
  }
  CHECK(algorithms.size() == 10);
  CHECK(Contains(*OfType(pipeline, EventType::AlgorithmEligibilityEvaluated).front().FindAttribute("note"),
                 "not a runtime decision"));
  CHECK(std::string(aafr::telemetry::ToString(EventType::AlgorithmExcluded)) ==
        "eligibility.algorithm_excluded");

  // Nothing changed: the next step publishes nothing and re-reads nothing.
  const auto quiet_before = service.Stats();
  service.ProcessOnce();
  const auto quiet_after = service.Stats();
  CHECK(quiet_after.diagnostics_published == quiet_before.diagnostics_published);
  CHECK(quiet_after.steps == quiet_before.steps + 1);

  // Real V0.3 telemetry: a capture with three intervals.
  PublishLifecycle(pipeline, EventType::CaptureStarted);
  for (int i = 0; i < 3; ++i) PublishInterval(pipeline);
  const auto published = pipeline.Stats().published;
  snapshot = service.ProcessOnce();
  const NetworkState& state = snapshot->state;
  CHECK(state.sources.capture == CaptureStatus::Active);
  CHECK(state.sources.capture_interface == "Wi-Fi");
  CHECK(state.capture_session_id == pipeline.SessionId() + "/c1");
  CHECK(state.validity == StateValidity::Degraded);
  const Feature* rtt = state.FindFeature("rtt.handshake.mean_ms");
  CHECK(rtt && rtt->Usable());
  CHECK_NEAR(*rtt->value, 40.0, 1e-9);  // (30 + 50) / 2 in every interval.
  CHECK(rtt->sample_count == 3);
  CHECK_NEAR(*state.FindFeature("rtt.handshake.min_ms")->value, 30.0, 1e-9);
  CHECK_NEAR(*state.FindFeature("rtt.handshake.max_ms")->value, 50.0, 1e-9);
  CHECK_NEAR(*state.FindFeature("tcp.retransmissions")->value, 6.0, 1e-9);
  // 2 retransmissions among 5 TCP packets per interval.
  CHECK_NEAR(*state.FindFeature("tcp.retransmission_ratio")->value, 0.4, 1e-9);
  CHECK_NEAR(*state.FindFeature("traffic.packets")->value, 24.0, 1e-9);
  CHECK_NEAR(*state.FindFeature("quic.packets")->value, 3.0, 1e-9);
  // 4678 frame bytes per 1000 ms interval.
  CHECK_NEAR(*state.FindFeature("throughput.mean_mbps")->value, 4678.0 * 8.0 / 1e6, 1e-9);
  CHECK(state.quic_versions == std::vector<std::string>{"0x00000001"});
  CHECK(state.freshness_ms.has_value() && *state.freshness_ms < 5000.0);
  // Interface traffic stays unattributed; page-load features stay missing.
  CHECK(rtt->attribution == aafr::telemetry::AttributionQuality::Unattributed);
  CHECK(state.FindFeature("nav.duration.median_ms")->quality == FeatureQuality::Missing);

  // NetworkState -> constraints -> eligibility refer to the same state.
  CHECK(snapshot->constraints.network_state_id == state.state_id);
  CHECK(snapshot->constraints.Bool(constraint_id::kStateFresh) == true);
  CHECK(snapshot->constraints.Bool(constraint_id::kTopologyAvailable) == false);
  for (const auto& result : snapshot->eligibility) {
    CHECK(result.network_state_id == state.state_id);
    CHECK(result.status == EligibilityStatus::NotImplemented);
    // With telemetry present the latency requirement is now met.
    CHECK(result.Find(aafr::algorithms::EligibilityCheck::RequiredFeatures)->outcome ==
          aafr::algorithms::CheckOutcome::Passed);
  }
  CHECK(CountType(pipeline, EventType::NetworkStateUpdated) == 2);

  // Incremental: only the new events were read, each exactly once.
  const auto stats = service.Stats();
  CHECK(stats.events_missed == 0);
  CHECK(stats.events_seen >= published);
  const auto seen = stats.events_seen;
  service.ProcessOnce();
  // The step above published diagnostics; those are all that is new.
  CHECK(service.Stats().events_seen - seen == pipeline.Stats().published - published);
  CHECK(snapshot->state.counters.duplicates == 0);

  // An invalid measurement is reported, not absorbed.
  TelemetryEvent bad;
  bad.type = EventType::NetworkMetric;
  bad.source = aafr::telemetry::Source::TShark;
  bad.metric = "net.tcp.handshake_rtt.mean";
  bad.value = -12.0;
  bad.unit = "ms";
  pipeline.Publish(bad);
  pipeline.Publish(bad);
  snapshot = service.ProcessOnce();
  const auto failures_reported = OfType(pipeline, EventType::FeatureValidationFailed);
  CHECK(failures_reported.size() == 1);
  CHECK(*failures_reported.front().FindAttribute("metric") == "net.tcp.handshake_rtt.mean");
  CHECK(*failures_reported.front().FindAttribute("issue") == "negative");
  CHECK(*failures_reported.front().FindAttribute("count") == "2");
  CHECK(snapshot->state.counters.rejected_invalid == 2);
  CHECK_NEAR(*snapshot->state.FindFeature("rtt.handshake.mean_ms")->value, 40.0, 1e-9);

  // Capture interruption reaches the state and the constraints.
  PublishLifecycle(pipeline, EventType::CaptureStopped);
  snapshot = service.ProcessOnce();
  CHECK(snapshot->state.sources.capture == CaptureStatus::Stopped);
  CHECK(snapshot->constraints.Bool(constraint_id::kCaptureInterrupted) == true);
  // A second capture starts a new capture session with no carried-over data.
  PublishLifecycle(pipeline, EventType::CaptureStarted);
  snapshot = service.ProcessOnce();
  CHECK(snapshot->state.capture_session_id == pipeline.SessionId() + "/c2");
  CHECK(!snapshot->state.FindFeature("rtt.handshake.mean_ms")->value.has_value());

  // The pipeline never produced a runtime decision or an active algorithm.
  for (const TelemetryEvent& event : pipeline.EventsAfter(0, 100000)) {
    CHECK(event.FindAttribute("active_algorithm") == nullptr);
    CHECK(event.FindAttribute("selected_algorithm") == nullptr);
  }
}

void TestStaleMeasurementsAreReported() {
  TelemetryPipeline pipeline;
  ServiceConfig config;
  config.builder.quality.network_stale_after_ms = 100;
  config.constraints.max_state_age_ms = 100.0;
  NetworkStateService service(pipeline, config);
  PublishLifecycle(pipeline, EventType::CaptureStarted);
  for (int i = 0; i < 3; ++i) PublishInterval(pipeline);
  auto snapshot = service.ProcessOnce();
  CHECK(snapshot->state.FindFeature("traffic.packets")->quality == FeatureQuality::Valid);
  CHECK(CountType(pipeline, EventType::MeasurementStale) == 0);

  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  snapshot = service.ProcessOnce();
  CHECK(snapshot->state.FindFeature("traffic.packets")->quality == FeatureQuality::Stale);
  CHECK(snapshot->state.quality.stale > 0);
  CHECK(snapshot->constraints.Bool(constraint_id::kStateFresh) == false);
  const auto stale = OfType(pipeline, EventType::MeasurementStale);
  CHECK(stale.size() == 1);
  CHECK(Contains(*stale.front().FindAttribute("features"), "traffic.packets"));
  // Reported once on the transition, not on every step.
  service.ProcessOnce();
  CHECK(CountType(pipeline, EventType::MeasurementStale) == 1);
  for (const auto& result : snapshot->eligibility) {
    CHECK(result.HasReason(ReasonCode::StaleNetworkState) || result.HasReason(ReasonCode::NetworkStateUnusable));
  }
}

void TestVerifiedFixtureEndToEnd() {
  // A registry fixture with one verified algorithm in a graph environment
  // shows the whole path can report "eligible" when the facts support it.
  CapabilityRegistry registry = CapabilityRegistry::BuiltIn();
  aafr::algorithms::ImplementationProfile profile;
  profile.worst_case_runtime_ms = 2.0;
  profile.peak_memory_bytes = 500'000;
  profile.execution_environments = {"in_process_cpp"};
  profile.verification_note = "test fixture";
  CHECK(registry.SetImplementation(AlgorithmId::Dijkstra, profile));
  CHECK(registry.SetAvailability(AlgorithmId::Dijkstra,
                                 aafr::algorithms::ImplementationAvailability::VerifiedExecutable));

  ServiceConfig config;
  ExecutionEnvironment::Topology topology;
  topology.available = true;
  topology.info.representations = {TopologyRepresentation::WeightedDirectedGraph};
  topology.info.node_count = 6;
  topology.info.edge_count = 9;
  topology.info.edge_weights = EdgeWeightInfo{true, false, std::nullopt};
  config.environment.topology = topology;
  config.environment.forwarding_operations = std::set<ForwardingOperation>{ForwardingOperation::SelectNextHop};
  config.environment.problem = ProblemRepresentation::SingleSourceShortestPath;
  config.environment.objective = OptimizationObjective::MinimizeAdditivePathCost;
  config.environment.computation_deadline_ms = 20.0;
  config.environment.memory_budget_bytes = 8'000'000;

  TelemetryPipeline pipeline;
  NetworkStateService service(pipeline, config, registry);
  auto snapshot = service.ProcessOnce();
  // No telemetry yet: the verified algorithm is still not eligible.
  CHECK(snapshot->CountWithStatus(EligibilityStatus::Eligible) == 0);
  CHECK(snapshot->eligibility.front().status == EligibilityStatus::Ineligible);
  CHECK(snapshot->eligibility.front().HasReason(ReasonCode::RequiredFeatureMissing));

  PublishLifecycle(pipeline, EventType::CaptureStarted);
  for (int i = 0; i < 3; ++i) PublishInterval(pipeline);
  snapshot = service.ProcessOnce();
  CHECK(snapshot->CountWithStatus(EligibilityStatus::Eligible) == 1);
  CHECK(snapshot->CountWithStatus(EligibilityStatus::NotImplemented) == 9);
  CHECK(snapshot->eligibility.front().algorithm == AlgorithmId::Dijkstra);
  CHECK(snapshot->eligibility.front().reason_codes ==
        std::vector<ReasonCode>{ReasonCode::EligibilityConfirmed});
  CHECK(snapshot->state.execution.forwarding_topology_available == true);
  // The change is reported once. Dijkstra is no longer excluded; the other
  // nine are re-reported because their reasons changed (latency is now there).
  const auto evaluations = OfType(pipeline, EventType::AlgorithmEligibilityEvaluated);
  CHECK(evaluations.size() == 2);
  CHECK(*evaluations.back().FindAttribute("eligible") == "1");
  const auto excluded = OfType(pipeline, EventType::AlgorithmExcluded);
  CHECK(excluded.size() == 19);
  int dijkstra_exclusions = 0;
  for (const TelemetryEvent& event : excluded) {
    dijkstra_exclusions += *event.FindAttribute("algorithm") == "dijkstra";
  }
  CHECK(dijkstra_exclusions == 1);
}

void TestWorkerThreadAndSnapshots() {
  aafr::telemetry::PipelineConfig pipeline_config;
  pipeline_config.history_capacity = 4000;
  TelemetryPipeline pipeline(pipeline_config);
  ServiceConfig config;
  config.tick = std::chrono::milliseconds(2);
  NetworkStateService service(pipeline, config);
  service.Start();
  CHECK(service.Latest() != nullptr);  // Start() runs the first step itself.
  CHECK(service.Stats().running);
  service.Start();  // A second Start() is harmless.

  std::atomic<bool> done{false};
  std::atomic<int> reads{0};
  std::atomic<int> regressions{0};
  std::vector<std::thread> readers;
  for (int i = 0; i < 4; ++i) {
    readers.emplace_back([&] {
      std::uint64_t last_id = 0;
      while (!done) {
        const auto snapshot = service.Latest();
        if (!snapshot) continue;
        // A published snapshot is complete and never goes backwards.
        if (snapshot->state.state_id < last_id) ++regressions;
        if (snapshot->eligibility.size() != 10 ||
            snapshot->constraints.network_state_id != snapshot->state.state_id ||
            snapshot->state.windows.size() != 3) {
          ++regressions;
        }
        last_id = snapshot->state.state_id;
        ++reads;
      }
    });
  }
  std::thread producer([&] {
    PublishLifecycle(pipeline, EventType::CaptureStarted);
    for (int i = 0; i < 100; ++i) {
      PublishInterval(pipeline);
      if (i % 10 == 0) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  });
  // Steps requested from another thread are serialized with the worker.
  for (int i = 0; i < 50; ++i) service.ProcessOnce();
  producer.join();
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (service.Latest()->state.FindFeature("traffic.packets")->sample_count < 100 &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  done = true;
  for (std::thread& reader : readers) reader.join();
  CHECK(regressions == 0);
  CHECK(reads > 0);
  const auto snapshot = service.Latest();
  CHECK(snapshot->state.FindFeature("traffic.packets")->sample_count == 100);
  CHECK_NEAR(*snapshot->state.FindFeature("traffic.packets")->value, 800.0, 1e-9);
  CHECK(service.Stats().events_missed == 0);
  // A snapshot held by a reader stays valid and unchanged after newer ones.
  const std::uint64_t held_id = snapshot->state.state_id;
  service.ProcessOnce();
  CHECK(snapshot->state.state_id == held_id);
  CHECK(service.Latest()->state.state_id > held_id);

  // Safe shutdown: stop is idempotent and the worker really stops.
  service.Stop();
  CHECK(!service.Stats().running);
  const auto steps = service.Stats().steps;
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  CHECK(service.Stats().steps == steps);
  service.Stop();
  // The service still works synchronously, and can be restarted.
  CHECK(service.ProcessOnce() != nullptr);
  service.Start();
  CHECK(service.Stats().running);
  // Destroying a running service joins its worker (end of scope).
}

void TestBoundsUnderLoad() {
  // A small telemetry history that overflows between steps: the service
  // counts what it missed and carries on.
  aafr::telemetry::PipelineConfig pipeline_config;
  pipeline_config.history_capacity = 64;
  TelemetryPipeline pipeline(pipeline_config);
  ServiceConfig config;
  config.builder.max_samples_per_series = 32;
  NetworkStateService service(pipeline, config);
  service.ProcessOnce();
  PublishLifecycle(pipeline, EventType::CaptureStarted);
  for (int i = 0; i < 200; ++i) PublishInterval(pipeline);
  auto snapshot = service.ProcessOnce();
  auto stats = service.Stats();
  CHECK(stats.events_missed > 0);
  // The capture start was evicted; metrics still imply an active capture.
  CHECK(snapshot->state.sources.capture == CaptureStatus::Active);
  CHECK(stats.stored_samples <= 32u * static_cast<std::size_t>(Series::kCount));

  for (int round = 0; round < 50; ++round) {
    for (int i = 0; i < 3; ++i) PublishInterval(pipeline);
    service.ProcessOnce();
  }
  stats = service.Stats();
  CHECK(stats.stored_samples <= 32u * static_cast<std::size_t>(Series::kCount));
  // Diagnostics do not accumulate: far fewer than one per step.
  CHECK(stats.diagnostics_published < 40);
  CHECK(stats.last_step_us >= 0 && stats.max_step_us >= stats.last_step_us);
  std::cout << "  step time: last " << stats.last_step_us << " us, max " << stats.max_step_us
            << " us over " << stats.steps << " steps\n";
}

}  // namespace

int main() {
  return RunTests({
      {"telemetry_to_eligibility", TestTelemetryToEligibility},
      {"stale_measurements_reported", TestStaleMeasurementsAreReported},
      {"verified_fixture_end_to_end", TestVerifiedFixtureEndToEnd},
      {"worker_thread_and_snapshots", TestWorkerThreadAndSnapshots},
      {"bounds_under_load", TestBoundsUnderLoad},
  });
}
