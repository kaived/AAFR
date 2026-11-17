#include "runtime/adaptive/aafr_runtime.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <fstream>
#include <functional>
#include <sstream>

#include "algorithms/common/path_algorithm.h"

namespace aafr::runtime {

namespace fwd = aafr::forwarding;
namespace alg = aafr::algorithms;

std::vector<std::string> RuntimeConfig::Validate() const {
  std::vector<std::string> issues;
  for (char c : session_id) {
    if (!std::isalnum(static_cast<unsigned char>(c))) {
      issues.push_back("session_id may contain only letters and digits");
      break;
    }
  }
  if (session_id.size() > 24) issues.push_back("session_id longer than 24 characters");
  if (!(tick_ms >= 10.0 && tick_ms <= 60'000.0)) issues.push_back("tick_ms must be within 10..60000");
  if (max_challengers < 1 || max_challengers > ProbeConfig::kMaxArms - 1) {
    issues.push_back("max_challengers must be within 1.." + std::to_string(ProbeConfig::kMaxArms - 1));
  }
  if (activation_attempts < 1 || activation_attempts > 5) issues.push_back("activation_attempts must be within 1..5");
  if (!(activation_timeout_ms > 0.0 && activation_timeout_ms <= 10'000.0)) issues.push_back("activation_timeout_ms must be within (0, 10000]");
  if (!(health_check_interval_ms >= 0.0)) issues.push_back("health_check_interval_ms must be nonnegative");
  if (failures_for_safety_override < 1) issues.push_back("failures_for_safety_override must be at least 1");
  if (!(computation_deadline_ms > 0.0)) issues.push_back("computation_deadline_ms must be positive");
  if (max_iterations == 0 || max_iterations > 100'000) issues.push_back("max_iterations must be within 1..100000");
  if (decision_history == 0 || event_history == 0) issues.push_back("history sizes must be positive");
  for (const auto& i : detector.Validate()) issues.push_back("detector: " + i);
  for (const auto& i : stability.Validate()) issues.push_back("stability: " + i);
  for (const auto& i : arbitration.Validate()) issues.push_back("arbitration: " + i);
  for (const auto& i : probe.Validate()) issues.push_back("probe: " + i);
  return issues;
}

struct AAFRRuntime::EvaluationInputs {
  std::string evaluation_id;
  PendingEvaluation pending;
  std::shared_ptr<const fwd::ForwardingGraph> graph;
  fwd::ForwardingProblem problem;
  std::shared_ptr<const netstate::StateSnapshot> snapshot;
  std::vector<CandidateEvaluation> candidates;
  std::optional<ActiveConfiguration> active;
  bool active_verified{false};
};

struct AAFRRuntime::Attempt {
  bool confirmed{false};
  ActivationStatus status{ActivationStatus::Failed};
  std::string request_id;
  double latency_ms{0.0};
  std::string evidence;
  std::string detail;
};

namespace {

std::string Fixed(double v, int precision = 3) {
  std::ostringstream out;
  out.setf(std::ios::fixed);
  out.precision(precision);
  out << v;
  return out.str();
}

telemetry::EventType TelemetryType(RuntimeEventType type) {
  using T = telemetry::EventType;
  switch (type) {
    case RuntimeEventType::RuntimeInitialized: return T::RuntimeInitialized;
    case RuntimeEventType::LifecycleChanged: return T::RuntimeLifecycleChanged;
    case RuntimeEventType::NetworkChangeDetected: return T::NetworkChangeDetected;
    case RuntimeEventType::EvaluationStarted: return T::RuntimeEvaluationStarted;
    case RuntimeEventType::RecommendationGenerated: return T::RecommendationGenerated;
    case RuntimeEventType::ProbingStarted: return T::ProbingStarted;
    case RuntimeEventType::ProbingCompleted: return T::ProbingCompleted;
    case RuntimeEventType::StatisticalEvaluationCompleted: return T::StatisticalEvaluationCompleted;
    case RuntimeEventType::AlgorithmRetained: return T::AlgorithmRetained;
    case RuntimeEventType::SwitchRequested: return T::SwitchRequested;
    case RuntimeEventType::SwitchCommitted: return T::SwitchCommitted;
    case RuntimeEventType::SwitchFailed: return T::SwitchFailed;
    case RuntimeEventType::RecoveryStarted: return T::RecoveryStarted;
    case RuntimeEventType::RecoveryCompleted: return T::RecoveryCompleted;
    case RuntimeEventType::EvaluationDiscarded: return T::RuntimeEvaluationDiscarded;
  }
  return T::PipelineNotice;
}

bool HostStateCheck(alg::EligibilityCheck check) {
  return check == alg::EligibilityCheck::RequiredFeatures || check == alg::EligibilityCheck::DataQuality;
}

void AddReason(DecisionRecord& r, RuntimeReason reason) {
  if (!r.HasReason(reason)) r.reasons.push_back(reason);
}

std::string QualityText(const netstate::QualitySummary& q) {
  return "valid=" + std::to_string(q.valid) + " missing=" + std::to_string(q.missing) + " stale=" +
         std::to_string(q.stale) + " insufficient=" + std::to_string(q.insufficient_samples) + " invalid=" +
         std::to_string(q.invalid) + " unattributed=" + std::to_string(q.unattributed);
}

std::string HashText(const std::string& text) {
  std::ostringstream out;
  out << std::hex << std::hash<std::string>{}(text);
  return out.str();
}

}  // namespace

AAFRRuntime::AAFRRuntime(RuntimeDependencies dependencies, RuntimeConfig config)
    : deps_(std::move(dependencies)),
      config_([&] {
        RuntimeConfig c = std::move(config);
        if (c.session_id.empty()) {
          const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
          c.session_id = "rt" + std::to_string(static_cast<unsigned long long>(now) % 1000000000000ull);
        }
        return c;
      }()),
      provider_(nullptr),
      clock_(nullptr),
      detector_(config_.detector),
      stability_(config_.stability),
      arbitration_(config_.arbitration),
      probes_(deps_.executor, deps_.clock ? deps_.clock : &steady_clock_),
      scheduler_(config_.scheduler),
      outcomes_(config_.outcome_window_ms) {
  if (!deps_.provider) owned_provider_ = std::make_unique<DeterministicRecommendationProvider>();
  provider_ = deps_.provider ? deps_.provider : owned_provider_.get();
  clock_ = deps_.clock ? deps_.clock : &steady_clock_;
  status_.session_id = config_.session_id;
  status_.provider_id = provider_->Id() + "/v" + provider_->Version();
  status_.statistical_arbitration = config_.statistical_arbitration;
  status_.health = "not started";
}

AAFRRuntime::~AAFRRuntime() { Stop(); }

std::string AAFRRuntime::NextId(const char* prefix) {
  return config_.session_id + "-" + prefix + std::to_string(++id_seq_);
}

void AAFRRuntime::AddError(const std::string& error) {
  std::lock_guard<std::mutex> lock(state_mutex_);
  status_.errors.push_back(error);
  while (status_.errors.size() > config_.error_history) status_.errors.erase(status_.errors.begin());
}

void AAFRRuntime::Emit(RuntimeEventType type, const std::string& decision_id, const std::string& detail) {
  RuntimeEvent event;
  event.monotonic_ns = clock_->NowNs();
  event.type = type;
  event.decision_id = decision_id;
  event.detail = detail;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    event.sequence = ++event_seq_;
    events_.push_back(event);
    while (events_.size() > config_.event_history) events_.pop_front();
  }
  if (deps_.pipeline) {
    telemetry::TelemetryEvent t;
    t.type = TelemetryType(type);
    t.source = telemetry::Source::AdaptiveRuntime;
    t.scope = telemetry::MeasurementScope::Pipeline;
    t.attribution = telemetry::AttributionQuality::NotApplicable;
    t.attributes = {{"runtime_session", config_.session_id}, {"event", ToString(type)}, {"detail", detail}};
    if (!decision_id.empty()) t.attributes.push_back({"decision_id", decision_id});
    deps_.pipeline->Publish(std::move(t));
  }
}

bool AAFRRuntime::SetLifecycle(RuntimeLifecycle to) {
  RuntimeLifecycle from;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    from = status_.lifecycle;
    if (from == to) return true;
    if (!IsValidTransition(from, to)) {
      status_.errors.push_back(std::string("invalid lifecycle transition ") + ToString(from) + " -> " + ToString(to));
      while (status_.errors.size() > config_.error_history) status_.errors.erase(status_.errors.begin());
      return false;
    }
    status_.lifecycle = to;
  }
  Emit(RuntimeEventType::LifecycleChanged, "", std::string(ToString(from)) + " -> " + ToString(to));
  return true;
}

void AAFRRuntime::RefreshStatusTimers(std::int64_t now_ns) {
  const double dwell = stability_.DwellRemainingMs(now_ns);
  const double cooldown = stability_.CooldownRemainingMs(now_ns);
  std::lock_guard<std::mutex> lock(state_mutex_);
  dwell_until_ns_ = now_ns + MsToNs(dwell);
  cooldown_until_ns_ = now_ns + MsToNs(cooldown);
  std::lock_guard<std::mutex> sched(sched_mutex_);
  status_.pending_evaluation = scheduler_.HasPending();
  status_.counters.coalesced_requests = scheduler_.coalesced_requests();
}

// ---------------------------------------------------------------- lifecycle

bool AAFRRuntime::Initialize() {
  std::lock_guard<std::mutex> step(step_mutex_);
  if (stop_requested_) return false;  // Stop() before (or during) startup wins.
  if (initialized_) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return status_.lifecycle == RuntimeLifecycle::Ready;
  }
  initialized_ = true;
  SetLifecycle(RuntimeLifecycle::Initializing);
  std::vector<std::string> issues = config_.Validate();
  if (!deps_.graph_source) issues.push_back("no forwarding graph source");
  if (!deps_.problem_for) issues.push_back("no problem factory");
  if (!deps_.executor) issues.push_back("no forwarding executor");
  if (!deps_.eligibility) issues.push_back("no eligibility engine");
  if (!issues.empty()) {
    for (const auto& issue : issues) AddError(issue);
    SetLifecycle(RuntimeLifecycle::Failed);
    std::lock_guard<std::mutex> lock(state_mutex_);
    status_.health = "failed: " + issues.front();
    return false;
  }
  const std::int64_t now = clock_->NowNs();
  // The startup observation becomes the change-detection baseline.
  ObserveSources(now);
  {
    std::lock_guard<std::mutex> lock(sched_mutex_);
    scheduler_.Request(EvaluationTrigger::Startup, now, "runtime initialization");
  }
  std::optional<PendingEvaluation> pending;
  {
    std::lock_guard<std::mutex> lock(sched_mutex_);
    pending = scheduler_.Take(now);
  }
  if (pending) Evaluate(*pending);
  RefreshStatusTimers(clock_->NowNs());
  bool ready;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    ready = status_.lifecycle == RuntimeLifecycle::Ready;
  }
  Emit(RuntimeEventType::RuntimeInitialized, "", ready ? "initial configuration confirmed" : "no confirmed configuration (degraded)");
  next_health_ns_ = clock_->NowNs() + MsToNs(config_.health_check_interval_ms);
  next_degraded_retry_ns_ = clock_->NowNs() + MsToNs(config_.degraded_retry_ms);
  return ready;
}

void AAFRRuntime::Start() {
  if (stop_requested_) return;
  Initialize();
  std::lock_guard<std::mutex> lock(worker_mutex_);
  if (worker_.joinable() || stop_requested_) return;
  worker_ = std::thread([this] {
    while (!stop_requested_) {
      Step();
      std::unique_lock<std::mutex> wait(worker_mutex_);
      wake_.wait_for(wait, std::chrono::microseconds(static_cast<long long>(config_.tick_ms * 1000.0)),
                     [this] { return stop_requested_.load(); });
    }
  });
}

void AAFRRuntime::Stop() {
  stop_requested_ = true;
  cancel_ = true;
  std::thread worker;
  {
    // Notify under the lock so the worker cannot miss the wake-up, and join
    // outside it: the worker takes this mutex to wait between steps.
    std::lock_guard<std::mutex> lock(worker_mutex_);
    worker = std::move(worker_);
    wake_.notify_all();
  }
  if (worker.joinable()) worker.join();
  std::lock_guard<std::mutex> step(step_mutex_);
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (status_.lifecycle == RuntimeLifecycle::Stopped) return;
  }
  SetLifecycle(RuntimeLifecycle::Stopping);
  SetLifecycle(RuntimeLifecycle::Stopped);
  std::lock_guard<std::mutex> lock(state_mutex_);
  status_.health = "stopped";
}

void AAFRRuntime::RequestEvaluation(EvaluationTrigger trigger, const std::string& reason) {
  {
    std::lock_guard<std::mutex> lock(sched_mutex_);
    scheduler_.Request(trigger, clock_->NowNs(), reason.empty() ? std::string(ToString(trigger)) : reason);
  }
  wake_.notify_all();
}

// ---------------------------------------------------------------- step

void AAFRRuntime::Step() {
  std::lock_guard<std::mutex> step(step_mutex_);
  RuntimeLifecycle lifecycle;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    lifecycle = status_.lifecycle;
  }
  if (!initialized_ || cancel_ || lifecycle == RuntimeLifecycle::Stopped || lifecycle == RuntimeLifecycle::Stopping ||
      lifecycle == RuntimeLifecycle::Failed) {
    return;
  }
  const std::int64_t now = clock_->NowNs();
  HealthCheck(now);
  ObserveSources(now);
  if (lifecycle == RuntimeLifecycle::Degraded && now >= next_degraded_retry_ns_) {
    next_degraded_retry_ns_ = now + MsToNs(config_.degraded_retry_ms);
    std::lock_guard<std::mutex> lock(sched_mutex_);
    scheduler_.Request(EvaluationTrigger::Periodic, now, "retry while degraded");
  }
  if (followup_due_ns_ != 0 && now >= followup_due_ns_) {
    followup_due_ns_ = 0;
    std::lock_guard<std::mutex> lock(sched_mutex_);
    scheduler_.Request(EvaluationTrigger::Periodic, now, "re-check a challenger blocked by time-based stability gates");
  }
  std::optional<PendingEvaluation> pending;
  {
    std::lock_guard<std::mutex> lock(sched_mutex_);
    pending = scheduler_.Take(now);
  }
  if (pending) Evaluate(*pending);
  RefreshStatusTimers(clock_->NowNs());
}

