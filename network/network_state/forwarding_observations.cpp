#include "network/network_state/forwarding_observations.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <numeric>
#include <sstream>

namespace aafr::netstate {

const char* ToString(ObservationSource value) {
  switch (value) {
    case ObservationSource::Gateway: return "gateway";
    case ObservationSource::Relay: return "relay";
    case ObservationSource::Runtime: return "runtime";
    case ObservationSource::Topology: return "topology";
  }
  return "unknown";
}

const char* ToString(ObservationKind value) {
  switch (value) {
    case ObservationKind::TunnelEstablished: return "tunnel_established";
    case ObservationKind::TunnelFailed: return "tunnel_failed";
    case ObservationKind::ConnectionCompleted: return "connection_completed";
    case ObservationKind::FirstByte: return "first_byte";
    case ObservationKind::RelayHop: return "relay_hop";
    case ObservationKind::LinkTransition: return "link_transition";
    case ObservationKind::ConfirmedSwitch: return "confirmed_switch";
  }
  return "unknown";
}

const char* ExpectedUnit(ObservationKind kind) {
  switch (kind) {
    case ObservationKind::TunnelEstablished:
    case ObservationKind::ConnectionCompleted:
    case ObservationKind::FirstByte:
    case ObservationKind::RelayHop: return "ms";
    case ObservationKind::TunnelFailed:
    case ObservationKind::LinkTransition:
    case ObservationKind::ConfirmedSwitch: return "event";
  }
  return "";
}

const char* ToString(Impairment value) {
  switch (value) {
    case Impairment::None: return "none";
    case Impairment::EmulatedEdgeDelay: return "emulated_edge_delay";
  }
  return "unknown";
}

const char* ToString(ObservationValidity value) {
  switch (value) {
    case ObservationValidity::Accepted: return "accepted";
    case ObservationValidity::NonFinite: return "non_finite";
    case ObservationValidity::NegativeDuration: return "negative_duration";
    case ObservationValidity::WrongUnit: return "wrong_unit";
    case ObservationValidity::MissingValue: return "missing_value";
    case ObservationValidity::UnexpectedValue: return "unexpected_value";
    case ObservationValidity::IntervalReversed: return "interval_reversed";
    case ObservationValidity::DurationExceedsInterval: return "duration_exceeds_interval";
    case ObservationValidity::FutureTimestamp: return "future_timestamp";
    case ObservationValidity::TooOld: return "too_old";
    case ObservationValidity::Duplicate: return "duplicate";
    case ObservationValidity::MissingId: return "missing_id";
    case ObservationValidity::SourceDisabled: return "source_disabled";
  }
  return "unknown";
}

const char* ToString(ForwardingScope value) {
  switch (value) {
    case ForwardingScope::ActivePath: return "active_path";
    case ForwardingScope::Edge: return "edge";
    case ForwardingScope::Gateway: return "gateway";
    case ForwardingScope::Topology: return "topology";
    case ForwardingScope::Runtime: return "runtime";
  }
  return "unknown";
}

const ForwardingFeature* ForwardingFeatureSet::Find(const std::string& feature_id) const {
  for (const ForwardingFeature& f : features) {
    if (f.feature.id == feature_id) return &f;
  }
  for (const ForwardingFeature& f : edges) {
    if (f.feature.id == feature_id) return &f;
  }
  return nullptr;
}

QualitySummary ForwardingFeatureSet::Quality() const {
  QualitySummary q;
  for (const ForwardingFeature& f : features) {
    switch (f.feature.quality) {
      case FeatureQuality::Valid: ++q.valid; break;
      case FeatureQuality::Missing: ++q.missing; break;
      case FeatureQuality::Stale: ++q.stale; break;
      case FeatureQuality::InsufficientSamples: ++q.insufficient_samples; break;
      case FeatureQuality::Invalid: ++q.invalid; break;
      case FeatureQuality::Unattributed: ++q.unattributed; break;
    }
  }
  return q;
}

const std::vector<std::string>& ForwardingFeatureIds() {
  static const std::vector<std::string> ids = {
      "fwd.gateway.connections",       "fwd.gateway.active_connections", "fwd.gateway.forwarded_mbps",
      "fwd.path.attempts",             "fwd.path.success_ratio",         "fwd.path.failure_ratio",
      "fwd.path.timeout_ratio",        "fwd.path.establish_ms.mean",     "fwd.path.establish_ms.p50",
      "fwd.path.establish_ms.p90",     "fwd.path.establish_ms.p95",      "fwd.path.establish_ms.p99",
      "fwd.path.establish_ms.min",     "fwd.path.establish_ms.max",      "fwd.path.establish_ms.std",
      "fwd.path.establish_jitter_ms",  "fwd.path.first_byte_ms.mean",    "fwd.path.goodput_mbps.mean",
      "fwd.path.goodput_fairness",     "fwd.topology.link_failure_events", "fwd.topology.link_recovery_events",
      "fwd.runtime.reroute_count",
  };
  return ids;
}

double Percentile(std::vector<double> values, double q) {
  if (values.empty()) return std::nan("");
  std::sort(values.begin(), values.end());
  const double rank = std::clamp(q, 0.0, 100.0) / 100.0 * static_cast<double>(values.size() - 1);
  const std::size_t lo = static_cast<std::size_t>(std::floor(rank));
  const std::size_t hi = std::min(values.size() - 1, lo + 1);
  return values[lo] + (values[hi] - values[lo]) * (rank - static_cast<double>(lo));
}

std::optional<double> MeanAbsoluteSuccessiveDifference(const std::vector<double>& ordered) {
  if (ordered.size() < 2) return std::nullopt;
  double sum = 0.0;
  for (std::size_t i = 1; i < ordered.size(); ++i) sum += std::fabs(ordered[i] - ordered[i - 1]);
  return sum / static_cast<double>(ordered.size() - 1);
}

std::int64_t ForwardingObservationStore::SteadyNowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

ForwardingObservationStore::ForwardingObservationStore(ForwardingWindowPolicy policy) : policy_(policy) {
  for (ObservationSource s : {ObservationSource::Gateway, ObservationSource::Relay, ObservationSource::Runtime,
                              ObservationSource::Topology}) {
    sources_[s].source = s;
  }
  counters_.capacity = policy_.capacity;
}

namespace {
constexpr double kNsPerMs = 1'000'000.0;

bool IsDurationKind(ObservationKind k) { return std::string(ExpectedUnit(k)) == "ms"; }

double MicrosSince(std::chrono::steady_clock::time_point start) {
  return std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count();
}
}  // namespace

ObservationValidity ForwardingObservationStore::Ingest(const ForwardingObservation& o, std::int64_t now_ns) {
  const auto started = std::chrono::steady_clock::now();
  const ObservationProvenance& p = o.provenance;
  ObservationValidity v = ObservationValidity::Accepted;
  std::lock_guard<std::mutex> lock(mutex_);
  SourceStatus& source = sources_[p.source_type];
  if (!source.enabled) {
    v = ObservationValidity::SourceDisabled;
  } else if (p.measurement_id.empty()) {
    v = ObservationValidity::MissingId;
  } else if (ids_.count(p.measurement_id)) {
    v = ObservationValidity::Duplicate;
  } else if (p.unit != ExpectedUnit(o.kind)) {
    v = ObservationValidity::WrongUnit;
  } else if (IsDurationKind(o.kind) && !o.value) {
    v = ObservationValidity::MissingValue;
  } else if (!IsDurationKind(o.kind) && o.value) {
    v = ObservationValidity::UnexpectedValue;
  } else if (o.value && !std::isfinite(*o.value)) {
    v = ObservationValidity::NonFinite;
  } else if (o.value && *o.value < 0.0) {
    v = ObservationValidity::NegativeDuration;
  } else if (p.interval_start_ns > p.monotonic_ns) {
    v = ObservationValidity::IntervalReversed;
  } else if (o.value && p.interval_start_ns < p.monotonic_ns &&
             *o.value > static_cast<double>(p.monotonic_ns - p.interval_start_ns) / kNsPerMs + 1.0) {
    v = ObservationValidity::DurationExceedsInterval;
  } else if (static_cast<double>(p.monotonic_ns - now_ns) > policy_.future_skew_ms * kNsPerMs) {
    v = ObservationValidity::FutureTimestamp;
  } else if (static_cast<double>(now_ns - p.monotonic_ns) > policy_.retention_ms * kNsPerMs) {
    v = ObservationValidity::TooOld;
  }
  if (v != ObservationValidity::Accepted) {
    ++counters_.rejected[ToString(v)];
  } else {
    ++counters_.accepted;
    if (p.config_generation != 0 && generation_ != 0 && p.config_generation < generation_) ++counters_.retired;
    observations_.push_back(o);
    ids_.insert(p.measurement_id);
    id_order_.push_back(p.measurement_id);
    if (!source.active_since_ns) source.active_since_ns = now_ns;
    source.last_seen_ns = std::max(source.last_seen_ns.value_or(now_ns), now_ns);
    // Bounds: capacity first, then retention (oldest first).
    while (observations_.size() > policy_.capacity) {
      observations_.pop_front();
      ++counters_.dropped_capacity;
    }
    while (!observations_.empty() &&
           static_cast<double>(now_ns - observations_.front().provenance.monotonic_ns) > policy_.retention_ms * kNsPerMs) {
      observations_.pop_front();
    }
    // Duplicate detection covers at most 2x capacity recent ids.
    while (id_order_.size() > 2 * policy_.capacity) {
      ids_.erase(id_order_.front());
      id_order_.pop_front();
    }
  }
  counters_.stored = observations_.size();
  counters_.last_ingest_us = MicrosSince(started);
  counters_.max_ingest_us = std::max(counters_.max_ingest_us, counters_.last_ingest_us);
  return v;
}

void ForwardingObservationStore::SetCurrentConfiguration(const std::string& graph_id, std::uint64_t graph_version,
                                                         std::uint64_t generation, const std::string& path,
                                                         Impairment impairment) {
  std::lock_guard<std::mutex> lock(mutex_);
  // A configuration generation never goes backwards: an older one cannot
  // replace a newer one (stale update rejection).
  if (generation < generation_) return;
  graph_id_ = graph_id;
  graph_version_ = graph_version;
  generation_ = generation;
  path_ = path;
  impairment_ = impairment;
}

void ForwardingObservationStore::Heartbeat(ObservationSource source, std::int64_t now_ns) {
  std::lock_guard<std::mutex> lock(mutex_);
  SourceStatus& s = sources_[source];
  if (!s.enabled) return;
  if (!s.active_since_ns) s.active_since_ns = now_ns;
  s.last_seen_ns = std::max(s.last_seen_ns.value_or(now_ns), now_ns);
}

void ForwardingObservationStore::SetSourceEnabled(ObservationSource source, bool enabled) {
  std::lock_guard<std::mutex> lock(mutex_);
  SourceStatus& s = sources_[source];
  if (s.enabled == enabled) return;
  s.enabled = enabled;
  // A re-enabled source starts a new observation interval: whatever it did
  // not see while disabled must not count as "zero observed".
  s.active_since_ns.reset();
  s.last_seen_ns.reset();
  if (source == ObservationSource::Gateway && !enabled) active_connections_.reset();
}

void ForwardingObservationStore::SetActiveConnections(std::size_t active, std::int64_t now_ns) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!sources_[ObservationSource::Gateway].enabled) return;
  active_connections_ = active;
  active_connections_ns_ = now_ns;
}

