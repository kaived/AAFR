#pragma once

// Rolling observation windows and feature extraction. Plain C++17, no clock:
// callers pass monotonic nanoseconds, so results are deterministic.
//
// V0.3 publishes one summary per capture interval, not one event per packet.
// Features are therefore aggregated over interval summaries, and each feature
// definition says exactly how (see FeatureExtractor::Extract).

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <vector>

#include "network/network_state/network_state.h"

namespace aafr::netstate {

struct Sample {
  std::int64_t t_ns{0};
  double value{0.0};
  // What the value stands for: the number of underlying measurements for a
  // mean, the interval length in ms for a byte count, otherwise 1.
  double weight{1.0};
  std::uint64_t event_id{0};  // 0 when the source assigns no id.
};

enum class InsertOutcome { Accepted, Duplicate, Late };

struct WindowStats {
  std::uint64_t count{0};
  double sum{0.0};
  double weight_sum{0.0};
  double weighted_sum{0.0};  // Sum of value * weight.
  double min{0.0};
  double max{0.0};
  std::optional<double> median;
  // Sample standard deviation (n - 1 denominator); needs two samples.
  std::optional<double> sample_stddev;
  std::optional<std::int64_t> newest_ns;

  std::optional<double> Mean() const;
  std::optional<double> WeightedMean() const;
};

// Median of a non-empty list; the mean of the two middle values for an even
// count. Returns nullopt for an empty list.
std::optional<double> Median(std::vector<double> values);

// Time-ordered samples of one series with a fixed capacity. When full, the
// oldest sample is dropped, so memory stays bounded whatever the event rate.
class RollingSeries {
 public:
  explicit RollingSeries(std::size_t capacity = 1024);

  // Samples older than `horizon_start_ns` are refused as Late. A sample whose
  // nonzero event id is already stored is refused as Duplicate. Out-of-order
  // samples inside the horizon are inserted at their time position.
  InsertOutcome Insert(const Sample& sample, std::int64_t horizon_start_ns);
  void Prune(std::int64_t horizon_start_ns);
  void Clear();

  // Statistics over samples with now - window < t <= now.
  WindowStats Stats(std::int64_t now_ns, std::int64_t window_ns) const;

  std::size_t Size() const { return samples_.size(); }
  std::size_t Capacity() const { return capacity_; }
  // True once any sample was accepted since the last Clear().
  bool EverObserved() const { return ever_observed_; }
  std::uint64_t DroppedForCapacity() const { return dropped_for_capacity_; }

 private:
  std::size_t capacity_;
  std::deque<Sample> samples_;
  std::uint64_t max_event_id_{0};
  bool ever_observed_{false};
  std::uint64_t dropped_for_capacity_{0};
};

enum class Series {
  HandshakeRttMean,  // Interval mean; weight = connections in the interval.
  HandshakeRttMin,
  HandshakeRttMax,
  AckDelayMean,      // Weight = ACK samples in the interval.
  AckDelayJitter,    // Weight = consecutive ACK-sample pairs (samples - 1).
  DnsTimeMean,       // Weight = DNS responses timed in the interval.
  DnsTimeMax,
  DnsResponses,
  Retransmissions,
  Throughput,
  Packets,
  Bytes,             // Weight = interval length in ms (0 when not reported).
  TcpPackets,
  UdpPackets,
  QuicPackets,
  NavDuration,       // Successful navigations only.
  NavSucceeded,
  NavFailed,
  kCount,
};

// The series a telemetry metric feeds, if any. Navigation series are fed by
// event type, not by metric name, so they are not listed here.
std::optional<Series> SeriesForMetric(const std::string& metric);
bool IsNetworkSeries(Series series);

// A feature value before its quality is judged.
struct FeatureDraft {
  Feature feature;  // id, value, unit, kind, sample_count, source, scope, attribution.
  std::optional<std::int64_t> newest_ns;
  bool network{true};          // Capture-interface feature (false: browser).
  bool ever_observed{false};   // Its inputs were seen at some point this capture.
  std::uint64_t min_samples{1};
  // Why there is no value although inputs exist, e.g. "no TCP packets".
  std::string note;
  // Set when inputs were rejected inside the window.
  std::string rejection;
};

class FeatureExtractor {
 public:
  explicit FeatureExtractor(std::size_t capacity_per_series = 1024);

  InsertOutcome Add(Series series, const Sample& sample, std::int64_t horizon_start_ns);
  // Records that a measurement for `series` failed validation at `t_ns`.
  void NoteRejected(Series series, std::int64_t t_ns, const std::string& detail);
  void Prune(std::int64_t horizon_start_ns);
  // Drops capture-interface samples: a new capture never mixes with the last.
  void ClearNetwork();
  void ClearAll();

  // One draft per feature id, in FeatureIds() order. `statistic_min_samples`
  // is the number of observations a mean, median or ratio needs;
  // `browser_observed` says whether any browser event was seen.
  std::vector<FeatureDraft> Extract(std::int64_t now_ns, std::int64_t window_ns,
                                    std::uint64_t statistic_min_samples,
                                    bool browser_observed) const;

  const RollingSeries& Get(Series series) const { return series_[static_cast<std::size_t>(series)]; }
  std::size_t StoredSamples() const;

 private:
  struct Rejection {
    std::int64_t t_ns{0};
    std::string detail;
    bool set{false};
  };
  std::array<RollingSeries, static_cast<std::size_t>(Series::kCount)> series_;
  std::array<Rejection, static_cast<std::size_t>(Series::kCount)> rejections_;
};

}  // namespace aafr::netstate
