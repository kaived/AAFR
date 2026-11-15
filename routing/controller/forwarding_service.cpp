#include "routing/controller/forwarding_service.h"

#include <functional>

#include "routing/topology/scenario_catalog.h"

namespace aafr::forwarding {

ForwardingService::ForwardingService(telemetry::TelemetryPipeline* pipeline)
    : pipeline_(pipeline), graph_(std::make_shared<const ForwardingGraph>(LoopbackDemoGraph())) {
  network_ = std::make_unique<LoopbackForwardingNetwork>(*graph_);
  executor_ = std::make_unique<LoopbackForwardingExecutor>(*network_);
  // Until verification finishes the engine sees the V0.4 registry, in which
  // nothing is implemented, so every request is refused as ineligible.
  engine_ = std::make_unique<algorithms::AlgorithmExecutionEngine>(pipeline_, algorithms::CapabilityRegistry::BuiltIn(),
                                                                   executor_.get());
}

ForwardingService::~ForwardingService() { Stop(); }

void ForwardingService::Start(RegistryCallback on_verified) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (status_.state != State::NotStarted) return;
  status_.state = State::Starting;
  status_.message = "starting loopback relays and verifying implementations";
  worker_ = std::thread([this, on_verified = std::move(on_verified)] {
    std::string error;
    if (!network_->Start(&error)) {
      std::lock_guard<std::mutex> lock(mutex_);
      status_.state = State::Failed;
      status_.message = "loopback relays could not start: " + error;
      return;
    }
    algorithms::VerificationReport report = algorithms::VerifyImplementations(executor_.get(), &cancel_);
    const algorithms::CapabilityRegistry registry =
        algorithms::ApplyVerification(algorithms::CapabilityRegistry::BuiltIn(), report);
    if (cancel_) return;
    engine_->SetRegistry(registry);
    {
      std::lock_guard<std::mutex> lock(mutex_);
      status_.state = State::Ready;
      status_.relays_running = network_->Running();
      status_.executor = report.forwarding_executor;
      status_.implementations_passed = report.CountImplementationsPassed();
      status_.verified_executable = report.CountVerifiedExecutable();
      status_.verification_ms = report.total_ms;
      status_.message = std::to_string(status_.verified_executable) +
                        " of 10 implementations verified with real loopback forwarding";
      report_ = std::move(report);
    }
    if (on_verified) on_verified(registry);
  });
}

void ForwardingService::Stop() {
  cancel_ = true;
  engine_->CancelAll();
  if (worker_.joinable()) worker_.join();
  // The engine's own worker may still be forwarding; destroy order in the
  // members (engine before executor and network) takes care of the rest, but
  // stopping the relays here makes any in-flight request fail fast.
  network_->Stop();
  std::lock_guard<std::mutex> lock(mutex_);
  status_.relays_running = false;
}

ForwardingService::Status ForwardingService::GetStatus() const {
  std::lock_guard<std::mutex> lock(mutex_);
  Status status = status_;
  status.relays_running = network_->Running();
  return status;
}

std::optional<algorithms::VerificationReport> ForwardingService::Verification() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return report_;
}

ForwardingProblem ForwardingService::Problem() const { return LoopbackDemoProblem(*graph_); }

algorithms::ExecutionRequest ForwardingService::MakeRequest(algorithms::AlgorithmId id, std::uint64_t seed,
                                                            std::uint64_t max_iterations, bool forward,
                                                            std::optional<netstate::NetworkState> state,
                                                            bool enforce_host_state_checks) const {
  algorithms::ExecutionRequest request;
  request.algorithm = id;
  request.graph = graph_;
  request.problem = Problem();
  request.budget.max_iterations = max_iterations;
  request.seed = seed;
  request.execute_forwarding = forward;
  request.scenario_id = "browser_manual";
  request.network_state = std::move(state);
  request.enforce_host_state_checks = enforce_host_state_checks;
  return request;
}

std::vector<ForwardingService::CandidatePath> ForwardingService::CandidatePaths() const {
  const ForwardingProblem problem = Problem();
  const ProblemView view = BuildView(*graph_, problem);
  std::vector<CandidatePath> paths;
  std::vector<char> on_path(view.node_ids.size(), 0);
  Path current;
  std::function<void(int)> visit = [&](int node) {
    if (paths.size() >= 64) return;
    current.push_back(view.node_ids[static_cast<std::size_t>(node)]);
    on_path[static_cast<std::size_t>(node)] = 1;
    if (node == view.destination) {
      paths.push_back({current, ValidatePath(*graph_, problem, current)});
    } else {
      for (const UsableEdge& edge : view.out[static_cast<std::size_t>(node)]) {
        if (!on_path[static_cast<std::size_t>(edge.to)]) visit(edge.to);
      }
    }
    on_path[static_cast<std::size_t>(node)] = 0;
    current.pop_back();
  };
  if (view.source >= 0) visit(view.source);
  return paths;
}

const char* ToString(ForwardingService::State value) {
  switch (value) {
    case ForwardingService::State::NotStarted: return "not_started";
    case ForwardingService::State::Starting: return "starting";
    case ForwardingService::State::Ready: return "ready";
    case ForwardingService::State::Failed: return "failed";
  }
  return "unknown";
}

}  // namespace aafr::forwarding
