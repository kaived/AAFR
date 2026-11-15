#include "algorithms/execution/algorithm_execution_engine.h"

#include <algorithm>
#include <cstdio>

#include "network/network_state/network_constraints.h"

namespace aafr::algorithms {
namespace {

namespace fwd = forwarding;
using telemetry::Attribute;
using telemetry::EventType;

std::string Number(double value) {
  char buffer[48];
  std::snprintf(buffer, sizeof(buffer), "%.6g", value);
  return buffer;
}

std::string Upper(std::string text) {
  for (char& c : text) c = static_cast<char>(c >= 'a' && c <= 'z' ? c - 'a' + 'A' : c);
  return text;
}

bool HostStateCheck(EligibilityCheck check) {
  return check == EligibilityCheck::RequiredFeatures || check == EligibilityCheck::DataQuality;
}

// Simple source-to-destination paths over usable edges, counted by DFS and
// capped at `cap` (the count is then "at least cap").
std::uint64_t CountSimplePaths(const fwd::ProblemView& view, std::uint64_t cap) {
  if (view.source < 0 || view.destination < 0) return 0;
  std::uint64_t count = 0;
  std::uint64_t steps = 0;
  std::vector<char> on_path(view.node_ids.size(), 0);
  struct Frame {
    int node;
    std::size_t next;
  };
  std::vector<Frame> stack{{view.source, 0}};
  on_path[static_cast<std::size_t>(view.source)] = 1;
  while (!stack.empty() && count < cap && ++steps < cap * 64) {
    Frame& top = stack.back();
    const auto& edges = view.out[static_cast<std::size_t>(top.node)];
    if (top.node == view.destination || top.next >= edges.size()) {
      if (top.node == view.destination) ++count;
      on_path[static_cast<std::size_t>(top.node)] = 0;
      stack.pop_back();
      continue;
    }
    const int next = edges[top.next++].to;
    if (on_path[static_cast<std::size_t>(next)]) continue;
    on_path[static_cast<std::size_t>(next)] = 1;
    stack.push_back({next, 0});
  }
  return std::min(count, cap);
}

}  // namespace

bool ExecutionRecord::Finished() const {
  return stage == ExecutionStage::Completed || stage == ExecutionStage::Rejected ||
         stage == ExecutionStage::Failed || stage == ExecutionStage::Cancelled;
}

AlgorithmExecutionEngine::AlgorithmExecutionEngine(telemetry::TelemetryPipeline* pipeline,
                                                   CapabilityRegistry registry,
                                                   fwd::IForwardingExecutor* executor)
    : AlgorithmExecutionEngine(pipeline, std::move(registry), executor, Config{}) {}

AlgorithmExecutionEngine::AlgorithmExecutionEngine(telemetry::TelemetryPipeline* pipeline,
                                                   CapabilityRegistry registry,
                                                   fwd::IForwardingExecutor* executor, Config config)
    : pipeline_(pipeline), config_(config), registry_(std::move(registry)), executor_(executor) {}

AlgorithmExecutionEngine::~AlgorithmExecutionEngine() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stop_ = true;
    for (auto& entry : cancel_flags_) entry.second->store(true);
  }
  wake_.notify_all();
  if (worker_.joinable()) worker_.join();
}

std::string AlgorithmExecutionEngine::NewRecord(const ExecutionRequest& request,
                                                std::shared_ptr<std::atomic<bool>> cancel) {
  std::lock_guard<std::mutex> lock(mutex_);
  ExecutionRecord record;
  record.execution_id = "exec-" + std::to_string(next_id_++);
  record.algorithm = request.algorithm;
  record.scenario_id = request.scenario_id;
  record.network_state_id = request.network_state ? request.network_state->state_id : 0;
  if (request.graph) {
    record.graph_id = request.graph->id();
    record.graph_version = request.graph->version();
  }
  record.submitted_at = telemetry::WallClock::now();
  records_.push_back(record);
  while (records_.size() > config_.history && records_.front().Finished()) {
    cancel_flags_.erase(records_.front().execution_id);
    records_.pop_front();
  }
  cancel_flags_[record.execution_id] = std::move(cancel);
  return record.execution_id;
}

