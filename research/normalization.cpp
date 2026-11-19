#include "research/normalization.h"

#include <cmath>

namespace aafr::research {

bool WelfordAccumulator::Add(double value) {
  if (!std::isfinite(value)) return false;
  ++count_;
  const double delta = value - mean_;
  mean_ += delta / static_cast<double>(count_);
  m2_ += delta * (value - mean_);
  return true;
}

std::optional<double> WelfordAccumulator::Variance(int ddof) const {
  if (ddof < 0 || count_ <= static_cast<std::size_t>(ddof)) return std::nullopt;
  return m2_ / static_cast<double>(count_ - static_cast<std::size_t>(ddof));
}

std::optional<FeatureStatistics> Freeze(const WelfordAccumulator& accumulator, int ddof) {
  if (ddof != 0 && ddof != 1) return std::nullopt;
  const auto variance = accumulator.Variance(ddof);
  if (!variance) return std::nullopt;
  FeatureStatistics s;
  s.count = accumulator.count();
  s.mean = accumulator.mean();
  s.standard_deviation = std::sqrt(*variance);
  s.ddof = ddof;
  return s;
}

std::vector<std::string> NormalizationStatistics::Validate() const {
  std::vector<std::string> issues;
  if (contract != kNormalizationContract) issues.push_back("contract '" + contract + "' is not " + kNormalizationContract);
  if (source.empty()) issues.push_back("source is not declared");
  for (const auto& [feature, s] : features) {
    const std::string name = ToString(feature);
    if (s.ddof != 0 && s.ddof != 1) issues.push_back(name + ": ddof is not declared as 0 or 1");
    if (s.count <= static_cast<std::size_t>(s.ddof < 0 ? 0 : s.ddof)) issues.push_back(name + ": too few rows");
    if (!std::isfinite(s.mean)) issues.push_back(name + ": mean is not finite");
    if (!(std::isfinite(s.standard_deviation) && s.standard_deviation > 0.0)) {
      issues.push_back(name + ": standard deviation is not positive");
    }
  }
  return issues;
}

std::optional<double> ZScore(const NormalizationStatistics& statistics, NormalizedFeature feature, double x) {
  if (statistics.contract != kNormalizationContract || !std::isfinite(x)) return std::nullopt;
  const auto it = statistics.features.find(feature);
  if (it == statistics.features.end()) return std::nullopt;
  const FeatureStatistics& s = it->second;
  if ((s.ddof != 0 && s.ddof != 1) || !std::isfinite(s.mean) ||
      !(std::isfinite(s.standard_deviation) && s.standard_deviation > 0.0)) {
    return std::nullopt;
  }
  return (x - s.mean) / s.standard_deviation;
}

const char* ToString(NormalizedFeature feature) {
  switch (feature) {
    case NormalizedFeature::Delay: return "delay_ms";
    case NormalizedFeature::Loss: return "loss_ratio";
    case NormalizedFeature::InverseBandwidth: return "inverse_bandwidth_per_mbps";
    case NormalizedFeature::Hops: return "hops";
    case NormalizedFeature::Bandwidth: return "bandwidth_mbps";
    case NormalizedFeature::Rtt: return "rtt_ms";
    case NormalizedFeature::Throughput: return "throughput_mbps";
  }
  return "unknown";
}

}  // namespace aafr::research
