#include "experiments/benchmark/benchmark_runner.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <sstream>

#include "network/telemetry/telemetry_event.h"
#include "routing/topology/scenario_catalog.h"

namespace aafr::benchmark {
namespace {

namespace alg = algorithms;
namespace fwd = forwarding;

std::string Num(double value) {
  if (!std::isfinite(value)) return "null";
  char buffer[48];
  std::snprintf(buffer, sizeof(buffer), "%.10g", value);
  return buffer;
}

std::string Str(const std::string& text) { return "\"" + JsonEscape(text) + "\""; }

std::string OptNum(const std::optional<double>& value) { return value ? Num(*value) : "null"; }

std::string StatsJson(const Stats& s) {
  return "{\"count\":" + std::to_string(s.count) + ",\"mean\":" + Num(s.mean) + ",\"stddev\":" + Num(s.stddev) +
         ",\"min\":" + Num(s.min) + ",\"median\":" + Num(s.median) + ",\"max\":" + Num(s.max) + "}";
}

std::string MapJson(const std::map<std::string, std::string>& values) {
  std::string out = "{";
  for (const auto& entry : values) out += (out.size() > 1 ? "," : "") + Str(entry.first) + ":" + Str(entry.second);
  return out + "}";
}

std::string CsvField(const std::string& text) {
  if (text.find_first_of(",\"\n") == std::string::npos) return text;
  std::string out = "\"";
  for (const char c : text) out += c == '"' ? std::string("\"\"") : std::string(1, c);
  return out + "\"";
}

std::vector<fwd::Scenario> SelectedScenarios(const BenchmarkConfig& config) {
  std::vector<fwd::Scenario> all = fwd::BenchmarkScenarios();
  if (config.scenario_ids.empty()) return all;
  std::vector<fwd::Scenario> chosen;
  for (const std::string& id : config.scenario_ids) {
    for (fwd::Scenario& s : all) {
      if (s.id == id) chosen.push_back(s);
    }
  }
  return chosen;
}

std::vector<alg::AlgorithmId> SelectedAlgorithms(const BenchmarkConfig& config) {
  if (!config.algorithms.empty()) return config.algorithms;
  return {alg::kAllAlgorithmIds.begin(), alg::kAllAlgorithmIds.end()};
}

// Best feasible exact objective for one epoch, or nullopt.
std::optional<double> Reference(const fwd::ForwardingGraph& graph, const fwd::ForwardingProblem& problem) {
  const fwd::ProblemView view = fwd::BuildView(graph, problem);
  const auto exact = alg::CreateAlgorithm(view.has_negative_cost ? alg::AlgorithmId::BellmanFord : alg::AlgorithmId::Dijkstra);
  alg::AlgorithmRequest request;
  request.graph = &graph;
  request.problem = &problem;
  const alg::AlgorithmResult result = exact->Run(request);
  if (result.status == alg::AlgorithmStatus::PathFound && result.feasible) return result.objective;
  return std::nullopt;
}

}  // namespace

std::string JsonEscape(const std::string& text) {
  std::string out;
  for (const char c : text) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char buffer[8];
          std::snprintf(buffer, sizeof(buffer), "\\u%04x", c);
          out += buffer;
        } else {
          out += c;
        }
    }
  }
  return out;
}

Stats Summarize(std::vector<double> values) {
  Stats s;
  values.erase(std::remove_if(values.begin(), values.end(), [](double v) { return !std::isfinite(v); }), values.end());
  s.count = values.size();
  if (values.empty()) return s;
  std::sort(values.begin(), values.end());
  double sum = 0.0;
  for (const double v : values) sum += v;
  s.mean = sum / static_cast<double>(values.size());
  double squares = 0.0;
  for (const double v : values) squares += (v - s.mean) * (v - s.mean);
  // Sample standard deviation; zero for a single value.
  s.stddev = values.size() > 1 ? std::sqrt(squares / static_cast<double>(values.size() - 1)) : 0.0;
  s.min = values.front();
  s.max = values.back();
  const std::size_t mid = values.size() / 2;
  s.median = values.size() % 2 ? values[mid] : (values[mid - 1] + values[mid]) / 2.0;
  return s;
}

