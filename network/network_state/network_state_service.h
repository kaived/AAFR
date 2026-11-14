#pragma once

// The V0.4 NetworkState update pipeline. Plain C++17, no Qt.
//
//   telemetry events -> validation -> aggregation -> feature extraction
//     -> normalization -> quality evaluation -> NetworkState snapshot
//     -> constraint evaluation -> algorithm eligibility evaluation
//
// The service pulls only events it has not seen (by event id) from the
// TelemetryPipeline, so nothing is rebuilt from history. It publishes one
// immutable StateSnapshot per step; readers get a shared_ptr and never block
// the producer for longer than a pointer copy.
//
// Start() runs the steps on a worker thread, off the GUI thread. ProcessOnce()
// runs one step on the caller's thread and may be used with or without the
// worker; steps are serialized.
//
// The service observes and reports. It selects no algorithm and switches
// nothing.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "algorithms/capability_registry.h"
#include "algorithms/eligibility_evaluator.h"
#include "network/network_state/execution_environment.h"
#include "network/network_state/network_constraints.h"
#include "network/network_state/network_state.h"
#include "network/network_state/network_state_builder.h"
#include "network/telemetry/telemetry_pipeline.h"

namespace aafr::netstate {

struct ServiceConfig {
  BuilderConfig builder;
  ConstraintPolicy constraints;
  algorithms::EligibilityPolicy eligibility;
  ExecutionEnvironment environment{BrowserDefaultEnvironment()};
  // Time between steps on the worker thread.
  std::chrono::milliseconds tick{1000};
  // Most events ingested in one step; the rest wait for the next step.
  std::size_t max_events_per_step{8192};
  // A state.updated diagnostic is published when validity or the quality
  // summary changes, and otherwise at most this often.
  std::chrono::milliseconds state_event_interval{5000};
};

// Everything one step produced. Immutable once published.
struct StateSnapshot {
  NetworkState state;
  NetworkConstraints constraints;
  std::vector<algorithms::EligibilityResult> eligibility;
  std::vector<algorithms::AlgorithmCapability> capabilities;

  std::uint64_t CountWithStatus(algorithms::EligibilityStatus status) const;
};

struct ServiceStats {
  std::uint64_t steps{0};
  std::uint64_t events_seen{0};
  // Events that left telemetry history before the service read them.
  std::uint64_t events_missed{0};
  std::uint64_t diagnostics_published{0};
  // Wall time of the most recent and the slowest step, in microseconds.
  std::int64_t last_step_us{0};
  std::int64_t max_step_us{0};
  std::size_t stored_samples{0};
  bool running{false};
};

class NetworkStateService {
 public:
  NetworkStateService(telemetry::TelemetryPipeline& pipeline, ServiceConfig config = {},
                      algorithms::CapabilityRegistry registry = algorithms::CapabilityRegistry::BuiltIn());
  ~NetworkStateService();

  NetworkStateService(const NetworkStateService&) = delete;
  NetworkStateService& operator=(const NetworkStateService&) = delete;

  // Runs the first step on the caller's thread, so a snapshot exists when
  // this returns, then starts the worker.
  void Start();
  // Stops and joins the worker. Safe to call repeatedly and without Start().
  void Stop();

  // Runs one step now. Returns the snapshot it published.
  std::shared_ptr<const StateSnapshot> ProcessOnce();

  // The latest snapshot; null until the first step.
  std::shared_ptr<const StateSnapshot> Latest() const;
  ServiceStats Stats() const;
  const ServiceConfig& config() const { return config_; }

 private:
  void Run();
  void PublishDiagnostics(const StateSnapshot& snapshot,
                          const std::map<std::string, std::uint64_t>& rejections);
  void Publish(telemetry::EventType type, std::vector<telemetry::Attribute> attributes);

  telemetry::TelemetryPipeline& pipeline_;
  const ServiceConfig config_;
  const algorithms::CapabilityRegistry registry_;

  // Guards everything a step touches.
  mutable std::mutex step_mutex_;
  NetworkStateBuilder builder_;
  ConstraintEvaluator constraint_evaluator_;
  algorithms::AlgorithmEligibilityEvaluator eligibility_evaluator_;
  std::uint64_t last_event_id_{0};
  // What was last reported, so diagnostics are published on change only.
  std::string reported_state_signature_;
  std::int64_t reported_state_ns_{0};
  std::uint64_t reported_accepted_{0};
  bool state_reported_{false};
  std::string reported_constraints_signature_;
  std::string reported_eligibility_signature_;
  std::map<std::string, std::string> reported_exclusions_;
  std::vector<std::string> reported_stale_;
  ServiceStats stats_;

  mutable std::mutex snapshot_mutex_;
  std::shared_ptr<const StateSnapshot> latest_;

  std::mutex worker_mutex_;
  std::condition_variable wake_;
  std::thread worker_;
  bool stop_requested_{false};
  std::atomic<bool> running_{false};
};

}  // namespace aafr::netstate
