#pragma once

// V0.7.4 forwarding observations: what the browser forwarding gateway, the
// relays, the runtime and the forwarding graph actually observed, with
// provenance, turned into windowed NetworkState features. Plain C++17.
//
//   gateway connection records, relay StreamTraversals, graph availability,
//   confirmed switches
//     -> ForwardingObservationStore::Ingest (validation, bounded, O(1) under a lock)
//     -> ForwardingObservationStore::Snapshot (windowed statistics + quality)
//     -> ForwardingFeatureSet, attached to the immutable NetworkState
//        (NetworkState::forwarding, feature ids "fwd.*")
//
// Rules this contract enforces:
//  - Every observation carries a provenance record (id, source component and
//    type, monotonic end time and interval, unit, connection, node or edge,
//    graph id and version, configuration generation, collection method and
//    whether an emulated impairment was active). Duplicated ids, non-finite
//    or negative durations, units that do not match the kind, intervals that
//    end before they start, and timestamps from the future are rejected.
//  - Time: every monotonic value is std::chrono::steady_clock nanoseconds
//    since the steady clock's epoch (the gateway's and the relays' clock).
//    Wall-clock time is carried for reporting only and never compared.
//  - Path features describe the configuration generation that is current
//    when the snapshot is built. Observations of a superseded generation stay
//    in the store but are never counted as current; nothing measured on one
//    path is copied to another path or to an edge that no stream crossed.
//  - A missing measurement stays missing. A count or a ratio is reported
//    only when its source observed the whole window (a ratio also needs a
//    minimum denominator); zero then means zero was observed.
//  - Relays may sleep each edge's configured delay (LoopbackOptions::
//    inject_edge_latency). Durations measured on such a path are real elapsed
//    time, but the condition was emulated: they are labelled
//    Impairment::EmulatedEdgeDelay and never presented as a natural network
//    condition. No configured delay value is ever copied into a feature.
//  - Nothing here stores a URL path, query, header value, cookie,
//    credential or payload: targets are host:port at most.

#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "network/network_state/network_state.h"

