#pragma once

// The browser's V0.5 forwarding service. Plain C++17, no Qt.
//
// Owns the controlled loopback topology (LoopbackDemoGraph), its relays on
// 127.0.0.1, the loopback executor, implementation verification and the
// manual AlgorithmExecutionEngine. Start() returns at once; relays are
// started and the ten implementations verified on a background thread, and
// the verified registry is then handed to the engine (and to whoever asked
// through the callback).
//
// V0.6: the topology is versioned. SetEdgeLatency / SetEdgeAvailable /
// RestoreConfiguredTopology publish a new immutable graph version (copy on
// write) and apply it to the running relays, so a controlled condition such
// as a congested relay can be reproduced on real loopback forwarding. Readers
// take GraphSnapshot(); a snapshot never changes after it is published.
//
// Browser traffic does not pass through these relays: Qt WebEngine offers no
// per-profile or per-request proxy routing in this build, and AAFR does not
// change the application-wide or system proxy. The relays carry only the
// requests the engine sends after a manual execution.

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "algorithms/execution/algorithm_execution_engine.h"
#include "algorithms/execution/implementation_verifier.h"
#include "routing/forwarding/loopback_forwarding.h"
#include "routing/topology/forwarding_graph.h"
#include "routing/topology/forwarding_problem.h"

namespace aafr::forwarding {

class ForwardingService {
 public:
  enum class State { NotStarted, Starting, Ready, Failed };

  struct Status {
    State state{State::NotStarted};
    std::string message;
    bool relays_running{false};
    std::string executor;
    std::size_t implementations_passed{0};
    std::size_t verified_executable{0};
    double verification_ms{0.0};
  };

  struct CandidatePath {
    Path path;
    PathValidation validation;
  };

  // Called on the worker thread once verification finishes.
  using RegistryCallback = std::function<void(const algorithms::CapabilityRegistry&)>;

  explicit ForwardingService(telemetry::TelemetryPipeline* pipeline, LoopbackOptions options = {});
  ~ForwardingService();
  ForwardingService(const ForwardingService&) = delete;
  ForwardingService& operator=(const ForwardingService&) = delete;

  void Start(RegistryCallback on_verified = {});
  // Cancels running work, stops the relays and joins the worker.
  void Stop();

  Status GetStatus() const;
  std::optional<algorithms::VerificationReport> Verification() const;
  // The current graph version. The reference stays valid for the service's
  // lifetime (every published version is retained), but prefer
  // GraphSnapshot() on threads other than the one that changes the topology.
  const ForwardingGraph& graph() const { return *GraphSnapshot(); }
  std::shared_ptr<const ForwardingGraph> GraphSnapshot() const;
  // The problem posed against the current graph version.
  ForwardingProblem Problem() const;

  // Controlled topology changes (V0.6). Each publishes a new graph version
  // and syncs the relays. The injected value is labelled Simulated: it is a
  // condition AAFR imposes on its own loopback relays, not a measurement.
  // False (with a reason) for unknown edges, invalid values, or once
  // kMaxGraphVersions versions exist.
  static constexpr std::size_t kMaxGraphVersions = 512;
  bool SetEdgeLatency(const std::string& from, const std::string& to, double latency_ms, std::string* error = nullptr);
  bool SetEdgeAvailable(const std::string& from, const std::string& to, bool available, std::string* error = nullptr);
  // Back to the configured LoopbackDemoGraph values (as a new version).
  bool RestoreConfiguredTopology(std::string* error = nullptr);
  // Whether the relays sleep each edge's latency before forwarding.
  bool InjectsLatency() const;
  algorithms::AlgorithmExecutionEngine& engine() { return *engine_; }
  const algorithms::AlgorithmExecutionEngine& engine() const { return *engine_; }
  const LoopbackForwardingNetwork& network() const { return *network_; }
  // The loopback executor, for the V0.6 runtime's activation, probe and
  // health requests. Lives as long as the service.
  IForwardingExecutor* executor() const { return executor_.get(); }

  // A request for one manual run on the controlled topology.
  algorithms::ExecutionRequest MakeRequest(algorithms::AlgorithmId id, std::uint64_t seed,
                                           std::uint64_t max_iterations, bool forward,
                                           std::optional<netstate::NetworkState> state,
                                           bool enforce_host_state_checks) const;
  // Every simple entry -> destination path with its validation.
  std::vector<CandidatePath> CandidatePaths() const;

 private:
  telemetry::TelemetryPipeline* pipeline_;
  bool Publish(ForwardingGraph next, std::string* error);

  // Guarded by graph_mutex_. graphs_ holds every published version; the last
  // one is current.
  mutable std::mutex graph_mutex_;
  std::vector<std::shared_ptr<const ForwardingGraph>> graphs_;
  std::unique_ptr<LoopbackForwardingNetwork> network_;
  std::unique_ptr<LoopbackForwardingExecutor> executor_;
  std::unique_ptr<algorithms::AlgorithmExecutionEngine> engine_;

  mutable std::mutex mutex_;
  Status status_;
  std::optional<algorithms::VerificationReport> report_;
  std::atomic<bool> cancel_{false};
  std::thread worker_;
};

const char* ToString(ForwardingService::State value);

}  // namespace aafr::forwarding