void AAFRRuntime::ObserveSources(std::int64_t now_ns) {
  const auto graph = deps_.graph_source ? deps_.graph_source() : nullptr;
  const auto snapshot = deps_.state_source ? deps_.state_source() : nullptr;
  const ChangeObservation observation =
      MakeObservation(now_ns, snapshot ? &snapshot->state : nullptr, snapshot ? &snapshot->constraints : nullptr,
                      graph.get(), detector_.config());
  const ChangeAssessment assessment = detector_.Observe(observation);
  if (assessment.duplicate) return;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    status_.last_change = assessment.explanation;
    ++(assessment.meaningful ? status_.counters.meaningful_changes : status_.counters.ignored_changes);
  }
  if (!assessment.meaningful) return;
  Emit(RuntimeEventType::NetworkChangeDetected, "", assessment.explanation);
  std::lock_guard<std::mutex> lock(sched_mutex_);
  for (EvaluationTrigger trigger : assessment.triggers) scheduler_.Request(trigger, now_ns, assessment.explanation);
}

void AAFRRuntime::HealthCheck(std::int64_t now_ns) {
  if (config_.health_check_interval_ms <= 0.0 || now_ns < next_health_ns_) return;
  next_health_ns_ = now_ns + MsToNs(config_.health_check_interval_ms);
  std::optional<ActiveConfiguration> active;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (status_.lifecycle != RuntimeLifecycle::Ready) return;
    active = status_.active;
  }
  if (!active) return;
  const auto graph = deps_.graph_source();
  fwd::ForwardingRequest request;
  request.request_id = NextId("hc");
  request.path = active->path;
  request.payload = "aafr-health";
  request.timeout_ms = config_.activation_timeout_ms;
  request.graph_version = graph ? graph->version() : 0;
  fwd::ForwardingExecutionContext context;
  context.cancel = &cancel_;
  context.execution_id = request.request_id;
  context.scenario_id = "runtime_health";
  const fwd::ForwardingExecutionResult r = deps_.executor->Execute(request, context);
  const bool ok = r.status == fwd::ForwardingStatus::Delivered && r.path_confirmed;
  OutcomeObservation o;
  o.decision_id = active->decision_id;
  o.config_version = active->config_version;
  o.algorithm = active->algorithm;
  o.kind = OutcomeKind::HealthCheck;
  o.monotonic_ns = clock_->NowNs();
  o.sample = {ok, r.status == fwd::ForwardingStatus::Timeout, r.end_to_end_ms, request.payload.size()};
  outcomes_.Record(o);
  if (ok) {
    consecutive_failures_ = 0;
  } else if (++consecutive_failures_ >= config_.failures_for_safety_override) {
    std::lock_guard<std::mutex> lock(sched_mutex_);
    scheduler_.Request(EvaluationTrigger::FailureRecovery, now_ns,
                       "active path failed " + std::to_string(consecutive_failures_.load()) + " consecutive request(s): " +
                           r.response_status);
  }
  std::lock_guard<std::mutex> lock(state_mutex_);
  status_.consecutive_failures = consecutive_failures_;
  status_.health = ok ? "healthy: active path confirmed by health check"
                      : "active path request failed (" + std::string(fwd::ToString(r.status)) + ")";
}

