#pragma once

// Frozen normalization statistics for the research scores, and Welford's
// online mean and variance used to compute them. Plain C++17, no I/O.
//
// Contract aafr.research.normalization.v1: z(x) = (x - mean) / sd, with the
// mean and standard deviation of each feature frozen from the training set.
// The statistics are computed once (WelfordAccumulator over the training
// rows) and then fixed; nothing here updates them from live observations,
// and nothing normalizes against the current candidate paths.
//
// The contract carries no built-in constants. Which training-set column
// feeds which feature, and the delta degrees of freedom of the standard
// deviation, are declared by whoever produces the statistics; z-scores are
// refused for a feature without statistics.

#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace aafr::research {

inline constexpr const char* kNormalizationContract = "aafr.research.normalization.v1";

enum class NormalizedFeature {
  Delay,             // D, ms.
  Loss,              // L, ratio.
  InverseBandwidth,  // 1 / B, 1 / (Mb/s).
  Hops,              // H, links.
  Bandwidth,         // B, Mb/s.
  Rtt,               // Mean probe RTT, ms.
  Throughput,        // Measured goodput, Mb/s.
};

// Welford's online algorithm: numerically stable mean and sum of squared
// deviations in one pass.
class WelfordAccumulator {
 public:
  // False (and nothing added) for a non-finite value.
  bool Add(double value);
  std::size_t count() const { return count_; }
  double mean() const { return mean_; }
  // Sum of squared deviations from the mean.
  double m2() const { return m2_; }
  // Variance with ddof delta degrees of freedom (0: population, 1: sample);
  // nullopt when count <= ddof.
  std::optional<double> Variance(int ddof) const;

 private:
  std::size_t count_{0};
  double mean_{0.0};
  double m2_{0.0};
};

struct FeatureStatistics {
  std::size_t count{0};
  double mean{0.0};
  double standard_deviation{0.0};
  int ddof{-1};  // 0 or 1; -1: not declared (invalid).
};

// Freezes an accumulator; nullopt when the variance is undefined.
std::optional<FeatureStatistics> Freeze(const WelfordAccumulator& accumulator, int ddof);

struct NormalizationStatistics {
  std::string contract{kNormalizationContract};
  std::string source;  // e.g. dataset name and its content hash.
  std::map<NormalizedFeature, FeatureStatistics> features;

  // Problems; empty when the statistics can be used.
  std::vector<std::string> Validate() const;
};

// (x - mean) / sd; nullopt when the feature has no valid statistics, the
// standard deviation is not positive, or x is not finite.
std::optional<double> ZScore(const NormalizationStatistics& statistics, NormalizedFeature feature, double x);

const char* ToString(NormalizedFeature feature);

}  // namespace aafr::research