template <typename Update>
void AlgorithmExecutionEngine::Modify(const std::string& id, Update update) {
  std::lock_guard<std::mutex> lock(mutex_);
  for (ExecutionRecord& record : records_) {
    if (record.execution_id == id) {
      update(record);
      return;
    }
  }
}

std::string AlgorithmExecutionEngine::Submit(ExecutionRequest request) {
  auto cancel = std::make_shared<std::atomic<bool>>(false);
  const std::string id = NewRecord(request, cancel);
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (queue_.size() >= config_.max_pending || stop_) {
      for (ExecutionRecord& record : records_) {
        if (record.execution_id != id) continue;
        record.stage = ExecutionStage::Rejected;
        record.outcome = "QUEUE_FULL";
        record.message = "too many executions are waiting";
        record.finished_at = telemetry::WallClock::now();
      }
      return id;
    }
    queue_.push_back({id, std::move(request), cancel});
    if (!worker_.joinable()) worker_ = std::thread([this] { Run(); });
  }
  wake_.notify_one();
  return id;
}

ExecutionRecord AlgorithmExecutionEngine::RunSync(ExecutionRequest request) {
  auto cancel = std::make_shared<std::atomic<bool>>(false);
  const std::string id = NewRecord(request, cancel);
  return Process(id, request, *cancel);
}

void AlgorithmExecutionEngine::Run() {
  for (;;) {
    Job job;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      wake_.wait(lock, [this] { return stop_ || !queue_.empty(); });
      if (stop_ && queue_.empty()) return;
      job = std::move(queue_.front());
      queue_.pop_front();
    }
    Process(job.id, job.request, *job.cancel);
  }
}

bool AlgorithmExecutionEngine::Cancel(const std::string& execution_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto found = cancel_flags_.find(execution_id);
  if (found == cancel_flags_.end()) return false;
  for (const ExecutionRecord& record : records_) {
    if (record.execution_id == execution_id && record.Finished()) return false;
  }
  found->second->store(true);
  return true;
}

void AlgorithmExecutionEngine::CancelAll() {
  std::lock_guard<std::mutex> lock(mutex_);
  for (auto& entry : cancel_flags_) entry.second->store(true);
}

std::optional<ExecutionRecord> AlgorithmExecutionEngine::Find(const std::string& execution_id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  for (const ExecutionRecord& record : records_) {
    if (record.execution_id == execution_id) return record;
  }
  return std::nullopt;
}

std::vector<ExecutionRecord> AlgorithmExecutionEngine::Recent() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return {records_.begin(), records_.end()};
}

std::optional<ExecutionRecord> AlgorithmExecutionEngine::Latest() const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (records_.empty()) return std::nullopt;
  return records_.back();
}

std::size_t AlgorithmExecutionEngine::Pending() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return queue_.size();
}

void AlgorithmExecutionEngine::SetRegistry(CapabilityRegistry registry) {
  std::lock_guard<std::mutex> lock(mutex_);
  registry_ = std::move(registry);
}

CapabilityRegistry AlgorithmExecutionEngine::Registry() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return registry_;
}

void AlgorithmExecutionEngine::SetExecutor(fwd::IForwardingExecutor* executor) {
  std::lock_guard<std::mutex> lock(mutex_);
  executor_ = executor;
}

bool AlgorithmExecutionEngine::ForwardingAvailable() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return executor_ && executor_->Ready();
}

