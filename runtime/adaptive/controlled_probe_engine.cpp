#include "runtime/adaptive/controlled_probe_engine.h"

#include <algorithm>
#include <chrono>
#include <numeric>
#include <thread>

#include "algorithms/common/path_algorithm.h"

namespace aafr::runtime {

std::vector<std::string> ProbeConfig::Validate() const {
  std::vector<std::string> issues;
  if (rounds < 2 || rounds > kMaxRounds) issues.push_back("rounds must be within 2.." + std::to_string(kMaxRounds));
  if (payload_bytes == 0 || payload_bytes > forwarding::ForwardingRequest::kMaxPayloadBytes) {
    issues.push_back("payload_bytes must be within 1.." + std::to_string(forwarding::ForwardingRequest::kMaxPayloadBytes));
  }
  if (!(probe_timeout_ms > 0.0) || probe_timeout_ms > 10'000.0) issues.push_back("probe_timeout_ms must be within (0, 10000]");
  if (!(campaign_deadline_ms > 0.0) || campaign_deadline_ms > 120'000.0) {
    issues.push_back("campaign_deadline_ms must be within (0, 120000]");
  }
  if (!(inter_probe_gap_ms >= 0.0) || inter_probe_gap_ms > 1000.0) issues.push_back("inter_probe_gap_ms must be within 0..1000");
  return issues;
}

std::vector<const ProbeObservation*> ProbeCampaignResult::ForArm(const std::string& arm_id) const {
  std::vector<const ProbeObservation*> out;
  for (const auto& o : observations) {
    if (o.arm_id == arm_id) out.push_back(&o);
  }
  return out;
}

std::string ControlledProbeEngine::Unavailable(const forwarding::ForwardingGraph& graph,
                                               const forwarding::ForwardingProblem& problem,
                                               const std::vector<ProbeArm>& arms) const {
  if (!executor_) return "no forwarding executor";
  if (!executor_->Ready()) return "forwarding executor is not ready";
  const forwarding::ForwardingCapability capability = executor_->Capability();
  if (capability.operations.count(netstate::ForwardingOperation::SelectPath) == 0) {
    return "executor " + capability.executor + " cannot select a path per request";
  }
  if (arms.size() < 2) return "fewer than two arms";
  if (arms.size() > ProbeConfig::kMaxArms) return "more than " + std::to_string(ProbeConfig::kMaxArms) + " arms";
  for (std::size_t i = 0; i < arms.size(); ++i) {
    for (std::size_t j = i + 1; j < arms.size(); ++j) {
      if (arms[i].path == arms[j].path) return "arms " + arms[i].arm_id + " and " + arms[j].arm_id + " share a path";
    }
    const forwarding::PathValidation v = forwarding::ValidatePath(graph, problem, arms[i].path);
    if (!v.valid) return "arm " + arms[i].arm_id + " path " + forwarding::PathText(arms[i].path) + " does not validate";
    if (arms[i].path.size() > capability.max_hops + 1) return "arm " + arms[i].arm_id + " exceeds the executor hop limit";
  }
  return {};
}

ProbeCampaignResult ControlledProbeEngine::Run(const std::string& scenario_id,
                                               const forwarding::ForwardingGraph& graph,
                                               const forwarding::ForwardingProblem& problem,
                                               const std::vector<ProbeArm>& arms, const ProbeConfig& config,
                                               const std::atomic<bool>* cancel, bool simulated_delay) const {
  ProbeCampaignResult result;
  result.scenario_id = scenario_id;
  result.arms = arms;
  result.rounds_requested = config.rounds;
  result.graph_version = graph.version();
  result.seed = config.seed;
  result.simulated_delay = simulated_delay;
  const std::vector<std::string> issues = config.Validate();
  if (!issues.empty()) {
    result.unavailable_reason = "invalid probe configuration: " + issues.front();
    return result;
  }
  result.unavailable_reason = Unavailable(graph, problem, arms);
  if (!result.unavailable_reason.empty()) return result;
  result.ran = true;
  result.executor = executor_->Capability().executor;

  const std::int64_t start_ns = clock_->NowNs();
  const auto wall_start = std::chrono::steady_clock::now();
  const std::string payload = [&] {
    std::string p = "aafr-probe:" + scenario_id + ":";
    p.resize(config.payload_bytes, 'p');
    return p;
  }();
  algorithms::Rng rng(config.seed ^ 0x9e3779b97f4a7c15ull);
  std::vector<std::size_t> order(arms.size());
  forwarding::ForwardingExecutionContext context;
  context.cancel = cancel;
  context.scenario_id = "runtime_probe";

  for (std::size_t round = 0; round < config.rounds; ++round) {
    std::iota(order.begin(), order.end(), std::size_t{0});
    // Fisher-Yates with the portable generator: same seed, same order.
    for (std::size_t i = order.size(); i > 1; --i) std::swap(order[i - 1], order[rng.Index(i)]);
    bool round_complete = true;
    for (std::size_t k = 0; k < order.size(); ++k) {
      if (cancel && cancel->load()) {
        result.cancelled = true;
        break;
      }
      const double elapsed =
          std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - wall_start).count();
      if (elapsed >= config.campaign_deadline_ms) {
        result.deadline_reached = true;
        break;
      }
      const ProbeArm& arm = arms[order[k]];
      ProbeObservation o;
      o.scenario_id = scenario_id;
      o.round = round;
      o.order_in_round = k;
      o.arm_id = arm.arm_id;
      o.path_id = forwarding::PathText(arm.path);
      o.algorithm = arm.algorithm;
      o.payload_bytes = payload.size();
      o.graph_version = graph.version();
      o.executor = result.executor;
      o.probe_id = scenario_id + "-r" + std::to_string(round) + "-" + arm.arm_id;
      forwarding::ForwardingRequest request;
      request.request_id = o.probe_id;
      request.path = arm.path;
      request.payload = payload;
      request.timeout_ms = std::min(config.probe_timeout_ms, config.campaign_deadline_ms - elapsed);
      request.graph_version = graph.version();
      context.execution_id = o.probe_id;
      o.started_ns = clock_->NowNs();
      const forwarding::ForwardingExecutionResult r = executor_->Execute(request, context);
      o.completed_ns = clock_->NowNs();
      o.duration_ms = r.end_to_end_ms;
      o.status = forwarding::ToString(r.status);
      o.response_status = r.response_status;
      o.provenance = r.provenance;
      o.timeout = r.status == forwarding::ForwardingStatus::Timeout;
      o.success = r.status == forwarding::ForwardingStatus::Delivered && r.path_confirmed;
      if (o.timeout) ++result.timeouts;
      result.observations.push_back(std::move(o));
      if (config.inter_probe_gap_ms > 0.0) {
        std::this_thread::sleep_for(std::chrono::microseconds(static_cast<long long>(config.inter_probe_gap_ms * 1000.0)));
      }
    }
    if (result.cancelled || result.deadline_reached) round_complete = false;
    if (!round_complete) break;
    ++result.rounds_completed;
  }
  (void)start_ns;
  result.total_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - wall_start).count();
  return result;
}

}  // namespace aafr::runtime