BenchmarkReport RunBenchmark(const BenchmarkConfig& config, const std::atomic<bool>* cancel) {
  const auto started = std::chrono::steady_clock::now();
  BenchmarkReport report;
  report.config = config;
  report.started_at = telemetry::FormatUtc(telemetry::WallClock::now());
  const std::vector<alg::AlgorithmId> algorithm_ids = SelectedAlgorithms(config);

  for (const fwd::Scenario& scenario : SelectedScenarios(config)) {
    fwd::ForwardingGraph graph = scenario.graph;
    fwd::ForwardingProblem problem = scenario.problem;
    const std::size_t epochs = 1 + scenario.epochs.size();
    for (std::size_t epoch = 0; epoch < epochs; ++epoch) {
      if (epoch > 0) fwd::ApplyChanges(graph, scenario.epochs[epoch - 1]);
      problem.graph_version = graph.version();
      const fwd::ProblemView view = fwd::BuildView(graph, problem);
      const std::optional<double> reference = Reference(graph, problem);
      alg::ExecutionBudget budget = config.budget;
      if (scenario.max_iterations) budget.max_iterations = std::min(budget.max_iterations, *scenario.max_iterations);
      if (scenario.max_evaluations) {
        budget.max_evaluations = budget.max_evaluations ? std::min(*budget.max_evaluations, *scenario.max_evaluations)
                                                        : *scenario.max_evaluations;
      }
      fwd::SimulatedForwardingExecutor simulator(graph, scenario.generator_seed);

      for (const alg::AlgorithmId id : algorithm_ids) {
        const auto algorithm = alg::CreateAlgorithm(id);
        const std::vector<std::uint64_t> seeds =
            algorithm->deterministic() || config.seeds.empty()
                ? std::vector<std::uint64_t>{config.seeds.empty() ? 1 : config.seeds.front()}
                : config.seeds;
        for (const std::uint64_t seed : seeds) {
          if (cancel && cancel->load()) {
            report.cancelled = true;
            break;
          }
          BenchmarkRun run;
          run.scenario_id = scenario.id;
          run.epoch = static_cast<int>(epoch);
          run.algorithm = id;
          run.seed = seed;
          run.nodes = graph.nodes().size();
          run.edges = graph.edges().size();
          run.usable_edges = view.usable_edge_count;
          alg::AlgorithmRequest request;
          request.graph = &graph;
          request.problem = &problem;
          request.budget = budget;
          request.seed = seed;
          request.cancel = cancel;
          const auto params = config.parameters.find(id);
          if (params != config.parameters.end()) request.parameters = params->second;
          run.result = algorithm->Run(request);
          if (!run.result.path.empty()) {
            const fwd::PathValidation validation = fwd::ValidatePath(graph, problem, run.result.path);
            run.path_latency_ms = validation.latency_ms;
            run.hops = validation.hops;
          }
          run.reference_objective = reference;
          if (reference && run.result.feasible && run.result.objective) {
            run.gap_percent = (*run.result.objective - *reference) / std::max(std::fabs(*reference), 1e-9) * 100.0;
          }
          if (config.simulate_forwarding && run.result.status == alg::AlgorithmStatus::PathFound && run.result.feasible) {
            fwd::ForwardingRequest forward;
            forward.request_id = "b-" + scenario.id + "-" + std::to_string(epoch) + "-" + alg::ToString(id) + "-" +
                                 std::to_string(seed);
            forward.path = run.result.path;
            forward.graph_version = graph.version();
            forward.timeout_ms = 10000.0;
            run.forwarding = simulator.Execute(forward, {cancel, forward.request_id, scenario.id});
          }
          report.runs.push_back(std::move(run));
        }
      }
    }
  }

  // Summaries per (scenario, algorithm), in run order.
  std::vector<std::pair<std::string, alg::AlgorithmId>> keys;
  for (const BenchmarkRun& run : report.runs) {
    const auto key = std::make_pair(run.scenario_id, run.algorithm);
    if (std::find(keys.begin(), keys.end(), key) == keys.end()) keys.push_back(key);
  }
  for (const auto& key : keys) {
    BenchmarkSummary summary;
    summary.scenario_id = key.first;
    summary.algorithm = key.second;
    std::vector<double> objectives, times, gaps, iterations;
    for (const BenchmarkRun& run : report.runs) {
      if (run.scenario_id != key.first || run.algorithm != key.second) continue;
      ++summary.runs;
      ++summary.statuses[alg::ToString(run.result.status)];
      times.push_back(run.result.duration_ms);
      iterations.push_back(static_cast<double>(run.result.iterations));
      const bool feasible = run.result.status == alg::AlgorithmStatus::PathFound && run.result.feasible;
      if (feasible && run.result.objective) {
        ++summary.feasible;
        objectives.push_back(*run.result.objective);
      } else if (run.result.status != alg::AlgorithmStatus::NoPath) {
        ++summary.failed;  // A path existed or the run broke; NoPath on an unreachable graph is correct.
      }
      if (run.gap_percent) {
        gaps.push_back(*run.gap_percent);
        if (std::fabs(*run.gap_percent) < 1e-9) ++summary.at_reference;
      }
      if (run.forwarding && run.forwarding->status == fwd::ForwardingStatus::Delivered) ++summary.delivered;
    }
    summary.objective = Summarize(objectives);
    summary.computation_ms = Summarize(times);
    summary.gap_percent = Summarize(gaps);
    summary.iterations = Summarize(iterations);
    report.summaries.push_back(std::move(summary));
  }
  report.total_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
  return report;
}

