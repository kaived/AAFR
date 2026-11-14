#include "network/network_state/network_state_service.h"

#include <algorithm>
#include <cstdio>
#include <utility>

namespace aafr::netstate {
namespace {

using algorithms::EligibilityResult;
using algorithms::EligibilityStatus;
using telemetry::Attribute;
using telemetry::EventType;

std::string Join(const std::vector<std::string>& items) {
  std::string text;
  for (const std::string& item : items) {
    if (!text.empty()) text += ',';
    text += item;
  }
  return text;
}

std::string StateSignature(const NetworkState& state) {
  const QualitySummary& q = state.quality;
  return std::string(ToString(state.validity)) + '|' + ToString(state.sources.capture) + '|' +
         std::to_string(q.valid) + '/' + std::to_string(q.missing) + '/' + std::to_string(q.stale) +
         '/' + std::to_string(q.insufficient_samples) + '/' + std::to_string(q.invalid) + '|' +
         state.capture_session_id;
}

std::string ConstraintsSignature(const NetworkConstraints& constraints) {
  std::string signature;
  for (const Constraint& constraint : constraints.constraints) {
    signature += constraint.id + '=' + ValueText(constraint) + '|' + ToString(constraint.validity) + ';';
  }
  return signature;
}

std::string EligibilitySignature(const EligibilityResult& result) {
  return std::string(ToString(result.status)) + ':' + algorithms::ReasonCodesText(result.reason_codes);
}

}  // namespace

std::uint64_t StateSnapshot::CountWithStatus(EligibilityStatus status) const {
  return static_cast<std::uint64_t>(
      std::count_if(eligibility.begin(), eligibility.end(),
                    [status](const EligibilityResult& result) { return result.status == status; }));
}

NetworkStateService::NetworkStateService(telemetry::TelemetryPipeline& pipeline, ServiceConfig config,
                                         algorithms::CapabilityRegistry registry)
    : pipeline_(pipeline),
      config_(std::move(config)),
      registry_(std::move(registry)),
      builder_(config_.builder, config_.environment),
      constraint_evaluator_(config_.constraints),
      eligibility_evaluator_(config_.eligibility) {}

NetworkStateService::~NetworkStateService() { Stop(); }

void NetworkStateService::Start() {
  {
    std::lock_guard<std::mutex> lock(worker_mutex_);
    if (worker_.joinable()) return;
    stop_requested_ = false;
  }
  ProcessOnce();
  std::lock_guard<std::mutex> lock(worker_mutex_);
  if (worker_.joinable()) return;
  running_ = true;
  worker_ = std::thread([this] { Run(); });
}

void NetworkStateService::Stop() {
  std::thread worker;
  {
    std::lock_guard<std::mutex> lock(worker_mutex_);
    stop_requested_ = true;
    worker = std::move(worker_);
  }
  wake_.notify_all();
  if (worker.joinable()) worker.join();
  running_ = false;
}

void NetworkStateService::Run() {
  std::unique_lock<std::mutex> lock(worker_mutex_);
  while (!stop_requested_) {
    // Wait first: Start() already ran a step.
    if (wake_.wait_for(lock, config_.tick, [this] { return stop_requested_; })) break;
    lock.unlock();
    ProcessOnce();
    lock.lock();
  }
}

std::shared_ptr<const StateSnapshot> NetworkStateService::Latest() const {
  std::lock_guard<std::mutex> lock(snapshot_mutex_);
  return latest_;
}

ServiceStats NetworkStateService::Stats() const {
  std::lock_guard<std::mutex> lock(step_mutex_);
  ServiceStats stats = stats_;
  stats.running = running_;
  stats.stored_samples = builder_.StoredSamples();
  return stats;
}

std::shared_ptr<const StateSnapshot> NetworkStateService::ProcessOnce() {
  std::lock_guard<std::mutex> lock(step_mutex_);
  const auto started = std::chrono::steady_clock::now();

  // Validation, normalization and aggregation, for new events only.
  std::map<std::string, std::uint64_t> rejections;
  std::size_t ingested = 0;
  while (ingested < config_.max_events_per_step) {
    const auto events = pipeline_.EventsAfter(
        last_event_id_, std::min<std::size_t>(1024, config_.max_events_per_step - ingested));
    if (events.empty()) break;
    for (const telemetry::TelemetryEvent& event : events) {
      if (event.event_id > last_event_id_ + 1) {
        stats_.events_missed += event.event_id - last_event_id_ - 1;
      }
      last_event_id_ = event.event_id;
      const IngestResult result = builder_.Ingest(event);
      if (result.outcome == IngestOutcome::RejectedInvalid) {
        ++rejections[result.metric + '|' + ToString(result.issue)];
      }
    }
    ingested += events.size();
  }
  stats_.events_seen += ingested;

  auto snapshot = std::make_shared<StateSnapshot>();
  snapshot->state = builder_.Build(pipeline_.MonotonicNowNs(), telemetry::WallClock::now());
  if (snapshot->state.telemetry_session_id.empty()) {
    snapshot->state.telemetry_session_id = pipeline_.SessionId();
  }
  snapshot->constraints = constraint_evaluator_.Evaluate(snapshot->state, config_.environment);
  snapshot->eligibility = eligibility_evaluator_.EvaluateAll(snapshot->state, snapshot->constraints,
                                                             registry_, config_.environment);
  snapshot->capabilities = registry_.All();

  PublishDiagnostics(*snapshot, rejections);
  {
    std::lock_guard<std::mutex> snapshot_lock(snapshot_mutex_);
    latest_ = snapshot;
  }
  ++stats_.steps;
  stats_.last_step_us = std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now() - started)
                            .count();
  stats_.max_step_us = std::max(stats_.max_step_us, stats_.last_step_us);
  return snapshot;
}

