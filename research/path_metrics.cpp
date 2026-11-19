#include "research/path_metrics.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>

namespace aafr::research {
namespace {

void AddIssue(PathMetrics& m, PathIssue issue) {
  if (!m.Has(issue)) m.issues.push_back(issue);
}

bool InRange(double v, double low, double high) { return std::isfinite(v) && v >= low && v <= high; }
bool NonNegative(double v) { return std::isfinite(v) && v >= 0.0; }

Score Unavailable(std::string reason) { return Score{std::nullopt, std::move(reason)}; }

std::optional<double> Z(const NormalizationStatistics& s, NormalizedFeature f, double x, std::string& missing) {
  auto z = ZScore(s, f, x);
  if (!z && missing.empty()) missing = std::string("no valid normalization statistics for ") + ToString(f);
  return z;
}

}  // namespace

bool PathMetrics::Has(PathIssue issue) const {
  return std::find(issues.begin(), issues.end(), issue) != issues.end();
}

PathMetrics ComputePathMetrics(const std::vector<LinkObservation>& links) {
  PathMetrics m;
  if (links.empty()) {
    AddIssue(m, PathIssue::EmptyPath);
    return m;
  }
  std::set<std::string> visited{links.front().from};
  for (std::size_t i = 0; i < links.size(); ++i) {
    if (i > 0 && links[i].from != links[i - 1].to) AddIssue(m, PathIssue::Disconnected);
    if (!visited.insert(links[i].to).second) AddIssue(m, PathIssue::RepeatedNode);
  }
  if (m.Has(PathIssue::Disconnected) || m.Has(PathIssue::RepeatedNode)) return m;
  m.path_valid = true;
  m.hops = links.size();

  bool delay_ok = true, loss_ok = true, bandwidth_ok = true;
  double delay = 0.0;
  double delivery = 1.0;
  double bottleneck = std::numeric_limits<double>::infinity();
  for (const LinkObservation& link : links) {
    if (!link.delay_ms) { AddIssue(m, PathIssue::MissingDelay); delay_ok = false; }
    else if (!NonNegative(*link.delay_ms)) { AddIssue(m, PathIssue::InvalidDelay); delay_ok = false; }
    else delay += *link.delay_ms;

    if (!link.loss_ratio) { AddIssue(m, PathIssue::MissingLoss); loss_ok = false; }
    else if (!InRange(*link.loss_ratio, 0.0, 1.0)) { AddIssue(m, PathIssue::InvalidLoss); loss_ok = false; }
    else delivery *= 1.0 - *link.loss_ratio;

    bool link_bandwidth_ok = true;
    if (!link.capacity_mbps) { AddIssue(m, PathIssue::MissingCapacity); link_bandwidth_ok = false; }
    else if (!NonNegative(*link.capacity_mbps)) { AddIssue(m, PathIssue::InvalidCapacity); link_bandwidth_ok = false; }
    if (!link.utilization) { AddIssue(m, PathIssue::MissingUtilization); link_bandwidth_ok = false; }
    else if (!InRange(*link.utilization, 0.0, 1.0)) { AddIssue(m, PathIssue::InvalidUtilization); link_bandwidth_ok = false; }
    if (link_bandwidth_ok) bottleneck = std::min(bottleneck, *link.capacity_mbps * (1.0 - *link.utilization));
    else bandwidth_ok = false;
  }
  if (delay_ok) m.delay_ms = delay;
  if (loss_ok) m.loss_ratio = 1.0 - delivery;
  if (bandwidth_ok) m.bandwidth_mbps = bottleneck;
  if (m.bandwidth_mbps && m.loss_ratio) m.throughput_mbps = *m.bandwidth_mbps * (1.0 - *m.loss_ratio);
  return m;
}

Score QualityScore(const PathMetrics& m, const QualityScoreWeights& w) {
  if (!m.path_valid) return Unavailable("not a valid path");
  if (!m.delay_ms) return Unavailable("delay unavailable");
  if (!m.loss_ratio) return Unavailable("loss unavailable");
  if (!m.bandwidth_mbps) return Unavailable("bandwidth unavailable");
  if (!(*m.bandwidth_mbps > 0.0)) return Unavailable("bottleneck bandwidth is zero; 1/B is undefined");
  const double h = static_cast<double>(*m.hops);
  return Score{w.hop * h + w.inverse_bandwidth / *m.bandwidth_mbps + w.delay * *m.delay_ms + w.loss * *m.loss_ratio, {}};
}

Score EngineCost(const PathMetrics& m, const NormalizationStatistics& s, const EngineCostWeights& w) {
  if (!m.path_valid) return Unavailable("not a valid path");
  if (!m.delay_ms) return Unavailable("delay unavailable");
  if (!m.loss_ratio) return Unavailable("loss unavailable");
  if (!m.bandwidth_mbps) return Unavailable("bandwidth unavailable");
  if (!(*m.bandwidth_mbps > 0.0)) return Unavailable("bottleneck bandwidth is zero; 1/B is undefined");
  std::string missing;
  const auto zd = Z(s, NormalizedFeature::Delay, *m.delay_ms, missing);
  const auto zl = Z(s, NormalizedFeature::Loss, *m.loss_ratio, missing);
  const auto zb = Z(s, NormalizedFeature::InverseBandwidth, 1.0 / *m.bandwidth_mbps, missing);
  const auto zh = Z(s, NormalizedFeature::Hops, static_cast<double>(*m.hops), missing);
  if (!zd || !zl || !zb || !zh) return Unavailable(missing);
  return Score{w.delay * *zd + w.loss * *zl + w.inverse_bandwidth * *zb + w.hop * *zh, {}};
}

Score Goodness(const ArmOutcome& o, const NormalizationStatistics& s, const GoodnessWeights& w) {
  if (!o.hops) return Unavailable("hop count unavailable");
  if (!o.bandwidth_mbps || !NonNegative(*o.bandwidth_mbps)) return Unavailable("bandwidth unavailable or invalid");
  if (!o.mean_rtt_ms || !(std::isfinite(*o.mean_rtt_ms) && *o.mean_rtt_ms > 0.0)) {
    return Unavailable("mean RTT unavailable or invalid");
  }
  if (!o.delivery_ratio || !InRange(*o.delivery_ratio, 0.0, 1.0)) return Unavailable("delivery ratio unavailable or invalid");
  const double throughput = *o.bandwidth_mbps * *o.delivery_ratio;
  std::string missing;
  const auto zh = Z(s, NormalizedFeature::Hops, static_cast<double>(*o.hops), missing);
  const auto zb = Z(s, NormalizedFeature::Bandwidth, *o.bandwidth_mbps, missing);
  const auto zr = Z(s, NormalizedFeature::Rtt, *o.mean_rtt_ms, missing);
  const auto zt = Z(s, NormalizedFeature::Throughput, throughput, missing);
  if (!zh || !zb || !zr || !zt) return Unavailable(missing);
  return Score{w.hop * *zh + w.bandwidth * *zb + w.latency * *zr + w.throughput * *zt, {}};
}

Score GoodnessDifference(const Score& challenger, const Score& incumbent) {
  if (!challenger.value) return Unavailable("challenger goodness unavailable: " + challenger.unavailable_reason);
  if (!incumbent.value) return Unavailable("incumbent goodness unavailable: " + incumbent.unavailable_reason);
  return Score{*challenger.value - *incumbent.value, {}};
}

const char* ToString(PathIssue issue) {
  switch (issue) {
    case PathIssue::EmptyPath: return "empty_path";
    case PathIssue::Disconnected: return "disconnected";
    case PathIssue::RepeatedNode: return "repeated_node";
    case PathIssue::MissingDelay: return "missing_delay";
    case PathIssue::MissingLoss: return "missing_loss";
    case PathIssue::MissingCapacity: return "missing_capacity";
    case PathIssue::MissingUtilization: return "missing_utilization";
    case PathIssue::InvalidDelay: return "invalid_delay";
    case PathIssue::InvalidLoss: return "invalid_loss";
    case PathIssue::InvalidCapacity: return "invalid_capacity";
    case PathIssue::InvalidUtilization: return "invalid_utilization";
  }
  return "unknown";
}

}  // namespace aafr::research
