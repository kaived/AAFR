#include "runtime/adaptive/network_change_detector.h"

#include <algorithm>
#include <cmath>
#include <sstream>

#include "network/network_state/forwarding_observations.h"

namespace aafr::runtime {

std::vector<FeatureThreshold> ChangeDetectorConfig::DefaultFeatureThresholds() {
  // Units follow the V0.4 feature definitions. Absolute floors are chosen
  // above typical interval-to-interval noise of a home connection; relative
  // thresholds catch proportionally large shifts on already-large values.
  return {
      {"rtt.handshake.median_ms", 10.0, 0.30},
      {"ack_delay.jitter_ms", 5.0, 0.50},
      {"dns.response_time.mean_ms", 20.0, 0.50},
      {"tcp.retransmission_ratio", 0.02, 0.50},
      {"throughput.mean_mbps", 1.0, 0.50},
      {"nav.failure_ratio", 0.10, 0.50},
      // V0.7.4 forwarding observations of the active path. Set-up time is
      // dominated by per-connection noise, so the floor is generous; the
      // failure ratio needs its minimum denominator to be Valid at all.
      {"fwd.path.establish_ms.p50", 20.0, 0.50},
      {"fwd.path.failure_ratio", 0.20, 0.50},
  };
}

std::vector<std::string> ChangeDetectorConfig::Validate() const {
  std::vector<std::string> issues;
  if (sustain_observations < 1) issues.push_back("sustain_observations must be at least 1");
  if (!(max_feature_age_ms > 0.0)) issues.push_back("max_feature_age_ms must be positive");
  if (!(edge_latency_absolute_ms >= 0.0) || !(edge_latency_relative >= 0.0)) {
    issues.push_back("edge latency thresholds must be nonnegative");
  }
  for (const FeatureThreshold& f : features) {
    if (f.feature_id.empty()) issues.push_back("feature threshold without id");
    // A positive absolute floor is required: a zero baseline would otherwise
    // make every nonzero observation a change.
    if (!(f.absolute > 0.0) || !(f.relative >= 0.0) || !std::isfinite(f.absolute) || !std::isfinite(f.relative)) {
      issues.push_back("invalid thresholds for " + f.feature_id);
    }
  }
  return issues;
}

std::string ConstraintSignature(const netstate::NetworkConstraints& constraints) {
  std::ostringstream out;
  for (const netstate::Constraint& c : constraints.constraints) {
    // The list of currently usable measurements is covered feature by feature
    // by the detector; including it would turn every quality flicker into a
    // constraint change.
    if (c.id == netstate::constraint_id::kAvailableMeasurements) continue;
    out << c.id << '=' << netstate::ToString(c.validity) << ':' << netstate::ValueText(c) << ';';
  }
  return out.str();
}

ChangeObservation MakeObservation(std::int64_t monotonic_ns, const netstate::NetworkState* state,
                                  const netstate::NetworkConstraints* constraints,
                                  const forwarding::ForwardingGraph* graph, const ChangeDetectorConfig& config) {
  ChangeObservation o;
  o.monotonic_ns = monotonic_ns;
  if (state) {
    o.network_state_id = state->state_id;
    o.capture_interface = state->sources.capture_interface;
    for (const FeatureThreshold& t : config.features) {
      ObservedFeature f;
      if (const netstate::Feature* feature = state->FindFeature(t.feature_id)) {
        f.value = feature->value;
        f.quality = feature->quality;
        f.age_ms = feature->age_ms;
        f.samples = feature->sample_count;
      }
      if (state->forwarding) {
        if (const netstate::ForwardingFeature* ff = state->forwarding->Find(t.feature_id)) {
          if (ff->config_generation != 0) f.context = "generation " + std::to_string(ff->config_generation);
        }
      }
      o.features[t.feature_id] = f;
    }
  }
  if (constraints) o.constraint_signature = ConstraintSignature(*constraints);
  if (graph) {
    o.has_graph = true;
    o.graph_version = graph->version();
    for (const auto& node : graph->nodes()) o.node_available[node.id] = node.available;
    for (const auto& edge : graph->edges()) {
      o.edges[edge.from + "->" + edge.to] = {edge.available, edge.latency_ms.value, edge.policy_cost};
    }
  }
  return o;
}

NetworkChangeDetector::NetworkChangeDetector(ChangeDetectorConfig config) : config_(std::move(config)) {}

void NetworkChangeDetector::Reset() {
  has_previous_ = false;
  baselines_.clear();
  has_graph_baseline_ = false;
  node_baseline_.clear();
  edge_baseline_.clear();
}

namespace {
std::string Fixed(double v) {
  std::ostringstream out;
  out.setf(std::ios::fixed);
  out.precision(3);
  out << v;
  return out.str();
}

void AddTrigger(ChangeAssessment& a, EvaluationTrigger t) {
  if (std::find(a.triggers.begin(), a.triggers.end(), t) == a.triggers.end()) a.triggers.push_back(t);
}
void AddReason(ChangeAssessment& a, RuntimeReason r) {
  if (std::find(a.reasons.begin(), a.reasons.end(), r) == a.reasons.end()) a.reasons.push_back(r);
}
}  // namespace

ChangeAssessment NetworkChangeDetector::Observe(const ChangeObservation& o) {
  ChangeAssessment a;
  const bool new_state = !has_previous_ || o.network_state_id != last_state_id_;
  const bool new_graph = !has_previous_ || o.graph_version != last_graph_version_;
  if (has_previous_ && !new_state && !new_graph) {
    a.duplicate = true;
    a.reasons.push_back(RuntimeReason::NetworkChangeIgnored);
    a.explanation = "same NetworkState and graph version as the previous observation";
    return a;
  }
  std::vector<std::string> notes;

  // ---- Capture interface ----
  if (has_previous_ && o.capture_interface != capture_interface_ && !o.capture_interface.empty() &&
      !capture_interface_.empty()) {
    a.interface_changed = true;
    a.meaningful = true;
    AddTrigger(a, EvaluationTrigger::MeaningfulChange);
    AddReason(a, RuntimeReason::NetworkChangeMeaningful);
    notes.push_back("capture interface changed from " + capture_interface_ + " to " + o.capture_interface +
                    "; feature baselines reset");
    baselines_.clear();
  }
  if (!o.capture_interface.empty()) capture_interface_ = o.capture_interface;

  // ---- Constraints ----
  if (has_previous_ && o.constraint_signature != constraint_signature_ && o.network_state_id != 0) {
    a.constraint_changed = true;
    a.meaningful = true;
    AddTrigger(a, EvaluationTrigger::ConstraintChange);
    AddReason(a, RuntimeReason::ConstraintChanged);
    notes.push_back("constraint set changed");
  }
  if (o.network_state_id != 0) constraint_signature_ = o.constraint_signature;

  // ---- NetworkState features (only on a new snapshot) ----
  if (new_state && o.network_state_id != 0) {
    for (const FeatureThreshold& t : config_.features) {
      FeatureChange change;
      change.feature_id = t.feature_id;
      FeatureBaseline& base = baselines_[t.feature_id];
      change.baseline = base.value;
      const auto found = o.features.find(t.feature_id);
      const ObservedFeature current = found == o.features.end() ? ObservedFeature{} : found->second;
      change.current = current.value;
      const bool usable = current.quality == netstate::FeatureQuality::Valid && current.value.has_value() &&
                          std::isfinite(*current.value);
      if (!usable) {
        base.streak = 0;
        change.status = std::string("unusable:") + netstate::ToString(current.quality);
        a.features.push_back(change);
        continue;
      }
      if (current.age_ms && *current.age_ms > config_.max_feature_age_ms) {
        base.streak = 0;
        change.status = "stale_age";
        a.features.push_back(change);
        continue;
      }
      if (!base.value) {
        base.value = current.value;
        base.streak = 0;
        base.context = current.context;
        change.status = "baseline_set";
        a.features.push_back(change);
        continue;
      }
      if (base.context != current.context) {
        base.value = current.value;
        base.streak = 0;
        base.context = current.context;
        change.status = "baseline_reset:context";
        a.features.push_back(change);
        continue;
      }
      change.delta = std::fabs(*current.value - *base.value);
      change.threshold = std::max(t.absolute, t.relative * std::fabs(*base.value));
      if (change.delta >= change.threshold) {
        ++base.streak;
        change.streak = base.streak;
        if (base.streak >= config_.sustain_observations) {
          change.status = "meaningful";
          a.meaningful = true;
          AddTrigger(a, EvaluationTrigger::MeaningfulChange);
          AddReason(a, RuntimeReason::NetworkChangeMeaningful);
          notes.push_back(t.feature_id + " " + Fixed(*base.value) + " -> " + Fixed(*current.value) + " (|delta| " +
                          Fixed(change.delta) + " >= " + Fixed(change.threshold) + " for " +
                          std::to_string(base.streak) + " observations)");
          base.value = current.value;
          base.streak = 0;
        } else {
          change.status = "pending_sustain";
        }
      } else {
        base.streak = 0;
        change.streak = 0;
        change.status = "within_threshold";
      }
      a.features.push_back(change);
    }
  }

  // ---- Forwarding graph ----
  if (o.has_graph) {
    if (has_graph_baseline_ && new_graph) {
      for (const auto& node : o.node_available) {
        const auto before = node_baseline_.find(node.first);
        if (before == node_baseline_.end() || before->second != node.second) {
          a.topology_changes.push_back("node " + node.first + (node.second ? " available" : " unavailable"));
        }
      }
      for (const auto& edge : o.edges) {
        const auto before = edge_baseline_.find(edge.first);
        if (before == edge_baseline_.end()) {
          a.topology_changes.push_back("edge " + edge.first + " added");
          continue;
        }
        if (before->second.available != edge.second.available) {
          a.topology_changes.push_back("edge " + edge.first + (edge.second.available ? " available" : " unavailable"));
        }
        if (before->second.policy_cost != edge.second.policy_cost) {
          a.topology_changes.push_back("edge " + edge.first + " policy cost " + Fixed(before->second.policy_cost) +
                                       " -> " + Fixed(edge.second.policy_cost));
        }
        const auto& old_latency = before->second.latency_ms;
        const auto& new_latency = edge.second.latency_ms;
        if (old_latency.has_value() != new_latency.has_value()) {
          a.topology_changes.push_back("edge " + edge.first + " latency became " + (new_latency ? "known" : "unknown"));
        } else if (old_latency && new_latency) {
          const double delta = std::fabs(*new_latency - *old_latency);
          const double threshold =
              std::max(config_.edge_latency_absolute_ms, config_.edge_latency_relative * std::fabs(*old_latency));
          if (delta >= threshold) {
            a.topology_changes.push_back("edge " + edge.first + " latency " + Fixed(*old_latency) + " -> " +
                                         Fixed(*new_latency) + " ms");
          }
        }
      }
      for (const auto& edge : edge_baseline_) {
        if (o.edges.count(edge.first) == 0) a.topology_changes.push_back("edge " + edge.first + " removed");
      }
    }
    // Baseline follows the graph: configured values are not noisy, so a
    // change below threshold is simply absorbed.
    if (!has_graph_baseline_ || !a.topology_changes.empty()) {
      node_baseline_ = o.node_available;
      edge_baseline_ = o.edges;
      has_graph_baseline_ = true;
    } else {
      // Track small drifts so they cannot accumulate unseen: rebaseline only
      // availability, keep latency baselines until a change crosses its threshold.
      for (const auto& node : o.node_available) node_baseline_[node.first] = node.second;
    }
    if (!a.topology_changes.empty()) {
      a.meaningful = true;
      AddTrigger(a, EvaluationTrigger::TopologyChange);
      AddReason(a, RuntimeReason::TopologyChanged);
      for (const auto& change : a.topology_changes) notes.push_back(change);
    }
  }

  has_previous_ = true;
  last_state_id_ = o.network_state_id;
  last_graph_version_ = o.graph_version;
  if (!a.meaningful) {
    AddReason(a, RuntimeReason::NetworkChangeIgnored);
    std::size_t pending = 0, unusable = 0;
    for (const auto& f : a.features) {
      if (f.status == "pending_sustain") ++pending;
      if (f.status.rfind("unusable", 0) == 0 || f.status == "stale_age") ++unusable;
    }
    a.explanation = "no meaningful change (" + std::to_string(pending) + " feature(s) awaiting sustain, " +
                    std::to_string(unusable) + " unusable)";
  } else {
    for (const auto& note : notes) a.explanation += (a.explanation.empty() ? "" : "; ") + note;
  }
  return a;
}

// ---- Scheduler ----

void EvaluationScheduler::Request(EvaluationTrigger trigger, std::int64_t now_ns, const std::string& reason) {
  if (!pending_) {
    pending_ = PendingEvaluation{trigger, {trigger}, 1, now_ns, reason};
    return;
  }
  ++coalesced_total_;
  ++pending_->requests;
  pending_->coalesced.insert(trigger);
  if (Priority(trigger) > Priority(pending_->trigger)) {
    pending_->trigger = trigger;
    pending_->reason = reason;
  }
}

std::optional<PendingEvaluation> EvaluationScheduler::Take(std::int64_t now_ns) {
  if (!pending_) return std::nullopt;
  bool safety = false;
  for (EvaluationTrigger t : pending_->coalesced) safety = safety || IsSafetyTrigger(t);
  if (!safety && last_started_ns_ &&
      static_cast<double>(now_ns - *last_started_ns_) < config_.min_interval_ms * 1'000'000.0) {
    return std::nullopt;
  }
  std::optional<PendingEvaluation> taken = std::move(pending_);
  pending_.reset();
  last_started_ns_ = now_ns;
  ++started_total_;
  return taken;
}

}  // namespace aafr::runtime