namespace aafr::netstate {

enum class ObservationSource { Gateway, Relay, Runtime, Topology };
const char* ToString(ObservationSource value);

enum class ObservationKind {
  // Gateway: one browser connection's relay tunnel set-up (value: ms from
  // tunnel request to the relays' confirmed TUNRSP OK, executor-measured).
  TunnelEstablished,
  // Gateway: a connection whose tunnel could not be established (no value).
  // outcome: relay_failure | timeout | destination_failure | rejected
  TunnelFailed,
  // Gateway: a finished connection (value: duration ms; bytes up/down set).
  ConnectionCompleted,
  // Gateway, plain HTTP: request forwarded -> first response byte (ms).
  FirstByte,
  // Relay: one hop of a tunnel (value: ms between the TUN request reaching
  // edge_from and reaching edge_to, both relays' own steady-clock records).
  RelayHop,
  // Topology: an effective link availability transition (outcome
  // "failure" or "recovery"); no value.
  LinkTransition,
  // Runtime: a confirmed configuration switch (a reroute); no value.
  ConfirmedSwitch,
};
const char* ToString(ObservationKind value);
// The unit an observation of this kind must carry ("ms" or "event").
const char* ExpectedUnit(ObservationKind kind);

enum class Impairment { None, EmulatedEdgeDelay };
const char* ToString(Impairment value);

struct ObservationProvenance {
  std::string measurement_id;     // Unique within the store.
  std::string source_component;   // e.g. "gateway", "relay:relay_a", "runtime", "forwarding_graph".
  ObservationSource source_type{ObservationSource::Gateway};
  std::int64_t monotonic_ns{0};        // steady_clock: end of the measured interval.
  std::int64_t interval_start_ns{0};   // steady_clock: start (== monotonic_ns for events).
  std::optional<telemetry::WallClock::time_point> wall_time;  // Reporting only.
  std::string unit;                    // "ms" | "event"
  std::string connection_id;
  std::string node;
  std::string edge_from;
  std::string edge_to;
  std::string graph_id;
  std::uint64_t graph_version{0};
  std::uint64_t config_generation{0};  // 0: not tied to a configuration.
  std::string path;                    // Pinned path text, e.g. "entry>relay_a>dest".
  std::string method;                  // How it was collected.
  Impairment impairment{Impairment::None};
};

struct ForwardingObservation {
  ObservationKind kind{ObservationKind::TunnelEstablished};
  ObservationProvenance provenance;
  std::optional<double> value;  // In provenance.unit; empty for events.
  std::string outcome;          // Kind-specific, see ObservationKind.
  std::uint64_t bytes_up{0};    // ConnectionCompleted: browser -> server.
  std::uint64_t bytes_down{0};  // ConnectionCompleted: server -> browser.
};

enum class ObservationValidity {
  Accepted,
  NonFinite,
  NegativeDuration,
  WrongUnit,
  MissingValue,      // A duration kind without a value.
  UnexpectedValue,   // An event kind with a value.
  IntervalReversed,  // interval_start_ns > monotonic_ns.
  DurationExceedsInterval,  // A duration longer than its own measured interval.
  FutureTimestamp,   // monotonic_ns beyond now + allowed skew.
  TooOld,            // Older than the retention window when ingested.
  Duplicate,         // measurement_id already seen.
  MissingId,
  SourceDisabled,    // The source was disabled (an experiment switch).
};
const char* ToString(ObservationValidity value);

enum class ForwardingScope { ActivePath, Edge, Gateway, Topology, Runtime };
const char* ToString(ForwardingScope value);

// One windowed forwarding feature. `feature` is the ordinary NetworkState
// feature view (id, value, unit, kind, quality, samples, age) so existing
// consumers (NetworkState::FindFeature, the change detector) read it
// unchanged; the remaining fields are the provenance of the aggregate.
struct ForwardingFeature {
  Feature feature;
  ForwardingScope scope{ForwardingScope::ActivePath};
  std::string association;  // e.g. "generation 3 entry>relay_a>dest", "edge relay_a->dest".
  std::string source;       // Source component(s).
  std::string method;       // Collection method of the inputs.
  std::string formula;      // How the value is computed from the observations.
  std::int64_t window_ms{0};
  std::uint64_t min_samples{0};
  std::int64_t stale_after_ms{0};  // 0: bounded by the window only.
  Impairment impairment{Impairment::None};
  std::uint64_t config_generation{0};
  std::uint64_t graph_version{0};
};

struct ObservationCounters {
  std::uint64_t accepted{0};
  std::map<std::string, std::uint64_t> rejected;  // ObservationValidity text -> count.
  std::uint64_t dropped_capacity{0};  // Oldest observations evicted by the bound.
  std::uint64_t retired{0};           // Accepted observations of a superseded generation.
  std::size_t stored{0};
  std::size_t capacity{0};
  double last_ingest_us{0.0};
  double max_ingest_us{0.0};
  double last_snapshot_us{0.0};
  double max_snapshot_us{0.0};
};

struct SourceStatus {
  ObservationSource source{ObservationSource::Gateway};
  bool enabled{true};
  // steady ns when the source was first / last seen alive (an observation
  // or a heartbeat); empty when never.
  std::optional<std::int64_t> active_since_ns;
  std::optional<std::int64_t> last_seen_ns;
};

// Immutable once built.
struct ForwardingFeatureSet {
  std::uint64_t version{0};          // Increases per snapshot; 0: never built.
  std::int64_t built_steady_ns{0};
  std::string clock_domain{"steady_clock"};
  std::string graph_id;
  std::uint64_t graph_version{0};
  std::uint64_t config_generation{0};  // The generation path features describe.
  std::string path;
  Impairment impairment{Impairment::None};
  std::vector<ForwardingFeature> features;  // Gateway, path, topology, runtime features.
  std::vector<ForwardingFeature> edges;     // Only edges a stream of the current generation crossed.
  std::vector<SourceStatus> sources;
  ObservationCounters counters;
  std::optional<double> last_valid_measurement_age_ms;