std::string ToJsonl(const BenchmarkReport& report) {
  std::ostringstream out;
  const BenchmarkConfig& c = report.config;
  // ---- Header ----
  out << "{\"record\":\"header\",\"schema\":" << kBenchmarkSchemaVersion << ",\"started_at\":" << Str(report.started_at)
      << ",\"total_ms\":" << Num(report.total_ms) << ",\"cancelled\":" << (report.cancelled ? "true" : "false")
      << ",\"objective\":" << Str(fwd::ForwardingObjective{}.Describe())
      << ",\"budget\":{\"max_iterations\":" << c.budget.max_iterations
      << ",\"max_evaluations\":" << (c.budget.max_evaluations ? std::to_string(*c.budget.max_evaluations) : "null")
      << ",\"deadline_ms\":" << OptNum(c.budget.deadline_ms) << "},\"seeds\":[";
  for (std::size_t i = 0; i < c.seeds.size(); ++i) out << (i ? "," : "") << c.seeds[i];
  out << "],\"provenance\":{\"graph_attributes\":\"simulated (seeded generator; see scenarios)\","
         "\"computation_ms\":\"measured on this host (steady clock)\","
         "\"forwarding\":\"simulated (SimulatedForwardingExecutor; no network traffic)\"},"
         "\"features\":["
         "{\"name\":\"computation_ms\",\"unit\":\"ms\",\"provenance\":\"measured\",\"description\":\"wall time of the algorithm run\"},"
         "{\"name\":\"objective\",\"unit\":\"J(P)\",\"provenance\":\"computed\",\"description\":\"shared forwarding objective of the returned path\"},"
         "{\"name\":\"feasible\",\"unit\":\"bool\",\"provenance\":\"computed\",\"description\":\"path meets every hard constraint\"},"
         "{\"name\":\"gap_percent\",\"unit\":\"percent\",\"provenance\":\"computed\",\"description\":\"objective above the exact feasible reference\"},"
         "{\"name\":\"path_latency_ms\",\"unit\":\"ms\",\"provenance\":\"simulated\",\"description\":\"sum of edge latency on the path\"},"
         "{\"name\":\"hops\",\"unit\":\"edges\",\"provenance\":\"computed\",\"description\":\"edges on the path\"},"
         "{\"name\":\"iterations\",\"unit\":\"count\",\"provenance\":\"measured\",\"description\":\"iterations, node expansions or relaxation rounds\"},"
         "{\"name\":\"evaluations\",\"unit\":\"count\",\"provenance\":\"measured\",\"description\":\"objective evaluations or edge relaxations\"},"
         "{\"name\":\"forwarding_ms\",\"unit\":\"ms\",\"provenance\":\"simulated\",\"description\":\"simulated end-to-end forwarding delay\"}"
         "],\"scenarios\":[";
  bool first = true;
  for (const fwd::Scenario& s : SelectedScenarios(c)) {
    out << (first ? "" : ",") << "{\"id\":" << Str(s.id) << ",\"description\":" << Str(s.description)
        << ",\"generator_seed\":" << s.generator_seed << ",\"nodes\":" << s.graph.nodes().size()
        << ",\"edges\":" << s.graph.edges().size() << ",\"epochs\":" << (1 + s.epochs.size())
        << ",\"configuration\":" << MapJson(s.configuration) << "}";
    first = false;
  }
  out << "],\"algorithms\":[";
  first = true;
  for (const alg::AlgorithmId id : SelectedAlgorithms(c)) {
    const auto algorithm = alg::CreateAlgorithm(id);
    alg::AlgorithmParameters parameters = algorithm->DefaultParameters();
    const auto custom = c.parameters.find(id);
    if (custom != c.parameters.end()) {
      for (const auto& entry : custom->second) parameters[entry.first] = entry.second;
    }
    out << (first ? "" : ",") << "{\"id\":" << Str(alg::ToString(id)) << ",\"version\":" << Str(algorithm->version())
        << ",\"encoding\":" << Str(algorithm->encoding())
        << ",\"deterministic\":" << (algorithm->deterministic() ? "true" : "false")
        << ",\"parameters\":" << MapJson(parameters) << "}";
    first = false;
  }
  out << "]}\n";

  // ---- Runs ----
  for (const BenchmarkRun& run : report.runs) {
    const alg::AlgorithmResult& r = run.result;
    out << "{\"record\":\"run\",\"scenario_id\":" << Str(run.scenario_id) << ",\"epoch\":" << run.epoch
        << ",\"algorithm\":" << Str(alg::ToString(run.algorithm)) << ",\"algorithm_version\":" << Str(r.algorithm_version)
        << ",\"seed\":" << run.seed << ",\"nodes\":" << run.nodes << ",\"edges\":" << run.edges
        << ",\"usable_edges\":" << run.usable_edges << ",\"graph_version\":" << r.graph_version
        << ",\"status\":" << Str(alg::ToString(r.status)) << ",\"termination\":" << Str(alg::ToString(r.termination))
        << ",\"feasible\":" << (r.feasible ? "true" : "false") << ",\"objective\":" << OptNum(r.objective)
        << ",\"reference_objective\":" << OptNum(run.reference_objective) << ",\"gap_percent\":" << OptNum(run.gap_percent)
        << ",\"computation_ms\":" << Num(r.duration_ms) << ",\"iterations\":" << r.iterations
        << ",\"evaluations\":" << r.evaluations << ",\"accounted_memory_bytes\":" << r.accounted_memory_bytes
        << ",\"path\":" << Str(fwd::PathText(r.path)) << ",\"path_latency_ms\":" << Num(run.path_latency_ms)
        << ",\"hops\":" << run.hops << ",\"optimality_claimed\":" << (r.optimality_claimed ? "true" : "false")
        << ",\"message\":" << Str(r.message);
    if (run.forwarding) {
      out << ",\"forwarding\":{\"status\":" << Str(fwd::ToString(run.forwarding->status))
          << ",\"path_confirmed\":" << (run.forwarding->path_confirmed ? "true" : "false")
          << ",\"forwarding_ms\":" << Num(run.forwarding->end_to_end_ms)
          << ",\"provenance\":" << Str(fwd::ToString(run.forwarding->provenance)) << "}";
    } else {
      out << ",\"forwarding\":null";
    }
    out << "}\n";
  }

  // ---- Summaries ----
  for (const BenchmarkSummary& s : report.summaries) {
    out << "{\"record\":\"summary\",\"scenario_id\":" << Str(s.scenario_id) << ",\"algorithm\":"
        << Str(alg::ToString(s.algorithm)) << ",\"runs\":" << s.runs << ",\"feasible\":" << s.feasible
        << ",\"failed\":" << s.failed << ",\"at_reference\":" << s.at_reference << ",\"delivered_simulated\":" << s.delivered
        << ",\"statuses\":{";
    bool comma = false;
    for (const auto& entry : s.statuses) {
      out << (comma ? "," : "") << Str(entry.first) << ":" << entry.second;
      comma = true;
    }
    out << "},\"objective\":" << StatsJson(s.objective) << ",\"computation_ms\":" << StatsJson(s.computation_ms)
        << ",\"gap_percent\":" << StatsJson(s.gap_percent) << ",\"iterations\":" << StatsJson(s.iterations) << "}\n";
  }
  return out.str();
}