fwd::ForwardingExecutionResult AAFRRuntime::ForwardActive(const std::string& request_id, const std::string& payload,
                                                          double timeout_ms) {
  fwd::ForwardingExecutionResult result;
  result.request_id = request_id;
  std::optional<ActiveConfiguration> active;
  bool verified = false;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    active = status_.active;
    verified = status_.active_verified;
  }
  if (!active || !verified || !deps_.executor) {
    result.status = fwd::ForwardingStatus::RequestRejected;
    result.response_status = "NO_ACTIVE_CONFIGURATION";
    result.message = "the runtime has no confirmed active configuration";
    return result;
  }
  const auto graph = deps_.graph_source();
  fwd::ForwardingRequest request;
  request.request_id = request_id;
  request.path = active->path;
  request.payload = payload;
  request.timeout_ms = timeout_ms;
  request.graph_version = graph ? graph->version() : 0;
  fwd::ForwardingExecutionContext context;
  context.cancel = &cancel_;
  context.execution_id = request_id;
  context.scenario_id = "runtime_application";
  result = deps_.executor->Execute(request, context);
  const bool ok = result.status == fwd::ForwardingStatus::Delivered && result.path_confirmed;
  OutcomeObservation o;
  o.decision_id = active->decision_id;
  o.config_version = active->config_version;
  o.algorithm = active->algorithm;
  o.kind = OutcomeKind::Application;
  o.monotonic_ns = clock_->NowNs();
  o.sample = {ok, result.status == fwd::ForwardingStatus::Timeout, result.end_to_end_ms, payload.size()};
  outcomes_.Record(o);
  if (ok) {
    consecutive_failures_ = 0;
  } else if (result.status != fwd::ForwardingStatus::RequestRejected &&
             ++consecutive_failures_ >= config_.failures_for_safety_override) {
    RequestEvaluation(EvaluationTrigger::FailureRecovery, "application requests failed on the active path");
  }
  return result;
}

// ---------------------------------------------------------------- evaluation

std::vector<CandidateEvaluation> AAFRRuntime::ComputeCandidates(const EvaluationInputs& in, double* total_ms) {
  std::vector<CandidateEvaluation> out;
  *total_ms = 0.0;
  for (alg::AlgorithmId id : alg::kAllAlgorithmIds) {
    CandidateEvaluation c;
    c.algorithm = id;
    alg::ExecutionRequest request;
    request.algorithm = id;
    request.graph = in.graph;
    request.problem = in.problem;
    request.budget.max_iterations = config_.max_iterations;
    request.budget.deadline_ms = config_.computation_deadline_ms;
    request.seed = config_.algorithm_seed;
    request.execute_forwarding = false;
    request.scenario_id = "runtime";
    if (in.snapshot) request.network_state = in.snapshot->state;
    request.enforce_host_state_checks = config_.enforce_host_state_checks;
    const alg::EligibilityResult e = deps_.eligibility->Eligibility(id, request);
    c.eligibility = e.status;
    c.eligibility_reasons = alg::ReasonCodesText(e.reason_codes);
    c.considered = e.status == alg::EligibilityStatus::Eligible;
    if (!c.considered && !config_.enforce_host_state_checks && e.status != alg::EligibilityStatus::NotImplemented) {
      bool others_pass = true;
      std::vector<std::string> overridden;
      for (const alg::CheckResult& check : e.checks) {
        if (check.outcome == alg::CheckOutcome::Passed) continue;
        if (HostStateCheck(check.check)) overridden.push_back(alg::ToString(check.check));
        else others_pass = false;
      }
      if (others_pass && !overridden.empty()) {
        c.considered = true;
        c.overridden_checks = overridden;
      }
    }
    if (!c.considered) {
      c.note = "not eligible: " + c.eligibility_reasons;
      out.push_back(std::move(c));
      continue;
    }
    if (cancel_) break;
    const auto algorithm = alg::CreateAlgorithm(id);
    alg::AlgorithmRequest run;
    run.graph = in.graph.get();
    run.problem = &in.problem;
    run.budget = request.budget;
    run.seed = config_.algorithm_seed;
    run.cancel = &cancel_;
    const alg::AlgorithmResult result = algorithm->Run(run);
    c.computed = true;
    c.computation_status = alg::ToString(result.status);
    c.computation_ms = result.duration_ms;
    *total_ms += result.duration_ms;
    if (result.status == alg::AlgorithmStatus::PathFound) {
      c.path = result.path;
      const fwd::PathValidation v = fwd::ValidatePath(*in.graph, in.problem, c.path);
      c.path_valid = v.valid;
      c.path_feasible = v.feasible;
      c.objective = v.objective;
      if (!v.valid) c.note = "candidate path rejected by the validator";
      else if (!v.feasible) c.note = "candidate path violates a path-level constraint";
    } else {
      c.note = result.message;
    }
    out.push_back(std::move(c));
  }
  return out;
}

Recommendation AAFRRuntime::DetourRecommendation(const EvaluationInputs& in, const fwd::Path& failed,
                                                 double* total_ms) {
  *total_ms = 0.0;
  EvaluationInputs detour = in;
  std::size_t forbidden = 0;
  for (std::size_t i = 1; i + 1 < failed.size(); ++i) {
    if (failed[i] == in.problem.source || failed[i] == in.problem.destination) continue;
    detour.problem.constraints.forbidden_nodes.insert(failed[i]);
    ++forbidden;
  }
  if (forbidden == 0) {
    Recommendation none;
    none.status = RecommendationStatus::Unavailable;
    none.provider_id = provider_->Id() + "/v" + provider_->Version();
    none.explanation = "the failed path has no relay to route around";
    return none;
  }
  detour.candidates = ComputeCandidates(detour, total_ms);
  RecommendationInput rin;
  rin.graph = in.graph;
  rin.problem = detour.problem;
  rin.candidates = detour.candidates;
  if (in.active) {
    rin.active = in.active->algorithm;
    rin.active_path = in.active->path;
  }
  rin.benchmark = deps_.benchmark;
  rin.context = {in.evaluation_id, in.pending.trigger, in.snapshot ? in.snapshot->state.state_id : 0, in.graph->version()};
  Recommendation r = provider_->Recommend(rin);
  std::string relays;
  for (const auto& node : detour.problem.constraints.forbidden_nodes) relays += (relays.empty() ? "" : ",") + node;
  r.explanation = "detour with " + relays + " excluded after measured failures: " + r.explanation;
  return r;
}

