#pragma once

// Builds NetworkState snapshots from telemetry events, incrementally.
// Plain C++17. Not thread-safe: NetworkStateService serializes access.
//
// Ingest() runs validation, normalization and aggregation for one event in
// O(log n) to O(n) of one bounded series. Build() computes the features of
// each window from what is stored; nothing is recomputed from full history.
//
// Aggregation policy across sessions:
//  - A new telemetry session id discards everything.
//  - A new capture (CaptureStarted) discards all capture-interface samples:
//    the interface or the network may have changed, so two captures are never
//    averaged together. Browser samples are kept.
//  - A stopped or failed capture keeps its samples; they age into Stale.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "network/network_state/execution_environment.h"
#include "network/network_state/feature_extractor.h"
#include "network/network_state/feature_validator.h"
#include "network/network_state/measurement_quality.h"
#include "network/network_state/network_state.h"
#include "network/telemetry/telemetry_event.h"

namespace aafr::netstate {

struct WindowConfig {
  std::string name;
  std::chrono::milliseconds duration{0};
};

struct BuilderConfig {
  // Defaults are starting points for experiments, not universal constants.
  std::vector<WindowConfig> windows{
      {"short", std::chrono::seconds(10)},
      {"medium", std::chrono::seconds(60)},
      {"baseline", std::chrono::seconds(300)},
  };
  std::string primary_window{"medium"};
  QualityPolicy quality;
  // Observations a mean, median or ratio needs before it is Valid.
  std::uint64_t statistic_min_samples{3};
  // Upper bound per series; 1024 holds 300 s of 1 s intervals with room.
  std::size_t max_samples_per_series{1024};
};

// Problems with a configuration; empty when it is usable.
std::vector<std::string> ValidateConfig(const BuilderConfig& config);

enum class IngestOutcome { Accepted, Ignored, RejectedInvalid, Duplicate, Late };

struct IngestResult {
  IngestOutcome outcome{IngestOutcome::Ignored};
  ValidationIssue issue{ValidationIssue::None};
  std::string metric;
  std::string detail;
};

class NetworkStateBuilder {
 public:
  // An unusable configuration is replaced by the defaults; see ConfigIssues().
  explicit NetworkStateBuilder(BuilderConfig config = {},
                               ExecutionEnvironment environment = BrowserDefaultEnvironment());

  IngestResult Ingest(const telemetry::TelemetryEvent& event);

  // `now_monotonic_ns` is on the events' clock (ns since session start).
  NetworkState Build(std::int64_t now_monotonic_ns, telemetry::WallClock::time_point now_wall);

  const BuilderConfig& config() const { return config_; }
  const std::vector<std::string>& ConfigIssues() const { return config_issues_; }
  const IngestCounters& counters() const { return counters_; }
  std::size_t StoredSamples() const { return extractor_.StoredSamples(); }

 private:
  void ResetForSession(const std::string& session_id);
  void BeginCapture(const telemetry::TelemetryEvent* started);
  IngestResult IngestNetworkMetric(const telemetry::TelemetryEvent& event);
  IngestResult IngestNavigation(const telemetry::TelemetryEvent& event);
  IngestResult Count(IngestResult result);
  std::int64_t HorizonStart() const;

  BuilderConfig config_;
  std::vector<std::string> config_issues_;
  ExecutionEnvironment environment_;
  MeasurementQualityEvaluator quality_;
  FeatureExtractor extractor_;
  std::int64_t longest_window_ns_{0};

  std::string session_id_;
  std::uint64_t next_state_id_{1};
  std::int64_t latest_event_ns_{0};
  std::optional<std::int64_t> newest_measurement_ns_;
  TelemetrySources sources_;
  BrowserContext browser_;
  std::map<std::uint64_t, double> tab_progress_;
  std::vector<std::string> quic_versions_;
  IngestCounters counters_;
};

const char* ToString(IngestOutcome value);

}  // namespace aafr::netstate
