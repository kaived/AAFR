#include "network/telemetry/telemetry_event.h"

#include <cmath>
#include <cstdio>
#include <ctime>
#include <iomanip>
#include <sstream>

namespace aafr::telemetry {
namespace {

void AppendEscaped(std::string& out, const std::string& input) {
  out += '"';
  for (const char ch : input) {
    switch (ch) {
      case '\\': out += "\\\\"; break;
      case '"': out += "\\\""; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (static_cast<unsigned char>(ch) < 0x20) {
          char buffer[7];
          std::snprintf(buffer, sizeof(buffer), "\\u%04x",
                        static_cast<unsigned>(static_cast<unsigned char>(ch)));
          out += buffer;
        } else {
          out += ch;
        }
    }
  }
  out += '"';
}

void AppendNumber(std::string& out, double value) {
  if (!std::isfinite(value)) {
    out += "null";
    return;
  }
  std::ostringstream stream;
  stream << std::setprecision(10) << value;
  out += stream.str();
}

void AppendField(std::string& out, const char* name, const std::string& value) {
  out += ",\"";
  out += name;
  out += "\":";
  AppendEscaped(out, value);
}

}  // namespace

const std::string* TelemetryEvent::FindAttribute(const std::string& key) const {
  for (const Attribute& attribute : attributes) {
    if (attribute.key == key) return &attribute.value;
  }
  return nullptr;
}

const char* ToString(EventType value) {
  switch (value) {
    case EventType::TabCreated: return "tab.created";
    case EventType::TabActivated: return "tab.activated";
    case EventType::TabClosed: return "tab.closed";
    case EventType::NavigationStarted: return "navigation.started";
    case EventType::NavigationSucceeded: return "navigation.succeeded";
    case EventType::NavigationFailed: return "navigation.failed";
    case EventType::NavigationStopped: return "navigation.stopped";
    case EventType::LoadProgress: return "navigation.progress";
    case EventType::UrlChanged: return "navigation.url_changed";
    case EventType::RendererTerminated: return "renderer.terminated";
    case EventType::NetworkMetric: return "network.metric";
    case EventType::CaptureStarted: return "capture.started";
    case EventType::CaptureStopped: return "capture.stopped";
    case EventType::CaptureError: return "capture.error";
    case EventType::PipelineNotice: return "pipeline.notice";
    case EventType::NetworkStateUpdated: return "state.updated";
    case EventType::FeatureValidationFailed: return "state.feature_validation_failed";
    case EventType::MeasurementStale: return "state.measurement_stale";
    case EventType::ConstraintEvaluated: return "constraints.evaluated";
    case EventType::AlgorithmEligibilityEvaluated: return "eligibility.evaluated";
    case EventType::AlgorithmExcluded: return "eligibility.algorithm_excluded";
    case EventType::AlgorithmExecutionStarted: return "algorithm.execution_started";
    case EventType::AlgorithmExecutionCompleted: return "algorithm.execution_completed";
    case EventType::AlgorithmExecutionFailed: return "algorithm.execution_failed";
    case EventType::CandidatePathRejected: return "algorithm.candidate_path_rejected";
    case EventType::ForwardingExecutionStarted: return "forwarding.execution_started";
    case EventType::ForwardingExecutionCompleted: return "forwarding.execution_completed";
    case EventType::ForwardingExecutionFailed: return "forwarding.execution_failed";
    case EventType::RuntimeInitialized: return "runtime.initialized";
    case EventType::RuntimeLifecycleChanged: return "runtime.lifecycle_changed";
    case EventType::NetworkChangeDetected: return "runtime.network_change_detected";
    case EventType::RuntimeEvaluationStarted: return "runtime.evaluation_started";
    case EventType::RecommendationGenerated: return "runtime.recommendation_generated";
    case EventType::ProbingStarted: return "runtime.probing_started";
    case EventType::ProbingCompleted: return "runtime.probing_completed";
    case EventType::StatisticalEvaluationCompleted: return "runtime.statistical_evaluation_completed";
    case EventType::AlgorithmRetained: return "runtime.algorithm_retained";
    case EventType::SwitchRequested: return "runtime.switch_requested";
    case EventType::SwitchCommitted: return "runtime.switch_committed";
    case EventType::SwitchFailed: return "runtime.switch_failed";
    case EventType::RecoveryStarted: return "runtime.recovery_started";
    case EventType::RecoveryCompleted: return "runtime.recovery_completed";
    case EventType::RuntimeEvaluationDiscarded: return "runtime.evaluation_discarded";
    case EventType::ModelWorkerStarted: return "model.worker_started";
    case EventType::ModelLoaded: return "model.loaded";
    case EventType::ModelReady: return "model.ready";
    case EventType::InferenceRequested: return "model.inference_requested";
    case EventType::InferenceCompleted: return "model.inference_completed";
    case EventType::InferenceTimedOut: return "model.inference_timed_out";
    case EventType::InferenceFailed: return "model.inference_failed";
    case EventType::ModelRecommendationRejected: return "model.recommendation_rejected";
    case EventType::DeterministicFallbackActivated: return "model.deterministic_fallback";
    case EventType::ModelWorkerRestarted: return "model.worker_restarted";
    case EventType::StaleInferenceDiscarded: return "model.stale_inference_discarded";
    case EventType::ModelWorkerStopped: return "model.worker_stopped";
    case EventType::ModelWorkerFailed: return "model.worker_failed";
  }
  return "unknown";
}

const char* ToString(Source value) {
  switch (value) {
    case Source::QtWebEngine: return "qt_webengine";
    case Source::TShark: return "tshark";
    case Source::Pipeline: return "pipeline";
    case Source::StatePipeline: return "state_pipeline";
    case Source::ForwardingEngine: return "forwarding_engine";
    case Source::AdaptiveRuntime: return "adaptive_runtime";
    case Source::ExpertModel: return "expert_model";
  }
  return "unknown";
}

const char* ToString(MeasurementScope value) {
  switch (value) {
    case MeasurementScope::Tab: return "tab";
    case MeasurementScope::Navigation: return "navigation";
    case MeasurementScope::Browser: return "browser";
    case MeasurementScope::CaptureInterface: return "capture_interface";
    case MeasurementScope::Pipeline: return "pipeline";
  }
  return "unknown";
}

const char* ToString(Availability value) {
  switch (value) {
    case Availability::Available: return "available";
    case Availability::Unavailable: return "unavailable";
    case Availability::Error: return "error";
  }
  return "unknown";
}

const char* ToString(AttributionQuality value) {
  switch (value) {
    case AttributionQuality::Direct: return "direct";
    case AttributionQuality::Unattributed: return "unattributed";
    case AttributionQuality::NotApplicable: return "not_applicable";
  }
  return "unknown";
}

std::string FormatUtc(WallClock::time_point time) {
  const auto since_epoch = time.time_since_epoch();
  const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(since_epoch);
  auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(since_epoch - seconds).count();
  std::time_t raw = static_cast<std::time_t>(seconds.count());
  if (millis < 0) {
    millis += 1000;
    --raw;
  }
  std::tm utc{};
#ifdef _WIN32
  if (gmtime_s(&utc, &raw) != 0) return "1970-01-01T00:00:00.000Z";
#else
  if (gmtime_r(&raw, &utc) == nullptr) return "1970-01-01T00:00:00.000Z";
#endif
  char buffer[32];
  std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
                utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday, utc.tm_hour,
                utc.tm_min, utc.tm_sec, static_cast<int>(millis));
  return buffer;
}