ObservationCounters ForwardingObservationStore::Counters() const {
  std::lock_guard<std::mutex> lock(mutex_);
  ObservationCounters c = counters_;
  c.stored = observations_.size();
  return c;
}

namespace {

struct Builder {
  const ForwardingWindowPolicy& policy;
  std::int64_t now_ns;
  std::int64_t window_start_ns;
  const std::map<ObservationSource, SourceStatus>& sources;
  std::uint64_t generation;
  std::uint64_t graph_version;
  std::string association;
  Impairment impairment;

  double AgeMs(std::int64_t t) const { return static_cast<double>(now_ns - t) / kNsPerMs; }

  ForwardingFeature Make(const std::string& id, Unit unit, ValueKind kind, ForwardingScope scope,
                         const std::string& source, const std::string& method, const std::string& formula,
                         std::uint64_t min_samples, std::int64_t stale_after_ms) const {
    ForwardingFeature f;
    f.feature.id = id;
    f.feature.unit = unit;
    f.feature.kind = kind;
    f.feature.quality = FeatureQuality::Missing;
    f.feature.source = telemetry::Source::Pipeline;
    f.feature.scope = telemetry::MeasurementScope::Pipeline;
    f.feature.attribution = telemetry::AttributionQuality::NotApplicable;
    f.scope = scope;
    f.association = scope == ForwardingScope::ActivePath ? association : std::string("all generations");
    f.source = source;
    f.method = method;
    f.formula = formula;
    f.window_ms = policy.window_ms;
    f.min_samples = min_samples;
    f.stale_after_ms = stale_after_ms;
    f.impairment = scope == ForwardingScope::ActivePath || scope == ForwardingScope::Edge ? impairment
                                                                                          : Impairment::None;
    f.config_generation = scope == ForwardingScope::ActivePath || scope == ForwardingScope::Edge ? generation : 0;
    f.graph_version = graph_version;
    return f;
  }

