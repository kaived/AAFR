#pragma once

// Shared fixtures for the V0.6 runtime tests: a versioned graph holder, a
// deterministic test executor with fault injection and seeded latency noise,
// and helpers that build NetworkStates and probe campaigns. Header-only;
// include from exactly one translation unit per test program.

#include <atomic>
#include <chrono>
#include <cmath>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "algorithms/common/path_algorithm.h"
#include "algorithms/execution/implementation_verifier.h"
#include "network/network_state/network_state.h"
#include "routing/forwarding/forwarding_executor.h"
#include "routing/forwarding/loopback_forwarding.h"
#include "routing/topology/forwarding_graph.h"
#include "routing/topology/forwarding_problem.h"
#include "routing/topology/scenario_catalog.h"
#include "runtime/adaptive/controlled_probe_engine.h"

namespace aafr::test {

namespace fwd = aafr::forwarding;

// Immutable graph snapshots; Mutate publishes a new version.
class GraphHolder {
 public:
  explicit GraphHolder(fwd::ForwardingGraph graph) : graph_(std::make_shared<const fwd::ForwardingGraph>(std::move(graph))) {}
  std::shared_ptr<const fwd::ForwardingGraph> Get() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return graph_;
  }
  template <typename F>
  void Mutate(F f) {
    std::lock_guard<std::mutex> lock(mutex_);
    fwd::ForwardingGraph copy = *graph_;
    f(copy);
    graph_ = std::make_shared<const fwd::ForwardingGraph>(std::move(copy));
  }
  void SetLatency(const std::string& from, const std::string& to, double ms) {
    Mutate([&](fwd::ForwardingGraph& g) { g.SetEdgeLatency(from, to, fwd::Measurement::Of(ms, fwd::Provenance::Simulated)); });
  }

 private:
  mutable std::mutex mutex_;
  std::shared_ptr<const fwd::ForwardingGraph> graph_;
};

// entry -> relay_a -> dest (fast, 1 + 1 ms) and entry -> relay_b -> dest
// (slow, 10 + 10 ms). Path A has J = 3, path B J = 21.
inline fwd::ForwardingGraph TwoPathGraph() {
  fwd::ForwardingGraph graph("two_path");
  graph.AddNode({"entry", fwd::NodeRole::Entry, true});
  graph.AddNode({"relay_a", fwd::NodeRole::Relay, true});
  graph.AddNode({"relay_b", fwd::NodeRole::Relay, true});
  graph.AddNode({"dest", fwd::NodeRole::Destination, true});
  const auto edge = [&graph](const char* from, const char* to, double latency) {
    fwd::ForwardingEdge e;
    e.from = from;
    e.to = to;
    e.latency_ms = fwd::Measurement::Of(latency, fwd::Provenance::Configured);
    e.loss_ratio = fwd::Measurement::Of(0.0, fwd::Provenance::Configured);
    e.bandwidth_mbps = fwd::Measurement::Of(100.0, fwd::Provenance::Configured);
    graph.AddEdge(e);
  };
  edge("entry", "relay_a", 1.0);
  edge("relay_a", "dest", 1.0);
  edge("entry", "relay_b", 10.0);
  edge("relay_b", "dest", 10.0);
  return graph;
}

inline fwd::ForwardingProblem ProblemFor(const fwd::ForwardingGraph& graph) {
  fwd::ForwardingProblem p;
  p.id = "runtime_test";
  p.source = "entry";
  p.destination = "dest";
  p.graph_version = graph.version();
  p.constraints.max_hops = 4;
  p.constraints.computation_deadline_ms = 1000.0;
  return p;
}

inline const fwd::Path kPathA = {"entry", "relay_a", "dest"};
inline const fwd::Path kPathB = {"entry", "relay_b", "dest"};

