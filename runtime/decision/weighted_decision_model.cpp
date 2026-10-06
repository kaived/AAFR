#include "runtime/decision/weighted_decision_model.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace ngr {
namespace {

double Clamp(double value, double low, double high) {
  return std::max(low, std::min(value, high));
}

double PositiveFromLow(double value, double max_value) {
  if (max_value <= 0.0) {
    return 0.0;
  }
  return Clamp(1.0 - (value / max_value), -1.0, 1.0);
}

double PositiveFromHigh(double value, double target_value) {
  if (target_value <= 0.0) {
    return 0.0;
  }
  return Clamp(value / target_value, -1.0, 1.0);
}

double ContentFit(const EnvironmentSnapshot& snap) {
  switch (snap.content_class) {
    case ContentClass::Text:
      return 1.0;
    case ContentClass::Image:
      return -0.15;
    case ContentClass::Video:
      return -0.65;
    case ContentClass::Binary:
      return -0.5;
    case ContentClass::Unknown:
      return 0.0;
  }
  return 0.0;
}

}  // namespace

void PolicyRegistry::SetDecisionModelVersion(std::string version) {
  decision_model_version_ = std::move(version);
}

const std::string& PolicyRegistry::DecisionModelVersion() const {
  return decision_model_version_;
}

void PolicyRegistry::SetPolicy(SubsystemId subsystem, SubsystemPolicy policy) {
  policies_[subsystem] = std::move(policy);
}

const SubsystemPolicy* PolicyRegistry::FindPolicy(SubsystemId subsystem) const {
  const auto found = policies_.find(subsystem);
  if (found == policies_.end()) {
    return nullptr;
  }
  return &found->second;
}

DecisionResult WeightedDecisionModel::Evaluate(const EnvironmentSnapshot& snap,
                                               SubsystemId subsystem,
                                               const PolicyRegistry& registry) const {
  DecisionResult result;
  result.subsystem = subsystem;
  result.confidence = Clamp(snap.stability_confidence, 0.0, 1.0);
  result.evaluated_at = std::chrono::system_clock::now();

  const SubsystemPolicy* policy = registry.FindPolicy(subsystem);
  if (policy == nullptr) {
    return result;
  }

  result.ranked.reserve(policy->candidates.size());

  for (const CandidatePolicy& candidate : policy->candidates) {
    ScoredCandidate scored;
    scored.id = candidate.id;

    for (const auto& weight_entry : candidate.weights) {
      const std::string& feature = weight_entry.first;
      const double weight = weight_entry.second;
      const double contribution = weight * FeatureValue(snap, subsystem, feature);
      scored.contributions[feature] = contribution;
      scored.score += contribution;
    }

    result.ranked.push_back(std::move(scored));
  }

  std::sort(result.ranked.begin(), result.ranked.end(),
            [](const ScoredCandidate& left, const ScoredCandidate& right) {
              if (left.score == right.score) {
                return left.id < right.id;
              }
              return left.score > right.score;
            });

  return result;
}

double WeightedDecisionModel::FeatureValue(const EnvironmentSnapshot& snap,
                                           SubsystemId subsystem,
                                           const std::string& feature) {
  if (feature == "latency_score") {
    return PositiveFromLow(snap.latency_ms, 800.0);
  }
  if (feature == "throughput_score") {
    return PositiveFromHigh(snap.throughput_mbps, 50.0);
  }
  if (feature == "reliability_score") {
    return PositiveFromLow(snap.packet_loss_pct, 10.0);
  }
  if (feature == "jitter_stability_score") {
    return PositiveFromLow(snap.jitter_ms, 200.0);
  }
  if (feature == "cpu_headroom") {
    return PositiveFromLow(snap.cpu_load_pct, 100.0);
  }
  if (feature == "memory_headroom") {
    return PositiveFromLow(snap.mem_pressure_pct, 100.0);
  }
  if (feature == "cache_reuse_score") {
    return Clamp(snap.object_reuse_ratio, 0.0, 1.0);
  }
  if (feature == "cache_locality_score") {
    return Clamp(snap.cache_locality_score, 0.0, 1.0);
  }
  if (feature == "request_burst_score") {
    return PositiveFromHigh(snap.request_burst_rate, 50.0);
  }
  if (feature == "content_fit") {
    return ContentFit(snap);
  }
  if (feature == "content_text_fit") {
    return snap.content_class == ContentClass::Text ? 1.0 : 0.0;
  }
  if (feature == "content_binary_penalty") {
    return snap.content_class == ContentClass::Binary ? -1.0 : 0.0;
  }

  if (subsystem == SubsystemId::Forwarding) {
    if (feature == "connection_reuse_benefit") {
      return PositiveFromHigh(static_cast<double>(snap.open_connections), 16.0);
    }
    if (feature == "retry_risk") {
      return -PositiveFromHigh(snap.packet_loss_pct + (snap.jitter_ms / 100.0), 15.0);
    }
    if (feature == "multiplexing_headroom") {
      return Clamp((PositiveFromHigh(snap.throughput_mbps, 30.0) +
                    PositiveFromLow(snap.packet_loss_pct, 5.0)) /
                       2.0,
                   -1.0, 1.0);
    }
  }

  if (subsystem == SubsystemId::Cache) {
    if (feature == "hit_rate_trend") {
      return Clamp(snap.object_reuse_ratio, 0.0, 1.0);
    }
    if (feature == "working_set_fit") {
      return Clamp(snap.cache_locality_score, 0.0, 1.0);
    }
    if (feature == "eviction_cost") {
      return -Clamp(snap.request_burst_rate / 50.0, 0.0, 1.0);
    }
  }

  if (subsystem == SubsystemId::Compression) {
    if (feature == "cpu_cost") {
      return -PositiveFromLow(snap.cpu_load_pct, 100.0);
    }
    if (feature == "bandwidth_gain") {
      return PositiveFromLow(snap.throughput_mbps, 50.0);
    }
  }

  return 0.0;
}

std::string ToString(ContentClass value) {
  switch (value) {
    case ContentClass::Unknown:
      return "UNKNOWN";
    case ContentClass::Text:
      return "TEXT";
    case ContentClass::Image:
      return "IMAGE";
    case ContentClass::Video:
      return "VIDEO";
    case ContentClass::Binary:
      return "BINARY";
  }
  return "UNKNOWN";
}

std::string ToString(SubsystemId value) {
  switch (value) {
    case SubsystemId::Forwarding:
      return "FORWARDING";
    case SubsystemId::Cache:
      return "CACHE";
    case SubsystemId::Compression:
      return "COMPRESSION";
  }
  return "UNKNOWN";
}

std::string ToString(TransitionOutcome value) {
  switch (value) {
    case TransitionOutcome::Applied:
      return "APPLIED";
    case TransitionOutcome::DeferredDwell:
      return "DEFERRED_DWELL";
    case TransitionOutcome::DeferredHysteresis:
      return "DEFERRED_HYSTERESIS";
    case TransitionOutcome::DeferredConfidence:
      return "DEFERRED_CONFIDENCE";
    case TransitionOutcome::DeferredCooldown:
      return "DEFERRED_COOLDOWN";
  }
  return "UNKNOWN";
}

}  // namespace ngr
