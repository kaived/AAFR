// aafr-benchmark: runs the V0.5 benchmark catalog and writes JSONL and CSV.
//
//   aafr-benchmark [--out DIR] [--seeds N] [--scenarios a,b] [--iterations N]
//
// Graph attributes and forwarding outcomes are simulated; computation time
// is measured on this machine. The output is data for analysis, not a
// recommendation of any algorithm.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include "experiments/benchmark/benchmark_runner.h"

int main(int argc, char** argv) {
  using namespace aafr;
  benchmark::BenchmarkConfig config;
  std::filesystem::path out = "benchmark-results";
  for (int i = 1; i + 1 < argc; i += 2) {
    const std::string flag = argv[i];
    const std::string value = argv[i + 1];
    if (flag == "--out") {
      out = value;
    } else if (flag == "--seeds") {
      config.seeds.clear();
      for (int s = 1; s <= std::atoi(value.c_str()); ++s) config.seeds.push_back(static_cast<std::uint64_t>(s));
    } else if (flag == "--scenarios") {
      std::stringstream list(value);
      std::string id;
      while (std::getline(list, id, ',')) config.scenario_ids.push_back(id);
    } else if (flag == "--iterations") {
      config.budget.max_iterations = static_cast<std::uint64_t>(std::atoll(value.c_str()));
    } else {
      std::cerr << "unknown option " << flag << "\n";
      return 2;
    }
  }
  const benchmark::BenchmarkReport report = benchmark::RunBenchmark(config);
  std::error_code error;
  std::filesystem::create_directories(out, error);
  std::ofstream(out / "benchmark_runs.jsonl", std::ios::binary) << benchmark::ToJsonl(report);
  std::ofstream(out / "benchmark_runs.csv", std::ios::binary) << benchmark::ToCsv(report);

  std::printf("%-22s %-13s %5s %8s %8s %12s %12s %10s %10s\n", "scenario", "algorithm", "runs", "feasible", "at_ref",
              "obj_mean", "obj_std", "gap_mean%", "ms_mean");
  for (const auto& s : report.summaries) {
    std::printf("%-22s %-13s %5zu %8zu %8zu %12.3f %12.3f %10.3f %10.3f\n", s.scenario_id.c_str(),
                algorithms::ToString(s.algorithm), s.runs, s.feasible, s.at_reference, s.objective.mean,
                s.objective.stddev, s.gap_percent.mean, s.computation_ms.mean);
  }
  std::printf("%zu runs in %.1f ms; written to %s\n", report.runs.size(), report.total_ms,
              std::filesystem::absolute(out).string().c_str());
  return 0;
}
