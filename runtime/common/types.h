#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace ngr {

using Timestamp = std::chrono::system_clock::time_point;
using StrategyId = std::string;
using FeatureContribMap = std::unordered_map<std::string, double>;

enum class ContentClass {
  Unknown,
  Text,
  Image,
  Video,
  Binary
};

enum class SubsystemId {
  Forwarding,
  Cache,
  Compression
};

struct EnvironmentSnapshot {
  Timestamp captured_at{std::chrono::system_clock::now()};

  double latency_ms{0.0};
  double throughput_mbps{0.0};
  double packet_loss_pct{0.0};
  double jitter_ms{0.0};
  double cpu_load_pct{0.0};
  double mem_pressure_pct{0.0};
  std::uint32_t open_connections{0};

  double request_burst_rate{0.0};
  double object_reuse_ratio{0.0};
  double cache_locality_score{0.0};
  ContentClass content_class{ContentClass::Unknown};

  // Initial confidence proxy until replay-based confidence is implemented.
  double stability_confidence{1.0};
};

struct ScoredCandidate {
  StrategyId id;
  double score{0.0};
  FeatureContribMap contributions;
};

struct DecisionResult {
  SubsystemId subsystem{SubsystemId::Forwarding};
  std::vector<ScoredCandidate> ranked;
  double confidence{0.0};
  Timestamp evaluated_at{std::chrono::system_clock::now()};
};

enum class TransitionOutcome {
  Applied,
  DeferredDwell,
  DeferredHysteresis,
  DeferredConfidence,
  DeferredCooldown
};

struct TransitionGateConfig {
  std::chrono::milliseconds dwell_min{1000};
  double hysteresis_margin_ratio{0.10};
  double confidence_threshold{0.55};
  std::chrono::milliseconds cooldown{5000};
};

struct TransitionState {
  StrategyId active;
  double active_score_ewma{0.0};
  Timestamp active_since{};
  Timestamp last_switch_at{};
};

struct TransitionGateStatus {
  bool dwell_ok{false};
  bool hysteresis_ok{false};
  bool confidence_ok{false};
  bool cooldown_ok{false};
};

struct TransitionDecision {
  SubsystemId subsystem{SubsystemId::Forwarding};
  StrategyId active_before;
  StrategyId active_after;
  TransitionOutcome outcome{TransitionOutcome::DeferredConfidence};
  DecisionResult source;
  TransitionGateStatus gates;
};

struct RequestContext {
  std::string url;
  ContentClass content_class{ContentClass::Unknown};
  std::uint64_t estimated_bytes{0};
};

std::string ToString(ContentClass value);
std::string ToString(SubsystemId value);
std::string ToString(TransitionOutcome value);

}  // namespace ngr
