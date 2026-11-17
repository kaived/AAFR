#pragma once

// V0.7.2 expert-model events: a bounded, thread-safe log that also publishes
// each event to the telemetry pipeline (source "expert_model"), so Developer
// Tools shows them in the Events tab. Plain C++17.

#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace aafr::telemetry {
class TelemetryPipeline;
}

namespace aafr::model {

enum class ModelEventType {
  ModelWorkerStarted,
  ModelLoaded,
  ModelReady,
  InferenceRequested,
  InferenceCompleted,
  InferenceTimedOut,
  InferenceFailed,
  RecommendationRejected,
  DeterministicFallbackActivated,
  ModelWorkerRestarted,
  StaleInferenceDiscarded,
  ModelWorkerStopped,
  ModelWorkerFailed,
};

const char* ToString(ModelEventType value);

struct ModelEvent {
  std::uint64_t sequence{0};
  std::int64_t monotonic_ns{0};
  ModelEventType type{ModelEventType::ModelWorkerStarted};
  std::string request_id;
  std::string detail;
};

class ModelEventLog {
 public:
  explicit ModelEventLog(std::size_t capacity = 256) : capacity_(capacity) {}
  void SetPipeline(telemetry::TelemetryPipeline* pipeline);
  void Emit(ModelEventType type, const std::string& detail, const std::string& request_id = "");
  std::vector<ModelEvent> Recent(std::size_t max_events) const;
  std::uint64_t Count(ModelEventType type) const;

 private:
  std::size_t capacity_;
  mutable std::mutex mutex_;
  std::deque<ModelEvent> events_;
  std::map<ModelEventType, std::uint64_t> counts_;
  std::uint64_t sequence_{0};
  telemetry::TelemetryPipeline* pipeline_{nullptr};
};

}  // namespace aafr::model
