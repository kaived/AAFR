#include "network/network_state/measurement_quality.h"

#include <cstdio>

#include "network/network_state/feature_normalizer.h"

namespace aafr::netstate {
namespace {

std::string Seconds(double ms) {
  char buffer[32];
  std::snprintf(buffer, sizeof(buffer), "%.1f s", ms / 1000.0);
  return buffer;
}

}  // namespace

Feature MeasurementQualityEvaluator::Classify(const FeatureDraft& draft, std::int64_t now_ns,
                                              CaptureStatus capture) const {
  Feature feature = draft.feature;
  if (draft.newest_ns) feature.age_ms = static_cast<double>(now_ns - *draft.newest_ns) / 1e6;

  const auto set = [&feature](FeatureQuality quality, std::string reason) {
    feature.quality = quality;
    feature.quality_reason = std::move(reason);
  };

  if (!feature.value) {
    if (!draft.rejection.empty()) {
      set(FeatureQuality::Invalid, "measurement rejected: " + draft.rejection);
    } else if (!draft.note.empty()) {
      set(FeatureQuality::Missing, draft.note);
    } else if (!draft.network) {
      set(FeatureQuality::Missing, draft.ever_observed ? "no navigation completed inside the window"
                                                       : "no browser navigation observed yet");
    } else if (capture == CaptureStatus::NeverStarted && !draft.ever_observed) {
      set(FeatureQuality::Missing, "packet capture has not been started");
    } else if (draft.ever_observed) {
      set(FeatureQuality::Stale, std::string("no observation inside the window; capture is ") +
                                     ToString(capture));
    } else {
      set(FeatureQuality::Missing, std::string("not observed in this capture; capture is ") +
                                       ToString(capture));
    }
    return feature;
  }

  if (!InUnitRange(*feature.value, feature.unit)) {
    feature.value.reset();
    set(FeatureQuality::Invalid, std::string("computed value is outside the range of ") +
                                     ToString(feature.unit));
    return feature;
  }
  if (draft.network && feature.age_ms &&
      *feature.age_ms > static_cast<double>(policy_.network_stale_after_ms)) {
    set(FeatureQuality::Stale, "newest observation is " + Seconds(*feature.age_ms) + " old (limit " +
                                   Seconds(static_cast<double>(policy_.network_stale_after_ms)) + ")");
    return feature;
  }
  if (feature.sample_count < draft.min_samples) {
    set(FeatureQuality::InsufficientSamples,
        std::to_string(feature.sample_count) + " of " + std::to_string(draft.min_samples) +
            " required observations");
    return feature;
  }
  set(FeatureQuality::Valid, "");
  return feature;
}

Feature MeasurementQualityEvaluator::ForScope(const Feature& feature,
                                              telemetry::MeasurementScope requested) {
  using telemetry::MeasurementScope;
  const bool narrower = (requested == MeasurementScope::Tab || requested == MeasurementScope::Navigation) &&
                        feature.scope != MeasurementScope::Tab &&
                        feature.scope != MeasurementScope::Navigation;
  if (!narrower) return feature;
  Feature scoped = feature;
  scoped.value.reset();
  scoped.quality = FeatureQuality::Unattributed;
  scoped.quality_reason = std::string("measured at ") + telemetry::ToString(feature.scope) +
                          " scope; there is no evidence tying it to one " +
                          telemetry::ToString(requested);
  return scoped;
}

QualitySummary MeasurementQualityEvaluator::Summarize(const std::vector<Feature>& features) {
  QualitySummary summary;
  for (const Feature& feature : features) {
    switch (feature.quality) {
      case FeatureQuality::Valid: ++summary.valid; break;
      case FeatureQuality::Missing: ++summary.missing; break;
      case FeatureQuality::Stale: ++summary.stale; break;
      case FeatureQuality::InsufficientSamples: ++summary.insufficient_samples; break;
      case FeatureQuality::Invalid: ++summary.invalid; break;
      case FeatureQuality::Unattributed: ++summary.unattributed; break;
    }
  }
  return summary;
}

}  // namespace aafr::netstate
