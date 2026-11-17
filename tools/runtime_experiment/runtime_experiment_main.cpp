// aafr-runtime-experiment: controlled comparison of forwarding policies
// (V0.6 section 19). Plain C++17, loopback only, no Internet access.
//
// Configurations, run on identical scenarios and seeds:
//   fixed        Dijkstra's initial path, never changed.
//   adaptive     runtime, estimate-only evidence (no probing), stability on.
//   adaptive_sa  runtime with controlled probing + statistical arbitration.
//   no_stability runtime, estimate-only, every stability gate disabled
//                (isolated experimental configuration; never used by the browser).
//
// Scenarios on a two-path loopback topology (path A 5+5 ms, path B 25+25 ms,
// delays slept by the relays and labelled simulated):
//   congestion   A gets +150 ms from 25% to 65% of the run, then recovers.
//   fluctuation  A alternates between +0 and +60 ms every 700 ms (while
//                congested, B is clearly better: J 71 vs 51).
// The workload sends application requests with seeded exponential gaps.
//
// Output: one CSV row per run and a JSON summary per configuration and
// scenario, plus a table on stdout. Usage:
//   aafr-runtime-experiment [--out DIR] [--seconds S] [--seeds 1,2,3]

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "algorithms/common/path_algorithm.h"
#include "algorithms/execution/implementation_verifier.h"
#include "experiments/benchmark/benchmark_runner.h"
#include "routing/forwarding/loopback_forwarding.h"
#include "routing/topology/scenario_catalog.h"
#include "runtime/adaptive/aafr_runtime.h"

namespace fwd = aafr::forwarding;
namespace alg = aafr::algorithms;
namespace rt = aafr::runtime;
using Clock = std::chrono::steady_clock;

namespace {

class Graphs {
 public:
  explicit Graphs(fwd::ForwardingGraph g) : g_(std::make_shared<const fwd::ForwardingGraph>(std::move(g))) {}
  std::shared_ptr<const fwd::ForwardingGraph> Get() const {
    std::lock_guard<std::mutex> lock(m_);
    return g_;
  }
  void SetLatency(const char* from, const char* to, double ms, fwd::LoopbackForwardingNetwork& network) {
    std::lock_guard<std::mutex> lock(m_);
    fwd::ForwardingGraph copy = *g_;
    copy.SetEdgeLatency(from, to, fwd::Measurement::Of(ms, fwd::Provenance::Simulated));
    network.SyncWith(copy);
    g_ = std::make_shared<const fwd::ForwardingGraph>(std::move(copy));
  }

