#include "network/telemetry/telemetry_pipeline.h"

#include <algorithm>
#include <cstdio>
#include <random>
#include <utility>

namespace aafr::telemetry {

TelemetryPipeline::TelemetryPipeline(PipelineConfig config)
    : session_id_(config.session_id.empty() ? GenerateSessionId() : config.session_id),
      session_start_(MonotonicClock::now()),
      history_capacity_(std::max<std::size_t>(1, config.history_capacity)) {}

TelemetryPipeline::~TelemetryPipeline() { Shutdown(); }

std::string TelemetryPipeline::GenerateSessionId() {
  std::random_device device;
  std::uniform_int_distribution<unsigned> distribution(0, 0xFFFF);
  const auto now = std::chrono::duration_cast<std::chrono::seconds>(
                       WallClock::now().time_since_epoch())
                       .count();
  char buffer[40];
  std::snprintf(buffer, sizeof(buffer), "s%llx-%04x%04x", static_cast<unsigned long long>(now),
                distribution(device), distribution(device));
  return buffer;
}

std::int64_t TelemetryPipeline::MonotonicNowNs() const {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(MonotonicClock::now() -
                                                              session_start_)
      .count();
}

std::uint64_t TelemetryPipeline::Publish(TelemetryEvent event) {
  event.schema_version = kTelemetrySchemaVersion;
  event.session_id = session_id_;
  event.wall_time = WallClock::now();
  event.monotonic_ns = MonotonicNowNs();

  std::lock_guard<std::mutex> lock(mutex_);
  event.event_id = next_id_++;
  if (!event.metric.empty() && event.value) latest_metrics_[event.metric] = event;
  if (writer_) writer_->Write(ToJson(event));
  const std::uint64_t id = event.event_id;
  history_.push_back(std::move(event));
  while (history_.size() > history_capacity_) {
    history_.pop_front();
    ++evicted_;
  }
  return id;
}

std::vector<TelemetryEvent> TelemetryPipeline::EventsAfter(std::uint64_t after_id,
                                                           std::size_t max_events) const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<TelemetryEvent> result;
  // Ids are contiguous and increasing, so locate the start directly.
  auto it = std::upper_bound(history_.begin(), history_.end(), after_id,
                             [](std::uint64_t id, const TelemetryEvent& e) { return id < e.event_id; });
  for (; it != history_.end() && result.size() < max_events; ++it) result.push_back(*it);
  return result;
}

std::optional<TelemetryEvent> TelemetryPipeline::LatestMetric(const std::string& metric) const {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto found = latest_metrics_.find(metric);
  if (found == latest_metrics_.end()) return std::nullopt;
  return found->second;
}

std::map<std::string, TelemetryEvent> TelemetryPipeline::LatestMetrics() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return latest_metrics_;
}

bool TelemetryPipeline::EnableJsonl(const std::filesystem::path& directory,
                                    JsonlRetention retention) {
  std::unique_ptr<JsonlTelemetryWriter> previous;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    previous = std::move(writer_);
  }
  previous.reset();  // Joins outside the lock.
  if (directory.empty()) return false;
  auto writer = std::make_unique<JsonlTelemetryWriter>(directory, "aafr-telemetry-" + session_id_,
                                                       retention);
  const bool healthy = writer->Healthy();
  std::lock_guard<std::mutex> lock(mutex_);
  if (healthy) writer_ = std::move(writer);
  return healthy;
}

void TelemetryPipeline::DisableJsonl() { EnableJsonl({}); }

std::string TelemetryPipeline::JsonlPath() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return writer_ ? writer_->ActivePath().u8string() : std::string();
}

PipelineStats TelemetryPipeline::Stats() const {
  std::lock_guard<std::mutex> lock(mutex_);
  PipelineStats stats;
  stats.published = next_id_ - 1;
  stats.evicted_from_history = evicted_;
  stats.jsonl_enabled = writer_ != nullptr;
  if (writer_) {
    stats.jsonl_written = writer_->Written();
    stats.jsonl_dropped = writer_->Dropped();
    stats.jsonl_healthy = writer_->Healthy();
  }
  return stats;
}

void TelemetryPipeline::Shutdown() {
  std::unique_ptr<JsonlTelemetryWriter> writer;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    writer = std::move(writer_);
  }
  if (writer) writer->Close();
}

}  // namespace aafr::telemetry
