#pragma once

// Quality classification for NetworkState features. Plain C++17.
//
// Rules, applied in this order:
//  capture-interface features
//   1. no value, inputs rejected in the window        -> Invalid
//      no value, inputs present but undefined result  -> Missing (e.g. a
//      ratio with no TCP packets)
//   2. no value, capture never started                -> Missing
//   3. no value, inputs seen earlier in this capture  -> Stale
//   4. no value otherwise                             -> Missing
//   5. value outside its unit's range                 -> Invalid
//   6. newest observation older than the stale limit  -> Stale (value kept)
//   7. fewer observations than the feature needs      -> InsufficientSamples
//   8. otherwise                                      -> Valid
//  browser features follow 1, 4, 5, 7, 8: navigations are sparse by nature,
//  so the window itself bounds their age and no separate stale limit applies.

#include <cstdint>
#include <vector>

#include "network/network_state/feature_extractor.h"
#include "network/network_state/network_state.h"

namespace aafr::netstate {

struct QualityPolicy {
  // A capture-interface feature whose newest observation is older than this
  // is stale. Default: 10 s, ten missed 1 s capture intervals, the same
  // limit the V0.3 Network tab uses.
  std::int64_t network_stale_after_ms{10'000};
};

class MeasurementQualityEvaluator {
 public:
  explicit MeasurementQualityEvaluator(QualityPolicy policy = {}) : policy_(policy) {}

  Feature Classify(const FeatureDraft& draft, std::int64_t now_ns, CaptureStatus capture) const;

  // The feature as seen from `requested`. A capture-interface feature asked
  // for at tab or navigation scope becomes Unattributed with no value: host
  // traffic is never presented as one tab's traffic.
  static Feature ForScope(const Feature& feature, telemetry::MeasurementScope requested);

  static QualitySummary Summarize(const std::vector<Feature>& features);

  const QualityPolicy& policy() const { return policy_; }

 private:
  QualityPolicy policy_;
};

}  // namespace aafr::netstate