netstate::ExecutionEnvironment AlgorithmExecutionEngine::EnvironmentFor(AlgorithmId id,
                                                                       const ExecutionRequest& request) const {
  netstate::ExecutionEnvironment env;
  const CapabilityRegistry registry = Registry();
  fwd::IForwardingExecutor* executor = nullptr;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    executor = executor_;
  }
  if (!request.graph) {
    env.topology = netstate::ExecutionEnvironment::Topology{false, {}};
    return env;
  }
  const fwd::ProblemView view = fwd::BuildView(*request.graph, request.problem);
  netstate::ExecutionEnvironment::Topology topology;
  topology.available = true;
  netstate::TopologyInfo& info = topology.info;
  // The graph is available directly and, when the destination is reachable,
  // as the set of simple paths the metaheuristics' encodings decode to.
  info.representations = {netstate::TopologyRepresentation::WeightedDirectedGraph};
  info.node_count = request.graph->nodes().size();
  info.edge_count = view.usable_edge_count;
  info.reachable = Reachable(view);
  info.candidate_path_count = CountSimplePaths(view, 100000);
  if (info.candidate_path_count > 0) {
    info.representations.insert(netstate::TopologyRepresentation::CandidatePathSet);
  }
  netstate::EdgeWeightInfo weights;
  weights.all_finite = true;  // Costs come only from validated finite values.
  weights.has_negative = view.has_negative_cost;
  if (view.has_negative_cost) {
    AlgorithmRequest probe;
    probe.graph = request.graph.get();
    probe.problem = &request.problem;
    weights.has_negative_cycle = CreateAlgorithm(AlgorithmId::BellmanFord)->Run(probe).status ==
                                 AlgorithmStatus::NegativeCycle;
  } else {
    weights.has_negative_cycle = false;
  }
  info.edge_weights = weights;
  info.heuristic_available = true;  // zero and hop_lower_bound are always computable.
  env.topology = topology;

  if (executor && executor->Ready()) {
    const fwd::ForwardingCapability capability = executor->Capability();
    env.forwarding_operations = capability.operations;
    env.supported_protocols = capability.protocols;
  } else {
    env.forwarding_operations = std::set<netstate::ForwardingOperation>{};
  }
  env.required_protocols = {request.problem.constraints.protocol};
  const AlgorithmCapability* capability = registry.Find(id);
  const bool metaheuristic = capability && capability->properties.family == AlgorithmFamily::Metaheuristic;
  env.problem = metaheuristic ? netstate::ProblemRepresentation::CandidateSolutionOptimization
                              : netstate::ProblemRepresentation::SinglePairShortestPath;
  env.objective = netstate::OptimizationObjective::MinimizeAdditivePathCost;
  env.computation_deadline_ms = request.problem.constraints.computation_deadline_ms
                                    ? request.problem.constraints.computation_deadline_ms
                                    : request.budget.deadline_ms ? request.budget.deadline_ms
                                                                 : std::optional<double>(config_.default_deadline_ms);
  env.memory_budget_bytes = request.problem.constraints.memory_budget_bytes
                                ? *request.problem.constraints.memory_budget_bytes
                                : config_.default_memory_budget_bytes;

  if (const auto algorithm = CreateAlgorithm(id)) {
    std::map<std::string, std::string> configuration = algorithm->DefaultParameters();
    for (const auto& entry : request.parameters) configuration[entry.first] = entry.second;
    configuration["solution_encoding"] = algorithm->encoding();
    configuration["objective_function"] = "forwarding_objective_v" + std::to_string(fwd::ForwardingObjective::kVersion);
    configuration["max_iterations"] = std::to_string(request.budget.max_iterations);
    configuration["random_seed"] = std::to_string(request.seed);
    env.algorithm_configuration[ToString(id)] = configuration;
  }
  return env;
}

