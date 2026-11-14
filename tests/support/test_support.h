#pragma once

// Shared helpers for the plain C++ V0.4 tests: the CHECK macros used by the
// existing core tests, and deterministic telemetry fixtures. Header-only;
// include it from exactly one translation unit per test program.

#include <cmath>
#include <cstdint>
#include <iostream>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "network/network_state/execution_environment.h"
#include "network/network_state/network_state_builder.h"
#include "network/telemetry/telemetry_event.h"

namespace aafr::test {

inline int failures = 0;
inline int checks = 0;
inline std::string current_test;

#define CHECK(condition)                                                              \
  do {                                                                                \
    ++aafr::test::checks;                                                             \
    if (!(condition)) {                                                               \
      ++aafr::test::failures;                                                         \
      std::cerr << "FAIL [" << aafr::test::current_test << "] " << __FILE__ << ':'   \
                << __LINE__ << ": " #condition "\n";                                \
    }                                                                                 \
  } while (0)

#define CHECK_NEAR(actual, expected, tolerance) \
  CHECK(std::fabs((actual) - (expected)) <= (tolerance))

inline int RunTests(const std::vector<std::pair<const char*, void (*)()>>& tests) {
  for (const auto& test : tests) {
    current_test = test.first;
    const int before = failures;
    test.second();
    std::cout << (failures == before ? "PASS " : "FAIL ") << test.first << "\n";
  }
  std::cout << checks << " checks, " << failures << " failures\n";
  return failures == 0 ? 0 : 1;
}

inline bool Contains(const std::string& haystack, const std::string& needle) {
  return haystack.find(needle) != std::string::npos;
}

// ---- Telemetry fixtures. Times are milliseconds since session start. ----

inline std::uint64_t next_event_id = 1;

inline telemetry::TelemetryEvent Stamp(telemetry::TelemetryEvent event, std::int64_t t_ms,
                                       const std::string& session = "s1") {
  event.event_id = next_event_id++;
  event.session_id = session;
  event.monotonic_ns = t_ms * 1'000'000;
  return event;
}

inline telemetry::TelemetryEvent Metric(const std::string& metric, double value,
                                        const std::string& unit, std::int64_t t_ms,
                                        std::vector<telemetry::Attribute> attributes = {}) {
  telemetry::TelemetryEvent event;
  event.type = telemetry::EventType::NetworkMetric;
  event.source = telemetry::Source::TShark;
  event.scope = telemetry::MeasurementScope::CaptureInterface;
  event.attribution = telemetry::AttributionQuality::Unattributed;
  event.metric = metric;
  event.value = value;
  event.unit = unit;
  event.attributes = std::move(attributes);
  return Stamp(std::move(event), t_ms);
}

inline telemetry::TelemetryEvent Capture(telemetry::EventType type, std::int64_t t_ms,
                                         const std::string& interface_name = "Wi-Fi") {
  telemetry::TelemetryEvent event;
  event.type = type;
  event.source = telemetry::Source::TShark;
  event.scope = telemetry::MeasurementScope::CaptureInterface;
  event.attributes.push_back({"interface", interface_name});
  return Stamp(std::move(event), t_ms);
}

inline telemetry::TelemetryEvent Browser(telemetry::EventType type, std::int64_t t_ms,
                                         std::uint64_t tab = 1) {
  telemetry::TelemetryEvent event;
  event.type = type;
  event.source = telemetry::Source::QtWebEngine;
  event.scope = telemetry::MeasurementScope::Tab;
  event.attribution = telemetry::AttributionQuality::Direct;
  event.tab_id = tab;
  return Stamp(std::move(event), t_ms);
}

inline telemetry::TelemetryEvent Navigation(telemetry::EventType type, std::optional<double> duration_ms,
                                            std::int64_t t_ms, std::uint64_t tab = 1) {
  telemetry::TelemetryEvent event = Browser(type, t_ms, tab);
  event.scope = telemetry::MeasurementScope::Navigation;
  event.metric = "navigation.duration";
  event.unit = "ms";
  event.value = duration_ms;
  if (!duration_ms) event.availability = telemetry::Availability::Unavailable;
  return event;
}

// One capture interval as V0.3 publishes it.
struct Interval {
  double rtt_mean_ms{40.0};
  double connections{1.0};
  double bytes{125'000.0};  // 1 Mbps over 1000 ms.
  double tcp_packets{100.0};
  double retransmissions{0.0};
  double throughput_mbps{1.0};
};

inline void IngestInterval(netstate::NetworkStateBuilder& builder, std::int64_t t_ms,
                           const Interval& interval = {}) {
  const std::string connections = std::to_string(static_cast<int>(interval.connections));
  builder.Ingest(Metric("net.tcp.handshake_rtt.min", interval.rtt_mean_ms - 5.0, "ms", t_ms,
                        {{"connections", connections}}));
  builder.Ingest(Metric("net.tcp.handshake_rtt.mean", interval.rtt_mean_ms, "ms", t_ms,
                        {{"connections", connections}}));
  builder.Ingest(Metric("net.tcp.handshake_rtt.max", interval.rtt_mean_ms + 5.0, "ms", t_ms,
                        {{"connections", connections}}));
  builder.Ingest(Metric("net.throughput", interval.throughput_mbps, "Mbps", t_ms,
                        {{"interval_ms", "1000"}}));
  builder.Ingest(Metric("net.tcp.retransmissions", interval.retransmissions, "segments", t_ms));
  builder.Ingest(Metric("net.dns.responses", 0.0, "responses", t_ms));
  builder.Ingest(Metric("net.packets", interval.tcp_packets, "packets", t_ms));
  builder.Ingest(Metric("net.bytes", interval.bytes, "bytes", t_ms, {{"interval_ms", "1000"}}));
  builder.Ingest(Metric("net.tcp.packets", interval.tcp_packets, "packets", t_ms));
  builder.Ingest(Metric("net.udp.packets", 0.0, "packets", t_ms));
  builder.Ingest(Metric("net.quic.packets", 0.0, "packets", t_ms));
}

inline std::int64_t Ns(std::int64_t t_ms) { return t_ms * 1'000'000; }

// A state built from a capture with five fresh 1 s intervals, read at 5 s.
inline netstate::NetworkState FreshCaptureState(
    netstate::ExecutionEnvironment environment = netstate::BrowserDefaultEnvironment()) {
  netstate::NetworkStateBuilder builder({}, std::move(environment));
  builder.Ingest(Capture(telemetry::EventType::CaptureStarted, 0));
  for (int second = 1; second <= 5; ++second) IngestInterval(builder, second * 1000);
  return builder.Build(Ns(5000), telemetry::WallClock::time_point{});
}

}  // namespace aafr::test
