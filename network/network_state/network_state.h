#pragma once

// NetworkState: a versioned, immutable snapshot of the latest validated
// observations relevant to AAFR's forwarding decisions. Plain C++17.
//
// Rules this contract enforces:
//  - A missing measurement is an empty optional with a quality and a reason.
//    Zero always means "zero was observed".
//  - Every feature names its unit, whether it was measured or derived, where
//    it came from, what it covers (scope) and how it is tied to a tab.
//  - Capture-interface features cover all host traffic. They are never
//    presented as belonging to a browser tab.
//  - Page-load time is a browser feature (nav.*), never an RTT feature, and
//    the retransmission ratio is not a packet-loss probability.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "network/telemetry/telemetry_event.h"

namespace aafr::netstate {

// Bump when a NetworkState field or a feature definition changes meaning.
inline constexpr int kNetworkStateSchemaVersion = 1;

enum class FeatureQuality {
  Valid,                // Enough fresh samples; the value can be used.
  Missing,              // Nothing was observed for this feature.
  Stale,                // The newest observation is too old.
  InsufficientSamples,  // Fewer observations than the feature needs.
  Invalid,              // Observations were rejected or the value is unusable.
  Unattributed,         // Cannot be tied to the scope that was asked for.
};

// Where a value comes from.
enum class ValueKind {
  Measured,    // Aggregated directly from observations.
  Derived,     // Computed from measured values (an estimate).
  Configured,  // Supplied by configuration.
  Unknown,
};

enum class Unit {
  Milliseconds,
  Mbps,     // 10^6 bits per second.
  Ratio,    // Dimensionless, 0..1.
  Count,    // Nonnegative integer carried as a double.
  Bytes,
  Percent,  // 0..100.
};

struct Feature {
  std::string id;  // For example "rtt.handshake.median_ms".
  std::optional<double> value;
  Unit unit{Unit::Count};
  ValueKind kind{ValueKind::Unknown};
  FeatureQuality quality{FeatureQuality::Missing};
  std::string quality_reason;
  // Observations inside the window that produced the value (capture intervals
  // or navigation events, not packets).
  std::uint64_t sample_count{0};
  // Age of the newest contributing observation when the state was built.
  std::optional<double> age_ms;
  telemetry::Source source{telemetry::Source::Pipeline};
  telemetry::MeasurementScope scope{telemetry::MeasurementScope::Pipeline};
  telemetry::AttributionQuality attribution{telemetry::AttributionQuality::NotApplicable};

  bool Usable() const { return quality == FeatureQuality::Valid && value.has_value(); }
};

struct WindowSnapshot {
  std::string name;  // For example "short", "medium", "baseline".
  std::int64_t duration_ms{0};
  std::vector<Feature> features;

  const Feature* Find(const std::string& feature_id) const;
};

enum class CaptureStatus {
  NeverStarted,  // Packet capture was not enabled in this telemetry session.
  Active,
  Stopped,
  Error,
};

struct TelemetrySources {
  bool browser_events{false};  // At least one browser event was observed.
  CaptureStatus capture{CaptureStatus::NeverStarted};
  std::string capture_interface;  // As reported by the capture, may be empty.
};

enum class NavigationOutcome { Succeeded, Failed, Stopped };

// Point-in-time browser context. Tab ids are the collector's own counters.
struct BrowserContext {
  std::optional<std::uint64_t> active_tab_id;
  std::optional<double> active_tab_load_progress_percent;
  std::optional<NavigationOutcome> last_navigation_outcome;
  std::optional<std::uint64_t> last_navigation_tab_id;
  // Monotonic time of the newest browser event, ns since session start.
  std::optional<std::int64_t> last_event_monotonic_ns;
};

// Configured facts about where an algorithm would run. Copied from the
// ExecutionEnvironment so a snapshot is self-describing. nullopt = unknown.
struct ExecutionContext {
  std::optional<bool> forwarding_topology_available;
  std::optional<std::vector<std::string>> supported_forwarding_operations;
  std::optional<double> computation_deadline_ms;
  std::optional<std::uint64_t> memory_budget_bytes;
  std::optional<double> cpu_budget_percent;
};

enum class StateValidity {
  Valid,     // Every feature of the primary window is usable.
  Degraded,  // Some features are usable, some are not.
  NoData,    // No feature is usable.
};

struct QualitySummary {
  std::uint64_t valid{0};
  std::uint64_t missing{0};
  std::uint64_t stale{0};
  std::uint64_t insufficient_samples{0};
  std::uint64_t invalid{0};
  std::uint64_t unattributed{0};

  std::uint64_t Total() const {
    return valid + missing + stale + insufficient_samples + invalid + unattributed;
  }
};

struct IngestCounters {
  std::uint64_t accepted{0};
  std::uint64_t ignored{0};            // Events that carry no feature input.
  std::uint64_t rejected_invalid{0};   // Failed validation.
  std::uint64_t duplicates{0};
  std::uint64_t late{0};               // Older than the longest window.
  std::uint64_t capture_sessions{0};   // Captures started in this session.
  std::uint64_t telemetry_session_resets{0};
};

struct NetworkState {
  int schema_version{kNetworkStateSchemaVersion};
  std::uint64_t state_id{0};  // Increases by one per built snapshot.
  std::string telemetry_session_id;
  // "<telemetry session>/c<n>" for the n-th capture; empty before any capture.
  std::string capture_session_id;
  telemetry::WallClock::time_point observed_at{};
  std::int64_t observed_monotonic_ns{0};

  std::vector<WindowSnapshot> windows;
  std::string primary_window;  // The window consumers should read by default.

  // Age of the newest accepted measurement; empty when nothing was accepted.
  std::optional<double> freshness_ms;
  StateValidity validity{StateValidity::NoData};
  std::string validity_reason;
  QualitySummary quality;  // Over the primary window.

  TelemetrySources sources;
  // What network features describe: the capture interface (all host traffic).
  telemetry::MeasurementScope network_scope{telemetry::MeasurementScope::CaptureInterface};
  BrowserContext browser;
  ExecutionContext execution;
  // QUIC versions seen in the newest capture interval, as TShark reports them.
  std::vector<std::string> quic_versions;
  IngestCounters counters;

  const WindowSnapshot* FindWindow(const std::string& name) const;
  const WindowSnapshot* PrimaryWindow() const { return FindWindow(primary_window); }
  // Feature from the primary window, or nullptr.
  const Feature* FindFeature(const std::string& feature_id) const;
  // Share of primary-window features that are usable, 0..1.
  double FeatureCompleteness() const;
};

const char* ToString(FeatureQuality value);
const char* ToString(ValueKind value);
const char* ToString(Unit value);
const char* ToString(CaptureStatus value);
const char* ToString(NavigationOutcome value);
const char* ToString(StateValidity value);

// Feature ids, in display order. Every window carries exactly these.
const std::vector<std::string>& FeatureIds();

}  // namespace aafr::netstate
