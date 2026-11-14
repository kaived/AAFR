#include "network/network_state/feature_extractor.h"

#include <algorithm>
#include <cmath>

#include "network/network_state/feature_normalizer.h"

namespace aafr::netstate {

std::optional<double> WindowStats::Mean() const {
  if (count == 0) return std::nullopt;
  return sum / static_cast<double>(count);
}

std::optional<double> WindowStats::WeightedMean() const { return SafeRatio(weighted_sum, weight_sum); }

std::optional<double> Median(std::vector<double> values) {
  if (values.empty()) return std::nullopt;
  std::sort(values.begin(), values.end());
  const std::size_t middle = values.size() / 2;
  if (values.size() % 2 == 1) return values[middle];
  return (values[middle - 1] + values[middle]) / 2.0;
}

RollingSeries::RollingSeries(std::size_t capacity) : capacity_(capacity == 0 ? 1 : capacity) {}

InsertOutcome RollingSeries::Insert(const Sample& sample, std::int64_t horizon_start_ns) {
  if (sample.t_ns < horizon_start_ns) return InsertOutcome::Late;
  if (sample.event_id != 0) {
    // Ids normally only grow, so a scan is needed only for a replayed id.
    if (sample.event_id <= max_event_id_) {
      for (const Sample& stored : samples_) {
        if (stored.event_id == sample.event_id) return InsertOutcome::Duplicate;
      }
    } else {
      max_event_id_ = sample.event_id;
    }
  }
  // Keep time order; equal timestamps stay in arrival order.
  auto position = samples_.end();
  while (position != samples_.begin() && std::prev(position)->t_ns > sample.t_ns) --position;
  samples_.insert(position, sample);
  ever_observed_ = true;
  while (samples_.size() > capacity_) {
    samples_.pop_front();
    ++dropped_for_capacity_;
  }
  return InsertOutcome::Accepted;
}

void RollingSeries::Prune(std::int64_t horizon_start_ns) {
  while (!samples_.empty() && samples_.front().t_ns < horizon_start_ns) samples_.pop_front();
}

void RollingSeries::Clear() {
  samples_.clear();
  max_event_id_ = 0;
  ever_observed_ = false;
}

WindowStats RollingSeries::Stats(std::int64_t now_ns, std::int64_t window_ns) const {
  WindowStats stats;
  std::vector<double> values;
  const std::int64_t start = now_ns - window_ns;
  for (const Sample& sample : samples_) {
    if (sample.t_ns <= start || sample.t_ns > now_ns) continue;
    if (stats.count == 0) {
      stats.min = stats.max = sample.value;
    } else {
      stats.min = std::min(stats.min, sample.value);
      stats.max = std::max(stats.max, sample.value);
    }
    ++stats.count;
    stats.sum += sample.value;
    stats.weight_sum += sample.weight;
    stats.weighted_sum += sample.value * sample.weight;
    stats.newest_ns = sample.t_ns;
    values.push_back(sample.value);
  }
  if (stats.count >= 2) {
    const double mean = stats.sum / static_cast<double>(stats.count);
    double squares = 0.0;
    for (const double value : values) squares += (value - mean) * (value - mean);
    stats.sample_stddev = std::sqrt(squares / static_cast<double>(stats.count - 1));
  }
  stats.median = Median(std::move(values));
  return stats;
}

std::optional<Series> SeriesForMetric(const std::string& metric) {
  static const std::pair<const char*, Series> kMap[] = {
      {"net.tcp.handshake_rtt.mean", Series::HandshakeRttMean},
      {"net.tcp.handshake_rtt.min", Series::HandshakeRttMin},
      {"net.tcp.handshake_rtt.max", Series::HandshakeRttMax},
      {"net.tcp.ack_rtt.mean", Series::AckDelayMean},
      {"net.tcp.ack_rtt.jitter", Series::AckDelayJitter},
      {"net.dns.response_time.mean", Series::DnsTimeMean},
      {"net.dns.response_time.max", Series::DnsTimeMax},
      {"net.dns.responses", Series::DnsResponses},
      {"net.tcp.retransmissions", Series::Retransmissions},
      {"net.throughput", Series::Throughput},
      {"net.packets", Series::Packets},
      {"net.bytes", Series::Bytes},
      {"net.tcp.packets", Series::TcpPackets},
      {"net.udp.packets", Series::UdpPackets},
      {"net.quic.packets", Series::QuicPackets},
  };
  for (const auto& entry : kMap) {
    if (metric == entry.first) return entry.second;
  }
  return std::nullopt;
}

bool IsNetworkSeries(Series series) {
  return series != Series::NavDuration && series != Series::NavSucceeded &&
         series != Series::NavFailed;
}

FeatureExtractor::FeatureExtractor(std::size_t capacity_per_series) {
  for (RollingSeries& series : series_) series = RollingSeries(capacity_per_series);
}

InsertOutcome FeatureExtractor::Add(Series series, const Sample& sample,
                                    std::int64_t horizon_start_ns) {
  return series_[static_cast<std::size_t>(series)].Insert(sample, horizon_start_ns);
}

void FeatureExtractor::NoteRejected(Series series, std::int64_t t_ns, const std::string& detail) {
  Rejection& rejection = rejections_[static_cast<std::size_t>(series)];
  rejection.t_ns = t_ns;
  rejection.detail = detail;
  rejection.set = true;
}

void FeatureExtractor::Prune(std::int64_t horizon_start_ns) {
  for (RollingSeries& series : series_) series.Prune(horizon_start_ns);
}

void FeatureExtractor::ClearNetwork() {
  for (std::size_t i = 0; i < series_.size(); ++i) {
    if (!IsNetworkSeries(static_cast<Series>(i))) continue;
    series_[i].Clear();
    rejections_[i] = Rejection{};
  }
}

void FeatureExtractor::ClearAll() {
  for (RollingSeries& series : series_) series.Clear();
  for (Rejection& rejection : rejections_) rejection = Rejection{};
}

std::size_t FeatureExtractor::StoredSamples() const {
  std::size_t total = 0;
  for (const RollingSeries& series : series_) total += series.Size();
  return total;
}

std::vector<FeatureDraft> FeatureExtractor::Extract(std::int64_t now_ns, std::int64_t window_ns,
                                                    std::uint64_t statistic_min_samples,
                                                    bool browser_observed) const {
  using telemetry::AttributionQuality;
  using telemetry::MeasurementScope;
  using telemetry::Source;

  std::array<WindowStats, static_cast<std::size_t>(Series::kCount)> stats;
  for (std::size_t i = 0; i < series_.size(); ++i) stats[i] = series_[i].Stats(now_ns, window_ns);
  const auto of = [&stats](Series series) -> const WindowStats& {
    return stats[static_cast<std::size_t>(series)];
  };
  const std::uint64_t statistic = std::max<std::uint64_t>(1, statistic_min_samples);

  std::vector<FeatureDraft> drafts;
  drafts.reserve(FeatureIds().size());

  // A feature built from one series (plus, for ratios, a second one).
  const auto add = [&](const char* id, Unit unit, ValueKind kind, Series series,
                       std::optional<double> value, std::uint64_t min_samples) -> FeatureDraft& {
    FeatureDraft draft;
    const WindowStats& base = of(series);
    const std::size_t index = static_cast<std::size_t>(series);
    draft.network = IsNetworkSeries(series);
    draft.feature.id = id;
    draft.feature.unit = unit;
    draft.feature.kind = kind;
    draft.feature.sample_count = base.count;
    draft.feature.value = base.count > 0 ? value : std::nullopt;
    draft.feature.source = draft.network ? Source::TShark : Source::QtWebEngine;
    draft.feature.scope = draft.network ? MeasurementScope::CaptureInterface : MeasurementScope::Browser;
    // Interface traffic belongs to the whole host; navigation events are
    // reported by Qt per tab and summarised here for the browser.
    draft.feature.attribution = draft.network ? AttributionQuality::Unattributed : AttributionQuality::Direct;
    draft.newest_ns = base.newest_ns;
    draft.ever_observed = series_[index].EverObserved();
    draft.min_samples = min_samples;
    const Rejection& rejection = rejections_[index];
    if (rejection.set && rejection.t_ns > now_ns - window_ns && rejection.t_ns <= now_ns) {
      draft.rejection = rejection.detail;
    }
    drafts.push_back(std::move(draft));
    return drafts.back();
  };
  const auto sum = [&](Series series) { return std::optional<double>(of(series).sum); };

  // --- Latency: TCP handshake round trip, per connection per interval. ---
  const WindowStats& rtt = of(Series::HandshakeRttMean);
  // Exact mean over connections: interval means weighted by connection count.
  add("rtt.handshake.mean_ms", Unit::Milliseconds, ValueKind::Measured, Series::HandshakeRttMean,
      rtt.WeightedMean(), statistic);
  // Median and variation are taken over interval means, because V0.3 does
  // not publish per-connection samples.
  add("rtt.handshake.median_ms", Unit::Milliseconds, ValueKind::Derived, Series::HandshakeRttMean,
      rtt.median, statistic);
  add("rtt.handshake.min_ms", Unit::Milliseconds, ValueKind::Measured, Series::HandshakeRttMin,
      of(Series::HandshakeRttMin).min, 1);
  add("rtt.handshake.max_ms", Unit::Milliseconds, ValueKind::Measured, Series::HandshakeRttMax,
      of(Series::HandshakeRttMax).max, 1);
  add("rtt.handshake.variation_ms", Unit::Milliseconds, ValueKind::Derived, Series::HandshakeRttMean,
      rtt.sample_stddev, std::max<std::uint64_t>(2, statistic));

  // --- ACK delay: TShark ACK RTT in both directions; not path latency. ---
  add("ack_delay.mean_ms", Unit::Milliseconds, ValueKind::Measured, Series::AckDelayMean,
      of(Series::AckDelayMean).WeightedMean(), statistic);
  add("ack_delay.jitter_ms", Unit::Milliseconds, ValueKind::Measured, Series::AckDelayJitter,
      of(Series::AckDelayJitter).WeightedMean(), statistic);

  // --- DNS ---
  add("dns.response_time.mean_ms", Unit::Milliseconds, ValueKind::Measured, Series::DnsTimeMean,
      of(Series::DnsTimeMean).WeightedMean(), statistic);
  add("dns.response_time.max_ms", Unit::Milliseconds, ValueKind::Measured, Series::DnsTimeMax,
      of(Series::DnsTimeMax).max, 1);
  add("dns.responses", Unit::Count, ValueKind::Measured, Series::DnsResponses,
      sum(Series::DnsResponses), 1);

  // --- Retransmissions: TShark's heuristic flag, not packet loss. ---
  add("tcp.retransmissions", Unit::Count, ValueKind::Measured, Series::Retransmissions,
      sum(Series::Retransmissions), 1);
  {
    const auto ratio = SafeRatio(of(Series::Retransmissions).sum, of(Series::TcpPackets).sum);
    FeatureDraft& draft = add("tcp.retransmission_ratio", Unit::Ratio, ValueKind::Derived,
                              Series::TcpPackets, ratio, statistic);
    if (of(Series::Retransmissions).count == 0) draft.feature.value.reset();
    if (!draft.feature.value && of(Series::TcpPackets).count > 0) {
      draft.note = "no TCP packets in the window, so a ratio is undefined";
    }
  }

  // --- Traffic volume ---
  {
    // Bytes over the summed interval lengths: a time-weighted mean rate.
    const WindowStats& bytes = of(Series::Bytes);
    const auto rate = BytesToMbps(bytes.sum, bytes.weight_sum);
    FeatureDraft& draft =
        add("throughput.mean_mbps", Unit::Mbps, ValueKind::Derived, Series::Bytes, rate, statistic);
    if (!draft.feature.value && bytes.count > 0) draft.note = "interval lengths were not reported";
  }
  add("throughput.peak_mbps", Unit::Mbps, ValueKind::Measured, Series::Throughput,
      of(Series::Throughput).max, 1);
  add("traffic.packets", Unit::Count, ValueKind::Measured, Series::Packets, sum(Series::Packets), 1);
  add("traffic.bytes", Unit::Bytes, ValueKind::Measured, Series::Bytes, sum(Series::Bytes), 1);
  add("tcp.packets", Unit::Count, ValueKind::Measured, Series::TcpPackets, sum(Series::TcpPackets), 1);
  add("udp.packets", Unit::Count, ValueKind::Measured, Series::UdpPackets, sum(Series::UdpPackets), 1);
  add("quic.packets", Unit::Count, ValueKind::Measured, Series::QuicPackets, sum(Series::QuicPackets), 1);
  // Connections with traffic, summed per interval: a connection active in
  // three intervals counts three times. Observed zero is a real zero, so the
  // evidence is the packet series, which reports every interval.
  add("tcp.connection_observations", Unit::Count, ValueKind::Measured, Series::Packets,
      rtt.weight_sum, 1);

  // --- Browser navigation (page-level; never an RTT) ---
  const WindowStats& duration = of(Series::NavDuration);
  add("nav.duration.median_ms", Unit::Milliseconds, ValueKind::Measured, Series::NavDuration,
      duration.median, statistic);
  add("nav.duration.mean_ms", Unit::Milliseconds, ValueKind::Measured, Series::NavDuration,
      duration.Mean(), statistic);
  const double succeeded = static_cast<double>(of(Series::NavSucceeded).count);
  const double failed = static_cast<double>(of(Series::NavFailed).count);
  // With the browser collector running, no navigation in a window is an
  // observed zero, not a missing value.
  const auto count_feature = [&](const char* id, Series series, double value) {
    FeatureDraft& draft = add(id, Unit::Count, ValueKind::Measured, series, value, 0);
    if (browser_observed) draft.feature.value = value;
    draft.ever_observed = draft.ever_observed || browser_observed;
  };
  count_feature("nav.succeeded", Series::NavSucceeded, succeeded);
  count_feature("nav.failed", Series::NavFailed, failed);
  {
    FeatureDraft draft;
    draft.network = false;
    draft.feature.id = "nav.failure_ratio";
    draft.feature.unit = Unit::Ratio;
    draft.feature.kind = ValueKind::Derived;
    draft.feature.sample_count = static_cast<std::uint64_t>(succeeded + failed);
    draft.feature.value = SafeRatio(failed, succeeded + failed);
    draft.feature.source = Source::QtWebEngine;
    draft.feature.scope = MeasurementScope::Browser;
    draft.feature.attribution = AttributionQuality::Direct;
    const auto newest_ok = of(Series::NavSucceeded).newest_ns;
    const auto newest_failed = of(Series::NavFailed).newest_ns;
    draft.newest_ns = newest_ok && newest_failed ? std::max(*newest_ok, *newest_failed)
                                                 : (newest_ok ? newest_ok : newest_failed);
    draft.ever_observed = Get(Series::NavSucceeded).EverObserved() || Get(Series::NavFailed).EverObserved();
    draft.min_samples = statistic;
    drafts.push_back(std::move(draft));
  }
  return drafts;
}

}  // namespace aafr::netstate
