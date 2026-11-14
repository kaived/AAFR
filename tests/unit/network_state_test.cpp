// Deterministic tests for V0.4 NetworkState: rolling statistics, unit
// normalization, validation, quality classification, the state builder and
// network constraints. No Qt, no clock and no packet capture: events carry
// fixed timestamps.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <set>
#include <string>
#include <variant>
#include <vector>

#include "network/network_state/feature_extractor.h"
#include "network/network_state/feature_normalizer.h"
#include "network/network_state/feature_validator.h"
#include "network/network_state/measurement_quality.h"
#include "network/network_state/network_constraints.h"
#include "network/network_state/network_state.h"
#include "network/network_state/network_state_builder.h"
#include "tests/support/test_support.h"

using namespace aafr::netstate;
using namespace aafr::test;
using aafr::telemetry::EventType;
using aafr::telemetry::WallClock;

namespace {

const Feature& Get(const NetworkState& state, const std::string& id, const std::string& window = "") {
  static const Feature absent;
  const WindowSnapshot* snapshot = window.empty() ? state.PrimaryWindow() : state.FindWindow(window);
  const Feature* feature = snapshot ? snapshot->Find(id) : nullptr;
  CHECK(feature != nullptr);
  return feature ? *feature : absent;
}

NetworkState BuildAt(NetworkStateBuilder& builder, std::int64_t t_ms) {
  return builder.Build(Ns(t_ms), WallClock::time_point{});
}

void TestStatistics() {
  CHECK(!Median({}).has_value());
  CHECK_NEAR(*Median({5.0}), 5.0, 1e-12);
  CHECK_NEAR(*Median({30.0, 10.0, 20.0}), 20.0, 1e-12);
  CHECK_NEAR(*Median({40.0, 10.0, 20.0, 30.0}), 25.0, 1e-12);

  RollingSeries series(8);
  CHECK(series.Insert({Ns(1000), 10.0, 1.0, 1}, 0) == InsertOutcome::Accepted);
  CHECK(series.Insert({Ns(2000), 20.0, 1.0, 2}, 0) == InsertOutcome::Accepted);
  CHECK(series.Insert({Ns(3000), 60.0, 2.0, 3}, 0) == InsertOutcome::Accepted);
  const WindowStats all = series.Stats(Ns(3000), Ns(10000));
  CHECK(all.count == 3);
  CHECK_NEAR(*all.Mean(), 30.0, 1e-9);
  CHECK_NEAR(*all.median, 20.0, 1e-9);
  CHECK_NEAR(all.min, 10.0, 1e-9);
  CHECK_NEAR(all.max, 60.0, 1e-9);
  // Sample standard deviation: sqrt((400 + 100 + 900) / 2).
  CHECK_NEAR(*all.sample_stddev, std::sqrt(700.0), 1e-9);
  // Weighted mean: (10 + 20 + 60 * 2) / 4.
  CHECK_NEAR(*all.WeightedMean(), 37.5, 1e-9);
  CHECK(*all.newest_ns == Ns(3000));

  // Window expiration: only samples with now - window < t <= now.
  const WindowStats recent = series.Stats(Ns(3000), Ns(1500));
  CHECK(recent.count == 2);
  CHECK_NEAR(*recent.Mean(), 40.0, 1e-9);
  // The boundary is exclusive at the old end.
  CHECK(series.Stats(Ns(3000), Ns(2000)).count == 2);
  // Samples after `now` are not part of the window.
  CHECK(series.Stats(Ns(1500), Ns(10000)).count == 1);
  // One sample has no deviation; none has no mean.
  CHECK(!series.Stats(Ns(1000), Ns(500)).sample_stddev.has_value());
  const WindowStats empty = series.Stats(Ns(60000), Ns(1000));
  CHECK(empty.count == 0 && !empty.Mean() && !empty.median && !empty.WeightedMean());

  // Duplicates, late samples and out-of-order arrival.
  CHECK(series.Insert({Ns(2000), 99.0, 1.0, 2}, 0) == InsertOutcome::Duplicate);
  CHECK(series.Insert({Ns(500), 5.0, 1.0, 9}, Ns(1000)) == InsertOutcome::Late);
  CHECK(series.Size() == 3);
  RollingSeries unordered(8);
  unordered.Insert({Ns(3000), 3.0, 1.0, 12}, 0);
  CHECK(unordered.Insert({Ns(1000), 1.0, 1.0, 10}, 0) == InsertOutcome::Accepted);
  CHECK(unordered.Insert({Ns(2000), 2.0, 1.0, 11}, 0) == InsertOutcome::Accepted);
  CHECK(*unordered.Stats(Ns(3000), Ns(10000)).newest_ns == Ns(3000));
  CHECK(unordered.Stats(Ns(2500), Ns(10000)).count == 2);

  // Capacity bounds memory by dropping the oldest sample.
  RollingSeries small(3);
  for (std::uint64_t i = 1; i <= 5; ++i) small.Insert({Ns(1000 * i), double(i), 1.0, 100 + i}, 0);
  CHECK(small.Size() == 3 && small.DroppedForCapacity() == 2);
  CHECK_NEAR(small.Stats(Ns(5000), Ns(60000)).min, 3.0, 1e-12);
  small.Prune(Ns(4500));
  CHECK(small.Size() == 1);
  CHECK(small.EverObserved());
  small.Clear();
  CHECK(small.Size() == 0 && !small.EverObserved());
}

void TestNormalization() {
  CHECK_NEAR(*ToCanonical(1.5, "s", Unit::Milliseconds), 1500.0, 1e-9);
  CHECK_NEAR(*ToCanonical(250.0, "us", Unit::Milliseconds), 0.25, 1e-12);
  CHECK_NEAR(*ToCanonical(12.0, "ms", Unit::Milliseconds), 12.0, 1e-12);
  CHECK_NEAR(*ToCanonical(2500.0, "kbps", Unit::Mbps), 2.5, 1e-12);
  CHECK_NEAR(*ToCanonical(3e6, "bps", Unit::Mbps), 3.0, 1e-9);
  CHECK_NEAR(*ToCanonical(25.0, "percent", Unit::Ratio), 0.25, 1e-12);
  CHECK_NEAR(*ToCanonical(7.0, "segments", Unit::Count), 7.0, 1e-12);
  // A unit that measures something else is refused, never reinterpreted.
  CHECK(!ToCanonical(12.0, "packets", Unit::Milliseconds));
  CHECK(!ToCanonical(12.0, "ms", Unit::Mbps));
  CHECK(!ToCanonical(12.0, "", Unit::Count));
  CHECK(!ToCanonical(std::numeric_limits<double>::quiet_NaN(), "ms", Unit::Milliseconds));

  CHECK_NEAR(*SafeRatio(1.0, 4.0), 0.25, 1e-12);
  CHECK(!SafeRatio(1.0, 0.0));  // No denominator: no ratio, not zero.
  CHECK(!SafeRatio(0.0, 0.0));
  CHECK_NEAR(*BytesToMbps(1'000'000.0, 1000.0), 8.0, 1e-9);
  CHECK_NEAR(*BytesToMbps(125'000.0, 1000.0), 1.0, 1e-9);
  CHECK(!BytesToMbps(1000.0, 0.0));
  CHECK(InUnitRange(1.0, Unit::Ratio) && !InUnitRange(1.01, Unit::Ratio));
  CHECK(InUnitRange(100.0, Unit::Percent) && !InUnitRange(101.0, Unit::Percent));
  CHECK(!InUnitRange(-0.1, Unit::Milliseconds));
  CHECK(std::string(ToString(Unit::Milliseconds)) == "ms" && std::string(ToString(Unit::Mbps)) == "Mbps");
}

void TestValidation() {
  const MetricContract* rtt = FindMetricContract("net.tcp.handshake_rtt.mean");
  const MetricContract* packets = FindMetricContract("net.packets");
  CHECK(rtt && packets);
  CHECK(FindMetricContract("net.capture.packets_total") == nullptr);
  CHECK(FindMetricContract("something.else") == nullptr);

  const auto ok = ValidateMeasurement(Metric("net.tcp.handshake_rtt.mean", 0.04, "s", 0), *rtt);
  CHECK(ok.Ok());
  CHECK_NEAR(*ok.canonical_value, 40.0, 1e-9);

  CHECK(ValidateMeasurement(Metric("net.tcp.handshake_rtt.mean", -1.0, "ms", 0), *rtt).issue ==
        ValidationIssue::Negative);
  CHECK(ValidateMeasurement(Metric("net.tcp.handshake_rtt.mean", std::nan(""), "ms", 0), *rtt).issue ==
        ValidationIssue::NonFinite);
  CHECK(ValidateMeasurement(Metric("net.tcp.handshake_rtt.mean",
                                   std::numeric_limits<double>::infinity(), "ms", 0), *rtt).issue ==
        ValidationIssue::NonFinite);
  CHECK(ValidateMeasurement(Metric("net.tcp.handshake_rtt.mean", 5.0, "packets", 0), *rtt).issue ==
        ValidationIssue::UnexpectedUnit);
  CHECK(ValidateMeasurement(Metric("net.tcp.handshake_rtt.mean", 1e9, "ms", 0), *rtt).issue ==
        ValidationIssue::OutOfRange);
  CHECK(ValidateMeasurement(Metric("net.packets", 2.5, "packets", 0), *packets).issue ==
        ValidationIssue::NotAnInteger);
  auto no_value = Metric("net.packets", 0.0, "packets", 0);
  no_value.value.reset();
  const auto missing = ValidateMeasurement(no_value, *packets);
  CHECK(missing.issue == ValidationIssue::MissingValue && !missing.canonical_value);
  // A rejected measurement yields no value at all.
  CHECK(!ValidateMeasurement(Metric("net.packets", -3.0, "packets", 0), *packets).canonical_value);
}

void TestValidAggregation() {
  NetworkStateBuilder builder;
  CHECK(builder.ConfigIssues().empty());
  builder.Ingest(Capture(EventType::CaptureStarted, 0));
  const double rtt[] = {20, 30, 40, 50, 60};
  const double connections[] = {1, 1, 2, 1, 1};
  const double retransmissions[] = {1, 0, 2, 0, 2};
  const double throughput[] = {1, 2, 3, 2, 1};
  for (int i = 0; i < 5; ++i) {
    Interval interval;
    interval.rtt_mean_ms = rtt[i];
    interval.connections = connections[i];
    interval.retransmissions = retransmissions[i];
    interval.throughput_mbps = throughput[i];
    IngestInterval(builder, (i + 1) * 1000, interval);
  }
  // DNS answered in two intervals only: 5 ms over 2 responses, 11 ms over 1.
  builder.Ingest(Metric("net.dns.response_time.mean", 5.0, "ms", 2000, {{"samples", "2"}}));
  builder.Ingest(Metric("net.dns.response_time.mean", 11.0, "ms", 4000, {{"samples", "1"}}));
  builder.Ingest(Metric("net.quic.packets", 4.0, "packets", 5000, {{"quic_versions", "0x00000001,0x6b3343cf"}}));

  const NetworkState state = BuildAt(builder, 5000);
  CHECK(state.schema_version == kNetworkStateSchemaVersion);
  CHECK(state.state_id == 1);
  CHECK(state.telemetry_session_id == "s1");
  CHECK(state.capture_session_id == "s1/c1");
  CHECK(state.observed_monotonic_ns == Ns(5000));
  CHECK(state.primary_window == "medium");
  CHECK(state.windows.size() == 3);
  CHECK(state.windows[0].name == "short" && state.windows[0].duration_ms == 10'000);
  CHECK(state.windows[2].name == "baseline" && state.windows[2].duration_ms == 300'000);
  for (const WindowSnapshot& window : state.windows) {
    CHECK(window.features.size() == FeatureIds().size());
    for (std::size_t i = 0; i < window.features.size(); ++i) CHECK(window.features[i].id == FeatureIds()[i]);
  }

  // Exact mean over connections: (20 + 30 + 40*2 + 50 + 60) / 6.
  const Feature& mean = Get(state, "rtt.handshake.mean_ms");
  CHECK(mean.Usable());
  CHECK_NEAR(*mean.value, 40.0, 1e-9);
  CHECK(mean.unit == Unit::Milliseconds && mean.kind == ValueKind::Measured);
  CHECK(mean.sample_count == 5);
  CHECK_NEAR(*mean.age_ms, 0.0, 1e-9);
  CHECK(mean.source == aafr::telemetry::Source::TShark);
  CHECK(mean.scope == aafr::telemetry::MeasurementScope::CaptureInterface);
  // Interface traffic is never attributed to a tab.
  CHECK(mean.attribution == aafr::telemetry::AttributionQuality::Unattributed);

  const Feature& median = Get(state, "rtt.handshake.median_ms");
  CHECK_NEAR(*median.value, 40.0, 1e-9);
  CHECK(median.kind == ValueKind::Derived);
  CHECK_NEAR(*Get(state, "rtt.handshake.min_ms").value, 15.0, 1e-9);
  CHECK_NEAR(*Get(state, "rtt.handshake.max_ms").value, 65.0, 1e-9);
  // Sample standard deviation of {20,30,40,50,60}.
  CHECK_NEAR(*Get(state, "rtt.handshake.variation_ms").value, std::sqrt(250.0), 1e-9);

  // Traffic rate: 5 * 125000 bytes over 5 * 1000 ms.
  const Feature& rate = Get(state, "throughput.mean_mbps");
  CHECK(rate.Usable() && rate.unit == Unit::Mbps && rate.kind == ValueKind::Derived);
  CHECK_NEAR(*rate.value, 1.0, 1e-9);
  CHECK_NEAR(*Get(state, "throughput.peak_mbps").value, 3.0, 1e-9);
  CHECK_NEAR(*Get(state, "traffic.bytes").value, 625'000.0, 1e-6);
  CHECK(Get(state, "traffic.bytes").unit == Unit::Bytes);
  CHECK_NEAR(*Get(state, "traffic.packets").value, 500.0, 1e-9);
  CHECK_NEAR(*Get(state, "tcp.packets").value, 500.0, 1e-9);
  CHECK_NEAR(*Get(state, "tcp.connection_observations").value, 6.0, 1e-9);

  // Retransmission indicator and its derived ratio.
  CHECK_NEAR(*Get(state, "tcp.retransmissions").value, 5.0, 1e-9);
  const Feature& ratio = Get(state, "tcp.retransmission_ratio");
  CHECK(ratio.Usable() && ratio.unit == Unit::Ratio && ratio.kind == ValueKind::Derived);
  CHECK_NEAR(*ratio.value, 0.01, 1e-12);

  // An observed zero is a value, and it is valid.
  const Feature& udp = Get(state, "udp.packets");
  CHECK(udp.Usable());
  CHECK_NEAR(*udp.value, 0.0, 1e-12);
  CHECK_NEAR(*Get(state, "quic.packets").value, 4.0, 1e-9);
  CHECK(state.quic_versions.size() == 2 && state.quic_versions[0] == "0x00000001");

  // DNS: weighted mean (5*2 + 11*1) / 3, but only two intervals had answers.
  const Feature& dns = Get(state, "dns.response_time.mean_ms");
  CHECK_NEAR(*dns.value, 7.0, 1e-9);
  CHECK(dns.quality == FeatureQuality::InsufficientSamples);
  CHECK(!dns.Usable());
  CHECK(Contains(dns.quality_reason, "2 of 3"));
  // DNS maximum was never reported in this capture: missing, with no value.
  const Feature& dns_max = Get(state, "dns.response_time.max_ms");
  CHECK(dns_max.quality == FeatureQuality::Missing && !dns_max.value);

  // Page-load features are separate from RTT and absent here.
  CHECK(Get(state, "nav.duration.median_ms").quality == FeatureQuality::Missing);
  CHECK(state.validity == StateValidity::Degraded);
  CHECK(state.quality.Total() == FeatureIds().size());
  CHECK(state.quality.valid > 0 && state.quality.missing > 0);
  CHECK(state.FeatureCompleteness() > 0.0 && state.FeatureCompleteness() < 1.0);
  CHECK_NEAR(*state.freshness_ms, 0.0, 1e-9);
  CHECK(state.sources.capture == CaptureStatus::Active);
  CHECK(state.sources.capture_interface == "Wi-Fi");
  CHECK(!state.sources.browser_events);
  CHECK(state.network_scope == aafr::telemetry::MeasurementScope::CaptureInterface);
  // The browser's own environment: no topology, nothing executable.
  CHECK(state.execution.forwarding_topology_available.has_value() &&
        !*state.execution.forwarding_topology_available);
  CHECK(state.execution.supported_forwarding_operations.has_value() &&
        state.execution.supported_forwarding_operations->empty());
  CHECK(!state.execution.computation_deadline_ms.has_value());

  // State versioning: each build gets the next id.
  CHECK(BuildAt(builder, 5000).state_id == 2);
  // The short window (10 s) holds the same five intervals here.
  CHECK_NEAR(*Get(state, "rtt.handshake.mean_ms", "short").value, 40.0, 1e-9);
}

void TestEmptyAndMissing() {
  NetworkStateBuilder builder;
  const NetworkState state = BuildAt(builder, 0);
  CHECK(state.validity == StateValidity::NoData);
  CHECK(!state.freshness_ms.has_value());
  CHECK(state.capture_session_id.empty());
  CHECK(state.sources.capture == CaptureStatus::NeverStarted);
  CHECK(state.quality.valid == 0 && state.quality.missing == FeatureIds().size());
  CHECK(state.FeatureCompleteness() == 0.0);
  CHECK(Contains(state.validity_reason, "never_started"));
  for (const WindowSnapshot& window : state.windows) {
    for (const Feature& feature : window.features) {
      // Unavailable data has no value; zero is never used as a stand-in.
      CHECK(!feature.value.has_value());
      CHECK(feature.quality == FeatureQuality::Missing);
      CHECK(!feature.quality_reason.empty());
      CHECK(!feature.Usable());
    }
  }
  CHECK(Contains(Get(state, "rtt.handshake.mean_ms").quality_reason, "has not been started"));
  CHECK(!state.browser.active_tab_id && !state.browser.last_navigation_outcome);

  // Capture running but a metric never reported: missing, not zero.
  NetworkStateBuilder partial;
  partial.Ingest(Capture(EventType::CaptureStarted, 0));
  partial.Ingest(Metric("net.packets", 10.0, "packets", 1000));
  const NetworkState partial_state = BuildAt(partial, 1000);
  CHECK(Get(partial_state, "traffic.packets").Usable());
  CHECK(Get(partial_state, "rtt.handshake.mean_ms").quality == FeatureQuality::Missing);
  CHECK(Contains(Get(partial_state, "rtt.handshake.mean_ms").quality_reason, "not observed in this capture"));
  // Bytes without interval lengths cannot give a rate.
  partial.Ingest(Metric("net.bytes", 5000.0, "bytes", 1000));
  const Feature rate = Get(BuildAt(partial, 1000), "throughput.mean_mbps");
  CHECK(!rate.value && rate.quality == FeatureQuality::Missing);
  CHECK(Contains(rate.quality_reason, "interval lengths"));
  // TCP packets counted as zero: the ratio is undefined, not zero.
  partial.Ingest(Metric("net.tcp.packets", 0.0, "packets", 1000));
  partial.Ingest(Metric("net.tcp.retransmissions", 0.0, "segments", 1000));
  const Feature ratio = Get(BuildAt(partial, 1000), "tcp.retransmission_ratio");
  CHECK(!ratio.value && ratio.quality == FeatureQuality::Missing);
  CHECK(Contains(ratio.quality_reason, "undefined"));
}

void TestStaleAndInterruption() {
  NetworkStateBuilder builder;
  builder.Ingest(Capture(EventType::CaptureStarted, 0));
  for (int second = 1; second <= 3; ++second) IngestInterval(builder, second * 1000);
  CHECK(Get(BuildAt(builder, 3000), "rtt.handshake.mean_ms").quality == FeatureQuality::Valid);

  // 17 s later: still inside the 60 s window, but older than the 10 s limit.
  const NetworkState stale = BuildAt(builder, 20'000);
  const Feature& medium = Get(stale, "rtt.handshake.mean_ms");
  CHECK(medium.quality == FeatureQuality::Stale);
  CHECK(medium.value.has_value() && !medium.Usable());
  CHECK_NEAR(*medium.age_ms, 17'000.0, 1e-6);
  CHECK(Contains(medium.quality_reason, "17.0 s old"));
  // Outside the 10 s window: stale and without a value.
  const Feature& short_window = Get(stale, "rtt.handshake.mean_ms", "short");
  CHECK(short_window.quality == FeatureQuality::Stale && !short_window.value);
  CHECK_NEAR(*stale.freshness_ms, 17'000.0, 1e-6);
  CHECK(stale.quality.stale > 0 && stale.quality.valid == 0);
  CHECK(stale.validity == StateValidity::NoData);

  // A capture interruption is recorded; old samples are not refreshed by it.
  builder.Ingest(Capture(EventType::CaptureStopped, 21'000));
  const NetworkState stopped = BuildAt(builder, 21'000);
  CHECK(stopped.sources.capture == CaptureStatus::Stopped);
  CHECK(Contains(stopped.validity_reason, "stopped"));
  CHECK(Get(stopped, "rtt.handshake.mean_ms").quality == FeatureQuality::Stale);
  CHECK(Contains(Get(stopped, "rtt.handshake.mean_ms", "short").quality_reason, "stopped"));
  builder.Ingest(Capture(EventType::CaptureError, 22'000));
  CHECK(BuildAt(builder, 22'000).sources.capture == CaptureStatus::Error);

  // After the longest window everything has expired; memory is released.
  const NetworkState expired = BuildAt(builder, 400'000);
  CHECK(builder.StoredSamples() == 0);
  CHECK(!Get(expired, "rtt.handshake.mean_ms", "baseline").value);

  // A custom stale limit is honoured.
  BuilderConfig config;
  config.quality.network_stale_after_ms = 60'000;
  NetworkStateBuilder patient(config);
  patient.Ingest(Capture(EventType::CaptureStarted, 0));
  for (int second = 1; second <= 3; ++second) IngestInterval(patient, second * 1000);
  CHECK(Get(BuildAt(patient, 20'000), "rtt.handshake.mean_ms").quality == FeatureQuality::Valid);
}

void TestDuplicatesAndOrdering() {
  NetworkStateBuilder builder;
  builder.Ingest(Capture(EventType::CaptureStarted, 0));
  const auto first = Metric("net.tcp.handshake_rtt.mean", 10.0, "ms", 1000);
  CHECK(builder.Ingest(first).outcome == IngestOutcome::Accepted);
  // The same event delivered again changes nothing.
  CHECK(builder.Ingest(first).outcome == IngestOutcome::Duplicate);
  CHECK(builder.counters().duplicates == 1);
  // Out of order but inside the horizon: accepted at its own time.
  CHECK(builder.Ingest(Metric("net.tcp.handshake_rtt.mean", 30.0, "ms", 3000)).outcome ==
        IngestOutcome::Accepted);
  CHECK(builder.Ingest(Metric("net.tcp.handshake_rtt.mean", 20.0, "ms", 2000)).outcome ==
        IngestOutcome::Accepted);
  const NetworkState state = BuildAt(builder, 3000);
  const Feature& mean = Get(state, "rtt.handshake.mean_ms");
  CHECK(mean.sample_count == 3);
  CHECK_NEAR(*mean.value, 20.0, 1e-9);
  CHECK_NEAR(*Get(state, "rtt.handshake.median_ms").value, 20.0, 1e-9);
  CHECK_NEAR(*mean.age_ms, 0.0, 1e-9);

  // Older than the longest window relative to the newest event: refused.
  builder.Ingest(Metric("net.tcp.handshake_rtt.mean", 40.0, "ms", 1'000'000));
  CHECK(builder.Ingest(Metric("net.tcp.handshake_rtt.mean", 50.0, "ms", 5000)).outcome ==
        IngestOutcome::Late);
  CHECK(builder.counters().late == 1);
  // Events that carry no feature input are ignored, not rejected.
  CHECK(builder.Ingest(Metric("net.capture.packets_total", 9.0, "packets", 1'000'000)).outcome ==
        IngestOutcome::Ignored);
  aafr::telemetry::TelemetryEvent notice;
  CHECK(builder.Ingest(notice).outcome == IngestOutcome::Ignored);
  aafr::telemetry::TelemetryEvent diagnostic;
  diagnostic.source = aafr::telemetry::Source::StatePipeline;
  diagnostic.type = EventType::NetworkStateUpdated;
  CHECK(builder.Ingest(diagnostic).outcome == IngestOutcome::Ignored);
}

void TestInvalidMeasurements() {
  NetworkStateBuilder builder;
  builder.Ingest(Capture(EventType::CaptureStarted, 0));
  const IngestResult rejected = builder.Ingest(Metric("net.tcp.handshake_rtt.mean", -4.0, "ms", 1000));
  CHECK(rejected.outcome == IngestOutcome::RejectedInvalid);
  CHECK(rejected.issue == ValidationIssue::Negative);
  CHECK(rejected.metric == "net.tcp.handshake_rtt.mean");
  CHECK(builder.counters().rejected_invalid == 1);
  const NetworkState state = BuildAt(builder, 1000);
  const Feature& mean = Get(state, "rtt.handshake.mean_ms");
  CHECK(mean.quality == FeatureQuality::Invalid);
  CHECK(!mean.value.has_value());
  CHECK(Contains(mean.quality_reason, "rejected"));
  CHECK(state.quality.invalid >= 1);
  // The rejected value never reached the statistics.
  CHECK(builder.StoredSamples() == 0);

  // A wrong unit is rejected rather than converted on a guess.
  CHECK(builder.Ingest(Metric("net.throughput", 5.0, "ms", 1000)).issue == ValidationIssue::UnexpectedUnit);
  // Valid samples afterwards make the feature usable again.
  for (int second = 2; second <= 4; ++second) {
    builder.Ingest(Metric("net.tcp.handshake_rtt.mean", 25.0, "ms", second * 1000));
  }
  CHECK(Get(BuildAt(builder, 4000), "rtt.handshake.mean_ms").quality == FeatureQuality::Valid);
}

void TestCaptureSessions() {
  NetworkStateBuilder builder;
  builder.Ingest(Capture(EventType::CaptureStarted, 0, "Wi-Fi"));
  for (int second = 1; second <= 3; ++second) {
    Interval slow;
    slow.rtt_mean_ms = 100.0;
    IngestInterval(builder, second * 1000, slow);
  }
  builder.Ingest(Navigation(EventType::NavigationSucceeded, 300.0, 3500));
  builder.Ingest(Capture(EventType::CaptureStopped, 4000));

  // A new capture, possibly on another interface, starts from nothing.
  builder.Ingest(Capture(EventType::CaptureStarted, 5000, "Ethernet"));
  const NetworkState restarted = BuildAt(builder, 5000);
  CHECK(restarted.capture_session_id == "s1/c2");
  CHECK(restarted.sources.capture_interface == "Ethernet");
  CHECK(restarted.counters.capture_sessions == 2);
  CHECK(Get(restarted, "rtt.handshake.mean_ms").quality == FeatureQuality::Missing);
  // Browser observations are not capture-bound and survive.
  CHECK_NEAR(*Get(restarted, "nav.succeeded").value, 1.0, 1e-12);

  for (int second = 6; second <= 8; ++second) {
    Interval fast;
    fast.rtt_mean_ms = 10.0;
    IngestInterval(builder, second * 1000, fast);
  }
  // Only the second capture contributes: 10 ms, not a blend with 100 ms.
  CHECK_NEAR(*Get(BuildAt(builder, 8000), "rtt.handshake.mean_ms").value, 10.0, 1e-9);

  // Metrics without a start event imply a capture.
  NetworkStateBuilder implicit;
  IngestInterval(implicit, 1000);
  const NetworkState implied = BuildAt(implicit, 1000);
  CHECK(implied.sources.capture == CaptureStatus::Active);
  CHECK(implied.capture_session_id == "s1/c1");

  // A different telemetry session discards everything from the old one.
  auto other = Metric("net.tcp.handshake_rtt.mean", 77.0, "ms", 100);
  other.session_id = "s2";
  builder.Ingest(other);
  const NetworkState reset = BuildAt(builder, 100);
  CHECK(reset.telemetry_session_id == "s2");
  CHECK(reset.counters.telemetry_session_resets == 1);
  CHECK(reset.capture_session_id == "s2/c1");
  const Feature& mean = Get(reset, "rtt.handshake.mean_ms");
  CHECK(mean.sample_count == 1);
  CHECK_NEAR(*mean.value, 77.0, 1e-9);
  CHECK(!Get(reset, "nav.succeeded").value.has_value());
}

void TestBrowserFeatures() {
  NetworkStateBuilder builder;
  // The collector is running but nothing has navigated: an observed zero.
  builder.Ingest(Browser(EventType::TabCreated, 100, 7));
  const NetworkState idle = BuildAt(builder, 100);
  CHECK(idle.sources.browser_events);
  CHECK(Get(idle, "nav.succeeded").Usable());
  CHECK_NEAR(*Get(idle, "nav.succeeded").value, 0.0, 1e-12);
  CHECK(Get(idle, "nav.duration.median_ms").quality == FeatureQuality::Missing);
  CHECK(Get(idle, "nav.failure_ratio").quality == FeatureQuality::Missing);

  builder.Ingest(Browser(EventType::TabActivated, 200, 7));
  builder.Ingest(Browser(EventType::NavigationStarted, 300, 7));
  auto progress = Browser(EventType::LoadProgress, 400, 7);
  progress.metric = "navigation.progress";
  progress.value = 50.0;
  progress.unit = "percent";
  builder.Ingest(progress);
  NetworkState loading = BuildAt(builder, 400);
  CHECK(loading.browser.active_tab_id == std::uint64_t{7});
  CHECK_NEAR(*loading.browser.active_tab_load_progress_percent, 50.0, 1e-12);
  CHECK(*loading.browser.last_event_monotonic_ns == Ns(400));

  builder.Ingest(Navigation(EventType::NavigationSucceeded, 100.0, 1000, 7));
  builder.Ingest(Navigation(EventType::NavigationSucceeded, 200.0, 2000, 7));
  builder.Ingest(Navigation(EventType::NavigationSucceeded, 600.0, 3000, 7));
  builder.Ingest(Navigation(EventType::NavigationFailed, 5000.0, 4000, 8));
  const NetworkState state = BuildAt(builder, 4000);
  const Feature& median = Get(state, "nav.duration.median_ms");
  CHECK(median.Usable());
  CHECK_NEAR(*median.value, 200.0, 1e-9);
  CHECK(median.unit == Unit::Milliseconds);
  CHECK(median.source == aafr::telemetry::Source::QtWebEngine);
  CHECK(median.scope == aafr::telemetry::MeasurementScope::Browser);
  // A failed navigation's time-to-failure is not a page-load duration.
  CHECK_NEAR(*Get(state, "nav.duration.mean_ms").value, 300.0, 1e-9);
  CHECK_NEAR(*Get(state, "nav.succeeded").value, 3.0, 1e-12);
  CHECK_NEAR(*Get(state, "nav.failed").value, 1.0, 1e-12);
  CHECK_NEAR(*Get(state, "nav.failure_ratio").value, 0.25, 1e-12);
  CHECK(state.browser.last_navigation_outcome == NavigationOutcome::Failed);
  CHECK(state.browser.last_navigation_tab_id == std::uint64_t{8});
  CHECK_NEAR(*state.browser.active_tab_load_progress_percent, 100.0, 1e-12);

  // Page-load time is never an RTT: the latency features stay missing.
  CHECK(Get(state, "rtt.handshake.mean_ms").quality == FeatureQuality::Missing);
  CHECK(!Get(state, "rtt.handshake.mean_ms").value.has_value());
  CHECK(!Get(state, "throughput.mean_mbps").value.has_value());
  // Reduced observability without packet capture is visible in the state.
  CHECK(state.validity == StateValidity::Degraded);
  CHECK(state.sources.capture == CaptureStatus::NeverStarted);

  // A navigation whose start was not seen has no duration; it still counts.
  const IngestResult untimed = builder.Ingest(Navigation(EventType::NavigationSucceeded, std::nullopt, 5000, 7));
  CHECK(untimed.outcome == IngestOutcome::Accepted);
  const NetworkState later = BuildAt(builder, 5000);
  CHECK_NEAR(*Get(later, "nav.succeeded").value, 4.0, 1e-12);
  CHECK(Get(later, "nav.duration.mean_ms").sample_count == 3);
  // An impossible duration is rejected.
  CHECK(builder.Ingest(Navigation(EventType::NavigationSucceeded, -5.0, 6000, 7)).outcome ==
        IngestOutcome::RejectedInvalid);
  // Progress outside 0..100 is rejected and does not move the context.
  auto bad_progress = progress;
  bad_progress.event_id = next_event_id++;
  bad_progress.monotonic_ns = Ns(7000);
  bad_progress.value = 150.0;
  CHECK(builder.Ingest(bad_progress).issue == ValidationIssue::OutOfRange);
  // Closing the active tab clears the active-tab context.
  builder.Ingest(Browser(EventType::TabClosed, 8000, 7));
  const NetworkState closed = BuildAt(builder, 8000);
  CHECK(!closed.browser.active_tab_id.has_value());
  CHECK(!closed.browser.active_tab_load_progress_percent.has_value());

  // Navigations age out of the short window but stay in the longer ones.
  const NetworkState aged = BuildAt(builder, 30'000);
  CHECK_NEAR(*Get(aged, "nav.succeeded", "short").value, 0.0, 1e-12);
  CHECK(Get(aged, "nav.duration.median_ms", "short").quality == FeatureQuality::Missing);
  CHECK(Get(aged, "nav.duration.median_ms").Usable());
}

void TestQualityClassification() {
  MeasurementQualityEvaluator evaluator;
  FeatureDraft draft;
  draft.feature.id = "rtt.handshake.mean_ms";
  draft.feature.unit = Unit::Milliseconds;
  draft.feature.scope = aafr::telemetry::MeasurementScope::CaptureInterface;
  draft.feature.value = 12.0;
  draft.feature.sample_count = 3;
  draft.newest_ns = Ns(1000);
  draft.ever_observed = true;
  draft.min_samples = 3;
  CHECK(evaluator.Classify(draft, Ns(2000), CaptureStatus::Active).quality == FeatureQuality::Valid);
  // Exactly at the limit is still fresh; just beyond is stale.
  CHECK(evaluator.Classify(draft, Ns(11'000), CaptureStatus::Active).quality == FeatureQuality::Valid);
  CHECK(evaluator.Classify(draft, Ns(11'001), CaptureStatus::Active).quality == FeatureQuality::Stale);
  draft.feature.sample_count = 2;
  CHECK(evaluator.Classify(draft, Ns(2000), CaptureStatus::Active).quality ==
        FeatureQuality::InsufficientSamples);
  draft.feature.sample_count = 3;
  draft.feature.unit = Unit::Ratio;
  draft.feature.value = 1.5;
  const Feature out_of_range = evaluator.Classify(draft, Ns(2000), CaptureStatus::Active);
  CHECK(out_of_range.quality == FeatureQuality::Invalid && !out_of_range.value);
  draft.feature.value.reset();
  draft.rejection = "value is negative";
  CHECK(evaluator.Classify(draft, Ns(2000), CaptureStatus::Active).quality == FeatureQuality::Invalid);
  draft.rejection.clear();
  CHECK(evaluator.Classify(draft, Ns(2000), CaptureStatus::Stopped).quality == FeatureQuality::Stale);
  draft.ever_observed = false;
  CHECK(evaluator.Classify(draft, Ns(2000), CaptureStatus::NeverStarted).quality == FeatureQuality::Missing);

  // Asking for a tab-level view of interface traffic gives no value.
  const NetworkState state = FreshCaptureState();
  const Feature& host = Get(state, "rtt.handshake.mean_ms");
  CHECK(host.Usable());
  const Feature per_tab = MeasurementQualityEvaluator::ForScope(host, aafr::telemetry::MeasurementScope::Tab);
  CHECK(per_tab.quality == FeatureQuality::Unattributed);
  CHECK(!per_tab.value.has_value() && !per_tab.Usable());
  CHECK(Contains(per_tab.quality_reason, "capture_interface"));
  const Feature same = MeasurementQualityEvaluator::ForScope(
      host, aafr::telemetry::MeasurementScope::CaptureInterface);
  CHECK(same.Usable());

  const QualitySummary summary = MeasurementQualityEvaluator::Summarize({host, per_tab, out_of_range});
  CHECK(summary.valid == 1 && summary.unattributed == 1 && summary.invalid == 1 && summary.Total() == 3);
  CHECK(std::string(ToString(FeatureQuality::InsufficientSamples)) == "insufficient_samples");
}

void TestConfigurationAndBounds() {
  CHECK(ValidateConfig(BuilderConfig{}).empty());
  BuilderConfig broken;
  broken.windows = {{"a", std::chrono::seconds(5)}, {"a", std::chrono::seconds(0)}};
  broken.primary_window = "missing";
  CHECK(ValidateConfig(broken).size() == 3);
  BuilderConfig none;
  none.windows.clear();
  CHECK(!ValidateConfig(none).empty());
  // An unusable configuration falls back to the defaults and says so.
  NetworkStateBuilder fallback(broken);
  CHECK(!fallback.ConfigIssues().empty());
  CHECK(fallback.config().windows.size() == 3);

  // Windows are configurable, not fixed.
  BuilderConfig custom;
  custom.windows = {{"fast", std::chrono::seconds(2)}, {"slow", std::chrono::seconds(4)}};
  custom.primary_window = "slow";
  custom.statistic_min_samples = 1;
  custom.max_samples_per_series = 16;
  NetworkStateBuilder builder(custom);
  CHECK(builder.ConfigIssues().empty());
  builder.Ingest(Capture(EventType::CaptureStarted, 0));
  for (int second = 1; second <= 4; ++second) {
    Interval interval;
    interval.rtt_mean_ms = second * 10.0;
    IngestInterval(builder, second * 1000, interval);
  }
  const NetworkState state = BuildAt(builder, 4000);
  CHECK(state.windows.size() == 2 && state.primary_window == "slow");
  // "fast" covers (2 s, 4 s]: 30 and 40. "slow" covers (0 s, 4 s]: all four.
  CHECK_NEAR(*Get(state, "rtt.handshake.mean_ms", "fast").value, 35.0, 1e-9);
  CHECK_NEAR(*Get(state, "rtt.handshake.mean_ms", "slow").value, 25.0, 1e-9);
  CHECK(Get(state, "rtt.handshake.mean_ms", "fast").sample_count == 2);

  // Bounded memory: many more intervals than capacity or window can hold.
  for (int second = 5; second <= 2000; ++second) IngestInterval(builder, second * 1000);
  builder.Build(Ns(2'000'000), WallClock::time_point{});
  const std::size_t series_count = static_cast<std::size_t>(Series::kCount);
  CHECK(builder.StoredSamples() <= custom.max_samples_per_series * series_count);
  // The 4 s horizon keeps at most a handful of 1 s intervals per series.
  CHECK(builder.StoredSamples() <= 5 * series_count);
}

// ---- Network constraints ----

ExecutionEnvironment GraphEnvironment() {
  ExecutionEnvironment environment;
  ExecutionEnvironment::Topology topology;
  topology.available = true;
  topology.info.representations = {TopologyRepresentation::WeightedDirectedGraph};
  topology.info.node_count = 10;
  topology.info.edge_count = 20;
  topology.info.reachable = true;
  topology.info.edge_weights = EdgeWeightInfo{true, false, std::nullopt};
  environment.topology = topology;
  environment.forwarding_operations = std::set<ForwardingOperation>{ForwardingOperation::SelectPath};
  environment.supported_protocols = std::set<TransportProtocol>{TransportProtocol::Tcp};
  environment.problem = ProblemRepresentation::SinglePairShortestPath;
  environment.objective = OptimizationObjective::MinimizeAdditivePathCost;
  environment.computation_deadline_ms = 50.0;
  environment.latency_budget_ms = 200.0;
  environment.memory_budget_bytes = 1'000'000;
  return environment;
}

void TestConfiguredConstraints() {
  namespace id = constraint_id;
  const ExecutionEnvironment environment = GraphEnvironment();
  const NetworkState state = FreshCaptureState(environment);
  const NetworkConstraints constraints = ConstraintEvaluator().Evaluate(state, environment);
  CHECK(constraints.network_state_id == state.state_id);
  CHECK(constraints.conflicts.empty());
  CHECK(constraints.CountByValidity(ConstraintValidity::Invalid) == 0);

  CHECK(constraints.Bool(id::kTopologyAvailable) == true);
  const Constraint* deadline = constraints.Find(id::kComputationDeadlineMs);
  CHECK(deadline != nullptr);
  CHECK(deadline->source == ConstraintSource::Configured);
  CHECK(deadline->category == ConstraintCategory::Performance);
  CHECK(deadline->type == ConstraintType::Number);
  CHECK(deadline->unit == "ms");
  CHECK(deadline->validity == ConstraintValidity::Valid);
  CHECK(!deadline->explanation.empty());
  CHECK_NEAR(*constraints.Number(id::kComputationDeadlineMs), 50.0, 1e-12);
  CHECK_NEAR(*constraints.Number(id::kTopologyNodeCount), 10.0, 1e-12);
  CHECK_NEAR(*constraints.Number(id::kMemoryBudgetBytes), 1e6, 1e-6);
  CHECK(constraints.List(id::kTopologyRepresentations)->front() == "weighted_directed_graph");
  CHECK(constraints.List(id::kForwardingOperations)->front() == "select_path");
  CHECK(*constraints.Text(id::kProblem) == "single_pair_shortest_path");
  CHECK(constraints.Bool(id::kEdgeWeightsNegative) == false);
  CHECK(constraints.Bool(id::kRequiredProtocolsSupported) == true);

  // Measured and derived constraints carry the observation time.
  const Constraint* measurements = constraints.Find(id::kAvailableMeasurements);
  CHECK(measurements->source == ConstraintSource::Measured && measurements->timestamp.has_value());
  const auto usable = *constraints.List(id::kAvailableMeasurements);
  CHECK(std::find(usable.begin(), usable.end(), "rtt.handshake.mean_ms") != usable.end());
  CHECK(std::find(usable.begin(), usable.end(), "nav.duration.median_ms") == usable.end());
  CHECK(constraints.Bool(id::kStateFresh) == true);
  CHECK(constraints.Find(id::kStateFresh)->source == ConstraintSource::Derived);
  CHECK(constraints.Bool(id::kMonitoringAvailable) == true);
  CHECK(constraints.Bool(id::kCaptureInterrupted) == false);
  CHECK(*constraints.Text(id::kStateValidity) == "degraded");

  // Every constraint is fully described and ids are unique.
  std::set<std::string> ids;
  for (const Constraint& constraint : constraints.constraints) {
    CHECK(!constraint.id.empty() && !constraint.explanation.empty());
    CHECK(ids.insert(constraint.id).second);
    CHECK((constraint.validity == ConstraintValidity::Unknown) ==
          std::holds_alternative<std::monostate>(constraint.value));
    CHECK((constraint.source == ConstraintSource::Unknown) ==
          (constraint.validity == ConstraintValidity::Unknown));
  }
  CHECK(constraints.CountBySource(ConstraintSource::Configured) > 0);
  CHECK(constraints.CountBySource(ConstraintSource::Measured) > 0);
  CHECK(constraints.CountBySource(ConstraintSource::Derived) > 0);
  CHECK(constraints.CountBySource(ConstraintSource::Unknown) > 0);  // e.g. CPU budget.

  // Reliability limit against the measured retransmission indicator.
  ExecutionEnvironment limited = environment;
  limited.max_retransmission_ratio = 0.05;
  CHECK(ConstraintEvaluator().Evaluate(state, limited).Bool(id::kReliabilityWithinLimit) == true);
  limited.max_retransmission_ratio = 0.0;
  NetworkStateBuilder lossy({}, limited);
  lossy.Ingest(Capture(EventType::CaptureStarted, 0));
  for (int second = 1; second <= 3; ++second) {
    Interval interval;
    interval.retransmissions = 2;
    IngestInterval(lossy, second * 1000, interval);
  }
  const NetworkConstraints over =
      ConstraintEvaluator().Evaluate(lossy.Build(Ns(3000), WallClock::time_point{}), limited);
  CHECK(over.Bool(id::kReliabilityWithinLimit) == false);
  CHECK(Contains(over.Find(id::kReliabilityWithinLimit)->explanation, "not a packet-loss rate"));
}

void TestUnknownConstraints() {
  namespace id = constraint_id;
  // Nothing is known about the environment.
  const ExecutionEnvironment unknown;
  const NetworkState empty = NetworkStateBuilder({}, unknown).Build(0, WallClock::time_point{});
  const NetworkConstraints constraints = ConstraintEvaluator().Evaluate(empty, unknown);
  for (const char* constraint_id :
       {id::kTopologyAvailable, id::kForwardingOperations, id::kComputationDeadlineMs,
        id::kMemoryBudgetBytes, id::kSupportedProtocols, id::kProblem, id::kObjective,
        id::kEdgeWeightsFinite, id::kHeuristicAvailable, id::kReliabilityWithinLimit}) {
    const Constraint* constraint = constraints.Find(constraint_id);
    CHECK(constraint != nullptr);
    CHECK(constraint->validity == ConstraintValidity::Unknown);
    CHECK(constraint->source == ConstraintSource::Unknown);
    CHECK(!constraint->Known());
  }
  // Unknown never reads as a value, and so never as "satisfied".
  CHECK(!constraints.Bool(id::kTopologyAvailable).has_value());
  CHECK(!constraints.Number(id::kComputationDeadlineMs).has_value());
  CHECK(!constraints.List(id::kForwardingOperations).has_value());
  CHECK(ValueText(*constraints.Find(id::kTopologyAvailable)) == "unknown");
  // Without any measurement the state cannot be fresh.
  CHECK(constraints.Bool(id::kStateFresh) == false);
  CHECK(constraints.Bool(id::kMonitoringAvailable) == false);
  CHECK(constraints.List(id::kAvailableMeasurements)->empty());

  // The browser's environment states what it lacks instead of leaving it open.
  const ExecutionEnvironment browser = BrowserDefaultEnvironment();
  const NetworkConstraints defaults = ConstraintEvaluator().Evaluate(empty, browser);
  CHECK(defaults.Bool(id::kTopologyAvailable) == false);
  CHECK(defaults.Find(id::kTopologyAvailable)->source == ConstraintSource::Configured);
  CHECK(defaults.List(id::kForwardingOperations).has_value() &&
        defaults.List(id::kForwardingOperations)->empty());
  // Topology details stay unknown when no topology exists.
  CHECK(!defaults.Number(id::kTopologyNodeCount).has_value());
  CHECK(!defaults.Bool(id::kEdgeWeightsNegative).has_value());

  // A reliability limit without a usable measurement stays unknown.
  ExecutionEnvironment limited = browser;
  limited.max_retransmission_ratio = 0.05;
  const NetworkConstraints no_data = ConstraintEvaluator().Evaluate(empty, limited);
  CHECK(!no_data.Bool(id::kReliabilityWithinLimit).has_value());
  CHECK(Contains(no_data.Find(id::kReliabilityWithinLimit)->explanation, "missing"));

  // Capture interruption and staleness are operational constraints.
  NetworkStateBuilder builder;
  builder.Ingest(Capture(EventType::CaptureStarted, 0));
  for (int second = 1; second <= 3; ++second) IngestInterval(builder, second * 1000);
  builder.Ingest(Capture(EventType::CaptureStopped, 4000));
  const NetworkConstraints interrupted =
      ConstraintEvaluator().Evaluate(builder.Build(Ns(30'000), WallClock::time_point{}), browser);
  CHECK(interrupted.Bool(id::kCaptureInterrupted) == true);
  CHECK(interrupted.Bool(id::kStateFresh) == false);
  CHECK(interrupted.Bool(id::kMonitoringAvailable) == false);
}

void TestConflictingConstraints() {
  namespace id = constraint_id;
  const NetworkState state = FreshCaptureState();

  // The algorithm deadline cannot exceed the whole latency budget.
  ExecutionEnvironment environment = GraphEnvironment();
  environment.computation_deadline_ms = 500.0;
  environment.latency_budget_ms = 100.0;
  NetworkConstraints constraints = ConstraintEvaluator().Evaluate(state, environment);
  CHECK(constraints.conflicts.size() == 1);
  CHECK(constraints.Find(id::kComputationDeadlineMs)->validity == ConstraintValidity::Invalid);
  CHECK(constraints.Find(id::kLatencyBudgetMs)->validity == ConstraintValidity::Invalid);
  // An invalid constraint cannot be read as a value.
  CHECK(!constraints.Number(id::kComputationDeadlineMs).has_value());
  CHECK(Contains(constraints.Find(id::kComputationDeadlineMs)->explanation, "Conflict"));

  // Insufficient or impossible budgets.
  environment = GraphEnvironment();
  environment.computation_deadline_ms = 0.0;
  environment.memory_budget_bytes = 0;
  environment.cpu_budget_percent = 150.0;
  environment.max_retransmission_ratio = 1.5;
  constraints = ConstraintEvaluator().Evaluate(state, environment);
  CHECK(constraints.Find(id::kComputationDeadlineMs)->validity == ConstraintValidity::Invalid);
  CHECK(constraints.Find(id::kMemoryBudgetBytes)->validity == ConstraintValidity::Invalid);
  CHECK(constraints.Find(id::kCpuBudgetPercent)->validity == ConstraintValidity::Invalid);
  CHECK(constraints.Find(id::kMaxRetransmissionRatio)->validity == ConstraintValidity::Invalid);
  CHECK(constraints.conflicts.size() == 4);

  // Invalid edge-weight description: a negative cycle without negative weights.
  environment = GraphEnvironment();
  environment.topology->info.edge_weights = EdgeWeightInfo{true, false, true};
  constraints = ConstraintEvaluator().Evaluate(state, environment);
  CHECK(constraints.Find(id::kNegativeCycle)->validity == ConstraintValidity::Invalid);
  CHECK(!constraints.Bool(id::kEdgeWeightsNegative).has_value());

  // Missing topology content behind an "available" claim.
  environment = GraphEnvironment();
  environment.topology->info.node_count = 0;
  constraints = ConstraintEvaluator().Evaluate(state, environment);
  CHECK(constraints.Find(id::kTopologyNodeCount)->validity == ConstraintValidity::Invalid);
  environment.topology->info.representations.clear();
  constraints = ConstraintEvaluator().Evaluate(state, environment);
  CHECK(constraints.Find(id::kTopologyAvailable)->validity == ConstraintValidity::Invalid);
  CHECK(!constraints.Bool(id::kTopologyAvailable).has_value());

  environment = GraphEnvironment();
  environment.topology->info.representations = {TopologyRepresentation::CandidatePathSet};
  environment.topology->info.candidate_path_count = 3;
  environment.topology->info.reachable = false;
  constraints = ConstraintEvaluator().Evaluate(state, environment);
  CHECK(constraints.Find(id::kReachable)->validity == ConstraintValidity::Invalid);

  // Unsupported protocols.
  environment = GraphEnvironment();
  environment.required_protocols = {TransportProtocol::Quic, TransportProtocol::Tcp};
  constraints = ConstraintEvaluator().Evaluate(state, environment);
  CHECK(constraints.Bool(id::kRequiredProtocolsSupported) == false);
  CHECK(Contains(constraints.Find(id::kRequiredProtocolsSupported)->explanation, "quic"));
  environment.supported_protocols.reset();
  constraints = ConstraintEvaluator().Evaluate(state, environment);
  CHECK(!constraints.Bool(id::kRequiredProtocolsSupported).has_value());
}

}  // namespace

int main() {
  return RunTests({
      {"statistics_and_rolling_windows", TestStatistics},
      {"normalization", TestNormalization},
      {"validation", TestValidation},
      {"valid_aggregation", TestValidAggregation},
      {"empty_and_missing", TestEmptyAndMissing},
      {"stale_and_interruption", TestStaleAndInterruption},
      {"duplicates_and_ordering", TestDuplicatesAndOrdering},
      {"invalid_measurements", TestInvalidMeasurements},
      {"capture_sessions", TestCaptureSessions},
      {"browser_features", TestBrowserFeatures},
      {"quality_classification", TestQualityClassification},
      {"configuration_and_bounds", TestConfigurationAndBounds},
      {"configured_constraints", TestConfiguredConstraints},
      {"unknown_constraints", TestUnknownConstraints},
      {"conflicting_constraints", TestConflictingConstraints},
  });
}
