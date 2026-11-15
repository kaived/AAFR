#pragma once

// Reproducible benchmarking of the ten path algorithms. Plain C++17.
//
// Every algorithm solves the same scenarios with the same objective and
// comparable budgets. Deterministic algorithms run once per scenario epoch;
// stochastic ones once per seed. Computation time is measured on this host;
// graph attributes and forwarding outcomes are simulated and labelled so.
// Nothing here selects a "best" algorithm.

#include <atomic>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "algorithms/common/path_algorithm.h"
#include "routing/forwarding/forwarding_executor.h"

namespace aafr::benchmark {

inline constexpr int kBenchmarkSchemaVersion = 1;

struct BenchmarkConfig {
  std::vector<std::string> scenario_ids;            // Empty: every catalog scenario.
  std::vector<algorithms::AlgorithmId> algorithms;  // Empty: all ten.
  std::vector<std::uint64_t> seeds{1, 2, 3, 4, 5};
  algorithms::ExecutionBudget budget{100, 4000, 5000.0};
  bool simulate_forwarding{true};
  std::map<algorithms::AlgorithmId, algorithms::AlgorithmParameters> parameters;
};

struct BenchmarkRun {
  std::string scenario_id;
  int epoch{0};
  algorithms::AlgorithmId algorithm{algorithms::AlgorithmId::Dijkstra};
  std::uint64_t seed{0};
  std::size_t nodes{0};
  std::size_t edges{0};
  std::size_t usable_edges{0};
  algorithms::AlgorithmResult result;
  double path_latency_ms{0.0};
  std::size_t hops{0};
  // Best feasible objective from an exact algorithm for this epoch.
  std::optional<double> reference_objective;
  std::optional<double> gap_percent;
  std::optional<forwarding::ForwardingExecutionResult> forwarding;
};

struct Stats {
  std::size_t count{0};
  double mean{0.0};
  double stddev{0.0};
  double min{0.0};
  double median{0.0};
  double max{0.0};
};

Stats Summarize(std::vector<double> values);

struct BenchmarkSummary {
  std::string scenario_id;
  algorithms::AlgorithmId algorithm{algorithms::AlgorithmId::Dijkstra};
  std::size_t runs{0};
  std::size_t feasible{0};
  std::size_t failed{0};
  std::map<std::string, std::size_t> statuses;
  Stats objective;        // Feasible runs only.
  Stats computation_ms;   // All runs.
  Stats gap_percent;      // Feasible runs with a reference.
  Stats iterations;
  std::size_t at_reference{0};
  std::size_t delivered{0};  // Simulated forwarding.
};

struct BenchmarkReport {
  BenchmarkConfig config;
  std::vector<BenchmarkRun> runs;
  std::vector<BenchmarkSummary> summaries;
  std::string started_at;
  double total_ms{0.0};
  bool cancelled{false};
};

BenchmarkReport RunBenchmark(const BenchmarkConfig& config, const std::atomic<bool>* cancel = nullptr);

// One header record (schema, units, provenance, scenario and algorithm
// configuration), one record per run, one per summary.
std::string ToJsonl(const BenchmarkReport& report);
// One row per run.
std::string ToCsv(const BenchmarkReport& report);
std::string JsonEscape(const std::string& text);

}  // namespace aafr::benchmark
