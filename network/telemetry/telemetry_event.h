#pragma once

// Typed, versioned telemetry records shared by the browser event collector,
// network monitors, the diagnostic JSONL writer and the Developer Tools panel.
// Plain C++17: no Qt dependency, so the future AAFR runtime can consume the
// same records without linking the browser UI.

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace aafr::telemetry {

// Bump when a field is added, removed or changes meaning. Serialized records
// carry this value as "schema".
inline constexpr int kTelemetrySchemaVersion = 1;

using WallClock = std::chrono::system_clock;
using MonotonicClock = std::chrono::steady_clock;

enum class EventType {
  // Browser lifecycle (Qt WebEngine page-level signals, not HTTP requests).
  TabCreated,
  TabActivated,
  TabClosed,
  NavigationStarted,
  NavigationSucceeded,
  NavigationFailed,
  NavigationStopped,
  LoadProgress,
  UrlChanged,
  RendererTerminated,
  // Network monitoring.
  NetworkMetric,
  CaptureStarted,
  CaptureStopped,
  CaptureError,
  // Pipeline self-reporting.
  PipelineNotice,
};

enum class Source {
  QtWebEngine,
  TShark,
  Pipeline,
};

// What the measurement describes.
enum class MeasurementScope {
  Tab,             // One browser tab.
  Navigation,      // One navigation within a tab.
  Browser,         // The whole browser process.
  CaptureInterface,  // All traffic seen on the capture interface (host-wide).
  Pipeline,        // The telemetry pipeline itself.
};

enum class Availability {
  Available,    // A value was measured.
  Unavailable,  // The source could not provide a value (not installed, not observed).
  Error,        // The source failed.
};

// How confidently the event is tied to a tab or navigation.
enum class AttributionQuality {
  Direct,         // Reported by Qt for this tab or navigation.
  Unattributed,   // Host-level observation; not tied to any tab.
  NotApplicable,  // Pipeline or capture lifecycle events.
};

struct Attribute {
  std::string key;
  std::string value;
};

struct TelemetryEvent {
  int schema_version{kTelemetrySchemaVersion};

  // Stamped by TelemetryPipeline::Publish.
  std::uint64_t event_id{0};
  std::string session_id;
  WallClock::time_point wall_time{};
  // Nanoseconds since the capture session started, from a monotonic clock.
  std::int64_t monotonic_ns{0};

  EventType type{EventType::PipelineNotice};
  Source source{Source::Pipeline};
  std::optional<std::uint64_t> tab_id;
  std::optional<std::uint64_t> navigation_id;

  // Metric identity and value. Lifecycle events may leave value empty.
  std::string metric;
  std::optional<double> value;
  std::string unit;

  MeasurementScope scope{MeasurementScope::Pipeline};
  Availability availability{Availability::Available};
  AttributionQuality attribution{AttributionQuality::NotApplicable};

  // Already-redacted descriptive fields (for example an origin+path URL).
  std::vector<Attribute> attributes;

  const std::string* FindAttribute(const std::string& key) const;
};

const char* ToString(EventType value);
const char* ToString(Source value);
const char* ToString(MeasurementScope value);
const char* ToString(Availability value);
const char* ToString(AttributionQuality value);

// One JSON object, no trailing newline. Non-finite values become null.
std::string ToJson(const TelemetryEvent& event);

// ISO-8601 UTC with millisecond precision, for example 2026-11-10T08:15:02.120Z.
std::string FormatUtc(WallClock::time_point time);

}  // namespace aafr::telemetry
