#pragma once

// Outcome observation after decisions. Plain C++17, thread-safe.
//
// Every request the runtime itself sends over the confirmed active
// configuration (activation confirmations, periodic health checks and
// application requests forwarded through AAFRRuntime::ForwardActive) is
// recorded with the decision, configuration version, algorithm and
// NetworkState it ran under. Summaries are derived on demand as numbered
// OutcomeRevisions; DecisionRecords are never rewritten.
//
// What this does not do: attribute causation (a better outcome after a
// switch is correlation, not proof), learn online or retrain anything.

#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "runtime/adaptive/decision_record.h"
#include "runtime/adaptive/performance_evaluator.h"

namespace aafr::runtime {

enum class OutcomeKind { Activation, HealthCheck, Application };

struct OutcomeObservation {
  std::string decision_id;
  std::uint64_t config_version{0};
  algorithms::AlgorithmId algorithm{algorithms::AlgorithmId::Dijkstra};
  std::uint64_t network_state_id{0};
  OutcomeKind kind{OutcomeKind::HealthCheck};
  std::int64_t monotonic_ns{0};
  RequestSample sample;
};

struct OutcomeCounters {
  std::uint64_t observations{0};
  std::uint64_t health_checks{0};
  std::uint64_t health_failures{0};
  std::uint64_t application_requests{0};
  std::uint64_t application_failures{0};
  std::uint64_t activation_failures{0};
  std::uint64_t recoveries_succeeded{0};
  std::uint64_t recoveries_failed{0};
  std::vector<double> switching_latency_ms;  // Bounded, newest last.
};

class RuntimeOutcomeObserver {
 public:
  RuntimeOutcomeObserver(double window_ms = 30'000.0, std::size_t max_per_decision = 512,
                         std::size_t max_decisions = 128)
      : window_ms_(window_ms), max_per_decision_(max_per_decision), max_decisions_(max_decisions) {}

  void Record(const OutcomeObservation& observation);
  void RecordSwitchLatency(double ms);
  void RecordActivationFailure();
  void RecordRecovery(bool succeeded);

  // The newest revision for a decision, evaluated at now_ns (the window is
  // complete once window_ms has passed since the first observation).
  std::optional<OutcomeRevision> Latest(const std::string& decision_id, std::int64_t now_ns) const;
  OutcomeCounters Counters() const;
  double window_ms() const { return window_ms_; }

 private:
  struct Bucket {
    std::uint64_t revision{0};
    std::vector<OutcomeObservation> observations;
  };
  const double window_ms_;
  const std::size_t max_per_decision_;
  const std::size_t max_decisions_;
  mutable std::mutex mutex_;
  std::map<std::string, Bucket> buckets_;
  std::deque<std::string> order_;
  OutcomeCounters counters_;
};

const char* ToString(OutcomeKind value);

}  // namespace aafr::runtime