EligibilityResult AlgorithmExecutionEngine::Eligibility(AlgorithmId id, const ExecutionRequest& request) const {
  const netstate::ExecutionEnvironment env = EnvironmentFor(id, request);
  const netstate::NetworkState state = request.network_state.value_or(netstate::NetworkState{});
  const netstate::NetworkConstraints constraints = netstate::ConstraintEvaluator{}.Evaluate(state, env);
  const CapabilityRegistry registry = Registry();
  const AlgorithmCapability* capability = registry.Find(id);
  if (!capability) {
    EligibilityResult result;
    result.algorithm = id;
    result.status = EligibilityStatus::NotImplemented;
    result.reason_codes = {ReasonCode::ImplementationUnavailable};
    result.explanation = "no capability profile is registered";
    return result;
  }
  return AlgorithmEligibilityEvaluator{}.Evaluate(state, constraints, *capability, env);
}

void AlgorithmExecutionEngine::Publish(EventType type, std::vector<Attribute> attributes) const {
  if (!pipeline_) return;
  telemetry::TelemetryEvent event;
  event.type = type;
  event.source = telemetry::Source::ForwardingEngine;
  event.scope = telemetry::MeasurementScope::Pipeline;
  event.attribution = telemetry::AttributionQuality::NotApplicable;
  event.attributes = std::move(attributes);
  pipeline_->Publish(std::move(event));
}

