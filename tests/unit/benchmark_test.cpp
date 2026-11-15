// V0.5: benchmark scenarios, reproducibility, statistics and export.

#include <cmath>
#include <sstream>

#include "experiments/benchmark/benchmark_runner.h"
#include "routing/topology/scenario_catalog.h"
#include "tests/support/test_support.h"

using namespace aafr::benchmark;
namespace alg = aafr::algorithms;

namespace {

BenchmarkConfig Quick(std::vector<std::string> scenarios) {
  BenchmarkConfig config;
  config.scenario_ids = std::move(scenarios);
  config.seeds = {1, 2, 3};
  config.budget.max_iterations = 20;
  config.budget.max_evaluations = 800;
  return config;
}

void TestScenarioLoading() {
  const auto ids = aafr::forwarding::BenchmarkScenarioIds();
  for (const char* required : {"small", "medium", "large", "sparse", "dense", "multiple_candidates", "disconnected",
                               "high_latency", "constrained_bandwidth", "link_failure", "limited_budget", "dynamic"}) {
    CHECK(std::find(ids.begin(), ids.end(), required) != ids.end());
  }
  const auto large = aafr::forwarding::FindScenario("large");
  const auto small = aafr::forwarding::FindScenario("small");
  CHECK(large->graph.nodes().size() == 152 && small->graph.nodes().size() == 8);
  CHECK(aafr::forwarding::FindScenario("dense")->graph.edges().size() >
        aafr::forwarding::FindScenario("sparse")->graph.edges().size());
}

void TestStatistics() {
  const Stats s = Summarize({4.0, 1.0, 3.0, 2.0});
  CHECK(s.count == 4);
  CHECK_NEAR(s.mean, 2.5, 1e-12);
  CHECK_NEAR(s.median, 2.5, 1e-12);
  CHECK_NEAR(s.stddev, std::sqrt(5.0 / 3.0), 1e-12);
  CHECK(s.min == 1.0 && s.max == 4.0);
  CHECK(Summarize({}).count == 0);
  CHECK(Summarize({7.0}).stddev == 0.0);
  CHECK(Summarize({1.0, INFINITY, 3.0}).count == 2);
}

void TestReproducibility() {
  const BenchmarkConfig config = Quick({"small", "multiple_candidates"});
  const BenchmarkReport a = RunBenchmark(config);
  const BenchmarkReport b = RunBenchmark(config);
  CHECK(a.runs.size() == b.runs.size());
  // 3 exact runs + 7 metaheuristics x 3 seeds, per scenario.
  CHECK(a.runs.size() == 2 * (3 + 7 * 3));
  bool identical = a.runs.size() == b.runs.size();
  for (std::size_t i = 0; identical && i < a.runs.size(); ++i) {
    identical = a.runs[i].result.path == b.runs[i].result.path && a.runs[i].result.objective == b.runs[i].result.objective &&
                a.runs[i].result.evaluations == b.runs[i].result.evaluations;
  }
  CHECK(identical);
  for (const BenchmarkRun& run : a.runs) {
    CHECK(run.result.status == alg::AlgorithmStatus::PathFound);
    CHECK(run.reference_objective.has_value());
    CHECK(run.gap_percent && *run.gap_percent >= -1e-9);
    if (run.algorithm == alg::AlgorithmId::Dijkstra) CHECK(std::fabs(*run.gap_percent) < 1e-9);
    CHECK(run.forwarding && run.forwarding->provenance == aafr::forwarding::Provenance::Simulated);
  }
}

void TestScenarioOutcomes() {
  const BenchmarkReport report = RunBenchmark(Quick({"disconnected", "limited_budget", "dynamic"}));
  std::size_t dynamic_epochs = 0;
  for (const BenchmarkRun& run : report.runs) {
    if (run.scenario_id == "disconnected") CHECK(run.result.status == alg::AlgorithmStatus::NoPath);
    if (run.scenario_id == "dynamic" && run.algorithm == alg::AlgorithmId::Dijkstra) ++dynamic_epochs;
  }
  CHECK(dynamic_epochs == 4);
  for (const BenchmarkRun& run : report.runs) {
    if (run.scenario_id != "limited_budget") continue;
    if (run.algorithm == alg::AlgorithmId::Dijkstra || run.algorithm == alg::AlgorithmId::AStar ||
        run.algorithm == alg::AlgorithmId::BellmanFord) {
      continue;
    }
    CHECK(run.result.iterations <= 3);
    CHECK(run.result.evaluations <= 30);
  }
  for (const BenchmarkSummary& s : report.summaries) {
    if (s.scenario_id == "disconnected") CHECK(s.feasible == 0 && s.failed == 0 && s.statuses.at("no_path") == s.runs);
  }
}

void TestExport() {
  const BenchmarkReport report = RunBenchmark(Quick({"small"}));
  const std::string jsonl = ToJsonl(report);
  std::istringstream lines(jsonl);
  std::string line;
  std::size_t count = 0, runs = 0, summaries = 0;
  while (std::getline(lines, line)) {
    ++count;
    CHECK(!line.empty() && line.front() == '{' && line.back() == '}');
    runs += aafr::test::Contains(line, "\"record\":\"run\"");
    summaries += aafr::test::Contains(line, "\"record\":\"summary\"");
  }
  CHECK(count == 1 + report.runs.size() + report.summaries.size());
  CHECK(runs == report.runs.size() && summaries == 10);
  for (const char* field : {"\"schema\":1", "\"provenance\"", "\"features\"", "\"scenarios\"", "\"algorithms\"",
                            "\"unit\":\"ms\"", "\"generator_seed\":101", "\"encoding\"", "\"configuration\""}) {
    CHECK(aafr::test::Contains(jsonl, field));
  }
  const std::string csv = ToCsv(report);
  CHECK(std::count(csv.begin(), csv.end(), '\n') == static_cast<long>(1 + report.runs.size()));
  CHECK(aafr::test::Contains(csv, "scenario_id,epoch,algorithm"));
  CHECK(JsonEscape("a\"b\\c\n") == "a\\\"b\\\\c\\n");
}

}  // namespace

int main() {
  return aafr::test::RunTests({
      {"scenario_loading", TestScenarioLoading},
      {"statistics", TestStatistics},
      {"fixed_seed_reproducibility", TestReproducibility},
      {"scenario_outcomes", TestScenarioOutcomes},
      {"export", TestExport},
  });
}