  // Source checks shared by every feature. False (quality set) when the
  // source cannot support a value.
  bool SourceUsable(ForwardingFeature& f, ObservationSource s, bool needs_full_window) const {
    const SourceStatus& st = sources.at(s);
    if (!st.enabled) {
      f.feature.quality = FeatureQuality::Missing;
      f.feature.quality_reason = std::string(ToString(s)) + " source disabled";
      return false;
    }
    if (!st.active_since_ns || !st.last_seen_ns) {
      f.feature.quality = FeatureQuality::Missing;
      f.feature.quality_reason = std::string(ToString(s)) + " source never observed";
      return false;
    }
    const double silent_ms = AgeMs(*st.last_seen_ns);
    if (silent_ms > static_cast<double>(policy.window_ms)) {
      f.feature.quality = FeatureQuality::Stale;
      f.feature.quality_reason = std::string(ToString(s)) + " source silent for " +
                                 std::to_string(static_cast<long long>(silent_ms)) + " ms";
      return false;
    }
    if (needs_full_window && *st.active_since_ns > window_start_ns) {
      f.feature.quality = FeatureQuality::InsufficientSamples;
      f.feature.quality_reason = std::string(ToString(s)) + " source observed " +
                                 std::to_string(static_cast<long long>(AgeMs(*st.active_since_ns))) + " of " +
                                 std::to_string(policy.window_ms) + " ms";
      return false;
    }
    return true;
  }