 private:
  mutable std::mutex m_;
  std::shared_ptr<const fwd::ForwardingGraph> g_;
};

fwd::ForwardingGraph Topology() {
  fwd::ForwardingGraph g("experiment_two_path");
  g.AddNode({"entry", fwd::NodeRole::Entry, true});
  g.AddNode({"relay_a", fwd::NodeRole::Relay, true});
  g.AddNode({"relay_b", fwd::NodeRole::Relay, true});
  g.AddNode({"dest", fwd::NodeRole::Destination, true});
  const auto edge = [&g](const char* from, const char* to, double ms) {
    fwd::ForwardingEdge e;
    e.from = from;
    e.to = to;
    e.latency_ms = fwd::Measurement::Of(ms, fwd::Provenance::Configured);
    e.loss_ratio = fwd::Measurement::Of(0.0, fwd::Provenance::Configured);
    e.bandwidth_mbps = fwd::Measurement::Of(100.0, fwd::Provenance::Configured);
    g.AddEdge(e);
  };
  edge("entry", "relay_a", 5);
  edge("relay_a", "dest", 5);
  edge("entry", "relay_b", 25);
  edge("relay_b", "dest", 25);
  return g;
}

fwd::ForwardingProblem ProblemFor(const fwd::ForwardingGraph& g) {
  fwd::ForwardingProblem p;
  p.id = "experiment";
  p.source = "entry";
  p.destination = "dest";
  p.graph_version = g.version();
  p.constraints.max_hops = 4;
  p.constraints.computation_deadline_ms = 1000.0;
  return p;
}

const fwd::Path kPathA = {"entry", "relay_a", "dest"};

struct RunResult {
  std::string config;
  std::string scenario;
  std::uint64_t seed{0};
  std::size_t requests{0};
  std::size_t delivered{0};
  std::vector<double> latencies;
  double payload_bytes{0};
  double request_ms_total{0};
  std::uint64_t switches{0};
  std::uint64_t failed_switches{0};
  std::uint64_t rejected{0};
  std::uint64_t evaluations{0};
  std::uint64_t probe_requests{0};
  double probe_ms{0};
  double decision_ms{0};
  double computation_ms{0};
  std::vector<double> switching_latency_ms;
  std::optional<double> adaptation_delay_ms;  // Congestion onset -> first switch off A.
  std::optional<double> recovery_delay_ms;    // Congestion end -> back on A.
  std::size_t measured_switches{0};           // Switches backed by probe evidence.
  std::size_t fixed_time_on_a_samples{0};
};

double Percentile(std::vector<double> v, double q) {
  if (v.empty()) return std::nan("");
  std::sort(v.begin(), v.end());
  const double pos = q * static_cast<double>(v.size() - 1);
  const std::size_t i = static_cast<std::size_t>(pos);
  const double f = pos - static_cast<double>(i);
  return i + 1 < v.size() ? v[i] * (1 - f) + v[i + 1] * f : v[i];
}

double Mean(const std::vector<double>& v) {
  if (v.empty()) return std::nan("");
  double s = 0;
  for (double x : v) s += x;
  return s / static_cast<double>(v.size());
}

RunResult Run(const std::string& config, const std::string& scenario, std::uint64_t seed, double seconds,
              const alg::CapabilityRegistry& registry) {
  RunResult result;
  result.config = config;
  result.scenario = scenario;
  result.seed = seed;
  Graphs graphs(Topology());
  fwd::LoopbackOptions options;
  options.inject_edge_latency = true;
  fwd::LoopbackForwardingNetwork network(*graphs.Get(), options);
  if (!network.Start()) {
    std::cerr << "relays did not start\n";
    return result;
  }
  fwd::LoopbackForwardingExecutor executor(network);
  alg::AlgorithmExecutionEngine engine(nullptr, registry, &executor);

  std::unique_ptr<rt::AAFRRuntime> runtime;
  if (config != "fixed") {
    rt::RuntimeConfig c;
    c.session_id = "x" + std::to_string(seed);
    c.tick_ms = 50;
    c.scheduler.min_interval_ms = 300;
    c.stability.min_dwell_ms = 1500;
    c.stability.cooldown_ms = 2500;
    c.stability.failed_activation_cooldown_ms = 500;
    c.stability.sustain_evaluations = 2;
    c.probe.rounds = 8;
    c.probe.seed = seed;
    c.arbitration.min_pairs = 6;
    c.arbitration.min_effective_pairs = 4;
    c.health_check_interval_ms = 1000;
    c.statistical_arbitration = config == "adaptive_sa";
    if (config == "no_stability") c.stability = rt::StabilityConfig::Disabled();
    rt::RuntimeDependencies d;
    d.graph_source = [&graphs] { return graphs.Get(); };
    d.problem_for = ProblemFor;
    d.executor = &executor;
    d.eligibility = &engine;
    d.executor_injects_delay = true;
    runtime = std::make_unique<rt::AAFRRuntime>(std::move(d), c);
    runtime->Start();
  }

  // Scenario driver.
  std::atomic<bool> done{false};
  alg::Rng rng(seed * 1000003ull + (scenario == "congestion" ? 1 : 2));
  const auto start = Clock::now();
  const double jitter_ms = rng.Uniform(-300.0, 300.0);  // seeded onset jitter
  const double onset_ms = seconds * 1000.0 * 0.25 + jitter_ms;
  const double clear_ms = seconds * 1000.0 * 0.65 + jitter_ms;
  std::optional<std::int64_t> onset_ns, clear_ns;
  std::thread driver([&] {
    bool congested = false;
    while (!done) {
      const double t = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
      if (scenario == "congestion") {
        const bool want = t >= onset_ms && t < clear_ms;
        if (want != congested) {
          graphs.SetLatency("entry", "relay_a", want ? 155.0 : 5.0, network);
          congested = want;
          const auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
          (want ? onset_ns : clear_ns) = now;
        }
      } else {
        const bool want = (static_cast<long long>(t / 700.0) % 2) == 1;
        if (want != congested) {
          graphs.SetLatency("entry", "relay_a", want ? 65.0 : 5.0, network);
          congested = want;
        }
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  });

  // Workload: application requests with exponential gaps (mean 60 ms).
  alg::Rng gaps(seed * 7919ull + 17);
  const std::string payload(64, 'w');
  std::size_t n = 0;
  while (std::chrono::duration<double>(Clock::now() - start).count() < seconds) {
    const std::string id = "w" + std::to_string(seed) + "-" + std::to_string(n++);
    fwd::ForwardingExecutionResult r;
    if (runtime) {
      r = runtime->ForwardActive(id, payload, 2000);
    } else {
      fwd::ForwardingRequest request;
      request.request_id = id;
      request.path = kPathA;  // Dijkstra on the configured graph.
      request.payload = payload;
      request.timeout_ms = 2000;
      request.graph_version = graphs.Get()->version();
      r = executor.Execute(request, {nullptr, id, "experiment"});
      if (r.response_status == "STALE_GRAPH") {  // raced a topology update; retry once
        request.graph_version = graphs.Get()->version();
        r = executor.Execute(request, {nullptr, id, "experiment"});
      }
    }
    ++result.requests;
    result.request_ms_total += r.end_to_end_ms;
    if (r.status == fwd::ForwardingStatus::Delivered && r.path_confirmed) {
      ++result.delivered;
      result.latencies.push_back(r.end_to_end_ms);
      result.payload_bytes += 2.0 * payload.size();
    }
    const double gap = -60.0 * std::log(1.0 - gaps.Uniform());
    std::this_thread::sleep_for(std::chrono::microseconds(static_cast<long long>(std::min(gap, 500.0) * 1000)));
  }
  done = true;
  driver.join();

  if (runtime) {
    runtime->Stop();
    const rt::RuntimeStatus s = runtime->Status();
    result.switches = s.counters.switches;
    result.failed_switches = s.counters.failed_switches;
    result.rejected = s.counters.rejected;
    result.evaluations = s.counters.evaluations;
    result.probe_requests = s.counters.probe_requests;
    result.probe_ms = s.counters.probe_ms;
    result.decision_ms = s.counters.decision_ms;
    result.computation_ms = s.counters.computation_ms;
    result.switching_latency_ms = runtime->OutcomeTotals().switching_latency_ms;
    for (const auto& d : runtime->Decisions(1000)) {
      const bool committed = d.activation == rt::ActivationStatus::Confirmed && d.type != rt::DecisionType::InitialActivation;
      if (!committed) continue;
      if (d.probing_attempted) ++result.measured_switches;
      if (onset_ns && !result.adaptation_delay_ms && d.monotonic_ns >= *onset_ns && d.confirmed_path != kPathA) {
        result.adaptation_delay_ms = (d.monotonic_ns - *onset_ns) / 1e6;
      }
      if (clear_ns && !result.recovery_delay_ms && d.monotonic_ns >= *clear_ns && d.confirmed_path == kPathA) {
        result.recovery_delay_ms = (d.monotonic_ns - *clear_ns) / 1e6;
      }
    }
  }
  network.Stop();
  return result;
}

std::string Num(double v, int precision = 2) {
  if (std::isnan(v)) return "";
  std::ostringstream out;
  out.setf(std::ios::fixed);
  out.precision(precision);
  out << v;
  return out.str();
}

}  // namespace

int main(int argc, char** argv) {
  std::string out_dir = "experiments/results/v0.6";
  double seconds = 20.0;
  std::vector<std::uint64_t> seeds = {1, 2, 3};
  for (int i = 1; i + 1 < argc; i += 2) {
    const std::string key = argv[i], value = argv[i + 1];
    if (key == "--out") out_dir = value;
    else if (key == "--seconds") seconds = std::max(4.0, std::stod(value));
    else if (key == "--seeds") {
      seeds.clear();
      std::stringstream list(value);
      std::string item;
      while (std::getline(list, item, ',')) seeds.push_back(std::stoull(item));
    }
  }
  std::filesystem::create_directories(out_dir);

  // Verify the ten implementations once over real loopback relays (V0.5).
  alg::CapabilityRegistry registry = alg::CapabilityRegistry::BuiltIn();
  {
    const fwd::ForwardingGraph demo = fwd::LoopbackDemoGraph();
    fwd::LoopbackForwardingNetwork network(demo);
    if (network.Start()) {
      fwd::LoopbackForwardingExecutor executor(network);
      registry = alg::ApplyVerification(registry, alg::VerifyImplementations(&executor));
      network.Stop();
    }
  }

  const std::vector<std::string> configs = {"fixed", "adaptive", "adaptive_sa", "no_stability"};
  const std::vector<std::string> scenarios = {"congestion", "fluctuation"};
  std::vector<RunResult> runs;
  std::ofstream csv(out_dir + "/runs.csv");
  csv << "scenario,config,seed,requests,success_rate,latency_mean_ms,latency_median_ms,latency_p95_ms,goodput_bytes_per_s,"
         "switches,failed_switches,rejected,evaluations,decision_ms_per_eval,computation_ms_per_eval,probe_requests,"
         "probe_ms,switch_latency_mean_ms,adaptation_delay_ms,recovery_delay_ms,measured_switches\n";
  for (const auto& scenario : scenarios) {
    for (const auto& config : configs) {
      for (std::uint64_t seed : seeds) {
        std::cerr << "running " << scenario << " / " << config << " / seed " << seed << "\n";
        RunResult r = Run(config, scenario, seed, seconds, registry);
        const double evals = r.evaluations ? static_cast<double>(r.evaluations) : std::nan("");
        csv << scenario << ',' << config << ',' << seed << ',' << r.requests << ','
            << Num(r.requests ? static_cast<double>(r.delivered) / r.requests : std::nan(""), 4) << ','
            << Num(Mean(r.latencies)) << ',' << Num(Percentile(r.latencies, 0.5)) << ','
            << Num(Percentile(r.latencies, 0.95)) << ','
            << Num(r.request_ms_total > 0 ? r.payload_bytes / (r.request_ms_total / 1000.0) : std::nan(""), 0) << ','
            << r.switches << ',' << r.failed_switches << ',' << r.rejected << ',' << r.evaluations << ','
            << Num(r.decision_ms / evals) << ',' << Num(r.computation_ms / evals) << ',' << r.probe_requests << ','
            << Num(r.probe_ms, 0) << ',' << Num(Mean(r.switching_latency_ms)) << ','
            << (r.adaptation_delay_ms ? Num(*r.adaptation_delay_ms, 0) : "") << ','
            << (r.recovery_delay_ms ? Num(*r.recovery_delay_ms, 0) : "") << ',' << r.measured_switches << "\n";
        runs.push_back(std::move(r));
      }
    }
  }

  // Summary: mean over seeds.
  std::ofstream json(out_dir + "/summary.json");
  json << "{\"schema\":\"aafr.runtime_experiment.v1\",\"seconds\":" << seconds << ",\"seeds\":" << seeds.size()
       << ",\"delays\":\"injected by loopback relays (simulated values, real forwarding)\",\"results\":[";
  std::cout << "\nscenario     config        success  mean_ms  p95_ms  switches  failed  evals  probe_req  "
               "adapt_ms  recover_ms\n";
  bool first = true;
  for (const auto& scenario : scenarios) {
    for (const auto& config : configs) {
      std::vector<double> success, mean, p95, switches, failed, evals, probes, adapt, recover, decision;
      for (const auto& r : runs) {
        if (r.scenario != scenario || r.config != config) continue;
        success.push_back(r.requests ? static_cast<double>(r.delivered) / r.requests : 0.0);
        mean.push_back(Mean(r.latencies));
        p95.push_back(Percentile(r.latencies, 0.95));
        switches.push_back(static_cast<double>(r.switches));
        failed.push_back(static_cast<double>(r.failed_switches));
        evals.push_back(static_cast<double>(r.evaluations));
        probes.push_back(static_cast<double>(r.probe_requests));
        if (r.adaptation_delay_ms) adapt.push_back(*r.adaptation_delay_ms);
        if (r.recovery_delay_ms) recover.push_back(*r.recovery_delay_ms);
        if (r.evaluations) decision.push_back(r.decision_ms / static_cast<double>(r.evaluations));
      }
      std::printf("%-12s %-13s %7.4f %8.2f %7.2f %9.1f %7.1f %6.1f %10.1f %9s %11s\n", scenario.c_str(), config.c_str(),
                  Mean(success), Mean(mean), Mean(p95), Mean(switches), Mean(failed), Mean(evals), Mean(probes),
                  Num(Mean(adapt), 0).c_str(), Num(Mean(recover), 0).c_str());
      json << (first ? "" : ",") << "{\"scenario\":\"" << scenario << "\",\"config\":\"" << config
           << "\",\"success_rate\":" << Num(Mean(success), 4) << ",\"latency_mean_ms\":" << Num(Mean(mean))
           << ",\"latency_p95_ms\":" << Num(Mean(p95)) << ",\"switches\":" << Num(Mean(switches))
           << ",\"failed_switches\":" << Num(Mean(failed)) << ",\"evaluations\":" << Num(Mean(evals))
           << ",\"decision_ms_per_evaluation\":" << (decision.empty() ? "null" : Num(Mean(decision), 3))
           << ",\"probe_requests\":" << Num(Mean(probes))
           << ",\"adaptation_delay_ms\":" << (adapt.empty() ? "null" : Num(Mean(adapt), 0))
           << ",\"adaptation_runs\":" << adapt.size()
           << ",\"recovery_delay_ms\":" << (recover.empty() ? "null" : Num(Mean(recover), 0))
           << ",\"recovery_runs\":" << recover.size() << "}";
      first = false;
    }
  }
  json << "]}\n";
  std::cout << "\nwrote " << out_dir << "/runs.csv and summary.json\n";
  return 0;
}
