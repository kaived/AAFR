#include "runtime/model/model_events.h"

#include <chrono>

#include "network/telemetry/telemetry_pipeline.h"

namespace aafr::model {

const char* ToString(ModelEventType value) {
  switch (value) {
    case ModelEventType::ModelWorkerStarted: return "ModelWorkerStarted";
    case ModelEventType::ModelLoaded: return "ModelLoaded";
    case ModelEventType::ModelReady: return "ModelReady";
    case ModelEventType::InferenceRequested: return "InferenceRequested";
    case ModelEventType::InferenceCompleted: return "InferenceCompleted";
    case ModelEventType::InferenceTimedOut: return "InferenceTimedOut";
    case ModelEventType::InferenceFailed: return "InferenceFailed";
    case ModelEventType::RecommendationRejected: return "RecommendationRejected";
    case ModelEventType::DeterministicFallbackActivated: return "DeterministicFallbackActivated";
    case ModelEventType::ModelWorkerRestarted: return "ModelWorkerRestarted";
    case ModelEventType::StaleInferenceDiscarded: return "StaleInferenceDiscarded";
    case ModelEventType::ModelWorkerStopped: return "ModelWorkerStopped";
    case ModelEventType::ModelWorkerFailed: return "ModelWorkerFailed";
  }
  return "unknown";
}

namespace {
telemetry::EventType TelemetryType(ModelEventType t) {
  using T = telemetry::EventType;
  switch (t) {
    case ModelEventType::ModelWorkerStarted: return T::ModelWorkerStarted;
    case ModelEventType::ModelLoaded: return T::ModelLoaded;
    case ModelEventType::ModelReady: return T::ModelReady;
    case ModelEventType::InferenceRequested: return T::InferenceRequested;
    case ModelEventType::InferenceCompleted: return T::InferenceCompleted;
    case ModelEventType::InferenceTimedOut: return T::InferenceTimedOut;
    case ModelEventType::InferenceFailed: return T::InferenceFailed;
    case ModelEventType::RecommendationRejected: return T::ModelRecommendationRejected;
    case ModelEventType::DeterministicFallbackActivated: return T::DeterministicFallbackActivated;
    case ModelEventType::ModelWorkerRestarted: return T::ModelWorkerRestarted;
    case ModelEventType::StaleInferenceDiscarded: return T::StaleInferenceDiscarded;
    case ModelEventType::ModelWorkerStopped: return T::ModelWorkerStopped;
    case ModelEventType::ModelWorkerFailed: return T::ModelWorkerFailed;
  }
  return T::PipelineNotice;
}
}  // namespace

void ModelEventLog::SetPipeline(telemetry::TelemetryPipeline* pipeline) {
  std::lock_guard<std::mutex> lock(mutex_);
  pipeline_ = pipeline;
}

void ModelEventLog::Emit(ModelEventType type, const std::string& detail, const std::string& request_id) {
  telemetry::TelemetryPipeline* pipeline;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    ModelEvent e;
    e.sequence = ++sequence_;
    e.monotonic_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                         std::chrono::steady_clock::now().time_since_epoch())
                         .count();
    e.type = type;
    e.request_id = request_id;
    e.detail = detail;
    events_.push_back(std::move(e));
    while (events_.size() > capacity_) events_.pop_front();
    ++counts_[type];
    pipeline = pipeline_;
  }
  if (pipeline) {
    telemetry::TelemetryEvent t;
    t.type = TelemetryType(type);
    t.source = telemetry::Source::ExpertModel;
    t.scope = telemetry::MeasurementScope::Pipeline;
    t.attribution = telemetry::AttributionQuality::NotApplicable;
    t.attributes = {{"event", ToString(type)}, {"detail", detail}};
    if (!request_id.empty()) t.attributes.push_back({"request_id", request_id});
    pipeline->Publish(std::move(t));
  }
}

std::vector<ModelEvent> ModelEventLog::Recent(std::size_t max_events) const {
  std::lock_guard<std::mutex> lock(mutex_);
  const std::size_t n = std::min(max_events, events_.size());
  return std::vector<ModelEvent>(events_.end() - static_cast<std::ptrdiff_t>(n), events_.end());
}

std::uint64_t ModelEventLog::Count(ModelEventType type) const {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto found = counts_.find(type);
  return found == counts_.end() ? 0 : found->second;
}

}  // namespace aafr::model