ExecutionRecord AlgorithmExecutionEngine::Process(const std::string& id, const ExecutionRequest& request,
                                                  const std::atomic<bool>& cancel) {
  const std::string algorithm_name = ToString(request.algorithm);
  std::vector<Attribute> base = {{"execution_id", id},
                                 {"algorithm", algorithm_name},
                                 {"scenario_id", request.scenario_id},
                                 {"network_state_id", std::to_string(request.network_state ? request.network_state->state_id : 0)}};
  if (request.graph) {
    base.push_back({"graph_id", request.graph->id()});
    base.push_back({"graph_version", std::to_string(request.graph->version())});
  }
  const auto with = [&base](std::vector<Attribute> more) {
    std::vector<Attribute> all = base;
    all.insert(all.end(), more.begin(), more.end());
    return all;
  };
  const auto finish = [&](ExecutionStage stage, const std::string& outcome, const std::string& message) {
    ExecutionRecord copy;
    Modify(id, [&](ExecutionRecord& r) {
      r.stage = stage;
      r.outcome = outcome;
      r.message = message;
      r.finished_at = telemetry::WallClock::now();
      copy = r;
    });
    return copy;
  };

  if (cancel.load()) {
    Publish(EventType::AlgorithmExecutionFailed, with({{"stage", "queued"}, {"reason", "CANCELLED"}}));
    return finish(ExecutionStage::Cancelled, "CANCELLED", "cancelled before it started");
  }
  Modify(id, [](ExecutionRecord& r) { r.stage = ExecutionStage::CheckingEligibility; });

  // ---- Request and problem validation ----
  const auto algorithm = CreateAlgorithm(request.algorithm);
  std::vector<std::string> issues;
  if (!request.graph) issues.push_back("no graph");
  if (!algorithm) issues.push_back("no implementation for " + algorithm_name);
  if (request.graph) {
    for (const std::string& issue : fwd::ValidateProblem(*request.graph, request.problem)) issues.push_back(issue);
  }
  if (algorithm) {
    AlgorithmParameters parameters = algorithm->DefaultParameters();
    for (const auto& entry : request.parameters) {
      if (parameters.count(entry.first) == 0) issues.push_back("unknown parameter '" + entry.first + "'");
      parameters[entry.first] = entry.second;
    }
    for (const std::string& issue : algorithm->ValidateParameters(parameters)) issues.push_back(issue);
  }
  if (!issues.empty()) {
    Publish(EventType::AlgorithmExecutionFailed, with({{"stage", "validation"}, {"reason", "INVALID_REQUEST"},
                                                       {"detail", issues.front()}}));
    return finish(ExecutionStage::Rejected, "INVALID_REQUEST", issues.front());
  }

  // ---- V0.4 eligibility ----
  const EligibilityResult eligibility = Eligibility(request.algorithm, request);
  bool proceed = eligibility.status == EligibilityStatus::Eligible;
  std::vector<std::string> overridden;
  if (!proceed && !request.enforce_host_state_checks && eligibility.status != EligibilityStatus::NotImplemented) {
    bool others_pass = true;
    for (const CheckResult& check : eligibility.checks) {
      if (check.outcome == CheckOutcome::Passed) continue;
      if (HostStateCheck(check.check)) overridden.push_back(ToString(check.check));
      else others_pass = false;
    }
    proceed = others_pass;
  }
  Modify(id, [&](ExecutionRecord& r) {
    r.eligibility = eligibility;
    r.overridden_checks = overridden;
  });
  if (!proceed) {
    const std::string codes = ReasonCodesText(eligibility.reason_codes);
    Publish(EventType::AlgorithmExecutionFailed,
            with({{"stage", "eligibility"}, {"reason", "INELIGIBLE"}, {"eligibility", ToString(eligibility.status)},
                  {"reason_codes", codes}}));
    return finish(ExecutionStage::Rejected, "INELIGIBLE",
                  std::string("eligibility is ") + ToString(eligibility.status) + ": " + codes);
  }

  // ---- Computation ----
  Modify(id, [](ExecutionRecord& r) { r.stage = ExecutionStage::Computing; });
  Publish(EventType::AlgorithmExecutionStarted,
          with({{"seed", std::to_string(request.seed)},
                {"max_iterations", std::to_string(request.budget.max_iterations)},
                {"overridden_checks", overridden.empty() ? "none" : [&] {
                   std::string text;
                   for (const auto& name : overridden) text += (text.empty() ? "" : ",") + name;
                   return text;
                 }()}}));
  AlgorithmRequest computation;
  computation.graph = request.graph.get();
  computation.problem = &request.problem;
  computation.budget = request.budget;
  if (!computation.budget.deadline_ms) computation.budget.deadline_ms = config_.default_deadline_ms;
  computation.parameters = request.parameters;
  computation.seed = request.seed;
  computation.cancel = &cancel;
  const AlgorithmResult result = algorithm->Run(computation);
  Modify(id, [&](ExecutionRecord& r) { r.computation = result; });
  const std::vector<Attribute> computed = {
      {"status", ToString(result.status)},
      {"termination", ToString(result.termination)},
      {"computation_ms", Number(result.duration_ms)},
      {"iterations", std::to_string(result.iterations)},
      {"evaluations", std::to_string(result.evaluations)},
      {"objective", result.objective ? Number(*result.objective) : "none"},
      {"feasible", result.feasible ? "true" : "false"},
      {"path", fwd::PathText(result.path)}};
  if (result.status == AlgorithmStatus::Cancelled || cancel.load()) {
    Publish(EventType::AlgorithmExecutionFailed, with(computed));
    return finish(ExecutionStage::Cancelled, "CANCELLED", "cancelled during computation");
  }
  if (result.path.empty()) {
    Publish(EventType::AlgorithmExecutionFailed, with(computed));
    return finish(ExecutionStage::Failed, "COMPUTATION_" + Upper(ToString(result.status)), result.message);
  }
  Publish(EventType::AlgorithmExecutionCompleted, with(computed));

  // ---- Candidate validation: nothing is forwarded unless it passes ----
  Modify(id, [](ExecutionRecord& r) { r.stage = ExecutionStage::ValidatingCandidate; });
  const fwd::PathValidation validation = fwd::ValidatePath(*request.graph, request.problem, result.path);
  Modify(id, [&](ExecutionRecord& r) { r.candidate = validation; });
  if (!validation.valid || !validation.feasible) {
    std::string codes;
    for (const fwd::PathIssue issue : validation.issues) codes += (codes.empty() ? "" : ",") + std::string(fwd::ToString(issue));
    Publish(EventType::CandidatePathRejected,
            with({{"path", fwd::PathText(result.path)}, {"issues", codes},
                  {"detail", validation.details.empty() ? "" : validation.details.front()}}));
    return finish(ExecutionStage::Failed, "CANDIDATE_REJECTED",
                  validation.details.empty() ? codes : validation.details.front());
  }
  if (!request.execute_forwarding) {
    return finish(ExecutionStage::Completed, "COMPLETED_WITHOUT_FORWARDING",
                  "candidate validated; forwarding was not requested");
  }

  // ---- Forwarding ----
  fwd::IForwardingExecutor* executor = nullptr;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    executor = executor_;
  }
  const std::string request_id = id + "-fwd";
  Modify(id, [&](ExecutionRecord& r) {
    r.stage = ExecutionStage::Forwarding;
    r.forwarding_request_id = request_id;
  });
  if (!executor || !executor->Ready()) {
    Publish(EventType::ForwardingExecutionFailed,
            with({{"request_id", request_id}, {"reason", "FORWARDING_UNAVAILABLE"}}));
    return finish(ExecutionStage::Failed, "FORWARDING_UNAVAILABLE", "no forwarding executor is ready");
  }
  const fwd::ForwardingCapability capability = executor->Capability();
  Publish(EventType::ForwardingExecutionStarted,
          with({{"request_id", request_id}, {"executor", capability.executor}, {"requested_path", fwd::PathText(result.path)}}));
  fwd::ForwardingRequest forwarding;
  forwarding.request_id = request_id;
  forwarding.path = result.path;
  forwarding.payload = "aafr-v0.5 " + id;
  forwarding.timeout_ms = request.forwarding_timeout_ms;
  forwarding.graph_version = 0;  // The executor's own topology is checked by the executor.
  fwd::ForwardingExecutionResult outcome;
  {
    std::lock_guard<std::mutex> serial(forwarding_mutex_);
    outcome = executor->Execute(forwarding, {&cancel, id, request.scenario_id});
  }
  Modify(id, [&](ExecutionRecord& r) { r.forwarding = outcome; });
  const std::vector<Attribute> forwarded = {
      {"request_id", request_id},
      {"executor", capability.executor},
      {"status", fwd::ToString(outcome.status)},
      {"response_status", outcome.response_status},
      {"requested_path", fwd::PathText(outcome.requested_path)},
      {"confirmed_path", fwd::PathText(outcome.confirmed_path)},
      {"path_confirmed", outcome.path_confirmed ? "true" : "false"},
      {"end_to_end_ms", Number(outcome.end_to_end_ms)},
      {"provenance", fwd::ToString(outcome.provenance)}};
  if (outcome.status == fwd::ForwardingStatus::Delivered && outcome.path_confirmed) {
    Publish(EventType::ForwardingExecutionCompleted, with(forwarded));
    return finish(ExecutionStage::Completed, "COMPLETED", outcome.evidence);
  }
  Publish(EventType::ForwardingExecutionFailed, with(forwarded));
  if (outcome.status == fwd::ForwardingStatus::Cancelled) {
    return finish(ExecutionStage::Cancelled, "CANCELLED", "cancelled during forwarding");
  }
  return finish(ExecutionStage::Failed,
                outcome.status == fwd::ForwardingStatus::Delivered ? "PATH_NOT_CONFIRMED"
                                                                   : "FORWARDING_" + Upper(fwd::ToString(outcome.status)),
                outcome.message);
}

const char* ToString(ExecutionStage value) {
  switch (value) {
    case ExecutionStage::Queued: return "queued";
    case ExecutionStage::CheckingEligibility: return "checking_eligibility";
    case ExecutionStage::Computing: return "computing";
    case ExecutionStage::ValidatingCandidate: return "validating_candidate";
    case ExecutionStage::Forwarding: return "forwarding";
    case ExecutionStage::Completed: return "completed";
    case ExecutionStage::Rejected: return "rejected";
    case ExecutionStage::Failed: return "failed";
    case ExecutionStage::Cancelled: return "cancelled";
  }
  return "unknown";
}

}  // namespace aafr::algorithms