std::string ToCsv(const BenchmarkReport& report) {
  std::ostringstream out;
  out << "schema,scenario_id,epoch,algorithm,algorithm_version,seed,nodes,edges,status,termination,feasible,objective,"
         "reference_objective,gap_percent,computation_ms,iterations,evaluations,path_latency_ms,hops,path,"
         "forwarding_status,forwarding_ms_simulated\n";
  for (const BenchmarkRun& run : report.runs) {
    const alg::AlgorithmResult& r = run.result;
    out << kBenchmarkSchemaVersion << "," << CsvField(run.scenario_id) << "," << run.epoch << ","
        << alg::ToString(run.algorithm) << "," << r.algorithm_version << "," << run.seed << "," << run.nodes << ","
        << run.edges << "," << alg::ToString(r.status) << "," << alg::ToString(r.termination) << ","
        << (r.feasible ? "true" : "false") << "," << (r.objective ? Num(*r.objective) : "") << ","
        << (run.reference_objective ? Num(*run.reference_objective) : "") << ","
        << (run.gap_percent ? Num(*run.gap_percent) : "") << "," << Num(r.duration_ms) << "," << r.iterations << ","
        << r.evaluations << "," << Num(run.path_latency_ms) << "," << run.hops << "," << CsvField(fwd::PathText(r.path))
        << "," << (run.forwarding ? fwd::ToString(run.forwarding->status) : "") << ","
        << (run.forwarding ? Num(run.forwarding->end_to_end_ms) : "") << "\n";
  }
  return out.str();
}

}  // namespace aafr::benchmark