// V0.5 counts only forwarding over real traffic as integration evidence, so
// the registry is verified once with real loopback relays (127.0.0.1).
inline algorithms::CapabilityRegistry VerifiedRegistry() {
  static const algorithms::CapabilityRegistry registry = [] {
    const fwd::ForwardingGraph demo = fwd::LoopbackDemoGraph();
    fwd::LoopbackForwardingNetwork network(demo);
    std::string error;
    if (!network.Start(&error)) return algorithms::CapabilityRegistry::BuiltIn();
    fwd::LoopbackForwardingExecutor executor(network);
    auto verified = algorithms::ApplyVerification(algorithms::CapabilityRegistry::BuiltIn(),
                                                  algorithms::VerifyImplementations(&executor));
    network.Stop();
    return verified;
  }();
  return registry;
}

// Forwards over the holder's current graph with the deterministic simulated
// executor, then applies faults and seeded multiplicative latency noise.
class TestExecutor final : public fwd::IForwardingExecutor {
 public:
  explicit TestExecutor(GraphHolder& graph, std::uint64_t seed = 11) : graph_(graph), seed_(seed) {}

  fwd::ForwardingCapability Capability() const override {
    fwd::ForwardingCapability c;
    c.executor = "test_simulated";
    c.operations = {netstate::ForwardingOperation::SelectPath};
    c.protocols = {netstate::TransportProtocol::Tcp};
    c.real_network = false;
    c.max_hops = max_hops;
    return c;
  }
  bool Ready() const override { return ready; }

  fwd::ForwardingExecutionResult Execute(const fwd::ForwardingRequest& request,
                                         const fwd::ForwardingExecutionContext& context) override {
    const int now = ++in_flight;
    int seen = max_in_flight.load();
    while (now > seen && !max_in_flight.compare_exchange_weak(seen, now)) {
    }
    ++requests;
    if (on_request) on_request(request, context);
    if (sleep_ms > 0.0) {
      const auto until = std::chrono::steady_clock::now() + std::chrono::microseconds(static_cast<long long>(sleep_ms * 1000));
      while (std::chrono::steady_clock::now() < until && !(context.cancel && context.cancel->load())) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    }
    const auto graph = graph_.Get();
    fwd::ForwardingRequest pinned = request;
    pinned.graph_version = 0;  // The holder may have moved on; the test controls staleness itself.
    fwd::SimulatedForwardingExecutor sim(*graph, seed_);
    fwd::ForwardingExecutionResult r = sim.Execute(pinned, context);
    std::lock_guard<std::mutex> lock(mutex_);
    if (context.cancel && context.cancel->load()) {
      r.status = fwd::ForwardingStatus::Cancelled;
      r.path_confirmed = false;
    } else if (fail_all || fail_paths.count(request.path)) {
      r.status = fwd::ForwardingStatus::RelayUnavailable;
      r.response_status = "NEXT_HOP_UNAVAILABLE";
      r.path_confirmed = false;
      r.confirmed_path.clear();
    } else if (timeout_paths.count(request.path)) {
      r.status = fwd::ForwardingStatus::Timeout;
      r.response_status = "TIMEOUT";
      r.path_confirmed = false;
      r.end_to_end_ms = request.timeout_ms;
    } else if (unconfirmed_paths.count(request.path)) {
      r.path_confirmed = false;  // Delivered, but the trace does not prove the path.
    } else if (r.status == fwd::ForwardingStatus::Delivered && noise > 0.0) {
      std::uint64_t h = seed_;
      for (char c : request.request_id) h = h * 1099511628211ull ^ static_cast<unsigned char>(c);
      algorithms::Rng rng(h);
      double factor = std::exp(noise * rng.Normal());
      if (heavy_tail && rng.Uniform() < 0.1) factor *= 4.0;
      r.end_to_end_ms *= factor;
    }
    --in_flight;
    return r;
  }