void AAFRRuntime::Evaluate(const PendingEvaluation& pending) {
  const std::int64_t started_ns = clock_->NowNs();
  const auto wall_start = std::chrono::steady_clock::now();
  RuntimeLifecycle lifecycle;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    lifecycle = status_.lifecycle;
  }
  const bool initializing = lifecycle == RuntimeLifecycle::Initializing;
  if (!initializing) SetLifecycle(RuntimeLifecycle::Evaluating);

  EvaluationInputs in;
  in.evaluation_id = NextId("e");
  in.pending = pending;
  in.graph = deps_.graph_source();
  in.snapshot = deps_.state_source ? deps_.state_source() : nullptr;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    in.active = status_.active;
    in.active_verified = status_.active_verified;
    ++status_.counters.evaluations;
    status_.last_trigger = pending.trigger;
    status_.last_evaluation_ns = started_ns;
    status_.last_evaluation_at = std::chrono::system_clock::now();
  }
  Emit(RuntimeEventType::EvaluationStarted, "", in.evaluation_id + " trigger=" + ToString(pending.trigger));

  DecisionRecord rec;
  rec.decision_id = NextId("d");
  rec.session_id = config_.session_id;
  rec.monotonic_ns = started_ns;
  rec.timestamp = telemetry::WallClock::now();
  rec.evaluation_id = in.evaluation_id;
  rec.trigger = pending.trigger;
  rec.trigger_detail = pending.reason;
  for (EvaluationTrigger t : pending.coalesced) rec.coalesced_triggers.push_back(ToString(t));
  rec.outcome_window_ms = config_.outcome_window_ms;
  rec.provider_id = provider_->Id() + "/v" + provider_->Version();
  rec.simulated_delay = deps_.executor_injects_delay;
  if (in.active) {
    rec.previous_algorithm = in.active->algorithm;
    rec.previous_path = in.active->path;
  }
  if (in.snapshot) {
    rec.network_state_id = in.snapshot->state.state_id;
    rec.network_validity = netstate::ToString(in.snapshot->state.validity);
    rec.measurement_quality = QualityText(in.snapshot->state.quality);
    rec.constraint_version = HashText(ConstraintSignature(in.snapshot->constraints));
  } else {
    rec.network_validity = "unavailable";
    rec.measurement_quality = "no NetworkState snapshot";
    AddReason(rec, RuntimeReason::NetworkStateUnavailable);
  }
  if (pending.trigger == EvaluationTrigger::Manual) AddReason(rec, RuntimeReason::ManualEvaluation);
  if (pending.coalesced.count(EvaluationTrigger::MeaningfulChange)) AddReason(rec, RuntimeReason::NetworkChangeMeaningful);
  if (pending.coalesced.count(EvaluationTrigger::ConstraintChange)) AddReason(rec, RuntimeReason::ConstraintChanged);
  if (pending.coalesced.count(EvaluationTrigger::TopologyChange)) AddReason(rec, RuntimeReason::TopologyChanged);

  const auto finish = [&](RuntimeLifecycle end_state) {
    rec.decision_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - wall_start).count() -
                      rec.probe_overhead_ms - rec.activation_latency_ms.value_or(0.0);
    if (rec.decision_ms < 0.0) rec.decision_ms = 0.0;
    FinishRecord(rec, started_ns);
    if (!cancel_) SetLifecycle(end_state);
  };

  if (!in.graph) {
    rec.type = DecisionType::Unavailable;
    AddReason(rec, RuntimeReason::RecommendationUnavailable);
    rec.explanation = "no forwarding graph is available";
    finish(in.active && in.active_verified ? RuntimeLifecycle::Ready : RuntimeLifecycle::Degraded);
    return;
  }
  in.problem = deps_.problem_for(*in.graph);
  rec.graph_id = in.graph->id();
  rec.graph_version = in.graph->version();

  in.candidates = ComputeCandidates(in, &rec.algorithm_computation_ms);
  if (cancel_) return;
  for (const CandidateEvaluation& c : in.candidates) {
    if (c.considered) rec.eligible_algorithms.push_back(alg::ToString(c.algorithm));
    for (const auto& check : c.overridden_checks) rec.overridden_checks.push_back(std::string(alg::ToString(c.algorithm)) + ":" + check);
  }

  // ---- Safety conditions on the active configuration ----
  std::optional<RuntimeReason> safety;
  std::string safety_detail;
  if (in.active) {
    const fwd::PathValidation v = fwd::ValidatePath(*in.graph, in.problem, in.active->path);
    const CandidateEvaluation* own = nullptr;
    for (const auto& c : in.candidates) {
      if (c.algorithm == in.active->algorithm) own = &c;
    }
    if (!in.active_verified) {
      safety = RuntimeReason::ActiveConfigurationUnknown;
      safety_detail = "the active configuration could not be verified";
    } else if (consecutive_failures_ >= config_.failures_for_safety_override) {
      safety = RuntimeReason::ActivePathFailed;
      safety_detail = std::to_string(consecutive_failures_.load()) + " consecutive failed request(s) on the active path";
    } else if (!v.valid) {
      safety = RuntimeReason::ActiveConfigurationInvalid;
      safety_detail = "active path " + fwd::PathText(in.active->path) + " no longer validates" +
                      (v.details.empty() ? "" : ": " + v.details.front());
    } else if (!v.feasible) {
      safety = RuntimeReason::ConstraintViolated;
      safety_detail = "active path violates a path-level constraint" + (v.details.empty() ? "" : ": " + v.details.front());
    } else if (own && !own->considered) {
      safety = RuntimeReason::ActiveAlgorithmIneligible;
      safety_detail = std::string(alg::ToString(in.active->algorithm)) + " is no longer eligible (" +
                      own->eligibility_reasons + ")";
    }
  }

  // ---- Recommendation ----
  RecommendationInput rin;
  rin.state = in.snapshot ? std::shared_ptr<const netstate::NetworkState>(in.snapshot, &in.snapshot->state) : nullptr;
  rin.constraints =
      in.snapshot ? std::shared_ptr<const netstate::NetworkConstraints>(in.snapshot, &in.snapshot->constraints) : nullptr;
  rin.graph = in.graph;
  rin.problem = in.problem;
  rin.candidates = in.candidates;
  if (in.active) {
    rin.active = in.active->algorithm;
    rin.active_path = in.active->path;
  }
  rin.benchmark = deps_.benchmark;
  rin.context = {in.evaluation_id, pending.trigger, rec.network_state_id, in.graph->version()};
  Recommendation recommendation = provider_->Recommend(rin);
  rec.recommendation_status = ToString(recommendation.status);
  rec.recommended_algorithm = recommendation.recommended;
  rec.ranking = recommendation.ranking;
  rec.evidence_source = ToString(recommendation.evidence);
  rec.estimated_improvement = recommendation.estimated_improvement;
  rec.estimated_improvement_relative = recommendation.estimated_improvement_relative;
  rec.recommendation_explanation = recommendation.explanation.empty() ? recommendation.error : recommendation.explanation;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    status_.eligible_algorithms = rec.eligible_algorithms;
    status_.candidate = recommendation.recommended;
    status_.candidate_path = recommendation.ranking.empty() ? fwd::Path{} : recommendation.ranking.front().path;
  }
  Emit(RuntimeEventType::RecommendationGenerated, rec.decision_id,
       std::string(ToString(recommendation.status)) +
           (recommendation.recommended ? std::string(" ") + alg::ToString(*recommendation.recommended) : std::string()));

  // ---- No active configuration: initial activation or recovery ----
  if (!in.active || (safety && *safety == RuntimeReason::ActiveConfigurationUnknown)) {
    const bool initial = !ever_committed_;
    if (initial) AddReason(rec, RuntimeReason::InitialActivation);
    else {
      AddReason(rec, RuntimeReason::SafetyOverride);
      AddReason(rec, RuntimeReason::ActiveConfigurationUnknown);
      rec.safety_override = true;
      rec.safety_reason = RuntimeReason::ActiveConfigurationUnknown;
    }
    if (recommendation.status != RecommendationStatus::Recommended) {
      rec.type = DecisionType::Unavailable;
      AddReason(rec, recommendation.status == RecommendationStatus::Unavailable ? RuntimeReason::NoEligibleAlgorithm
                                                                              : RuntimeReason::RecommendationUnavailable);
      rec.explanation = "no configuration can be activated: " + rec.recommendation_explanation;
      {
        std::lock_guard<std::mutex> lock(state_mutex_);
        status_.health = "degraded: " + rec.explanation;
      }
      finish(RuntimeLifecycle::Degraded);
      return;
    }
    const RankedCandidate& best = recommendation.ranking.front();
    rec.candidate_algorithm = best.algorithm;
    rec.candidate_path = best.path;
    rec.type = initial ? DecisionType::InitialActivation : DecisionType::Recovery;
    rec.evidence_validity = "not_run";
    const bool ok = Activate(best, rec, in, initial, recommendation);
    if (cancel_) {
      FinishRecord(rec, started_ns);
      return;
    }
    finish(ok ? RuntimeLifecycle::Ready : RuntimeLifecycle::Degraded);
    return;
  }

  if (safety) {
    rec.safety_override = true;
    rec.safety_reason = safety;
    AddReason(rec, RuntimeReason::SafetyOverride);
    AddReason(rec, *safety);
    if (*safety == RuntimeReason::ActivePathFailed) AddReason(rec, RuntimeReason::ActivePathFailed);
  }

  // ---- Challengers: distinct paths other than the active one ----
  std::vector<RankedCandidate> challengers;
  for (const RankedCandidate& c : recommendation.ranking) {
    if (c.path == in.active->path) continue;
    bool duplicate = false;
    for (const auto& seen : challengers) duplicate = duplicate || seen.path == c.path;
    if (!duplicate && challengers.size() < config_.max_challengers) challengers.push_back(c);
  }
  if (challengers.empty() && safety && *safety == RuntimeReason::ActivePathFailed) {
    double detour_ms = 0.0;
    const Recommendation detour = DetourRecommendation(in, in.active->path, &detour_ms);
    rec.algorithm_computation_ms += detour_ms;
    rec.recommendation_explanation += "; " + detour.explanation;
    if (detour.status == RecommendationStatus::Recommended) {
      recommendation = detour;
      for (const RankedCandidate& c : recommendation.ranking) {
        bool duplicate = c.path == in.active->path;
        for (const auto& seen : challengers) duplicate = duplicate || seen.path == c.path;
        if (!duplicate && challengers.size() < config_.max_challengers) challengers.push_back(c);
      }
    }
  }
  const bool best_is_active_path =
      !recommendation.ranking.empty() && recommendation.ranking.front().path == in.active->path;

  const auto retain = [&](const std::string& why) {
    rec.type = DecisionType::Retain;
    rec.confirmed_algorithm = in.active->algorithm;
    rec.confirmed_path = in.active->path;
    rec.config_version = in.active->config_version;
    rec.explanation = "retained " + std::string(alg::ToString(in.active->algorithm)) + " on " +
                      fwd::PathText(in.active->path) + ": " + why;
    stability_.NoChallenger();
    followup_due_ns_ = 0;
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      ++status_.counters.retained;
      status_.last_evaluation = rec.explanation;
    }
    Emit(RuntimeEventType::AlgorithmRetained, rec.decision_id, why);
    finish(RuntimeLifecycle::Ready);
  };

  if (recommendation.status != RecommendationStatus::Recommended || challengers.empty() ||
      (!safety && best_is_active_path)) {
    if (safety) {
      rec.type = DecisionType::Unavailable;
      AddReason(rec, recommendation.status == RecommendationStatus::Recommended ? RuntimeReason::RecoveryFailed
                                                                              : RuntimeReason::NoEligibleAlgorithm);
      rec.explanation = safety_detail + "; no alternative configuration is available";
      {
        std::lock_guard<std::mutex> lock(state_mutex_);
        status_.active_verified = false;
        status_.health = "degraded: " + rec.explanation;
      }
      AddReason(rec, RuntimeReason::ActiveConfigurationUnknown);
      finish(RuntimeLifecycle::Degraded);
      return;
    }
    if (recommendation.status != RecommendationStatus::Recommended) {
      AddReason(rec, RuntimeReason::RecommendationUnavailable);
      retain(rec.recommendation_explanation);
    } else {
      AddReason(rec, RuntimeReason::CandidateSameAsActive);
      rec.candidate_algorithm = recommendation.recommended;
      rec.candidate_path = recommendation.ranking.front().path;
      retain("the best candidate forwards over the active path");
    }
    return;
  }

  // ---- Evidence ----
  SwitchProposal proposal;
  RankedCandidate chosen = challengers.front();
  const auto estimate_for = [&](const RankedCandidate& c) -> std::optional<double> {
    // Both values from ValidatePath on this graph version and objective.
    const fwd::PathValidation active_v = fwd::ValidatePath(*in.graph, in.problem, in.active->path);
    ObjectiveRef a{in.problem.id, in.graph->version(), fwd::ForwardingObjective::kVersion, active_v.objective};
    ObjectiveRef b{in.problem.id, in.graph->version(), fwd::ForwardingObjective::kVersion, c.objective};
    if (!ForwardingPerformanceEvaluator::CompatibleObjectives(a, b) || std::fabs(*active_v.objective) < 1e-12) {
      return std::nullopt;
    }
    return (*active_v.objective - *c.objective) / std::fabs(*active_v.objective);
  };

  if (safety) {
    proposal.safety_reason = safety;
    proposal.evidence = EvidenceKind::EstimateOnly;
    proposal.evidence_valid = true;
    proposal.evidence_note = "safety override; replacement validated on graph version " + std::to_string(in.graph->version());
    proposal.estimated_improvement_relative = estimate_for(chosen);
    rec.evidence_validity = "not_run";
    rec.evidence_explanation = safety_detail;
  } else {
    bool measured = false;
    if (config_.statistical_arbitration) {
      std::vector<ProbeArm> arms;
      arms.push_back({"incumbent", in.active->algorithm, in.active->path, true});
      for (std::size_t i = 0; i < challengers.size(); ++i) {
        arms.push_back({"challenger" + std::to_string(i + 1), challengers[i].algorithm, challengers[i].path, false});
      }
      const std::string unavailable = probes_.Unavailable(*in.graph, in.problem, arms);
      if (!unavailable.empty()) {
        rec.probing_unavailable_reason = unavailable;
        AddReason(rec, RuntimeReason::ProbingUnavailable);
      } else {
        rec.probing_attempted = true;
        rec.probe_scenario_id = NextId("p");
        ProbeConfig probe = config_.probe;
        probe.seed = config_.probe.seed + id_seq_.load();
        Emit(RuntimeEventType::ProbingStarted, rec.decision_id,
             rec.probe_scenario_id + ": " + std::to_string(arms.size()) + " arms x " + std::to_string(probe.rounds) + " rounds");
        const ProbeCampaignResult campaign =
            probes_.Run(rec.probe_scenario_id, *in.graph, in.problem, arms, probe, &cancel_, deps_.executor_injects_delay);
        rec.probe_overhead_ms = campaign.total_ms;
        rec.probe_requests = campaign.observations.size();
        {
          std::lock_guard<std::mutex> lock(state_mutex_);
          ++status_.counters.probe_campaigns;
          status_.counters.probe_requests += campaign.observations.size();
          status_.counters.probe_ms += campaign.total_ms;
        }
        Emit(RuntimeEventType::ProbingCompleted, rec.decision_id,
             std::to_string(campaign.observations.size()) + " probes, " + std::to_string(campaign.rounds_completed) +
                 " rounds, " + Fixed(campaign.total_ms, 1) + " ms" + (campaign.cancelled ? ", cancelled" : ""));
        if (cancel_ || campaign.cancelled) return;
        if (campaign.timeouts > 0) AddReason(rec, RuntimeReason::ProbeTimeout);
        const ArbitrationResult arb = arbitration_.Evaluate(campaign);
        std::size_t shown = 0;
        if (arb.best) shown = *arb.best;
        if (!arb.comparisons.empty()) {
          const ComparisonResult& c = arb.comparisons[shown];
          rec.incumbent_samples = c.incumbent.probes;
          rec.challenger_samples = c.challenger.probes;
          rec.paired_samples = c.pairs_used;
          rec.statistical_method = c.method;
          rec.latency_ratio = c.latency_ratio;
          rec.ratio_ci_low = c.ratio_low;
          rec.ratio_ci_high = c.ratio_high;
          rec.ci_level = c.ci_level;
          rec.p_holm = c.p_holm;
          rec.assumption_validation = c.assumptions.summary;
          rec.evidence_validity = ToString(c.verdict);
          rec.incumbent_failure_rate = c.incumbent.failure_rate;
          rec.challenger_failure_rate = c.challenger.failure_rate;
          std::string all;
          for (const auto& comparison : arb.comparisons) {
            all += (all.empty() ? "" : " | ") + comparison.challenger_arm + " (" +
                   alg::ToString(comparison.challenger_algorithm) + "): " + ToString(comparison.verdict) + ", " +
                   comparison.explanation;
          }
          rec.evidence_explanation = all;
        }
        Emit(RuntimeEventType::StatisticalEvaluationCompleted, rec.decision_id,
             arb.best ? "challenger better: " + arb.comparisons[*arb.best].challenger_arm
                      : "no challenger shown better");
        if (arb.best) {
          const ComparisonResult& c = arb.comparisons[*arb.best];
          // Comparisons follow the challenger arms, which follow `challengers`.
          chosen = challengers[*arb.best];
          proposal.evidence = c.reliability_repair ? EvidenceKind::ReliabilityRepair : EvidenceKind::Measured;
          proposal.evidence_valid = true;
          proposal.measured_improvement_relative = c.improvement_relative;
          proposal.measured_improvement_lower = c.improvement_relative_lower;
          if (c.incumbent_median_ms && c.challenger_median_ms) {
            proposal.measured_improvement_ms = *c.incumbent_median_ms - *c.challenger_median_ms;
          }
          proposal.evidence_note = c.explanation;
          AddReason(rec, RuntimeReason::ImprovementConfirmed);
          measured = true;
        } else {
          const ComparisonResult& top = arb.comparisons.front();
          for (RuntimeReason r : top.reasons) AddReason(rec, r);
          rec.candidate_algorithm = challengers.front().algorithm;
          rec.candidate_path = challengers.front().path;
          retain("controlled probing did not show a better challenger (" + std::string(ToString(top.verdict)) + ")");
          return;
        }
      }
    }
    if (!measured) {
      // Conservative estimate-only policy: J(P) improvement on this graph
      // version against the larger estimate margin, with hysteresis.
      proposal.evidence = EvidenceKind::EstimateOnly;
      proposal.estimated_improvement_relative = estimate_for(chosen);
      proposal.evidence_valid = proposal.estimated_improvement_relative.has_value();
      proposal.evidence_note = rec.probing_unavailable_reason.empty() ? "statistical arbitration disabled"
                                                                      : "probing unavailable: " + rec.probing_unavailable_reason;
      rec.evidence_validity = "not_run";
      AddReason(rec, RuntimeReason::ImprovementEstimateOnly);
      if (!proposal.estimated_improvement_relative || *proposal.estimated_improvement_relative <= 0.0) {
        AddReason(rec, RuntimeReason::ChallengerNotBetter);
        rec.candidate_algorithm = chosen.algorithm;
        rec.candidate_path = chosen.path;
        retain("the challenger's estimated J(P) is not lower than the active path's");
        return;
      }
    }
  }

  rec.candidate_algorithm = chosen.algorithm;
  rec.candidate_path = chosen.path;
  const StabilityAssessment gates = stability_.Assess(proposal, clock_->NowNs());
  rec.stability = gates;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    status_.last_gates = gates;
  }
  if (!gates.allowed) {
    rec.type = DecisionType::SwitchRejected;
    for (RuntimeReason r : gates.blocking) AddReason(rec, r);
    // Only time can lift dwell, cooldown, hysteresis and backoff: schedule
    // the re-check so a valid switch does not wait for an unrelated trigger.
    bool time_based = true;
    for (RuntimeReason r : gates.blocking) {
      time_based = time_based && (r == RuntimeReason::MinimumDwellNotMet || r == RuntimeReason::CooldownActive ||
                                  r == RuntimeReason::HysteresisNotSatisfied || r == RuntimeReason::CandidateBackoff);
    }
    if (time_based) {
      const double wait = std::max({gates.dwell.remaining_ms.value_or(0.0), gates.cooldown.remaining_ms.value_or(0.0),
                                    gates.backoff.remaining_ms.value_or(0.0), config_.scheduler.min_interval_ms});
      followup_due_ns_ = clock_->NowNs() + MsToNs(wait + 1.0);
    }
    rec.confirmed_algorithm = in.active->algorithm;
    rec.confirmed_path = in.active->path;
    rec.config_version = in.active->config_version;
    rec.explanation = std::string(alg::ToString(chosen.algorithm)) + " on " + fwd::PathText(chosen.path) +
                      " is preferred but switching is blocked: " + ReasonsText(gates.blocking);
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      ++status_.counters.rejected;
      status_.last_evaluation = rec.explanation;
    }
    Emit(RuntimeEventType::AlgorithmRetained, rec.decision_id, rec.explanation);
    finish(safety ? RuntimeLifecycle::Degraded : RuntimeLifecycle::Ready);
    return;
  }
  rec.type = safety ? DecisionType::SafetySwitch : DecisionType::Switch;
  if (safety) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    ++status_.counters.safety_overrides;
  }
  const bool ok = Activate(chosen, rec, in, false, recommendation);
  if (cancel_) {
    FinishRecord(rec, started_ns);
    return;
  }
  bool verified;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    verified = status_.active_verified;
  }
  finish(ok || verified ? RuntimeLifecycle::Ready : RuntimeLifecycle::Degraded);
}

