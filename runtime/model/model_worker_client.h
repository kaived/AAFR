#pragma once

// ModelWorkerClient (V0.7.2): owns the persistent local Python inference
// worker and speaks the JSON Lines protocol (docs/V0.7.2_INFERENCE_PROTOCOL.md).
// Plain C++17, no Qt.
//
// Lifecycle: Start() spawns the worker and returns at once; a reader thread
// receives "hello", then "ready" (validated against the AAFR manifest and
// feature contract) or "fatal". Infer() is synchronous with a hard deadline
// and is called on the runtime's worker thread, never the GUI thread. One
// request is in flight at a time (no queue); a second caller gets Busy.
//
// Correlation: every request carries a unique request_id and the evaluation
// context (evaluation id, NetworkState id, graph version). A response is
// accepted only for the request being awaited and only when it echoes that
// context; a response that arrives after its deadline, or for another
// request, is discarded and counted as stale.
//
// Failure handling: a worker that exits, corrupts its output or times out
// repeatedly is killed. Automatic restarts are bounded (max_restarts within
// restart_window_ms, with a backoff), so a broken worker cannot cause a
// restart loop; after that the client stays Failed until Restart() is called.
// Startup failures caused by the artifacts or contract (fatal message,
// contract mismatch) are not retried automatically.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "runtime/model/child_process.h"
#include "runtime/model/json_value.h"
#include "runtime/model/model_contract.h"
#include "runtime/model/model_events.h"