  const ForwardingFeature* Find(const std::string& feature_id) const;
  QualitySummary Quality() const;  // Over `features`.
};

// Feature ids, in display order (edge features are "fwd.edge.<from>-><to>.hop_ms").
const std::vector<std::string>& ForwardingFeatureIds();

struct ForwardingWindowPolicy {
  std::int64_t window_ms{30'000};         // Statistics window.
  std::int64_t retention_ms{120'000};     // Older observations are dropped.
  std::size_t capacity{4096};             // Bound on stored observations.
  std::int64_t future_skew_ms{1'000};
  // A latency statistic is stale when its newest sample is older than this.
  std::int64_t latency_stale_after_ms{15'000};
  std::int64_t gauge_stale_after_ms{5'000};
  // Minimum samples per statistic (empirical quantiles need enough points).
  std::uint64_t min_samples_mean{3};
  std::uint64_t min_samples_p50{5};
  std::uint64_t min_samples_p90{10};
  std::uint64_t min_samples_p95{20};
  std::uint64_t min_samples_p99{100};
  std::uint64_t min_denominator{5};
  // A connection shorter than this or with no bytes says nothing about goodput.
  double goodput_min_duration_ms{50.0};
};

class ForwardingObservationStore {
 public:
  explicit ForwardingObservationStore(ForwardingWindowPolicy policy = {});

  // Validates and stores one observation. Never blocks for longer than a
  // bounded deque push; safe from any thread (gateway workers).
  ObservationValidity Ingest(const ForwardingObservation& observation, std::int64_t now_steady_ns);
  // The configuration path features describe from now on (the runtime's
  // confirmed active configuration). Observations of other generations stay
  // stored but are not current.
  void SetCurrentConfiguration(const std::string& graph_id, std::uint64_t graph_version, std::uint64_t generation,
                               const std::string& path, Impairment impairment);
  // A source is alive (it observed, even if nothing happened).
  void Heartbeat(ObservationSource source, std::int64_t now_steady_ns);
  // Disable (or re-enable) a source; observations of a disabled source are
  // rejected as SourceDisabled and its features become missing.
  void SetSourceEnabled(ObservationSource source, bool enabled);
  // Gateway gauge: connections currently open.
  void SetActiveConnections(std::size_t active, std::int64_t now_steady_ns);

  ForwardingFeatureSet Snapshot(std::int64_t now_steady_ns) const;
  ObservationCounters Counters() const;
  const ForwardingWindowPolicy& policy() const { return policy_; }

  static std::int64_t SteadyNowNs();

 private:
  const ForwardingWindowPolicy policy_;
  mutable std::mutex mutex_;
  std::deque<ForwardingObservation> observations_;
  std::set<std::string> ids_;
  std::deque<std::string> id_order_;
  std::map<ObservationSource, SourceStatus> sources_;
  std::string graph_id_;
  std::uint64_t graph_version_{0};
  std::uint64_t generation_{0};
  std::string path_;
  Impairment impairment_{Impairment::None};
  std::optional<std::size_t> active_connections_;
  std::int64_t active_connections_ns_{0};
  mutable std::uint64_t version_{0};
  mutable ObservationCounters counters_;
};

// Order statistics used by the snapshot; exposed for tests. Linear
// interpolation between closest ranks (numpy.percentile's default).
double Percentile(std::vector<double> values, double q);
// RFC 3393-style jitter as the training simulator computes it: the mean
// absolute difference of consecutive values (ordered by completion time).
std::optional<double> MeanAbsoluteSuccessiveDifference(const std::vector<double>& ordered);

}  // namespace aafr::netstate
