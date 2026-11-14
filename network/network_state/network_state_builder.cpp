#include "network/network_state/network_state_builder.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <set>
#include <sstream>

namespace aafr::netstate {
namespace {

using telemetry::EventType;
using telemetry::Source;
using telemetry::TelemetryEvent;

constexpr std::size_t kMaxTrackedTabs = 256;

// A nonnegative finite number from an event attribute, if present.
std::optional<double> AttributeNumber(const TelemetryEvent& event, const char* key) {
  const std::string* text = event.FindAttribute(key);
  if (!text || text->empty()) return std::nullopt;
  char* end = nullptr;
  const double value = std::strtod(text->c_str(), &end);
  if (end == text->c_str() || *end != '\0' || !std::isfinite(value) || value < 0.0) {
    return std::nullopt;
  }
  return value;
}

double WeightFor(Series series, const TelemetryEvent& event) {
  switch (series) {
    case Series::HandshakeRttMean:
      return std::max(1.0, AttributeNumber(event, "connections").value_or(1.0));
    case Series::AckDelayMean:
    case Series::DnsTimeMean:
      return std::max(1.0, AttributeNumber(event, "samples").value_or(1.0));
    case Series::AckDelayJitter:
      // Jitter averages consecutive differences: samples - 1 of them.
      return std::max(1.0, AttributeNumber(event, "samples").value_or(2.0) - 1.0);
    case Series::Bytes:
      return AttributeNumber(event, "interval_ms").value_or(0.0);
    default:
      return 1.0;
  }
}

std::vector<std::string> SplitCommas(const std::string& text) {
  std::vector<std::string> parts;
  std::istringstream stream(text);
  std::string part;
  while (std::getline(stream, part, ',')) {
    if (!part.empty()) parts.push_back(part);
  }
  return parts;
}

}  // namespace

std::vector<std::string> ValidateConfig(const BuilderConfig& config) {
  std::vector<std::string> issues;
  if (config.windows.empty()) issues.push_back("no observation window is configured");
  std::set<std::string> names;
  for (const WindowConfig& window : config.windows) {
    if (window.name.empty()) issues.push_back("an observation window has no name");
    if (window.duration.count() <= 0) {
      issues.push_back("window '" + window.name + "' has a non-positive duration");
    }
    if (!names.insert(window.name).second) {
      issues.push_back("window name '" + window.name + "' is used twice");
    }
  }
  if (names.count(config.primary_window) == 0) {
    issues.push_back("primary window '" + config.primary_window + "' is not configured");
  }
  if (config.quality.network_stale_after_ms <= 0) {
    issues.push_back("the stale limit must be positive");
  }
  if (config.max_samples_per_series == 0) issues.push_back("series capacity must be positive");
  return issues;
}

NetworkStateBuilder::NetworkStateBuilder(BuilderConfig config, ExecutionEnvironment environment)
    : config_(std::move(config)),
      config_issues_(ValidateConfig(config_)),
      environment_(std::move(environment)) {
  if (!config_issues_.empty()) config_ = BuilderConfig{};
  quality_ = MeasurementQualityEvaluator(config_.quality);
  extractor_ = FeatureExtractor(config_.max_samples_per_series);
  for (const WindowConfig& window : config_.windows) {
    longest_window_ns_ = std::max<std::int64_t>(
        longest_window_ns_, std::chrono::duration_cast<std::chrono::nanoseconds>(window.duration).count());
  }
}

std::int64_t NetworkStateBuilder::HorizonStart() const {
  return latest_event_ns_ - longest_window_ns_;
}

void NetworkStateBuilder::ResetForSession(const std::string& session_id) {
  if (!session_id_.empty()) ++counters_.telemetry_session_resets;
  session_id_ = session_id;
  extractor_.ClearAll();
  latest_event_ns_ = 0;
  newest_measurement_ns_.reset();
  sources_ = TelemetrySources{};
  browser_ = BrowserContext{};
  tab_progress_.clear();
  quic_versions_.clear();
  counters_.capture_sessions = 0;
}

void NetworkStateBuilder::BeginCapture(const TelemetryEvent* started) {
  extractor_.ClearNetwork();
  quic_versions_.clear();
  ++counters_.capture_sessions;
  sources_.capture = CaptureStatus::Active;
  sources_.capture_interface.clear();
  if (started) {
    if (const std::string* interface_name = started->FindAttribute("interface")) {
      sources_.capture_interface = *interface_name;
    }
  }
}

IngestResult NetworkStateBuilder::Count(IngestResult result) {
  switch (result.outcome) {
    case IngestOutcome::Accepted: ++counters_.accepted; break;
    case IngestOutcome::Ignored: ++counters_.ignored; break;
    case IngestOutcome::RejectedInvalid: ++counters_.rejected_invalid; break;
    case IngestOutcome::Duplicate: ++counters_.duplicates; break;
    case IngestOutcome::Late: ++counters_.late; break;
  }
  return result;
}

IngestResult NetworkStateBuilder::IngestNetworkMetric(const TelemetryEvent& event) {
  IngestResult result;
  result.metric = event.metric;
  const MetricContract* contract = FindMetricContract(event.metric);
  const auto series = SeriesForMetric(event.metric);
  if (!contract || !series) return result;  // Ignored: carries no feature input.

  // Metrics imply a running capture even if its start event was not seen
  // (for example evicted from telemetry history).
  if (sources_.capture != CaptureStatus::Active) BeginCapture(nullptr);

  const ValidationResult validation = ValidateMeasurement(event, *contract);
  if (!validation.Ok()) {
    extractor_.NoteRejected(*series, event.monotonic_ns, validation.detail);
    result.outcome = IngestOutcome::RejectedInvalid;
    result.issue = validation.issue;
    result.detail = validation.detail;
    return result;
  }
  Sample sample;
  sample.t_ns = event.monotonic_ns;
  sample.value = *validation.canonical_value;
  sample.weight = WeightFor(*series, event);
  sample.event_id = event.event_id;
  switch (extractor_.Add(*series, sample, HorizonStart())) {
    case InsertOutcome::Duplicate: result.outcome = IngestOutcome::Duplicate; return result;
    case InsertOutcome::Late: result.outcome = IngestOutcome::Late; return result;
    case InsertOutcome::Accepted: break;
  }
  if (!newest_measurement_ns_ || event.monotonic_ns >= *newest_measurement_ns_) {
    newest_measurement_ns_ = event.monotonic_ns;
    if (*series == Series::QuicPackets) {
      const std::string* versions = event.FindAttribute("quic_versions");
      quic_versions_ = versions ? SplitCommas(*versions) : std::vector<std::string>{};
    }
  }
  result.outcome = IngestOutcome::Accepted;
  return result;
}

IngestResult NetworkStateBuilder::IngestNavigation(const TelemetryEvent& event) {
  IngestResult result;
  result.metric = event.metric;
  if (event.type == EventType::NavigationStopped) {
    result.outcome = IngestOutcome::Accepted;  // Context only; no feature input.
    return result;
  }
  const Series outcome_series =
      event.type == EventType::NavigationSucceeded ? Series::NavSucceeded : Series::NavFailed;
  Sample outcome;
  outcome.t_ns = event.monotonic_ns;
  outcome.value = 1.0;
  outcome.event_id = event.event_id;
  switch (extractor_.Add(outcome_series, outcome, HorizonStart())) {
    case InsertOutcome::Duplicate: result.outcome = IngestOutcome::Duplicate; return result;
    case InsertOutcome::Late: result.outcome = IngestOutcome::Late; return result;
    case InsertOutcome::Accepted: break;
  }
  result.outcome = IngestOutcome::Accepted;
  if (!newest_measurement_ns_ || event.monotonic_ns > *newest_measurement_ns_) {
    newest_measurement_ns_ = event.monotonic_ns;
  }
  // A failed navigation's duration is time-to-failure, not a page load, so
  // only successful navigations feed the duration features. A navigation
  // whose start was not observed reports no duration; that is not an error.
  if (event.type != EventType::NavigationSucceeded || !event.value) return result;
  const ValidationResult validation =
      ValidateMeasurement(event, *FindMetricContract("navigation.duration"));
  if (!validation.Ok()) {
    extractor_.NoteRejected(Series::NavDuration, event.monotonic_ns, validation.detail);
    result.outcome = IngestOutcome::RejectedInvalid;
    result.issue = validation.issue;
    result.detail = validation.detail;
    return result;
  }
  Sample duration = outcome;
  duration.value = *validation.canonical_value;
  extractor_.Add(Series::NavDuration, duration, HorizonStart());
  return result;
}

IngestResult NetworkStateBuilder::Ingest(const TelemetryEvent& event) {
  // The pipeline's own notices and this pipeline's diagnostics are not
  // observations of the network or the browser.
  if (event.source == Source::Pipeline || event.source == Source::StatePipeline) {
    return Count(IngestResult{});
  }
  if (!event.session_id.empty() && event.session_id != session_id_) ResetForSession(event.session_id);
  latest_event_ns_ = std::max(latest_event_ns_, event.monotonic_ns);

  if (event.source == Source::TShark) {
    IngestResult result;
    switch (event.type) {
      case EventType::CaptureStarted:
        BeginCapture(&event);
        result.outcome = IngestOutcome::Accepted;
        return Count(result);
      case EventType::CaptureStopped:
        sources_.capture = CaptureStatus::Stopped;
        result.outcome = IngestOutcome::Accepted;
        return Count(result);
      case EventType::CaptureError:
        sources_.capture = CaptureStatus::Error;
        result.outcome = IngestOutcome::Accepted;
        return Count(result);
      case EventType::NetworkMetric:
        return Count(IngestNetworkMetric(event));
      default:
        return Count(result);
    }
  }

  // Browser events. Context from an event older than one already applied
  // would move the browser context backwards, so it is refused as late.
  if (browser_.last_event_monotonic_ns && event.monotonic_ns < *browser_.last_event_monotonic_ns &&
      event.type != EventType::NavigationSucceeded && event.type != EventType::NavigationFailed) {
    IngestResult late;
    late.outcome = IngestOutcome::Late;
    return Count(late);
  }
  sources_.browser_events = true;
  const bool newest = !browser_.last_event_monotonic_ns ||
                      event.monotonic_ns >= *browser_.last_event_monotonic_ns;
  if (newest) browser_.last_event_monotonic_ns = event.monotonic_ns;

  IngestResult result;
  switch (event.type) {
    case EventType::TabActivated:
      browser_.active_tab_id = event.tab_id;
      result.outcome = IngestOutcome::Accepted;
      break;
    case EventType::TabClosed:
      if (event.tab_id) {
        tab_progress_.erase(*event.tab_id);
        if (browser_.active_tab_id == event.tab_id) browser_.active_tab_id.reset();
      }
      result.outcome = IngestOutcome::Accepted;
      break;
    case EventType::NavigationStarted:
      if (event.tab_id) tab_progress_[*event.tab_id] = 0.0;
      result.outcome = IngestOutcome::Accepted;
      break;
    case EventType::LoadProgress: {
      result.metric = event.metric;
      const ValidationResult validation =
          ValidateMeasurement(event, *FindMetricContract("navigation.progress"));
      if (!validation.Ok()) {
        result.outcome = IngestOutcome::RejectedInvalid;
        result.issue = validation.issue;
        result.detail = validation.detail;
        break;
      }
      if (event.tab_id) tab_progress_[*event.tab_id] = *validation.canonical_value;
      result.outcome = IngestOutcome::Accepted;
      break;
    }
    case EventType::NavigationSucceeded:
    case EventType::NavigationFailed:
    case EventType::NavigationStopped:
      result = IngestNavigation(event);
      if (newest && result.outcome != IngestOutcome::Duplicate && result.outcome != IngestOutcome::Late) {
        browser_.last_navigation_tab_id = event.tab_id;
        browser_.last_navigation_outcome =
            event.type == EventType::NavigationSucceeded
                ? NavigationOutcome::Succeeded
                : (event.type == EventType::NavigationFailed ? NavigationOutcome::Failed
                                                             : NavigationOutcome::Stopped);
        if (event.type == EventType::NavigationSucceeded && event.tab_id) {
          tab_progress_[*event.tab_id] = 100.0;
        }
      }
      break;
    default:
      break;  // Ignored: tab.created, url changes, renderer termination.
  }
  // Tab ids are never reused, so closed tabs leave no entry; the cap guards
  // against a collector that never reports closures.
  while (tab_progress_.size() > kMaxTrackedTabs) tab_progress_.erase(tab_progress_.begin());
  return Count(result);
}

NetworkState NetworkStateBuilder::Build(std::int64_t now_monotonic_ns,
                                        telemetry::WallClock::time_point now_wall) {
  const std::int64_t now = std::max(now_monotonic_ns, latest_event_ns_);
  extractor_.Prune(now - longest_window_ns_);

  NetworkState state;
  state.state_id = next_state_id_++;
  state.telemetry_session_id = session_id_;
  if (counters_.capture_sessions > 0) {
    state.capture_session_id = session_id_ + "/c" + std::to_string(counters_.capture_sessions);
  }
  state.observed_at = now_wall;
  state.observed_monotonic_ns = now;
  state.primary_window = config_.primary_window;

  for (const WindowConfig& window : config_.windows) {
    WindowSnapshot snapshot;
    snapshot.name = window.name;
    snapshot.duration_ms = window.duration.count();
    const std::int64_t window_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(window.duration).count();
    const auto drafts = extractor_.Extract(now, window_ns, config_.statistic_min_samples,
                                           sources_.browser_events);
    snapshot.features.reserve(drafts.size());
    for (const FeatureDraft& draft : drafts) {
      snapshot.features.push_back(quality_.Classify(draft, now, sources_.capture));
    }
    state.windows.push_back(std::move(snapshot));
  }

  if (newest_measurement_ns_) {
    state.freshness_ms = static_cast<double>(now - *newest_measurement_ns_) / 1e6;
  }
  if (const WindowSnapshot* primary = state.PrimaryWindow()) {
    state.quality = MeasurementQualityEvaluator::Summarize(primary->features);
  }
  const std::uint64_t total = state.quality.Total();
  state.validity = state.quality.valid == 0
                       ? StateValidity::NoData
                       : (state.quality.valid == total ? StateValidity::Valid : StateValidity::Degraded);
  state.validity_reason = std::to_string(state.quality.valid) + " of " + std::to_string(total) +
                          " features usable in the '" + config_.primary_window + "' window";
  if (sources_.capture != CaptureStatus::Active) {
    state.validity_reason += std::string("; packet capture is ") + ToString(sources_.capture);
  }
  if (!sources_.browser_events) state.validity_reason += "; no browser events observed";

  state.sources = sources_;
  state.browser = browser_;
  if (browser_.active_tab_id) {
    const auto progress = tab_progress_.find(*browser_.active_tab_id);
    if (progress != tab_progress_.end()) {
      state.browser.active_tab_load_progress_percent = progress->second;
    }
  }
  if (environment_.topology) {
    state.execution.forwarding_topology_available = environment_.topology->available;
  }
  if (environment_.forwarding_operations) {
    std::vector<std::string> operations;
    for (const ForwardingOperation operation : *environment_.forwarding_operations) {
      operations.push_back(ToString(operation));
    }
    state.execution.supported_forwarding_operations = std::move(operations);
  }
  state.execution.computation_deadline_ms = environment_.computation_deadline_ms;
  state.execution.memory_budget_bytes = environment_.memory_budget_bytes;
  state.execution.cpu_budget_percent = environment_.cpu_budget_percent;
  state.quic_versions = quic_versions_;
  state.counters = counters_;
  return state;
}

const char* ToString(IngestOutcome value) {
  switch (value) {
    case IngestOutcome::Accepted: return "accepted";
    case IngestOutcome::Ignored: return "ignored";
    case IngestOutcome::RejectedInvalid: return "rejected_invalid";
    case IngestOutcome::Duplicate: return "duplicate";
    case IngestOutcome::Late: return "late";
  }
  return "unknown";
}

}  // namespace aafr::netstate