namespace aafr::model {

enum class WorkerState {
  NotConfigured,  // No interpreter, script, model directory or manifest configured.
  Stopped,
  Starting,       // Spawned; waiting for "ready".
  Ready,
  Restarting,     // Waiting for the restart backoff.
  Failed,         // Startup failure or restart budget exhausted.
};
const char* ToString(WorkerState value);

struct WorkerConfig {
  std::string python_executable;
  std::string worker_script;
  std::string model_dir;      // Model release root (contains models/ and ml/).
  std::string manifest_path;  // AAFR manifest with pinned SHA-256 values.
  double startup_timeout_ms{60'000.0};
  double request_timeout_ms{500.0};
  // A response whose worker-reported inference time exceeds this is rejected.
  double max_model_inference_ms{250.0};
  std::size_t max_message_bytes{65'536};
  std::uint64_t memory_limit_bytes{2ull * 1024 * 1024 * 1024};
  bool auto_restart{true};
  int max_restarts{3};
  double restart_window_ms{300'000.0};
  double restart_backoff_ms{1'000.0};
  int timeouts_before_restart{2};

  // Empty when usable; otherwise what is missing.
  std::vector<std::string> Validate() const;
};

struct InferenceRequest {
  std::string request_id;
  std::string evaluation_id;
  std::uint64_t network_state_id{0};
  std::uint64_t graph_version{0};
  std::string state_version;  // Free-form extra correlation (e.g. constraint hash).
  std::map<std::string, double> features;
};

enum class InferenceStatus {
  Ok,
  WorkerUnavailable,  // Not configured, starting, restarting, failed or exited.
  Busy,               // Another request is in flight.
  Timeout,
  WorkerError,        // The worker answered with an error status.
  InvalidResponse,    // Malformed, unexpected or inconsistent response.
  VersionMismatch,    // Model or feature-contract version differs.
  Stale,              // The response does not echo the request's evaluation context.
  ExcessiveLatency,   // Worker-reported inference time above the budget.
};
const char* ToString(InferenceStatus value);

struct InferenceResponse {
  InferenceStatus status{InferenceStatus::WorkerUnavailable};
  std::string request_id;
  std::string error_code;
  std::string message;
  std::string model_version;
  std::string feature_contract_version;
  std::string model_label;
  std::string label_source;
  std::optional<double> gate_probability;
  std::optional<double> gate_threshold;
  bool stage2_ran{false};
  std::vector<std::pair<std::string, double>> stage2_probabilities;
  bool probabilities_calibrated{false};
  std::vector<std::string> out_of_training_range;
  std::vector<std::string> distribution_warnings;
  std::optional<double> inference_ms;  // Worker-reported.
  double round_trip_ms{0.0};
};

struct WorkerStatus {
  WorkerState state{WorkerState::NotConfigured};
  std::string detail;
  std::uint64_t pid{0};
  std::uint64_t generation{0};
  std::string model_name;
  std::string model_version;
  std::string feature_contract_version;
  std::vector<std::string> labels;
  std::optional<double> gate_threshold;
  std::string python_version;
  std::string worker_version;
  std::map<std::string, std::string> library_versions;
  std::vector<std::string> load_warnings;
  std::optional<double> startup_ms;  // Spawn to validated ready.
  std::optional<double> verify_ms;
  std::optional<double> dependency_import_ms;
  std::optional<double> load_ms;
  std::optional<double> last_restart_ms;  // Restart request to ready.
  std::uint64_t restarts{0};
  std::uint64_t requests{0};
  std::uint64_t completed{0};
  std::uint64_t timeouts{0};
  std::uint64_t failures{0};
  std::uint64_t stale_discarded{0};
  std::optional<double> last_round_trip_ms;
  std::optional<double> last_inference_ms;
  std::string last_error;
  ProcessResources resources;
  std::vector<std::string> stderr_tail;
};

// The inference seam the AI provider uses; tests substitute a scripted fake.
class IModelInference {
 public:
  virtual ~IModelInference() = default;
  virtual InferenceResponse Infer(const InferenceRequest& request) = 0;
  virtual WorkerStatus Status() const = 0;
  // The manifest's model version and contract the responses must carry.
  virtual std::string ExpectedModelVersion() const = 0;
  virtual std::string ModelName() const = 0;
};

class ModelWorkerClient final : public IModelInference {
 public:
  ModelWorkerClient(WorkerConfig config, ModelEventLog* events);
  ~ModelWorkerClient() override;
  ModelWorkerClient(const ModelWorkerClient&) = delete;
  ModelWorkerClient& operator=(const ModelWorkerClient&) = delete;

  // Spawns the worker; false when configuration or spawning fails.
  bool Start();
  // Blocks until Ready or a terminal state; true when Ready.
  bool WaitUntilReady(double timeout_ms);
  // Sends "shutdown", waits briefly, then kills. Ends in Stopped.
  void Stop();
  // Manual restart (also resets the automatic restart budget).
  bool Restart(const std::string& reason);
  // Kills the worker without stopping the client (failure injection for
  // tests and the Developer Tools diagnostic action).
  void KillWorkerForTesting();
  // Health probe: a ping round trip within timeout_ms.
  bool Ping(double timeout_ms);

  InferenceResponse Infer(const InferenceRequest& request) override;
  WorkerStatus Status() const override;
  std::string ExpectedModelVersion() const override;
  std::string ModelName() const override;
  const WorkerConfig& config() const { return config_; }

 private:
  struct Generation;
  void Spawn(const std::string& reason, bool is_restart);
  void ReaderLoop(std::shared_ptr<Generation> gen);
  void StderrLoop(std::shared_ptr<Generation> gen);
  void HandleLine(const std::shared_ptr<Generation>& gen, const std::string& line);
  void HandleExit(const std::shared_ptr<Generation>& gen);
  void ScheduleRestart(const std::string& reason);  // Requires mutex_.
  void Fail(const std::string& reason);              // Requires mutex_.
  void MaintainWorker();
  void RetireGeneration();
  InferenceResponse Parse(const JsonValue& message, const InferenceRequest& request);

  const WorkerConfig config_;
  ModelEventLog* events_;
  std::optional<ModelManifest> manifest_;
  std::string manifest_error_;

  mutable std::mutex mutex_;
  std::condition_variable cv_;
  WorkerStatus status_;
  std::shared_ptr<Generation> current_;
  std::chrono::steady_clock::time_point spawned_at_{};
  std::optional<std::chrono::steady_clock::time_point> restart_requested_at_;
  std::chrono::steady_clock::time_point restart_due_{};
  std::deque<std::chrono::steady_clock::time_point> restart_times_;
  int consecutive_timeouts_{0};

  // The request being awaited and its response slot (guarded by mutex_).
  std::string awaited_id_;
  std::optional<JsonValue> response_;
  std::string response_error_;

  std::timed_mutex infer_mutex_;
  std::atomic<std::uint64_t> sequence_{0};
};

// Defaults from the environment and the build: AAFR_MODEL_PYTHON,
// AAFR_MODEL_DIR, AAFR_MODEL_MANIFEST, AAFR_MODEL_WORKER (each overriding the
// value compiled in from CMake, if any).
WorkerConfig WorkerConfigFromEnvironment();

}  // namespace aafr::model
