// V0.7.4 forwarding observations: ingestion validation, provenance,
// windowed statistics, freshness and sample rules, generation isolation,
// source liveness, bounds, concurrency, and NetworkState integration.
// Plain C++17; every timestamp is explicit (no sleeping).

#include "network/network_state/forwarding_observations.h"

#include <atomic>
#include <cmath>
#include <thread>

#include "network/network_state/network_state_service.h"
#include "network/telemetry/telemetry_pipeline.h"
#include "tests/support/test_support.h"

using namespace aafr;
using namespace aafr::netstate;
using aafr::test::Contains;

namespace {

constexpr std::int64_t kMs = 1'000'000;
// An arbitrary steady-clock origin far from zero.
constexpr std::int64_t kT0 = 1'000'000 * kMs;

int next_id = 0;

ForwardingObservation Establish(std::int64_t at_ms, double ms, std::uint64_t generation = 1) {
  ForwardingObservation o;
  o.kind = ObservationKind::TunnelEstablished;
  o.provenance.measurement_id = "m" + std::to_string(++next_id);
  o.provenance.source_component = "gateway";
  o.provenance.source_type = ObservationSource::Gateway;
  o.provenance.monotonic_ns = kT0 + at_ms * kMs;
  o.provenance.interval_start_ns = o.provenance.monotonic_ns - static_cast<std::int64_t>(ms * kMs);
  o.provenance.unit = "ms";
  o.provenance.connection_id = "c" + std::to_string(next_id);
  o.provenance.config_generation = generation;
  o.provenance.graph_id = "g";
  o.provenance.method = "test";
  o.value = ms;
  return o;
}

ForwardingObservation Failure(std::int64_t at_ms, const std::string& outcome, std::uint64_t generation = 1) {
  ForwardingObservation o = Establish(at_ms, 0.0, generation);
  o.kind = ObservationKind::TunnelFailed;
  o.provenance.unit = "event";
  o.provenance.interval_start_ns = o.provenance.monotonic_ns;
  o.value.reset();
  o.outcome = outcome;
  return o;
}

ForwardingObservation Event(ObservationKind kind, ObservationSource source, std::int64_t at_ms,
                            const std::string& outcome = "") {
  ForwardingObservation o;
  o.kind = kind;
  o.provenance.measurement_id = "e" + std::to_string(++next_id);
  o.provenance.source_type = source;
  o.provenance.monotonic_ns = kT0 + at_ms * kMs;
  o.provenance.interval_start_ns = o.provenance.monotonic_ns;
  o.provenance.unit = "event";
  o.outcome = outcome;
  return o;
}

std::int64_t At(std::int64_t ms) { return kT0 + ms * kMs; }

// A store whose gateway has been alive since t=0 and current generation 1.
void Ready(ForwardingObservationStore& store) {
  store.SetCurrentConfiguration("g", 3, 1, "entry>relay_a>dest", Impairment::EmulatedEdgeDelay);
  store.Heartbeat(ObservationSource::Gateway, At(0));
}

const Feature& F(const ForwardingFeatureSet& set, const std::string& id) {
  static Feature none;
  const ForwardingFeature* f = set.Find(id);
  return f ? f->feature : none;
}

void ValidObservationAccepted() {
  ForwardingObservationStore store;
  Ready(store);
  CHECK(store.Ingest(Establish(100, 12.5), At(100)) == ObservationValidity::Accepted);
  const auto c = store.Counters();
  CHECK(c.accepted == 1 && c.stored == 1 && c.rejected.empty());
  CHECK(c.last_ingest_us >= 0.0);
}

void InvalidObservationsRejected() {
  ForwardingObservationStore store;
  Ready(store);
  auto nan = Establish(10, 1.0);
  nan.value = std::nan("");
  CHECK(store.Ingest(nan, At(10)) == ObservationValidity::NonFinite);
  auto inf = Establish(10, 1.0);
  inf.value = INFINITY;
  CHECK(store.Ingest(inf, At(10)) == ObservationValidity::NonFinite);
  auto neg = Establish(10, 1.0);
  neg.value = -3.0;
  CHECK(store.Ingest(neg, At(10)) == ObservationValidity::NegativeDuration);
  auto unit = Establish(10, 1.0);
  unit.provenance.unit = "s";
  CHECK(store.Ingest(unit, At(10)) == ObservationValidity::WrongUnit);
  auto missing = Establish(10, 1.0);
  missing.value.reset();
  CHECK(store.Ingest(missing, At(10)) == ObservationValidity::MissingValue);
  auto event_value = Failure(10, "timeout");
  event_value.value = 5.0;
  CHECK(store.Ingest(event_value, At(10)) == ObservationValidity::UnexpectedValue);
  auto no_id = Establish(10, 1.0);
  no_id.provenance.measurement_id.clear();
  CHECK(store.Ingest(no_id, At(10)) == ObservationValidity::MissingId);
  const auto ok = Establish(10, 1.0);
  CHECK(store.Ingest(ok, At(10)) == ObservationValidity::Accepted);
  CHECK(store.Ingest(ok, At(11)) == ObservationValidity::Duplicate);
  const auto c = store.Counters();
  CHECK(c.accepted == 1);
  CHECK(c.rejected.at("non_finite") == 2 && c.rejected.at("negative_duration") == 1);
  CHECK(c.rejected.at("wrong_unit") == 1 && c.rejected.at("duplicate") == 1);
}

void TimestampOrderingRules() {
  ForwardingObservationStore store;
  Ready(store);
  auto reversed = Establish(100, 5.0);
  reversed.provenance.interval_start_ns = reversed.provenance.monotonic_ns + kMs;
  CHECK(store.Ingest(reversed, At(100)) == ObservationValidity::IntervalReversed);
  auto longer = Establish(100, 5.0);
  longer.value = 50.0;  // 50 ms inside a 5 ms interval
  CHECK(store.Ingest(longer, At(100)) == ObservationValidity::DurationExceedsInterval);
  CHECK(store.Ingest(Establish(5'000, 5.0), At(100)) == ObservationValidity::FutureTimestamp);
  CHECK(store.Ingest(Establish(100, 5.0), At(100 + 200'000)) == ObservationValidity::TooOld);
  // An observation that arrives late but inside the retention is kept and
  // counted by its own timestamp.
  CHECK(store.Ingest(Establish(1'000, 5.0), At(9'000)) == ObservationValidity::Accepted);
}

void WindowedStatisticsAndFormulas() {
  ForwardingObservationStore store;
  Ready(store);
  const double values[] = {10, 12, 11, 30, 13, 12, 11, 10, 14, 12};
  for (int i = 0; i < 10; ++i) CHECK(store.Ingest(Establish(1'000 + i * 100, values[i]), At(2'000)) == ObservationValidity::Accepted);
  store.Heartbeat(ObservationSource::Gateway, At(10'000));
  const auto set = store.Snapshot(At(10'000));  // newest sample 8.1 s old (< 15 s stale limit)
  const Feature& mean = F(set, "fwd.path.establish_ms.mean");
  CHECK(mean.quality == FeatureQuality::Valid && mean.sample_count == 10);
  CHECK_NEAR(*mean.value, 13.5, 1e-9);
  CHECK(mean.kind == ValueKind::Measured && mean.unit == Unit::Milliseconds);
  CHECK_NEAR(*F(set, "fwd.path.establish_ms.min").value, 10.0, 1e-9);
  CHECK_NEAR(*F(set, "fwd.path.establish_ms.max").value, 30.0, 1e-9);
  CHECK_NEAR(*F(set, "fwd.path.establish_ms.p50").value, 12.0, 1e-9);
  CHECK(F(set, "fwd.path.establish_ms.p50").quality == FeatureQuality::Valid);
  CHECK(F(set, "fwd.path.establish_ms.p90").quality == FeatureQuality::Valid);
  CHECK_NEAR(*F(set, "fwd.path.establish_ms.p90").value, Percentile({10, 12, 11, 30, 13, 12, 11, 10, 14, 12}, 90), 1e-9);
  // Quantiles that need more samples than the window has: value kept, not Valid.
  CHECK(F(set, "fwd.path.establish_ms.p95").quality == FeatureQuality::InsufficientSamples);
  CHECK(F(set, "fwd.path.establish_ms.p99").quality == FeatureQuality::InsufficientSamples);
  // Population std (numpy.std).
  double var = 0;
  for (double v : values) var += (v - 13.5) * (v - 13.5);
  CHECK_NEAR(*F(set, "fwd.path.establish_ms.std").value, std::sqrt(var / 10), 1e-9);
  // Jitter: mean |d_i - d_(i-1)| in completion order.
  double j = 0;
  for (int i = 1; i < 10; ++i) j += std::fabs(values[i] - values[i - 1]);
  CHECK_NEAR(*F(set, "fwd.path.establish_jitter_ms").value, j / 9, 1e-9);
  CHECK(F(set, "fwd.path.establish_jitter_ms").kind == ValueKind::Derived);
  // Provenance of the aggregate.
  const ForwardingFeature* p = set.Find("fwd.path.establish_ms.mean");
  CHECK(p->scope == ForwardingScope::ActivePath && p->config_generation == 1 && p->graph_version == 3);
  CHECK(p->impairment == Impairment::EmulatedEdgeDelay);
  CHECK(Contains(p->association, "generation 1") && Contains(p->association, "entry>relay_a>dest"));
  CHECK(p->window_ms == 30'000 && p->min_samples == 3 && !p->formula.empty());
  CHECK(set.counters.last_snapshot_us >= 0.0);
}

void PercentileMatchesNumpyLinear() {
  CHECK_NEAR(Percentile({1, 2, 3, 4}, 50), 2.5, 1e-12);
  CHECK_NEAR(Percentile({1, 2, 3, 4}, 90), 3.7, 1e-12);
  CHECK_NEAR(Percentile({5}, 99), 5.0, 1e-12);
  CHECK(!MeanAbsoluteSuccessiveDifference({1.0}));
  CHECK_NEAR(*MeanAbsoluteSuccessiveDifference({1.0, 4.0, 2.0}), 2.5, 1e-12);
}

void EmptyWindowsAndZeroDenominators() {
  ForwardingObservationStore store;
  Ready(store);
  store.Heartbeat(ObservationSource::Gateway, At(31'000));
  const auto set = store.Snapshot(At(31'000));
  // Nothing observed: latency statistics are missing, never 0.
  CHECK(F(set, "fwd.path.establish_ms.mean").quality == FeatureQuality::Missing);
  CHECK(!F(set, "fwd.path.establish_ms.mean").value);
  // Ratios with a zero denominator are missing, never 0 or 1.
  CHECK(F(set, "fwd.path.failure_ratio").quality == FeatureQuality::Missing);
  CHECK(!F(set, "fwd.path.failure_ratio").value);
  CHECK(Contains(F(set, "fwd.path.failure_ratio").quality_reason, "zero denominator"));
  // A count over a fully observed window: zero is a real observation.
  CHECK(F(set, "fwd.path.attempts").quality == FeatureQuality::Valid && *F(set, "fwd.path.attempts").value == 0.0);
  CHECK(F(set, "fwd.gateway.connections").quality == FeatureQuality::Valid);
  // Sources that never reported: missing, not zero.
  CHECK(F(set, "fwd.topology.link_failure_events").quality == FeatureQuality::Missing);
  CHECK(F(set, "fwd.runtime.reroute_count").quality == FeatureQuality::Missing);
}

void RatiosNeedDenominatorAndExcludeDestinationFailures() {
  ForwardingObservationStore store;
  Ready(store);
  store.Ingest(Establish(100, 5), At(100));
  store.Ingest(Failure(200, "relay_failure"), At(200));
  store.Ingest(Failure(300, "destination_failure"), At(300));  // not a path attempt
  store.Heartbeat(ObservationSource::Gateway, At(30'050));
  auto set = store.Snapshot(At(30'050));
  CHECK(*F(set, "fwd.path.attempts").value == 2.0);
  CHECK(F(set, "fwd.path.failure_ratio").quality == FeatureQuality::InsufficientSamples);
  CHECK_NEAR(*F(set, "fwd.path.failure_ratio").value, 0.5, 1e-12);
  store.Ingest(Establish(400, 5), At(400));
  store.Ingest(Failure(500, "timeout"), At(500));
  store.Ingest(Establish(600, 5), At(600));
  set = store.Snapshot(At(30'050));
  CHECK(F(set, "fwd.path.failure_ratio").quality == FeatureQuality::Valid);
  CHECK_NEAR(*F(set, "fwd.path.failure_ratio").value, 2.0 / 5.0, 1e-12);
  CHECK_NEAR(*F(set, "fwd.path.timeout_ratio").value, 1.0 / 5.0, 1e-12);
  CHECK_NEAR(*F(set, "fwd.path.success_ratio").value, 3.0 / 5.0, 1e-12);
  CHECK(*F(set, "fwd.gateway.connections").value == 6.0);  // every set-up, destination failures included
}

void CountsNeedAFullyObservedWindow() {
  ForwardingObservationStore store;
  store.SetCurrentConfiguration("g", 1, 1, "p", Impairment::None);
  store.Heartbeat(ObservationSource::Topology, At(0));
  store.Heartbeat(ObservationSource::Runtime, At(0));
  store.Ingest(Event(ObservationKind::LinkTransition, ObservationSource::Topology, 5'000, "failure"), At(5'000));
  store.Ingest(Event(ObservationKind::LinkTransition, ObservationSource::Topology, 6'000, "recovery"), At(6'000));
  auto set = store.Snapshot(At(10'000));
  CHECK(F(set, "fwd.topology.link_failure_events").quality == FeatureQuality::InsufficientSamples);
  CHECK(Contains(F(set, "fwd.topology.link_failure_events").quality_reason, "of 30000 ms"));
  store.Heartbeat(ObservationSource::Topology, At(31'000));
  store.Heartbeat(ObservationSource::Runtime, At(31'000));
  set = store.Snapshot(At(31'000));
  CHECK(F(set, "fwd.topology.link_failure_events").quality == FeatureQuality::Valid);
  CHECK(*F(set, "fwd.topology.link_failure_events").value == 1.0);
  CHECK(*F(set, "fwd.topology.link_recovery_events").value == 1.0);
  CHECK(*F(set, "fwd.runtime.reroute_count").value == 0.0);
  // Events leave the window: counts return to 0.
  store.Heartbeat(ObservationSource::Topology, At(40'000));
  store.Heartbeat(ObservationSource::Runtime, At(40'000));
  set = store.Snapshot(At(40'000));
  CHECK(*F(set, "fwd.topology.link_failure_events").value == 0.0);
}

void StaleMeasurementsAndSilentSources() {
  ForwardingObservationStore store;
  Ready(store);
  for (int i = 0; i < 5; ++i) store.Ingest(Establish(1'000 + i, 8.0), At(1'100));
  store.Heartbeat(ObservationSource::Gateway, At(20'000));
  auto set = store.Snapshot(At(20'000));
  // Newest sample 19 s old > 15 s: value kept, quality Stale.
  CHECK(F(set, "fwd.path.establish_ms.mean").quality == FeatureQuality::Stale);
  CHECK(F(set, "fwd.path.establish_ms.mean").value.has_value());
  CHECK(*F(set, "fwd.path.establish_ms.mean").age_ms > 15'000.0);
  // The gateway stops reporting entirely (no heartbeat for > window).
  set = store.Snapshot(At(60'000));
  CHECK(F(set, "fwd.gateway.connections").quality == FeatureQuality::Stale);
  CHECK(Contains(F(set, "fwd.gateway.connections").quality_reason, "silent"));
  // Gauge freshness: 5 s.
  store.SetActiveConnections(3, At(60'000));
  store.Heartbeat(ObservationSource::Gateway, At(60'000));
  set = store.Snapshot(At(62'000));
  CHECK(F(set, "fwd.gateway.active_connections").quality == FeatureQuality::Valid && *F(set, "fwd.gateway.active_connections").value == 3.0);
  set = store.Snapshot(At(68'000));
  CHECK(F(set, "fwd.gateway.active_connections").quality == FeatureQuality::Stale);
}

void DisabledSourceBecomesMissing() {
  ForwardingObservationStore store;
  Ready(store);
  for (int i = 0; i < 5; ++i) store.Ingest(Establish(1'000 + i, 8.0), At(1'100));
  store.SetSourceEnabled(ObservationSource::Gateway, false);
  CHECK(store.Ingest(Establish(2'000, 8.0), At(2'000)) == ObservationValidity::SourceDisabled);
  auto set = store.Snapshot(At(3'000));
  CHECK(F(set, "fwd.path.establish_ms.mean").quality == FeatureQuality::Missing);
  CHECK(Contains(F(set, "fwd.path.establish_ms.mean").quality_reason, "disabled"));
  // Re-enabled: a new observation interval starts, so counts are not valid
  // until a full window was observed again.
  store.SetSourceEnabled(ObservationSource::Gateway, true);
  store.Heartbeat(ObservationSource::Gateway, At(4'000));
  set = store.Snapshot(At(5'000));
  CHECK(F(set, "fwd.gateway.connections").quality == FeatureQuality::InsufficientSamples);
}

void GenerationIsolationAndRetiredObservations() {
  ForwardingObservationStore store;
  Ready(store);
  for (int i = 0; i < 5; ++i) store.Ingest(Establish(1'000 + i, 5.0, 1), At(1'100));
  // Switch to generation 2 on another path.
  store.SetCurrentConfiguration("g", 4, 2, "entry>relay_b>dest", Impairment::EmulatedEdgeDelay);
  // A late record of generation 1 is accepted but counted as retired.
  CHECK(store.Ingest(Establish(1'500, 6.0, 1), At(1'600)) == ObservationValidity::Accepted);
  CHECK(store.Counters().retired == 1);
  auto set = store.Snapshot(At(2'000));
  // Generation-1 measurements never describe generation 2.
  CHECK(F(set, "fwd.path.establish_ms.mean").quality == FeatureQuality::Missing);
  CHECK(set.config_generation == 2 && set.path == "entry>relay_b>dest");
  for (int i = 0; i < 3; ++i) store.Ingest(Establish(2'000 + i, 40.0, 2), At(2'100));
  set = store.Snapshot(At(2'200));
  CHECK_NEAR(*F(set, "fwd.path.establish_ms.mean").value, 40.0, 1e-9);
  // An older generation cannot replace the current one (stale update).
  store.SetCurrentConfiguration("g", 3, 1, "entry>relay_a>dest", Impairment::None);
  CHECK(store.Snapshot(At(2'300)).config_generation == 2);
}

void EdgesOnlyWhereStreamsCrossed() {
  ForwardingObservationStore store;
  Ready(store);
  for (int i = 0; i < 3; ++i) {
    ForwardingObservation hop = Establish(500 + i, 2.0);
    hop.kind = ObservationKind::RelayHop;
    hop.provenance.source_type = ObservationSource::Relay;
    hop.provenance.edge_from = "entry";
    hop.provenance.edge_to = "relay_a";
    store.Ingest(hop, At(600));
  }
  const auto set = store.Snapshot(At(1'000));
  CHECK(set.edges.size() == 1);
  CHECK(set.Find("fwd.edge.entry->relay_a.hop_ms") != nullptr);
  CHECK(set.Find("fwd.edge.entry->relay_a.hop_ms")->feature.quality == FeatureQuality::Valid);
  // Candidate edges no stream crossed have no feature at all.
  CHECK(set.Find("fwd.edge.entry->relay_b.hop_ms") == nullptr);
  CHECK(set.Find("fwd.edge.relay_a->dest.hop_ms") == nullptr);
}

void CapacityBoundAndDrops() {
  ForwardingWindowPolicy policy;
  policy.capacity = 16;
  ForwardingObservationStore store(policy);
  store.SetCurrentConfiguration("g", 1, 1, "p", Impairment::None);
  for (int i = 0; i < 40; ++i) store.Ingest(Establish(100 + i, 1.0), At(200));
  const auto c = store.Counters();
  CHECK(c.stored == 16 && c.accepted == 40 && c.dropped_capacity == 24 && c.capacity == 16);
}

void ConcurrentIngestAndImmutableSnapshots() {
  ForwardingObservationStore store;
  Ready(store);
  std::atomic<bool> stop{false};
  std::atomic<int> accepted{0};
  std::vector<std::thread> writers;
  for (int w = 0; w < 4; ++w) {
    writers.emplace_back([&, w] {
      for (int i = 0; i < 500; ++i) {
        ForwardingObservation o;
        o.kind = ObservationKind::TunnelEstablished;
        o.provenance.measurement_id = "w" + std::to_string(w) + "-" + std::to_string(i);
        o.provenance.source_type = ObservationSource::Gateway;
        o.provenance.monotonic_ns = At(1'000 + i);
        o.provenance.interval_start_ns = o.provenance.monotonic_ns - kMs;
        o.provenance.unit = "ms";
        o.provenance.config_generation = 1;
        o.value = 1.0;
        if (store.Ingest(o, At(2'000)) == ObservationValidity::Accepted) ++accepted;
      }
    });
  }
  std::vector<ForwardingFeatureSet> snaps;
  std::thread reader([&] {
    while (!stop) snaps.push_back(store.Snapshot(At(2'000)));
  });
  for (auto& t : writers) t.join();
  stop = true;
  reader.join();
  CHECK(accepted == 2000);
  // Snapshots are versioned, increase strictly, and each is internally consistent.
  for (std::size_t i = 1; i < snaps.size(); ++i) CHECK(snaps[i].version > snaps[i - 1].version);
  for (const auto& s : snaps) {
    const Feature& n = F(s, "fwd.path.attempts");
    const Feature& m = F(s, "fwd.path.establish_ms.mean");
    if (n.value && m.sample_count) CHECK(static_cast<std::uint64_t>(*n.value) == m.sample_count);
  }
  const auto final_set = store.Snapshot(At(2'000));
  CHECK(F(final_set, "fwd.path.establish_ms.mean").sample_count == 2000);
}

void NetworkStateCarriesForwardingSnapshot() {
  telemetry::TelemetryPipeline pipeline;
  NetworkStateService service(pipeline);
  auto first = service.ProcessOnce();
  CHECK(first->state.schema_version == 2);
  CHECK(!first->state.forwarding);  // nothing attached: no fabricated features
  CHECK(first->state.FindFeature("fwd.path.attempts") == nullptr);
  auto store = std::make_shared<ForwardingObservationStore>();
  int polls = 0;
  service.SetForwardingObservations(store, [&] { ++polls; });
  auto s1 = service.ProcessOnce();
  auto s2 = service.ProcessOnce();
  CHECK(polls == 2);
  CHECK(s1->state.forwarding && s2->state.forwarding);
  CHECK(s2->state.forwarding->version > s1->state.forwarding->version);
  // Existing snapshots keep their own forwarding view (immutable).
  CHECK(s1->state.forwarding.get() != s2->state.forwarding.get());
  // fwd.* ids resolve through NetworkState::FindFeature for existing consumers.
  const Feature* f = s2->state.FindFeature("fwd.gateway.connections");
  CHECK(f && f->quality == FeatureQuality::Missing);  // gateway never seen
  CHECK(s2->state.FindFeature("rtt.handshake.median_ms") != nullptr);  // V0.4 features unchanged
  service.SetForwardingObservations(nullptr);
  CHECK(!service.ProcessOnce()->state.forwarding);
}

}  // namespace

int main() {
  return aafr::test::RunTests({
      {"valid_observation_accepted", ValidObservationAccepted},
      {"invalid_observations_rejected", InvalidObservationsRejected},
      {"timestamp_ordering_rules", TimestampOrderingRules},
      {"windowed_statistics_and_formulas", WindowedStatisticsAndFormulas},
      {"percentile_matches_numpy_linear", PercentileMatchesNumpyLinear},
      {"empty_windows_and_zero_denominators", EmptyWindowsAndZeroDenominators},
      {"ratios_need_denominator", RatiosNeedDenominatorAndExcludeDestinationFailures},
      {"counts_need_full_window", CountsNeedAFullyObservedWindow},
      {"stale_measurements_and_silent_sources", StaleMeasurementsAndSilentSources},
      {"disabled_source_becomes_missing", DisabledSourceBecomesMissing},
      {"generation_isolation_and_retired", GenerationIsolationAndRetiredObservations},
      {"edges_only_where_streams_crossed", EdgesOnlyWhereStreamsCrossed},
      {"capacity_bound_and_drops", CapacityBoundAndDrops},
      {"concurrent_ingest_immutable_snapshots", ConcurrentIngestAndImmutableSnapshots},
      {"network_state_carries_forwarding", NetworkStateCarriesForwardingSnapshot},
  });
}