// ---------------------------------------------------------------- activation

AAFRRuntime::Attempt AAFRRuntime::TryActivate(const fwd::Path& path, const fwd::ForwardingGraph& graph,
                                              const std::string& decision_id, alg::AlgorithmId algorithm,
                                              std::uint64_t state_id) {
  Attempt a;
  const auto start = std::chrono::steady_clock::now();
  for (int attempt = 0; attempt < config_.activation_attempts && !a.confirmed && !cancel_; ++attempt) {
    fwd::ForwardingRequest request;
    request.request_id = NextId("act");
    request.path = path;
    request.payload = "aafr-activation";
    request.timeout_ms = config_.activation_timeout_ms;
    request.graph_version = graph.version();
    fwd::ForwardingExecutionContext context;
    context.cancel = &cancel_;
    context.execution_id = request.request_id;
    context.scenario_id = "runtime_activation";
    const fwd::ForwardingExecutionResult r = deps_.executor->Execute(request, context);
    a.request_id = request.request_id;
    a.confirmed = r.status == fwd::ForwardingStatus::Delivered && r.path_confirmed && r.confirmed_path == path;
    a.evidence = r.evidence;
    a.detail = std::string(fwd::ToString(r.status)) + (r.response_status.empty() ? "" : "/" + r.response_status) +
               (r.message.empty() ? "" : ": " + r.message);
    a.status = a.confirmed                                       ? ActivationStatus::Confirmed
               : r.status == fwd::ForwardingStatus::Timeout     ? ActivationStatus::TimedOut
               : r.status == fwd::ForwardingStatus::Cancelled   ? ActivationStatus::Cancelled
                                                                : ActivationStatus::Failed;
    OutcomeObservation o;
    o.decision_id = decision_id;
    o.algorithm = algorithm;
    o.network_state_id = state_id;
    o.kind = OutcomeKind::Activation;
    o.monotonic_ns = clock_->NowNs();
    o.sample = {a.confirmed, a.status == ActivationStatus::TimedOut, r.end_to_end_ms, request.payload.size()};
    outcomes_.Record(o);
  }
  if (cancel_ && !a.confirmed) a.status = ActivationStatus::Cancelled;
  a.latency_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
  return a;
}