std::string ToJson(const TelemetryEvent& event) {
  std::string out;
  out.reserve(256);
  out += "{\"schema\":";
  out += std::to_string(event.schema_version);
  out += ",\"event_id\":";
  out += std::to_string(event.event_id);
  AppendField(out, "session_id", event.session_id);
  AppendField(out, "ts", FormatUtc(event.wall_time));
  out += ",\"monotonic_ns\":";
  out += std::to_string(event.monotonic_ns);
  AppendField(out, "type", ToString(event.type));
  AppendField(out, "source", ToString(event.source));
  out += ",\"tab_id\":";
  out += event.tab_id ? std::to_string(*event.tab_id) : "null";
  out += ",\"navigation_id\":";
  out += event.navigation_id ? std::to_string(*event.navigation_id) : "null";
  AppendField(out, "metric", event.metric);
  out += ",\"value\":";
  if (event.value) {
    AppendNumber(out, *event.value);
  } else {
    out += "null";
  }
  AppendField(out, "unit", event.unit);
  AppendField(out, "scope", ToString(event.scope));
  AppendField(out, "availability", ToString(event.availability));
  AppendField(out, "attribution", ToString(event.attribution));
  out += ",\"attributes\":{";
  bool first = true;
  for (const Attribute& attribute : event.attributes) {
    if (!first) out += ',';
    first = false;
    AppendEscaped(out, attribute.key);
    out += ':';
    AppendEscaped(out, attribute.value);
  }
  out += "}}";
  return out;
}

}  // namespace aafr::telemetry