  std::mutex mutex_;
  std::set<fwd::Path> fail_paths;
  std::set<fwd::Path> timeout_paths;
  std::set<fwd::Path> unconfirmed_paths;
  bool fail_all{false};
  bool heavy_tail{false};
  double noise{0.05};
  double sleep_ms{0.0};
  std::atomic<bool> ready{true};
  std::size_t max_hops{16};
  std::function<void(const fwd::ForwardingRequest&, const fwd::ForwardingExecutionContext&)> on_request;
  std::atomic<int> in_flight{0};
  std::atomic<int> max_in_flight{0};
  std::atomic<int> requests{0};

 private:
  GraphHolder& graph_;
  std::uint64_t seed_;
};

// ---- NetworkState fixtures ----

inline netstate::Feature MakeFeature(const std::string& id, std::optional<double> value,
                                     netstate::FeatureQuality quality = netstate::FeatureQuality::Valid,
                                     double age_ms = 100.0) {
  netstate::Feature f;
  f.id = id;
  f.value = value;
  f.quality = quality;
  f.kind = netstate::ValueKind::Measured;
  f.sample_count = 10;
  f.age_ms = age_ms;
  return f;
}

inline netstate::NetworkState MakeState(std::uint64_t id, std::vector<netstate::Feature> features,
                                        const std::string& interface_name = "Wi-Fi") {
  netstate::NetworkState s;
  s.state_id = id;
  s.primary_window = "short";
  s.windows.push_back({"short", 10'000, std::move(features)});
  s.sources.capture_interface = interface_name;
  s.validity = netstate::StateValidity::Degraded;
  return s;
}

// ---- Synthetic probe campaigns ----

struct ArmSpec {
  std::string id;
  algorithms::AlgorithmId algorithm;
  fwd::Path path;
  bool incumbent;
  // Duration of round r; nullopt means the probe failed.
  std::function<std::optional<double>(std::size_t round)> duration;
};

// Interleaved by default (order alternates every round); blocked = true
// sends every probe of one arm before the next arm.
inline runtime::ProbeCampaignResult MakeCampaign(const std::vector<ArmSpec>& arms, std::size_t rounds,
                                                 bool blocked = false) {
  runtime::ProbeCampaignResult c;
  c.scenario_id = "synthetic";
  c.ran = true;
  c.rounds_requested = rounds;
  c.rounds_completed = rounds;
  c.graph_version = 1;
  c.executor = "synthetic";
  for (const auto& a : arms) c.arms.push_back({a.id, a.algorithm, a.path, a.incumbent});
  const auto add = [&](const ArmSpec& a, std::size_t r, std::size_t order) {
    runtime::ProbeObservation o;
    o.probe_id = a.id + "-" + std::to_string(r);
    o.scenario_id = c.scenario_id;
    o.round = r;
    o.order_in_round = order;
    o.arm_id = a.id;
    o.algorithm = a.algorithm;
    o.path_id = fwd::PathText(a.path);
    o.graph_version = 1;
    o.executor = "synthetic";
    o.payload_bytes = 64;
    const auto d = a.duration(r);
    o.success = d.has_value();
    o.duration_ms = d.value_or(0.0);
    o.status = d ? "delivered" : "lost";
    c.observations.push_back(o);
  };
  if (blocked) {
    for (const auto& a : arms)
      for (std::size_t r = 0; r < rounds; ++r) add(a, r, 0);
  } else {
    for (std::size_t r = 0; r < rounds; ++r) {
      for (std::size_t k = 0; k < arms.size(); ++k) {
        const std::size_t i = (r % 2 == 0) ? k : arms.size() - 1 - k;
        add(arms[i], r, k);
      }
    }
  }
  return c;
}

// Deterministic pseudo-noise in [-1, 1) for (seed, round).
inline double Jitter(std::uint64_t seed, std::size_t round) {
  algorithms::Rng rng(seed * 7919 + round * 104729 + 1);
  return rng.Uniform(-1.0, 1.0);
}

}  // namespace aafr::test
