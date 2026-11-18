#include "runtime/adaptive/outcome_observer.h"

#include "runtime/adaptive/runtime_clock.h"

namespace aafr::runtime {

void RuntimeOutcomeObserver::Record(const OutcomeObservation& o) {
  std::lock_guard<std::mutex> lock(mutex_);
  ++counters_.observations;
  if (o.kind == OutcomeKind::HealthCheck) {
    ++counters_.health_checks;
    if (!o.sample.success) ++counters_.health_failures;
  } else if (o.kind == OutcomeKind::Application) {
    ++counters_.application_requests;
    if (!o.sample.success) ++counters_.application_failures;
  } else if (o.kind == OutcomeKind::BrowserStream) {
    ++counters_.browser_streams;
    if (!o.sample.success) ++counters_.browser_stream_failures;
  }
  if (o.decision_id.empty()) return;
  auto found = buckets_.find(o.decision_id);
  if (found == buckets_.end()) {
    order_.push_back(o.decision_id);
    while (order_.size() > max_decisions_) {
      buckets_.erase(order_.front());
      order_.pop_front();
    }
    found = buckets_.emplace(o.decision_id, Bucket{}).first;
  }
  Bucket& b = found->second;
  ++b.revision;
  // The observation window is fixed per decision; anything beyond the cap is
  // counted in the revision number but not stored.
  if (b.observations.size() < max_per_decision_) b.observations.push_back(o);
}

void RuntimeOutcomeObserver::RecordSwitchLatency(double ms) {
  std::lock_guard<std::mutex> lock(mutex_);
  counters_.switching_latency_ms.push_back(ms);
  if (counters_.switching_latency_ms.size() > 256) {
    counters_.switching_latency_ms.erase(counters_.switching_latency_ms.begin());
  }
}

void RuntimeOutcomeObserver::RecordActivationFailure() {
  std::lock_guard<std::mutex> lock(mutex_);
  ++counters_.activation_failures;
}

void RuntimeOutcomeObserver::RecordRecovery(bool succeeded) {
  std::lock_guard<std::mutex> lock(mutex_);
  ++(succeeded ? counters_.recoveries_succeeded : counters_.recoveries_failed);
}

std::optional<OutcomeRevision> RuntimeOutcomeObserver::Latest(const std::string& decision_id,
                                                             std::int64_t now_ns) const {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto found = buckets_.find(decision_id);
  if (found == buckets_.end() || found->second.observations.empty()) return std::nullopt;
  const Bucket& b = found->second;
  OutcomeRevision r;
  r.decision_id = decision_id;
  r.revision = b.revision;
  r.window_ms = window_ms_;
  const OutcomeObservation& first = b.observations.front();
  // Activation requests are observed before the configuration version is
  // issued (it exists only after confirmation), so take the version from
  // the first observation that carries one.
  for (const OutcomeObservation& o : b.observations) {
    if (o.config_version != 0) {
      r.config_version = o.config_version;
      break;
    }
  }
  r.algorithm = first.algorithm;
  r.network_state_id = first.network_state_id;
  r.first_observation_ns = first.monotonic_ns;
  std::vector<RequestSample> samples;
  for (const OutcomeObservation& o : b.observations) {
    if (NsToMs(o.monotonic_ns - first.monotonic_ns) > window_ms_) break;
    r.last_observation_ns = o.monotonic_ns;
    if (o.kind == OutcomeKind::HealthCheck) ++r.health_checks;
    if (o.kind == OutcomeKind::Application) ++r.application_requests;
    // Browser streams include the real destination's connect time, so they
    // are counted apart and kept out of the relay-request performance.
    if (o.kind == OutcomeKind::BrowserStream) {
      ++r.browser_streams;
      if (!o.sample.success) ++r.browser_stream_failures;
      continue;
    }
    samples.push_back(o.sample);
  }
  r.window_complete = NsToMs(now_ns - first.monotonic_ns) >= window_ms_;
  r.performance = ForwardingPerformanceEvaluator().Summarize(samples);
  return r;
}

OutcomeCounters RuntimeOutcomeObserver::Counters() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return counters_;
}

const char* ToString(OutcomeKind value) {
  switch (value) {
    case OutcomeKind::Activation: return "activation";
    case OutcomeKind::HealthCheck: return "health_check";
    case OutcomeKind::Application: return "application";
    case OutcomeKind::BrowserStream: return "browser_stream";
  }
  return "unknown";
}

}  // namespace aafr::runtime