void NetworkStateService::Publish(EventType type, std::vector<Attribute> attributes) {
  telemetry::TelemetryEvent event;
  event.type = type;
  event.source = telemetry::Source::StatePipeline;
  event.scope = telemetry::MeasurementScope::Pipeline;
  event.attribution = telemetry::AttributionQuality::NotApplicable;
  event.attributes = std::move(attributes);
  pipeline_.Publish(std::move(event));
  ++stats_.diagnostics_published;
}

// Diagnostics are published when something changed, so a quiet system adds
// nothing to the bounded telemetry history.
void NetworkStateService::PublishDiagnostics(const StateSnapshot& snapshot,
                                             const std::map<std::string, std::uint64_t>& rejections) {
  const NetworkState& state = snapshot.state;
  const std::string state_id = std::to_string(state.state_id);

  for (const auto& rejection : rejections) {
    const auto split = rejection.first.find('|');
    Publish(EventType::FeatureValidationFailed,
            {{"metric", rejection.first.substr(0, split)},
             {"issue", rejection.first.substr(split + 1)},
             {"count", std::to_string(rejection.second)}});
  }

  const std::string state_signature = StateSignature(state);
  const std::int64_t interval_ns =
      std::chrono::duration_cast<std::chrono::nanoseconds>(config_.state_event_interval).count();
  const bool changed = !state_reported_ || state_signature != reported_state_signature_;
  const bool periodic = state.counters.accepted != reported_accepted_ &&
                        state.observed_monotonic_ns - reported_state_ns_ >= interval_ns;
  if (changed || periodic) {
    std::vector<Attribute> attributes = {
        {"state_id", state_id},
        {"validity", ToString(state.validity)},
        {"valid_features", std::to_string(state.quality.valid)},
        {"total_features", std::to_string(state.quality.Total())},
        {"capture", ToString(state.sources.capture)},
    };
    if (!state.capture_session_id.empty()) {
      attributes.push_back({"capture_session", state.capture_session_id});
    }
    if (state.freshness_ms) {
      char buffer[32];
      std::snprintf(buffer, sizeof(buffer), "%.0f", *state.freshness_ms);
      attributes.push_back({"freshness_ms", buffer});
    }
    Publish(EventType::NetworkStateUpdated, std::move(attributes));
    state_reported_ = true;
    reported_state_signature_ = state_signature;
    reported_state_ns_ = state.observed_monotonic_ns;
    reported_accepted_ = state.counters.accepted;
  }

  std::vector<std::string> stale;
  if (const WindowSnapshot* primary = state.PrimaryWindow()) {
    for (const Feature& feature : primary->features) {
      if (feature.quality == FeatureQuality::Stale) stale.push_back(feature.id);
    }
  }
  std::vector<std::string> newly_stale;
  for (const std::string& feature : stale) {
    if (std::find(reported_stale_.begin(), reported_stale_.end(), feature) == reported_stale_.end()) {
      newly_stale.push_back(feature);
    }
  }
  if (!newly_stale.empty()) {
    Publish(EventType::MeasurementStale, {{"state_id", state_id},
                                          {"window", state.primary_window},
                                          {"count", std::to_string(newly_stale.size())},
                                          {"features", Join(newly_stale)}});
  }
  reported_stale_ = std::move(stale);

  const NetworkConstraints& constraints = snapshot.constraints;
  const std::string constraints_signature = ConstraintsSignature(constraints);
  if (constraints_signature != reported_constraints_signature_) {
    reported_constraints_signature_ = constraints_signature;
    Publish(EventType::ConstraintEvaluated,
            {{"state_id", state_id},
             {"total", std::to_string(constraints.constraints.size())},
             {"measured", std::to_string(constraints.CountBySource(ConstraintSource::Measured))},
             {"configured", std::to_string(constraints.CountBySource(ConstraintSource::Configured))},
             {"derived", std::to_string(constraints.CountBySource(ConstraintSource::Derived))},
             {"unknown", std::to_string(constraints.CountByValidity(ConstraintValidity::Unknown))},
             {"invalid", std::to_string(constraints.CountByValidity(ConstraintValidity::Invalid))},
             {"conflicts", std::to_string(constraints.conflicts.size())}});
  }

  std::string eligibility_signature;
  for (const EligibilityResult& result : snapshot.eligibility) {
    eligibility_signature += std::string(algorithms::ToString(result.algorithm)) + '=' +
                             EligibilitySignature(result) + ';';
  }
  if (eligibility_signature != reported_eligibility_signature_) {
    reported_eligibility_signature_ = eligibility_signature;
    Publish(EventType::AlgorithmEligibilityEvaluated,
            {{"state_id", state_id},
             {"eligible", std::to_string(snapshot.CountWithStatus(EligibilityStatus::Eligible))},
             {"ineligible", std::to_string(snapshot.CountWithStatus(EligibilityStatus::Ineligible))},
             {"unknown", std::to_string(snapshot.CountWithStatus(EligibilityStatus::Unknown))},
             {"not_implemented",
              std::to_string(snapshot.CountWithStatus(EligibilityStatus::NotImplemented))},
             {"note", "eligibility diagnostic; not a runtime decision"}});
    for (const EligibilityResult& result : snapshot.eligibility) {
      const std::string algorithm = algorithms::ToString(result.algorithm);
      const std::string signature = EligibilitySignature(result);
      std::string& reported = reported_exclusions_[algorithm];
      if (reported == signature) continue;
      reported = signature;
      if (result.status == EligibilityStatus::Eligible) continue;
      Publish(EventType::AlgorithmExcluded,
              {{"algorithm", algorithm},
               {"status", algorithms::ToString(result.status)},
               {"reason_codes", algorithms::ReasonCodesText(result.reason_codes)},
               {"state_id", state_id}});
    }
  }
}

}  // namespace aafr::netstate