void AAFRRuntime::Commit(const RankedCandidate& candidate, DecisionRecord& rec, const EvaluationInputs& in,
                         const Attempt& attempt, bool initial) {
  ActiveConfiguration c;
  c.config_version = ++config_seq_;
  c.algorithm = candidate.algorithm;
  c.path = candidate.path;
  c.graph_id = in.graph->id();
  c.graph_version = in.graph->version();
  c.objective = candidate.objective;
  c.activated_monotonic_ns = clock_->NowNs();
  c.activation_request_id = attempt.request_id;
  c.decision_id = rec.decision_id;
  c.confirmation_evidence = attempt.evidence;
  c.activation_latency_ms = attempt.latency_ms;
  if (initial) stability_.OnInitialActivation(c.activated_monotonic_ns);
  else stability_.OnCommitted(c.activated_monotonic_ns);
  ever_committed_ = true;
  followup_due_ns_ = 0;
  consecutive_failures_ = 0;
  next_health_ns_ = c.activated_monotonic_ns + MsToNs(config_.health_check_interval_ms);
  outcomes_.RecordSwitchLatency(attempt.latency_ms);
  rec.activation = ActivationStatus::Confirmed;
  rec.activation_request_id = attempt.request_id;
  rec.activation_latency_ms = attempt.latency_ms;
  rec.confirmed_algorithm = c.algorithm;
  rec.confirmed_path = c.path;
  rec.config_version = c.config_version;
  std::lock_guard<std::mutex> lock(state_mutex_);
  status_.active = c;
  status_.active_verified = true;
  status_.consecutive_failures = 0;
  status_.activation_status = "confirmed " + attempt.request_id + " (" + Fixed(attempt.latency_ms, 2) + " ms)";
  status_.health = "healthy: activation confirmed over " + fwd::PathText(c.path);
  if (!initial) ++status_.counters.switches;
}