  // Sample-based statistic: needs min_samples and a recent newest sample.
  void SetSampled(ForwardingFeature& f, std::optional<double> value, std::uint64_t samples,
                  std::optional<std::int64_t> newest_ns) const {
    f.feature.sample_count = samples;
    if (newest_ns) f.feature.age_ms = AgeMs(*newest_ns);
    if (samples == 0 || !value) {
      f.feature.quality = FeatureQuality::Missing;
      f.feature.quality_reason = "no observation in the window";
      return;
    }
    if (!std::isfinite(*value) || *value < 0.0) {
      f.feature.quality = FeatureQuality::Invalid;
      f.feature.quality_reason = "statistic is not a finite nonnegative value";
      return;
    }
    f.feature.value = value;
    if (samples < f.min_samples) {
      f.feature.quality = FeatureQuality::InsufficientSamples;
      f.feature.quality_reason = std::to_string(samples) + " of " + std::to_string(f.min_samples) + " samples";
      return;
    }
    if (f.stale_after_ms > 0 && f.feature.age_ms && *f.feature.age_ms > static_cast<double>(f.stale_after_ms)) {
      f.feature.quality = FeatureQuality::Stale;
      f.feature.quality_reason = "newest sample older than " + std::to_string(f.stale_after_ms) + " ms";
      return;
    }
    f.feature.quality = FeatureQuality::Valid;
  }

  // Count over the full window (zero is a valid observation once the source
  // observed the whole window).
  void SetCount(ForwardingFeature& f, std::uint64_t count, std::optional<std::int64_t> newest_ns) const {
    f.feature.value = static_cast<double>(count);
    f.feature.sample_count = count;
    if (newest_ns) f.feature.age_ms = AgeMs(*newest_ns);
    f.feature.quality = FeatureQuality::Valid;
  }

  void SetRatio(ForwardingFeature& f, std::uint64_t numerator, std::uint64_t denominator,
                std::optional<std::int64_t> newest_ns) const {
    f.feature.sample_count = denominator;
    if (newest_ns) f.feature.age_ms = AgeMs(*newest_ns);
    if (denominator == 0) {
      f.feature.quality = FeatureQuality::Missing;
      f.feature.quality_reason = "zero denominator: no attempt in the window";
      return;
    }
    f.feature.value = static_cast<double>(numerator) / static_cast<double>(denominator);
    if (denominator < f.min_samples) {
      f.feature.quality = FeatureQuality::InsufficientSamples;
      f.feature.quality_reason = "denominator " + std::to_string(denominator) + " < " + std::to_string(f.min_samples);
      return;
    }
    f.feature.quality = FeatureQuality::Valid;
  }
};

std::string GatewayMethod() { return "gateway connection record, steady_clock"; }

}  // namespace

