#pragma once

// Manual algorithm execution. Plain C++17.
//
// One explicit request runs one named algorithm through:
//
//   problem validation -> V0.4 eligibility -> algorithm computation
//     -> candidate path validation -> forwarding execution -> telemetry
//
// The engine never chooses an algorithm, never compares algorithms to pick
// one, and never re-runs anything on its own. That is V0.6 work.
//
// Threading: Submit() queues the request for a single worker thread and
// returns at once, so a GUI thread never waits for a computation. RunSync()
// runs on the caller's thread (tests, tools). Records are snapshots; poll
// Find() for progress. Every wait is bounded: computation by the budget and
// deadline, forwarding by its timeout, and Cancel() stops both.

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "algorithms/capability_registry.h"
#include "algorithms/common/path_algorithm.h"
#include "algorithms/eligibility_evaluator.h"
#include "network/network_state/network_state.h"
#include "network/telemetry/telemetry_pipeline.h"
#include "routing/forwarding/forwarding_executor.h"

namespace aafr::algorithms {

struct ExecutionRequest {
  AlgorithmId algorithm{AlgorithmId::Dijkstra};
  // A snapshot the engine may read from its worker thread.
  std::shared_ptr<const forwarding::ForwardingGraph> graph;
  forwarding::ForwardingProblem problem;
  ExecutionBudget budget;
  AlgorithmParameters parameters;
  std::uint64_t seed{1};
  bool execute_forwarding{true};
  double forwarding_timeout_ms{3000.0};
  std::string scenario_id{"manual"};
  // The NetworkState the eligibility checks read. nullopt: an empty state
  // (validity no_data), which fails the host-state checks.
  std::optional<netstate::NetworkState> network_state;
  // RequiredFeatures and DataQuality judge the host's captured traffic, which
  // the controlled graph does not use for its costs. When false, those two
  // checks may fail without blocking the run; the record lists them as
  // overridden. Every other check must still pass.
  bool enforce_host_state_checks{true};
};

enum class ExecutionStage {
  Queued,
  CheckingEligibility,
  Computing,
  ValidatingCandidate,
  Forwarding,
  Completed,  // Computed, validated and (if requested) forwarded with a confirmed path.
  Rejected,   // Refused before computing: invalid request or ineligible.
  Failed,     // Computation, validation or forwarding did not succeed.
  Cancelled,
};

struct ExecutionRecord {
  std::string execution_id;
  AlgorithmId algorithm{AlgorithmId::Dijkstra};
  std::string scenario_id;
  std::uint64_t network_state_id{0};
  std::string graph_id;
  std::uint64_t graph_version{0};
  ExecutionStage stage{ExecutionStage::Queued};
  // Machine-readable outcome, e.g. "COMPLETED", "INELIGIBLE", "CANDIDATE_REJECTED".
  std::string outcome;
  std::string message;
  std::optional<EligibilityResult> eligibility;
  std::vector<std::string> overridden_checks;
  std::optional<AlgorithmResult> computation;
  std::optional<forwarding::PathValidation> candidate;
  std::string forwarding_request_id;
  std::optional<forwarding::ForwardingExecutionResult> forwarding;
  telemetry::WallClock::time_point submitted_at{};
  std::optional<telemetry::WallClock::time_point> finished_at;

  bool Finished() const;
};

class AlgorithmExecutionEngine {
 public:
  struct Config {
    std::size_t max_pending{8};
    std::size_t history{64};
    double default_deadline_ms{2000.0};
    std::uint64_t default_memory_budget_bytes{64ull * 1024 * 1024};
  };

  // `pipeline` and `executor` may be null and must outlive the engine.
  AlgorithmExecutionEngine(telemetry::TelemetryPipeline* pipeline, CapabilityRegistry registry,
                           forwarding::IForwardingExecutor* executor = nullptr);
  AlgorithmExecutionEngine(telemetry::TelemetryPipeline* pipeline, CapabilityRegistry registry,
                           forwarding::IForwardingExecutor* executor, Config config);
  ~AlgorithmExecutionEngine();
  AlgorithmExecutionEngine(const AlgorithmExecutionEngine&) = delete;
  AlgorithmExecutionEngine& operator=(const AlgorithmExecutionEngine&) = delete;

  // Queues the request; returns its execution id. When the queue is full the
  // record is created as Rejected ("QUEUE_FULL") and nothing runs.
  std::string Submit(ExecutionRequest request);
  ExecutionRecord RunSync(ExecutionRequest request);
  bool Cancel(const std::string& execution_id);
  void CancelAll();

  std::optional<ExecutionRecord> Find(const std::string& execution_id) const;
  // Oldest first, at most Config::history.
  std::vector<ExecutionRecord> Recent() const;
  std::optional<ExecutionRecord> Latest() const;
  std::size_t Pending() const;

  void SetRegistry(CapabilityRegistry registry);
  CapabilityRegistry Registry() const;
  void SetExecutor(forwarding::IForwardingExecutor* executor);
  bool ForwardingAvailable() const;

  // The V0.4 environment this request poses for one algorithm, and the
  // eligibility result under it. Used by the engine and by viewers.
  netstate::ExecutionEnvironment EnvironmentFor(AlgorithmId id, const ExecutionRequest& request) const;
  EligibilityResult Eligibility(AlgorithmId id, const ExecutionRequest& request) const;

 private:
  struct Job {
    std::string id;
    ExecutionRequest request;
    std::shared_ptr<std::atomic<bool>> cancel;
  };

  std::string NewRecord(const ExecutionRequest& request, std::shared_ptr<std::atomic<bool>> cancel);
  ExecutionRecord Process(const std::string& id, const ExecutionRequest& request, const std::atomic<bool>& cancel);
  template <typename Update>
  void Modify(const std::string& id, Update update);
  void Publish(telemetry::EventType type, std::vector<telemetry::Attribute> attributes) const;
  void Run();

  telemetry::TelemetryPipeline* pipeline_;
  const Config config_;

  mutable std::mutex mutex_;
  CapabilityRegistry registry_;
  forwarding::IForwardingExecutor* executor_;
  std::deque<ExecutionRecord> records_;
  std::map<std::string, std::shared_ptr<std::atomic<bool>>> cancel_flags_;
  std::deque<Job> queue_;
  std::uint64_t next_id_{1};
  bool stop_{false};
  std::condition_variable wake_;
  std::thread worker_;
  // Serializes forwarding execution: one request on the relays at a time.
  std::mutex forwarding_mutex_;
};

const char* ToString(ExecutionStage value);

}  // namespace aafr::algorithms