bool AAFRRuntime::Activate(const RankedCandidate& candidate, DecisionRecord& rec, const EvaluationInputs& in,
                           bool initial, const Recommendation& recommendation) {
  RuntimeLifecycle lifecycle;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    lifecycle = status_.lifecycle;
  }
  if (lifecycle == RuntimeLifecycle::Evaluating) SetLifecycle(RuntimeLifecycle::Switching);
  AddReason(rec, RuntimeReason::SwitchRequested);
  rec.activation = ActivationStatus::Pending;
  Emit(RuntimeEventType::SwitchRequested, rec.decision_id,
       std::string(alg::ToString(candidate.algorithm)) + " on " + fwd::PathText(candidate.path));

  const auto stale = [&] {
    const auto current = deps_.graph_source();
    return !current || current->version() != in.graph->version();
  };
  if (stale()) {
    rec.type = DecisionType::Discarded;
    rec.activation = ActivationStatus::NotRequested;
    AddReason(rec, RuntimeReason::EvaluationStale);
    rec.explanation = "the forwarding graph changed during evaluation; nothing was activated";
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      ++status_.counters.discarded;
    }
    Emit(RuntimeEventType::EvaluationDiscarded, rec.decision_id, rec.explanation);
    std::lock_guard<std::mutex> lock(sched_mutex_);
    scheduler_.Request(EvaluationTrigger::TopologyChange, clock_->NowNs(), "re-evaluate after stale evaluation");
    return false;
  }

  const Attempt attempt = TryActivate(candidate.path, *in.graph, rec.decision_id, candidate.algorithm, rec.network_state_id);
  rec.activation_request_id = attempt.request_id;
  rec.activation_latency_ms = attempt.latency_ms;
  rec.execution_detail = attempt.detail;
  if (attempt.confirmed && stale()) {
    // Confirmed on the relays, but the decision was made on an older graph:
    // do not commit it.
    rec.type = DecisionType::Discarded;
    AddReason(rec, RuntimeReason::EvaluationStale);
    rec.activation = ActivationStatus::Cancelled;
    rec.explanation = "activation confirmed but the graph changed before commit; previous configuration kept";
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      ++status_.counters.discarded;
    }
    Emit(RuntimeEventType::EvaluationDiscarded, rec.decision_id, rec.explanation);
    std::lock_guard<std::mutex> lock(sched_mutex_);
    scheduler_.Request(EvaluationTrigger::TopologyChange, clock_->NowNs(), "re-evaluate after stale activation");
    return false;
  }
  if (attempt.confirmed) {
    Commit(candidate, rec, in, attempt, initial);
    AddReason(rec, RuntimeReason::SwitchCommitted);
    if (rec.type == DecisionType::Recovery) {
      AddReason(rec, RuntimeReason::RecoverySucceeded);
      rec.recovery = RecoveryStatus::Recovered;
      outcomes_.RecordRecovery(true);
      std::lock_guard<std::mutex> lock(state_mutex_);
      ++status_.counters.recoveries;
    }
    std::ostringstream text;
    text << (initial ? "activated " : rec.type == DecisionType::Recovery ? "recovered to " : "switched to ")
         << alg::ToString(candidate.algorithm) << " on " << fwd::PathText(candidate.path);
    if (!initial && in.active) {
      text << " from " << alg::ToString(in.active->algorithm) << " on " << fwd::PathText(in.active->path);
    }
    text << "; confirmed by " << attempt.request_id << " in " << Fixed(attempt.latency_ms, 2) << " ms";
    if (!rec.evidence_explanation.empty()) text << "; evidence: " << rec.evidence_explanation;
    rec.explanation = text.str();
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      status_.last_switch = rec.explanation;
      status_.last_evaluation = rec.explanation;
    }
    Emit(RuntimeEventType::SwitchCommitted, rec.decision_id, rec.explanation);
    return true;
  }

  // ---- Activation failed ----
  rec.activation = attempt.status;
  AddReason(rec, RuntimeReason::SwitchFailed);
  if (attempt.status == ActivationStatus::TimedOut) AddReason(rec, RuntimeReason::ActivationTimeout);
  stability_.OnActivationFailed(candidate.path, clock_->NowNs());
  outcomes_.RecordActivationFailure();
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    ++status_.counters.failed_switches;
    status_.activation_status = "failed: " + attempt.detail;
    status_.last_switch = "activation of " + std::string(alg::ToString(candidate.algorithm)) + " on " +
                          fwd::PathText(candidate.path) + " failed: " + attempt.detail;
  }
  Emit(RuntimeEventType::SwitchFailed, rec.decision_id, attempt.detail);
  if (cancel_) {
    rec.explanation = "activation interrupted by shutdown; previous configuration unchanged";
    return false;
  }
  return Recover(candidate.path, rec, in, recommendation, initial);
}