ForwardingFeatureSet ForwardingObservationStore::Snapshot(std::int64_t now_ns) const {
  const auto started = std::chrono::steady_clock::now();
  std::lock_guard<std::mutex> lock(mutex_);
  ForwardingFeatureSet set;
  set.version = ++version_;
  set.built_steady_ns = now_ns;
  set.graph_id = graph_id_;
  set.graph_version = graph_version_;
  set.config_generation = generation_;
  set.path = path_;
  set.impairment = impairment_;
  for (const auto& s : sources_) set.sources.push_back(s.second);

  const std::int64_t window_start = now_ns - static_cast<std::int64_t>(policy_.window_ms * kNsPerMs);
  Builder b{policy_, now_ns, window_start, sources_, generation_, graph_version_,
            "generation " + std::to_string(generation_) + (path_.empty() ? "" : " " + path_), impairment_};

  // ---- One pass over the window ----
  std::uint64_t gw_connections = 0;
  std::uint64_t gw_bytes = 0;
  std::optional<std::int64_t> gw_newest;
  std::vector<std::pair<std::int64_t, double>> establish;  // (completion ns, ms), current generation
  std::uint64_t attempts = 0, failures_path = 0, timeouts = 0;
  std::optional<std::int64_t> attempt_newest;
  std::vector<double> first_byte;
  std::optional<std::int64_t> first_byte_newest;
  std::vector<double> goodput;
  std::optional<std::int64_t> goodput_newest;
  std::uint64_t link_failures = 0, link_recoveries = 0, reroutes = 0;
  std::optional<std::int64_t> topo_newest, runtime_newest;
  std::map<std::string, std::vector<double>> hop;  // "from->to" -> ms
  std::map<std::string, std::int64_t> hop_newest;
  std::map<std::string, std::string> hop_source;
  std::optional<std::int64_t> newest_valid;

  for (const ForwardingObservation& o : observations_) {
    const ObservationProvenance& p = o.provenance;
    if (p.monotonic_ns < window_start || p.monotonic_ns > now_ns) continue;
    if (!sources_.at(p.source_type).enabled) continue;
    const bool current = generation_ != 0 && p.config_generation == generation_ &&
                         (graph_id_.empty() || p.graph_id.empty() || p.graph_id == graph_id_);
    switch (o.kind) {
      case ObservationKind::TunnelEstablished:
        ++gw_connections;
        gw_newest = std::max(gw_newest.value_or(p.monotonic_ns), p.monotonic_ns);
        if (current) {
          ++attempts;
          attempt_newest = std::max(attempt_newest.value_or(p.monotonic_ns), p.monotonic_ns);
          establish.emplace_back(p.monotonic_ns, *o.value);
        }
        break;
      case ObservationKind::TunnelFailed:
        ++gw_connections;
        gw_newest = std::max(gw_newest.value_or(p.monotonic_ns), p.monotonic_ns);
        // A destination failure (DNS, refused, forbidden) says nothing about
        // the relay path, so it is not a path attempt.
        if (current && (o.outcome == "relay_failure" || o.outcome == "timeout")) {
          ++attempts;
          ++failures_path;
          if (o.outcome == "timeout") ++timeouts;
          attempt_newest = std::max(attempt_newest.value_or(p.monotonic_ns), p.monotonic_ns);
        }
        break;
      case ObservationKind::ConnectionCompleted: {
        gw_bytes += o.bytes_up + o.bytes_down;
        gw_newest = std::max(gw_newest.value_or(p.monotonic_ns), p.monotonic_ns);
        if (current && *o.value >= policy_.goodput_min_duration_ms && o.bytes_up + o.bytes_down > 0) {
          goodput.push_back(static_cast<double>(o.bytes_up + o.bytes_down) * 8.0 / (*o.value * 1000.0));
          goodput_newest = std::max(goodput_newest.value_or(p.monotonic_ns), p.monotonic_ns);
        }
        break;
      }
      case ObservationKind::FirstByte:
        if (current) {
          first_byte.push_back(*o.value);
          first_byte_newest = std::max(first_byte_newest.value_or(p.monotonic_ns), p.monotonic_ns);
        }
        break;
      case ObservationKind::RelayHop:
        if (current && !p.edge_from.empty() && !p.edge_to.empty()) {
          const std::string key = p.edge_from + "->" + p.edge_to;
          hop[key].push_back(*o.value);
          hop_newest[key] = std::max(hop_newest.count(key) ? hop_newest[key] : p.monotonic_ns, p.monotonic_ns);
          hop_source[key] = p.source_component;
        }
        break;
      case ObservationKind::LinkTransition:
        if (o.outcome == "failure") ++link_failures;
        else if (o.outcome == "recovery") ++link_recoveries;
        topo_newest = std::max(topo_newest.value_or(p.monotonic_ns), p.monotonic_ns);
        break;
      case ObservationKind::ConfirmedSwitch:
        ++reroutes;
        runtime_newest = std::max(runtime_newest.value_or(p.monotonic_ns), p.monotonic_ns);
        break;
    }
  }
  std::sort(establish.begin(), establish.end());
  std::vector<double> est;
  for (const auto& e : establish) est.push_back(e.second);
  const std::optional<std::int64_t> est_newest =
      establish.empty() ? std::nullopt : std::optional<std::int64_t>(establish.back().first);

  const auto add = [&set, &newest_valid](ForwardingFeature f, std::optional<std::int64_t> newest) {
    if (f.feature.quality == FeatureQuality::Valid && newest) {
      newest_valid = std::max(newest_valid.value_or(*newest), *newest);
    }
    set.features.push_back(std::move(f));
  };

  // ---- Gateway scope ----
  {
    auto f = b.Make("fwd.gateway.connections", Unit::Count, ValueKind::Measured, ForwardingScope::Gateway, "gateway",
                    GatewayMethod(), "count of tunnel set-ups (established or failed) completed in the window", 0, 0);
    if (b.SourceUsable(f, ObservationSource::Gateway, true)) b.SetCount(f, gw_connections, gw_newest);
    add(std::move(f), gw_newest);
  }
  {
    auto f = b.Make("fwd.gateway.active_connections", Unit::Count, ValueKind::Measured, ForwardingScope::Gateway,
                    "gateway", "gateway status gauge", "connections open when last sampled", 0,
                    policy_.gauge_stale_after_ms);
    if (b.SourceUsable(f, ObservationSource::Gateway, false)) {
      if (!active_connections_) {
        f.feature.quality = FeatureQuality::Missing;
        f.feature.quality_reason = "gauge never sampled";
      } else {
        f.feature.value = static_cast<double>(*active_connections_);
        f.feature.sample_count = 1;
        f.feature.age_ms = b.AgeMs(active_connections_ns_);
        f.feature.quality = *f.feature.age_ms > static_cast<double>(policy_.gauge_stale_after_ms)
                                ? FeatureQuality::Stale
                                : FeatureQuality::Valid;
        if (f.feature.quality == FeatureQuality::Stale) f.feature.quality_reason = "gauge sample too old";
      }
    }
    add(std::move(f), active_connections_ ? std::optional<std::int64_t>(active_connections_ns_) : std::nullopt);
  }
  {
    auto f = b.Make("fwd.gateway.forwarded_mbps", Unit::Mbps, ValueKind::Derived, ForwardingScope::Gateway, "gateway",
                    GatewayMethod(),
                    "8 * (bytes both directions of connections that finished in the window) / window; "
                    "application-limited, not capacity",
                    0, 0);
    if (b.SourceUsable(f, ObservationSource::Gateway, true)) {
      f.feature.value = static_cast<double>(gw_bytes) * 8.0 / (static_cast<double>(policy_.window_ms) * 1000.0);
      f.feature.sample_count = gw_connections;
      if (gw_newest) f.feature.age_ms = b.AgeMs(*gw_newest);
      f.feature.quality = FeatureQuality::Valid;
    }
    add(std::move(f), gw_newest);
  }

  // ---- Active path (current generation) ----
  const bool have_generation = generation_ != 0;
  const auto path_feature = [&](const std::string& id, Unit unit, ValueKind kind, const std::string& formula,
                                std::uint64_t min_samples, std::int64_t stale_after) {
    auto f = b.Make(id, unit, kind, ForwardingScope::ActivePath, "gateway", GatewayMethod(), formula, min_samples,
                    stale_after);
    if (!have_generation) {
      f.feature.quality = FeatureQuality::Missing;
      f.feature.quality_reason = "no confirmed configuration";
      return std::make_pair(f, false);
    }
    const bool ok = b.SourceUsable(f, ObservationSource::Gateway, false);
    return std::make_pair(f, ok);
  };
  {
    auto [f, ok] = path_feature("fwd.path.attempts", Unit::Count, ValueKind::Measured,
                                "tunnel set-ups on this generation in the window (destination failures excluded)", 0, 0);
    if (ok && b.SourceUsable(f, ObservationSource::Gateway, true)) b.SetCount(f, attempts, attempt_newest);
    add(std::move(f), attempt_newest);
  }
  for (const auto& r : std::vector<std::tuple<std::string, std::uint64_t, std::string>>{
           {"fwd.path.success_ratio", attempts - failures_path, "established / attempts"},
           {"fwd.path.failure_ratio", failures_path, "(relay failures + timeouts) / attempts"},
           {"fwd.path.timeout_ratio", timeouts, "timeouts / attempts"}}) {
    auto [f, ok] = path_feature(std::get<0>(r), Unit::Ratio, ValueKind::Derived, std::get<2>(r),
                                policy_.min_denominator, 0);
    if (ok) b.SetRatio(f, std::get<1>(r), attempts, attempt_newest);
    add(std::move(f), attempt_newest);
  }
  const auto est_stat = [&](const std::string& id, const std::string& formula, std::uint64_t min_samples,
                            std::optional<double> value) {
    auto [f, ok] = path_feature(id, Unit::Milliseconds, ValueKind::Measured, formula, min_samples,
                                policy_.latency_stale_after_ms);
    if (ok) b.SetSampled(f, value, est.size(), est_newest);
    add(std::move(f), est_newest);
  };
  const bool any = !est.empty();
  const double mean = any ? std::accumulate(est.begin(), est.end(), 0.0) / est.size() : 0.0;
  double var = 0.0;
  for (double x : est) var += (x - mean) * (x - mean);
  est_stat("fwd.path.establish_ms.mean", "mean of tunnel set-up durations", policy_.min_samples_mean,
           any ? std::optional<double>(mean) : std::nullopt);
  est_stat("fwd.path.establish_ms.p50", "50th percentile (linear interpolation)", policy_.min_samples_p50,
           any ? std::optional<double>(Percentile(est, 50)) : std::nullopt);
  est_stat("fwd.path.establish_ms.p90", "90th percentile (linear interpolation)", policy_.min_samples_p90,
           any ? std::optional<double>(Percentile(est, 90)) : std::nullopt);
  est_stat("fwd.path.establish_ms.p95", "95th percentile (linear interpolation)", policy_.min_samples_p95,
           any ? std::optional<double>(Percentile(est, 95)) : std::nullopt);
  est_stat("fwd.path.establish_ms.p99", "99th percentile (linear interpolation)", policy_.min_samples_p99,
           any ? std::optional<double>(Percentile(est, 99)) : std::nullopt);
  est_stat("fwd.path.establish_ms.min", "minimum", policy_.min_samples_mean,
           any ? std::optional<double>(*std::min_element(est.begin(), est.end())) : std::nullopt);
  est_stat("fwd.path.establish_ms.max", "maximum", policy_.min_samples_mean,
           any ? std::optional<double>(*std::max_element(est.begin(), est.end())) : std::nullopt);
  est_stat("fwd.path.establish_ms.std", "population standard deviation (numpy.std)", policy_.min_samples_mean,
           any ? std::optional<double>(std::sqrt(var / est.size())) : std::nullopt);
  {
    auto [f, ok] = path_feature("fwd.path.establish_jitter_ms", Unit::Milliseconds, ValueKind::Derived,
                                "mean |d_i - d_(i-1)| of set-up durations ordered by completion (simulator's jitter formula)",
                                policy_.min_samples_mean, policy_.latency_stale_after_ms);
    if (ok) {
      f.feature.sample_count = est.size();
      const auto j = MeanAbsoluteSuccessiveDifference(est);
      b.SetSampled(f, j, est.size(), est_newest);
    }
    add(std::move(f), est_newest);
  }
  {
    auto [f, ok] = path_feature("fwd.path.first_byte_ms.mean", Unit::Milliseconds, ValueKind::Measured,
                                "mean of request-forwarded -> first response byte (plain HTTP only)",
                                policy_.min_samples_mean, policy_.latency_stale_after_ms);
    if (ok) {
      const double m = first_byte.empty() ? 0.0
                                          : std::accumulate(first_byte.begin(), first_byte.end(), 0.0) / first_byte.size();
      b.SetSampled(f, first_byte.empty() ? std::nullopt : std::optional<double>(m), first_byte.size(),
                   first_byte_newest);
    }
    add(std::move(f), first_byte_newest);
  }
  {
    auto [f, ok] = path_feature("fwd.path.goodput_mbps.mean", Unit::Mbps, ValueKind::Derived,
                                "mean over finished connections of 8 * bytes / duration (application-limited; "
                                "includes idle keep-alive time)",
                                policy_.min_samples_mean, policy_.latency_stale_after_ms);
    if (ok) {
      const double m = goodput.empty() ? 0.0 : std::accumulate(goodput.begin(), goodput.end(), 0.0) / goodput.size();
      b.SetSampled(f, goodput.empty() ? std::nullopt : std::optional<double>(m), goodput.size(), goodput_newest);
    }
    add(std::move(f), goodput_newest);
  }
  {
    auto [f, ok] = path_feature("fwd.path.goodput_fairness", Unit::Ratio, ValueKind::Derived,
                                "Jain's index over per-connection goodput: (sum x)^2 / (n * sum x^2)",
                                policy_.min_samples_mean, policy_.latency_stale_after_ms);
    if (ok) {
      std::optional<double> jain;
      double s = 0.0, s2 = 0.0;
      for (double x : goodput) {
        s += x;
        s2 += x * x;
      }
      if (!goodput.empty() && s2 > 0.0) jain = std::clamp(s * s / (goodput.size() * s2), 0.0, 1.0);
      b.SetSampled(f, jain, goodput.size(), goodput_newest);
    }
    add(std::move(f), goodput_newest);
  }

  // ---- Topology and runtime dynamics (full window required) ----
  {
    auto f = b.Make("fwd.topology.link_failure_events", Unit::Count, ValueKind::Derived, ForwardingScope::Topology,
                    "forwarding_graph", "graph snapshot comparison", "effective edge availability true -> false in the window",
                    0, 0);
    if (b.SourceUsable(f, ObservationSource::Topology, true)) b.SetCount(f, link_failures, topo_newest);
    add(std::move(f), topo_newest);
  }
  {
    auto f = b.Make("fwd.topology.link_recovery_events", Unit::Count, ValueKind::Derived, ForwardingScope::Topology,
                    "forwarding_graph", "graph snapshot comparison", "effective edge availability false -> true in the window",
                    0, 0);
    if (b.SourceUsable(f, ObservationSource::Topology, true)) b.SetCount(f, link_recoveries, topo_newest);
    add(std::move(f), topo_newest);
  }
  {
    auto f = b.Make("fwd.runtime.reroute_count", Unit::Count, ValueKind::Derived, ForwardingScope::Runtime, "runtime",
                    "runtime switch counter", "confirmed configuration switches in the window", 0, 0);
    if (b.SourceUsable(f, ObservationSource::Runtime, true)) b.SetCount(f, reroutes, runtime_newest);
    add(std::move(f), runtime_newest);
  }

  // ---- Edges crossed by streams of the current generation ----
  if (have_generation && sources_.at(ObservationSource::Relay).enabled) {
    for (const auto& h : hop) {
      auto f = b.Make("fwd.edge." + h.first + ".hop_ms", Unit::Milliseconds, ValueKind::Measured, ForwardingScope::Edge,
                      hop_source[h.first], "relay StreamTraversal opened_ns, steady_clock",
                      "mean of (TUN request reached edge_to) - (reached edge_from)", policy_.min_samples_mean,
                      policy_.latency_stale_after_ms);
      f.association = "edge " + h.first + " (generation " + std::to_string(generation_) + ")";
      const double m = std::accumulate(h.second.begin(), h.second.end(), 0.0) / h.second.size();
      b.SetSampled(f, m, h.second.size(), hop_newest[h.first]);
      if (f.feature.quality == FeatureQuality::Valid) {
        newest_valid = std::max(newest_valid.value_or(hop_newest[h.first]), hop_newest[h.first]);
      }
      set.edges.push_back(std::move(f));
    }
  }

  if (newest_valid) set.last_valid_measurement_age_ms = b.AgeMs(*newest_valid);
  counters_.stored = observations_.size();
  counters_.last_snapshot_us = MicrosSince(started);
  counters_.max_snapshot_us = std::max(counters_.max_snapshot_us, counters_.last_snapshot_us);
  set.counters = counters_;
  return set;
}

}  // namespace aafr::netstate
