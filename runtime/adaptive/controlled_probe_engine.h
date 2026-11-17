#pragma once

// Controlled incumbent/challenger probing over AAFR's own forwarding
// executor. Plain C++17.
//
// Adapted concept (independently implemented; see the runtime README for
// the reference-repository notes): measure the competing paths in rounds,
// each round sending one probe over every arm in a randomized order, so
// slow drift of the host or relays affects all arms of a round alike and
// observations can be paired by round.
//
// A probe is an ordinary forwarding request over a validated path, tagged
// with scenario "runtime_probe" and a "aafr-probe" payload, so it is never
// confused with application traffic. Probing is refused unless the executor
// is ready, supports path selection, and every arm path validates on the
// pinned graph version: a computed path is not assumed executable.
//
// Bounds: at most kMaxArms arms and kMaxRounds rounds, a per-probe timeout,
// a total campaign deadline, and cancellation polled before every probe.

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "algorithms/algorithm_id.h"
#include "routing/forwarding/forwarding_executor.h"
#include "routing/topology/forwarding_graph.h"
#include "routing/topology/forwarding_problem.h"
#include "runtime/adaptive/runtime_clock.h"

namespace aafr::runtime {

struct ProbeArm {
  std::string arm_id;  // e.g. "incumbent", "challenger_1".
  algorithms::AlgorithmId algorithm{algorithms::AlgorithmId::Dijkstra};
  forwarding::Path path;
  bool incumbent{false};
};

struct ProbeConfig {
  static constexpr std::size_t kMaxArms = 4;
  static constexpr std::size_t kMaxRounds = 64;
  std::size_t rounds{12};
  std::size_t payload_bytes{64};
  double probe_timeout_ms{1500.0};
  double campaign_deadline_ms{20'000.0};
  double inter_probe_gap_ms{0.0};
  std::uint64_t seed{1};  // Order randomization; recorded with the result.

  std::vector<std::string> Validate() const;
};

struct ProbeObservation {
  std::string probe_id;
  std::string scenario_id;
  std::size_t round{0};
  std::size_t order_in_round{0};
  std::string arm_id;
  std::string path_id;  // PathText of the arm's path.
  algorithms::AlgorithmId algorithm{algorithms::AlgorithmId::Dijkstra};
  std::int64_t started_ns{0};
  std::int64_t completed_ns{0};
  double duration_ms{0.0};  // Executor-reported end-to-end duration.
  bool success{false};      // Delivered over a confirmed path.
  bool timeout{false};
  std::string status;       // ForwardingStatus text.
  std::string response_status;
  std::size_t payload_bytes{0};
  forwarding::Provenance provenance{forwarding::Provenance::Unknown};
  std::uint64_t graph_version{0};
  std::string executor;
};

struct ProbeCampaignResult {
  std::string scenario_id;
  bool ran{false};
  std::string unavailable_reason;  // Set when ran is false.
  std::vector<ProbeArm> arms;
  std::vector<ProbeObservation> observations;  // In send order.
  std::size_t rounds_requested{0};
  std::size_t rounds_completed{0};
  bool cancelled{false};
  bool deadline_reached{false};
  std::size_t timeouts{0};
  double total_ms{0.0};  // Wall time of the campaign (probe overhead).
  std::uint64_t graph_version{0};
  std::uint64_t seed{0};
  std::string executor;
  bool simulated_delay{false};  // The executor injects configured delays.

  std::vector<const ProbeObservation*> ForArm(const std::string& arm_id) const;
};

class ControlledProbeEngine {
 public:
  // `executor` and `clock` must outlive the engine.
  ControlledProbeEngine(forwarding::IForwardingExecutor* executor, const IMonotonicClock* clock)
      : executor_(executor), clock_(clock) {}

  // Why probing cannot run for these arms on this graph; empty when it can.
  std::string Unavailable(const forwarding::ForwardingGraph& graph, const forwarding::ForwardingProblem& problem,
                          const std::vector<ProbeArm>& arms) const;

  // Runs the campaign on the caller's thread. Never throws.
  ProbeCampaignResult Run(const std::string& scenario_id, const forwarding::ForwardingGraph& graph,
                          const forwarding::ForwardingProblem& problem, const std::vector<ProbeArm>& arms,
                          const ProbeConfig& config, const std::atomic<bool>* cancel = nullptr,
                          bool simulated_delay = false) const;

 private:
  forwarding::IForwardingExecutor* executor_;
  const IMonotonicClock* clock_;
};

}  // namespace aafr::runtime
