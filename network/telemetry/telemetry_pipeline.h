#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "network/telemetry/jsonl_telemetry_writer.h"
#include "network/telemetry/telemetry_event.h"

namespace aafr::telemetry {

struct PipelineConfig {
  // Events retained in memory for the Developer Tools panel.
  std::size_t history_capacity{2000};
  // Optional session id; generated when empty.
  std::string session_id;
};

struct PipelineStats {
  std::uint64_t published{0};
  std::uint64_t evicted_from_history{0};
  std::uint64_t jsonl_written{0};
  std::uint64_t jsonl_dropped{0};
  bool jsonl_enabled{false};
  bool jsonl_healthy{false};
};

// Stamps, retains and forwards telemetry events. Publish() is O(1) under a
// short lock and never waits for disk I/O: JSONL output goes through the
// writer's bounded queue. History is a ring buffer, so memory stays bounded.
class TelemetryPipeline {
 public:
  explicit TelemetryPipeline(PipelineConfig config = {});
  ~TelemetryPipeline();

  TelemetryPipeline(const TelemetryPipeline&) = delete;
  TelemetryPipeline& operator=(const TelemetryPipeline&) = delete;

  // Assigns event_id, session_id, wall_time and monotonic_ns, then stores it.
  // Returns the assigned id.
  std::uint64_t Publish(TelemetryEvent event);

  // Events with id > after_id, oldest first, at most max_events.
  std::vector<TelemetryEvent> EventsAfter(std::uint64_t after_id, std::size_t max_events) const;

  // Latest event for each metric name (NetworkMetric and duration metrics).
  std::optional<TelemetryEvent> LatestMetric(const std::string& metric) const;
  std::map<std::string, TelemetryEvent> LatestMetrics() const;

  // Opt-in diagnostic output. Passing an empty directory disables it.
  bool EnableJsonl(const std::filesystem::path& directory, JsonlRetention retention = {});
  void DisableJsonl();
  // UTF-8 path of the active JSONL file, empty when disabled.
  std::string JsonlPath() const;

  const std::string& SessionId() const { return session_id_; }
  MonotonicClock::time_point SessionStart() const { return session_start_; }
  std::int64_t MonotonicNowNs() const;
  PipelineStats Stats() const;

  // Flushes and closes JSONL output. Further Publish calls still update history.
  void Shutdown();

  static std::string GenerateSessionId();

 private:
  const std::string session_id_;
  const MonotonicClock::time_point session_start_;
  const std::size_t history_capacity_;

  mutable std::mutex mutex_;
  std::uint64_t next_id_{1};
  std::deque<TelemetryEvent> history_;
  std::map<std::string, TelemetryEvent> latest_metrics_;
  std::uint64_t evicted_{0};
  std::unique_ptr<JsonlTelemetryWriter> writer_;
};

}  // namespace aafr::telemetry