bool AAFRRuntime::Recover(const fwd::Path& failed_path, DecisionRecord& rec, const EvaluationInputs& in,
                          const Recommendation& recommendation, bool initial) {
  // 1. Is the previous configuration still working? Only meaningful when it
  //    was not the reason for leaving.
  if (in.active && in.active_verified && !rec.safety_override) {
    const Attempt again = TryActivate(in.active->path, *in.graph, in.active->decision_id, in.active->algorithm,
                                      rec.network_state_id);
    if (again.confirmed) {
      rec.recovery = RecoveryStatus::PreviousRetained;
      AddReason(rec, RuntimeReason::PreviousConfigurationRetained);
      rec.confirmed_algorithm = in.active->algorithm;
      rec.confirmed_path = in.active->path;
      rec.config_version = in.active->config_version;
      rec.explanation = "activation of " + fwd::PathText(failed_path) + " failed (" + rec.execution_detail +
                        "); previous configuration " + fwd::PathText(in.active->path) + " re-confirmed by " +
                        again.request_id + " and kept";
      {
        std::lock_guard<std::mutex> lock(state_mutex_);
        status_.active_verified = true;
        status_.last_evaluation = rec.explanation;
      }
      return false;
    }
  }
  // 2. Try the remaining ranked candidates that are not backing off.
  AddReason(rec, RuntimeReason::RecoveryRequested);
  Emit(RuntimeEventType::RecoveryStarted, rec.decision_id, "after failed activation of " + fwd::PathText(failed_path));
  for (const RankedCandidate& c : recommendation.ranking) {
    if (cancel_) break;
    if (c.path == failed_path) continue;
    if (in.active && c.path == in.active->path && (rec.safety_override || !in.active_verified)) continue;
    if (stability_.BackoffRemainingMs(c.path, clock_->NowNs()) > 0.0) continue;
    const Attempt a = TryActivate(c.path, *in.graph, rec.decision_id, c.algorithm, rec.network_state_id);
    if (!a.confirmed) {
      stability_.OnActivationFailed(c.path, clock_->NowNs());
      continue;
    }
    const auto current = deps_.graph_source();
    if (!current || current->version() != in.graph->version()) break;
    Commit(c, rec, in, a, initial);
    rec.type = initial ? DecisionType::InitialActivation : DecisionType::Recovery;
    rec.recovery = RecoveryStatus::Recovered;
    AddReason(rec, RuntimeReason::RecoverySucceeded);
    AddReason(rec, RuntimeReason::SwitchCommitted);
    rec.explanation = "activation of " + fwd::PathText(failed_path) + " failed; recovered to " +
                      alg::ToString(c.algorithm) + " on " + fwd::PathText(c.path) + ", confirmed by " + a.request_id;
    outcomes_.RecordRecovery(true);
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      ++status_.counters.recoveries;
      status_.last_switch = rec.explanation;
      status_.last_evaluation = rec.explanation;
    }
    Emit(RuntimeEventType::RecoveryCompleted, rec.decision_id, rec.explanation);
    return true;
  }
  // 3. Route around the failed path's relays (the graph may not show the
  //    failure, so the ranking can hold nothing else).
  if (!cancel_) {
    double detour_ms = 0.0;
    const Recommendation detour = DetourRecommendation(in, failed_path, &detour_ms);
    rec.algorithm_computation_ms += detour_ms;
    for (const RankedCandidate& c : detour.ranking) {
      if (cancel_) break;
      if (c.path == failed_path) continue;
      if (stability_.BackoffRemainingMs(c.path, clock_->NowNs()) > 0.0) continue;
      const Attempt a = TryActivate(c.path, *in.graph, rec.decision_id, c.algorithm, rec.network_state_id);
      if (!a.confirmed) {
        stability_.OnActivationFailed(c.path, clock_->NowNs());
        continue;
      }
      const auto current = deps_.graph_source();
      if (!current || current->version() != in.graph->version()) break;
      Commit(c, rec, in, a, initial);
      rec.type = initial ? DecisionType::InitialActivation : DecisionType::Recovery;
      rec.recovery = RecoveryStatus::Recovered;
      AddReason(rec, RuntimeReason::RecoverySucceeded);
      AddReason(rec, RuntimeReason::SwitchCommitted);
      rec.explanation = "activation of " + fwd::PathText(failed_path) + " failed; recovered via " + detour.explanation +
                        " -> " + alg::ToString(c.algorithm) + " on " + fwd::PathText(c.path) + ", confirmed by " +
                        a.request_id;
      outcomes_.RecordRecovery(true);
      {
        std::lock_guard<std::mutex> lock(state_mutex_);
        ++status_.counters.recoveries;
        status_.last_switch = rec.explanation;
        status_.last_evaluation = rec.explanation;
      }
      Emit(RuntimeEventType::RecoveryCompleted, rec.decision_id, rec.explanation);
      return true;
    }
  }
  // 4. Nothing could be confirmed.
  rec.recovery = RecoveryStatus::Failed;
  AddReason(rec, RuntimeReason::RecoveryFailed);
  AddReason(rec, RuntimeReason::ActiveConfigurationUnknown);
  if (initial) rec.type = DecisionType::Unavailable;
  rec.explanation = "activation of " + fwd::PathText(failed_path) + " failed (" + rec.execution_detail +
                    ") and no configuration could be confirmed";
  outcomes_.RecordRecovery(false);
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    status_.active_verified = false;
    status_.health = "degraded: " + rec.explanation;
    status_.last_evaluation = rec.explanation;
  }
  Emit(RuntimeEventType::RecoveryCompleted, rec.decision_id, "recovery failed");
  return false;
}

// ---------------------------------------------------------------- records

void AAFRRuntime::FinishRecord(DecisionRecord record, std::int64_t started_ns) {
  (void)started_ns;
  if (record.explanation.empty()) record.explanation = ReasonsText(record.reasons);
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    status_.counters.decision_ms += record.decision_ms;
    status_.counters.computation_ms += record.algorithm_computation_ms;
    if (status_.last_evaluation.empty() || record.type == DecisionType::Unavailable ||
        record.type == DecisionType::Discarded) {
      status_.last_evaluation = record.explanation;
    }
    decisions_.push_back(record);
    while (decisions_.size() > config_.decision_history) decisions_.pop_front();
  }
  if (!config_.decision_log_path.empty()) {
    std::ofstream out(config_.decision_log_path, std::ios::app);
    if (out) out << ToJson(record) << "\n";
  }
}

RuntimeStatus AAFRRuntime::Status() const {
  const std::int64_t now = clock_->NowNs();
  std::lock_guard<std::mutex> lock(state_mutex_);
  RuntimeStatus s = status_;
  s.dwell_remaining_ms = std::max(0.0, NsToMs(dwell_until_ns_ - now));
  s.cooldown_remaining_ms = std::max(0.0, NsToMs(cooldown_until_ns_ - now));
  if (stop_requested_ && s.lifecycle != RuntimeLifecycle::Stopped) s.health = "stopping";
  return s;
}

std::vector<DecisionRecord> AAFRRuntime::Decisions(std::size_t max_records) const {
  std::lock_guard<std::mutex> lock(state_mutex_);
  const std::size_t n = std::min(max_records, decisions_.size());
  return std::vector<DecisionRecord>(decisions_.end() - static_cast<std::ptrdiff_t>(n), decisions_.end());
}

std::vector<RuntimeEvent> AAFRRuntime::Events(std::size_t max_events) const {
  std::lock_guard<std::mutex> lock(state_mutex_);
  const std::size_t n = std::min(max_events, events_.size());
  return std::vector<RuntimeEvent>(events_.end() - static_cast<std::ptrdiff_t>(n), events_.end());
}

std::optional<OutcomeRevision> AAFRRuntime::Outcome(const std::string& decision_id) const {
  return outcomes_.Latest(decision_id, clock_->NowNs());
}

std::string AAFRRuntime::DecisionsJsonl() const {
  std::string out;
  for (const DecisionRecord& r : Decisions(config_.decision_history)) out += ToJson(r) + "\n";
  return out;
}

observability::RuntimeSnapshot AAFRRuntime::Snapshot() const {
  const RuntimeStatus s = Status();
  observability::RuntimeSnapshot snap;
  switch (s.lifecycle) {
    case RuntimeLifecycle::Uninitialized:
    case RuntimeLifecycle::Initializing: snap.state = observability::RuntimeState::Starting; break;
    case RuntimeLifecycle::Ready:
    case RuntimeLifecycle::Evaluating:
    case RuntimeLifecycle::Switching: snap.state = observability::RuntimeState::Running; break;
    case RuntimeLifecycle::Degraded:
    case RuntimeLifecycle::Failed: snap.state = observability::RuntimeState::Degraded; break;
    case RuntimeLifecycle::Stopping:
    case RuntimeLifecycle::Stopped: snap.state = observability::RuntimeState::Stopped; break;
  }
  if (s.active) {
    observability::AlgorithmStatus a;
    a.algorithm_id = alg::ToString(s.active->algorithm);
    a.activation_decision_id = s.active->decision_id;
    a.execution_state = s.active_verified ? observability::ExecutionState::Active : observability::ExecutionState::Failed;
    snap.active_algorithm = a;
    snap.activation_reason = s.last_switch;
  }
  snap.last_evaluation_at = s.last_evaluation_at;
  return snap;
}

std::vector<observability::DecisionRecord> AAFRRuntime::RecentDecisions(std::size_t max_records) const {
  std::vector<observability::DecisionRecord> out;
  for (const DecisionRecord& r : Decisions(max_records)) out.push_back(ToObservability(r));
  return out;
}

}  // namespace aafr::runtime
