// V0.7.2 ModelWorkerClient against real processes: the fault-injecting fake
// worker (protocol and lifecycle failures) and, when configured, the real
// expert-model worker with the pinned artifacts (load, sequential inference,
// latency, corrupt/missing artifacts, unsupported metadata).
//
// Needs a Python interpreter (AAFR_MODEL_PYTHON). The real-model cases also
// need AAFR_MODEL_DIR and the model's Python packages; they are reported as
// skipped (not passed) when those are absent.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <thread>

#include "runtime/model/model_worker_client.h"
#include "tests/support/model_test_support.h"
#include "tests/support/test_support.h"

using namespace aafr;
using namespace aafr::model;
namespace fs = std::filesystem;

using aafr::test::Contains;

namespace {

int skipped = 0;

fs::path TempDir(const std::string& name) {
  const fs::path dir = fs::temp_directory_path() / ("aafr_model_test_" + name);
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir);
  return dir;
}

// A fake-worker configuration whose behaviour comes from fake_mode.json.
WorkerConfig FakeConfig(const std::string& name, const std::string& mode_json) {
  WorkerConfig c = aafr::test::RealWorkerConfig();
  c.worker_script = aafr::test::FakeWorkerScript();
  const fs::path dir = TempDir(name);
  std::ofstream(dir / "fake_mode.json") << mode_json;
  c.model_dir = dir.u8string();
  c.request_timeout_ms = 400.0;
  c.startup_timeout_ms = 15'000.0;
  c.restart_backoff_ms = 50.0;
  return c;
}

InferenceRequest Request(const std::string& id, std::uint64_t state = 5, std::uint64_t graph = 3) {
  InferenceRequest r;
  r.request_id = id;
  r.evaluation_id = "e-" + id;
  r.network_state_id = state;
  r.graph_version = graph;
  // Mid-range values inside the training ranges.
  r.features = aafr::test::ConfiguredTelemetryProfile();
  const std::map<std::string, double> graph_part = {
      {"num_nodes", 40}, {"num_edges", 200}, {"network_density", 0.12}, {"avg_node_degree", 5.0}, {"diameter", 5},
      {"avg_clustering_coefficient", 0.2}, {"algebraic_connectivity", 1.0}, {"req_shortest_hops", 3},
      {"source_in_degree", 5}, {"source_out_degree", 5}, {"dest_in_degree", 5}, {"dest_out_degree", 5}};
  r.features.insert(graph_part.begin(), graph_part.end());
  return r;
}

bool PythonConfigured() { return !aafr::test::RealWorkerConfig().python_executable.empty() && fs::exists(aafr::test::RealWorkerConfig().python_executable); }

bool RealModelConfigured() {
  const WorkerConfig c = aafr::test::RealWorkerConfig();
  return c.Validate().empty();
}

void Skip(const char* why) {
  ++skipped;
  std::cout << "SKIP " << aafr::test::current_test << ": " << why << "\n";
}

void WaitFor(const std::function<bool()>& condition, int timeout_ms) {
  const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (!condition() && std::chrono::steady_clock::now() < until) std::this_thread::sleep_for(std::chrono::milliseconds(10));
}

// ---------------------------------------------------------------- configuration

void NotConfiguredWithoutInterpreter() {
  WorkerConfig c = aafr::test::RealWorkerConfig();
  c.python_executable = "Z:/no/python.exe";
  ModelEventLog events;
  ModelWorkerClient client(c, &events);
  CHECK(client.Status().state == WorkerState::NotConfigured);
  CHECK(Contains(client.Status().detail, "python interpreter not found"));
  CHECK(!client.Start());
  const InferenceResponse r = client.Infer(Request("r1"));
  CHECK(r.status == InferenceStatus::WorkerUnavailable);
}

// ---------------------------------------------------------------- fake worker

void ValidRequestAndCorrelation() {
  if (!PythonConfigured()) return Skip("no python");
  ModelEventLog events;
  ModelWorkerClient client(FakeConfig("valid", R"({"responses":[{"kind":"ok","label":"PSO"},{"kind":"ok","label":"Dijkstra"}]})"), &events);
  CHECK(client.Start());
  CHECK(client.WaitUntilReady(15'000));
  CHECK(client.Status().state == WorkerState::Ready);
  CHECK(client.Status().labels.size() == 5);
  InferenceResponse a = client.Infer(Request("r1"));
  CHECK(a.status == InferenceStatus::Ok);
  CHECK(a.request_id == "r1");
  CHECK(a.model_label == "PSO" && a.stage2_ran && a.stage2_probabilities.size() == 4);
  CHECK(!a.probabilities_calibrated);
  InferenceResponse b = client.Infer(Request("r2"));
  CHECK(b.status == InferenceStatus::Ok && b.model_label == "Dijkstra" && !b.stage2_ran);
  CHECK(client.Ping(2000));
  const WorkerStatus s = client.Status();
  CHECK(s.requests == 2 && s.completed == 2);
  CHECK(s.startup_ms.has_value());
  CHECK(events.Count(ModelEventType::ModelWorkerStarted) == 1);
  CHECK(events.Count(ModelEventType::ModelReady) == 1);
  CHECK(events.Count(ModelEventType::InferenceRequested) == 2);
  CHECK(events.Count(ModelEventType::InferenceCompleted) == 2);
  client.Stop();
  CHECK(client.Status().state == WorkerState::Stopped);
  CHECK(events.Count(ModelEventType::ModelWorkerStopped) == 1);
}

void TimeoutThenLateResponseDiscarded() {
  if (!PythonConfigured()) return Skip("no python");
  ModelEventLog events;
  WorkerConfig c = FakeConfig("late", R"({"responses":[{"kind":"late","delay_ms":700,"label":"GWO"},{"kind":"ok","label":"WOA"}]})");
  c.timeouts_before_restart = 5;
  ModelWorkerClient client(c, &events);
  client.Start();
  CHECK(client.WaitUntilReady(15'000));
  const InferenceResponse late = client.Infer(Request("r1"));
  CHECK(late.status == InferenceStatus::Timeout);
  // The GWO answer for r1 arrives while nothing (or r2) is awaited: discarded.
  std::this_thread::sleep_for(std::chrono::milliseconds(500));
  const InferenceResponse next = client.Infer(Request("r2"));
  CHECK(next.status == InferenceStatus::Ok);
  CHECK(next.model_label == "WOA");  // never the obsolete GWO answer
  CHECK(client.Status().stale_discarded >= 1);
  CHECK(events.Count(ModelEventType::InferenceTimedOut) == 1);
  CHECK(events.Count(ModelEventType::StaleInferenceDiscarded) >= 1);
}

void RepeatedTimeoutsRestartWorker() {
  if (!PythonConfigured()) return Skip("no python");
  ModelEventLog events;
  WorkerConfig c = FakeConfig("hang", R"({"responses":[{"kind":"hang"},{"kind":"hang"},{"kind":"ok","label":"ACO"}]})");
  c.timeouts_before_restart = 2;
  ModelWorkerClient client(c, &events);
  client.Start();
  CHECK(client.WaitUntilReady(15'000));
  CHECK(client.Infer(Request("r1")).status == InferenceStatus::Timeout);
  CHECK(client.Infer(Request("r2")).status == InferenceStatus::Timeout);
  CHECK(client.Status().state == WorkerState::Restarting);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  // The next request triggers the restart; it fails fast while loading.
  CHECK(client.Infer(Request("r3")).status == InferenceStatus::WorkerUnavailable);
  CHECK(client.WaitUntilReady(15'000));
  CHECK(client.Status().restarts == 1);
  CHECK(client.Status().last_restart_ms.has_value());
  CHECK(events.Count(ModelEventType::ModelWorkerRestarted) == 1);
}

void CrashDetectedAndRecovered() {
  if (!PythonConfigured()) return Skip("no python");
  ModelEventLog events;
  ModelWorkerClient client(FakeConfig("crash", R"({"responses":[{"kind":"crash"},{"kind":"ok","label":"PSO"}]})"), &events);
  client.Start();
  CHECK(client.WaitUntilReady(15'000));
  const InferenceResponse r = client.Infer(Request("r1"));
  CHECK(r.status == InferenceStatus::WorkerUnavailable);
  CHECK(Contains(r.message, "exited"));
  WaitFor([&] { return client.Status().state == WorkerState::Restarting; }, 3000);
  CHECK(client.Status().state == WorkerState::Restarting);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  client.Infer(Request("r2"));  // triggers the restart
  CHECK(client.WaitUntilReady(15'000));
  // The restarted worker re-reads its script: the crash comes first again.
  CHECK(client.Status().restarts == 1);
}

void RestartBudgetPreventsLoops() {
  if (!PythonConfigured()) return Skip("no python");
  ModelEventLog events;
  WorkerConfig c = FakeConfig("crashloop", R"({"responses":[{"kind":"crash"}]})");
  c.max_restarts = 2;
  ModelWorkerClient client(c, &events);
  client.Start();
  for (int i = 0; i < 12 && client.Status().state != WorkerState::Failed; ++i) {
    client.WaitUntilReady(15'000);
    client.Infer(Request("r" + std::to_string(i)));
    WaitFor([&] { return client.Status().state != WorkerState::Ready; }, 3000);
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
  }
  const WorkerStatus s = client.Status();
  CHECK(s.state == WorkerState::Failed);
  CHECK(s.restarts == 2);
  CHECK(Contains(s.detail, "restart budget exhausted"));
  // Failed stays failed: no further automatic restarts.
  client.Infer(Request("again"));
  CHECK(client.Status().restarts == 2);
  // A manual restart is still possible.
  CHECK(client.Restart("operator"));
}

void MalformedAndUnexpectedResponses() {
  if (!PythonConfigured()) return Skip("no python");
  ModelEventLog events;
  ModelWorkerClient client(
      FakeConfig("malformed", R"({"responses":[{"kind":"malformed"},{"kind":"wrong_version","label":"PSO"},{"kind":"bad_echo","label":"PSO"},{"kind":"error","code":"E_INFERENCE_FAILED"},{"kind":"bad_probs","label":"PSO"},{"kind":"slow_model","inference_ms":5000,"label":"PSO"},{"kind":"unknown_label","label":"Teleport","gate":0.1},{"kind":"ok","label":"PSO"}]})"),
      &events);
  client.Start();
  CHECK(client.WaitUntilReady(15'000));
  CHECK(client.Infer(Request("m1")).status == InferenceStatus::InvalidResponse);
  CHECK(client.Infer(Request("m2")).status == InferenceStatus::VersionMismatch);
  CHECK(client.Infer(Request("m3")).status == InferenceStatus::Stale);
  const InferenceResponse e = client.Infer(Request("m4"));
  CHECK(e.status == InferenceStatus::WorkerError && e.error_code == "E_INFERENCE_FAILED");
  CHECK(client.Infer(Request("m5")).status == InferenceStatus::InvalidResponse);
  CHECK(client.Infer(Request("m6")).status == InferenceStatus::ExcessiveLatency);
  // An unknown label is a well-formed response; the provider's mapping rejects it.
  const InferenceResponse u = client.Infer(Request("m7"));
  CHECK(u.status == InferenceStatus::Ok && u.model_label == "Teleport");
  CHECK(!MapModelLabel(u.model_label));
  CHECK(client.Infer(Request("m8")).status == InferenceStatus::Ok);
  CHECK(client.Status().state == WorkerState::Ready);  // none of these killed the worker
}

void OversizedOutputKillsWorker() {
  if (!PythonConfigured()) return Skip("no python");
  ModelEventLog events;
  WorkerConfig c = FakeConfig("oversize", R"({"responses":[{"kind":"oversize"},{"kind":"ok"}]})");
  c.max_message_bytes = 4096;
  ModelWorkerClient client(c, &events);
  client.Start();
  CHECK(client.WaitUntilReady(15'000));
  const InferenceResponse r = client.Infer(Request("big"));
  CHECK(r.status != InferenceStatus::Ok);
  std::cout << "  oversize response: " << ToString(r.status) << " " << r.message << "\n";
  WaitFor([&] { return client.Status().state == WorkerState::Restarting; }, 3000);
  std::cout << "  state " << ToString(client.Status().state) << ": " << client.Status().detail << "\n";
  CHECK(client.Status().state == WorkerState::Restarting);
}

void StartupFailures() {
  if (!PythonConfigured()) return Skip("no python");
  struct Case {
    const char* name;
    const char* mode;
    const char* expect;
  };
  for (const Case& c : std::vector<Case>{
           {"fatal", R"({"startup":"fatal","fatal_code":"E_ARTIFACT_HASH_MISMATCH"})", "E_ARTIFACT_HASH_MISMATCH"},
           {"exit", R"({"startup":"exit"})", "No module named"},
           {"contract", R"({"startup":"bad_contract"})", "differs from aafr.model_features.v1"},
           {"version", R"({"startup":"wrong_version"})", "model_version"},
       }) {
    ModelEventLog events;
    ModelWorkerClient client(FakeConfig(std::string("startup_") + c.name, c.mode), &events);
    client.Start();
    CHECK(!client.WaitUntilReady(15'000));
    const WorkerStatus s = client.Status();
    CHECK(s.state == WorkerState::Failed);
    CHECK(Contains(s.detail, c.expect));
    CHECK(events.Count(ModelEventType::ModelWorkerFailed) >= 1);
    CHECK(client.Infer(Request("x")).status == InferenceStatus::WorkerUnavailable);
    CHECK(s.restarts == 0);  // artifact/contract problems are not retried automatically
  }
  // A worker that never becomes ready.
  ModelEventLog events;
  WorkerConfig c = FakeConfig("startup_hang", R"({"startup":"hang"})");
  c.startup_timeout_ms = 300.0;
  ModelWorkerClient client(c, &events);
  client.Start();
  std::this_thread::sleep_for(std::chrono::milliseconds(400));
  CHECK(client.Infer(Request("x")).status == InferenceStatus::WorkerUnavailable);
  CHECK(client.Status().state == WorkerState::Failed);
  CHECK(Contains(client.Status().detail, "did not become ready"));
}

void BusyWhileInFlight() {
  if (!PythonConfigured()) return Skip("no python");
  ModelEventLog events;
  WorkerConfig c = FakeConfig("busy", R"({"responses":[{"kind":"late","delay_ms":300,"label":"PSO"},{"kind":"ok"}]})");
  c.request_timeout_ms = 1000.0;
  ModelWorkerClient client(c, &events);
  client.Start();
  CHECK(client.WaitUntilReady(15'000));
  std::thread first([&] { client.Infer(Request("slow")); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  WorkerConfig quick = c;
  const auto started = std::chrono::steady_clock::now();
  const InferenceResponse second = client.Infer(Request("second"));
  first.join();
  // Either it waited for the slot (bounded by the timeout) or reported Busy;
  // never two requests in flight.
  CHECK(second.status == InferenceStatus::Ok || second.status == InferenceStatus::Busy);
  CHECK(std::chrono::steady_clock::now() - started < std::chrono::milliseconds(2500));
}

// ---------------------------------------------------------------- real model

void RealModelLoadsAndPredicts() {
  if (!RealModelConfigured()) return Skip("AAFR_MODEL_DIR / AAFR_MODEL_PYTHON not configured");
  ModelEventLog events;
  WorkerConfig c = aafr::test::RealWorkerConfig();
  c.request_timeout_ms = 2000.0;
  ModelWorkerClient client(c, &events);
  CHECK(client.Start());
  if (!client.WaitUntilReady(60'000)) {
    CHECK(false);
    std::cout << "worker not ready: " << client.Status().detail << "\n";
    return;
  }
  const WorkerStatus s = client.Status();
  CHECK(s.model_version == "lgbm2stage-20261027_220608");
  CHECK(s.labels == std::vector<std::string>({"Dijkstra", "ACO", "GWO", "PSO", "WOA"}));
  CHECK(s.gate_threshold && std::fabs(*s.gate_threshold - 0.51) < 1e-9);
  CHECK(s.library_versions.count("lightgbm") == 1);
  std::cout << "  startup " << *s.startup_ms << " ms, verify " << s.verify_ms.value_or(-1) << " ms, imports "
            << s.dependency_import_ms.value_or(-1) << " ms, load " << s.load_ms.value_or(-1) << " ms\n";
  for (const auto& w : s.load_warnings) std::cout << "  load warning: " << w << "\n";
  // Multiple sequential requests with latency statistics.
  std::vector<double> rtt, model_ms;
  int labels = 0;
  for (int i = 0; i < 60; ++i) {
    InferenceRequest req = Request("real" + std::to_string(i), 100 + i, 9);
    req.features["latency"] = 5.0 + i * 2.0;  // vary the input
    req.features["loss"] = 0.005 * (i % 20);
    const InferenceResponse r = client.Infer(req);
    CHECK(r.status == InferenceStatus::Ok);
    if (r.status != InferenceStatus::Ok) {
      std::cout << "  failure: " << ToString(r.status) << " " << r.message << "\n";
      break;
    }
    CHECK(MapModelLabel(r.model_label).has_value());
    CHECK(r.gate_probability && *r.gate_probability >= 0.0 && *r.gate_probability <= 1.0);
    CHECK(r.stage2_ran == (*r.gate_probability >= *r.gate_threshold));
    CHECK(r.distribution_warnings.size() == 1);  // scenario one-hots all zero
    if (r.model_label != "Dijkstra") ++labels;
    rtt.push_back(r.round_trip_ms);
    model_ms.push_back(*r.inference_ms);
  }
  if (!rtt.empty()) {
    std::sort(rtt.begin(), rtt.end());
    std::sort(model_ms.begin(), model_ms.end());
    const auto pct = [](const std::vector<double>& v, double q) { return v[static_cast<std::size_t>(q * (v.size() - 1))]; };
    std::cout << "  " << rtt.size() << " requests: round trip p50 " << pct(rtt, 0.5) << " ms, p95 " << pct(rtt, 0.95)
              << " ms, max " << rtt.back() << " ms; model p50 " << pct(model_ms, 0.5) << " ms, p95 "
              << pct(model_ms, 0.95) << " ms; metaheuristic labels " << labels << "\n";
  }
  const WorkerStatus after = client.Status();
  if (after.resources.available) {
    std::cout << "  worker (job) working set " << after.resources.working_set_bytes / (1024 * 1024) << " MiB (peak committed "
              << after.resources.peak_working_set_bytes / (1024 * 1024) << " MiB), private "
              << after.resources.private_bytes / (1024 * 1024) << " MiB, CPU user " << after.resources.cpu_user_ms
              << " ms kernel " << after.resources.cpu_kernel_ms << " ms\n";
  }
  // Worker-side validation reaches the client as explicit errors.
  InferenceRequest missing = Request("missing");
  missing.features.erase("jitter");
  const InferenceResponse m = client.Infer(missing);
  CHECK(m.status == InferenceStatus::WorkerError && m.error_code == "E_MISSING_FEATURES");
  InferenceRequest extra = Request("extra");
  extra.features["scenario_category"] = 1.0;
  CHECK(client.Infer(extra).error_code == "E_UNEXPECTED_FEATURES");
  // Restart time of the real worker.
  const auto t0 = std::chrono::steady_clock::now();
  client.Restart("measure restart");
  CHECK(client.WaitUntilReady(60'000));
  std::cout << "  manual restart to ready: "
            << std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() << " ms\n";
  client.Stop();
}

// Copies the release (model files only) to a temp dir so an artifact can be
// removed or corrupted without touching the original.
fs::path CopyRelease(const std::string& name) {
  const fs::path src = aafr::test::RealWorkerConfig().model_dir;
  const fs::path dst = TempDir(name);
  for (const char* sub : {"models", "ml"}) {
    fs::create_directories(dst / sub);
    for (const auto& e : fs::directory_iterator(src / sub)) {
      if (e.is_regular_file()) fs::copy_file(e.path(), dst / sub / e.path().filename(), fs::copy_options::overwrite_existing);
    }
  }
  return dst;
}

void RealModelArtifactFailures() {
  if (!RealModelConfigured()) return Skip("AAFR_MODEL_DIR / AAFR_MODEL_PYTHON not configured");
  // Missing artifact.
  {
    const fs::path dir = CopyRelease("missing_artifact");
    fs::remove(dir / "models" / "stage2_selector.joblib");
    WorkerConfig c = aafr::test::RealWorkerConfig();
    c.model_dir = dir.u8string();
    ModelEventLog events;
    ModelWorkerClient client(c, &events);
    client.Start();
    CHECK(!client.WaitUntilReady(60'000));
    CHECK(Contains(client.Status().detail, "E_ARTIFACT_MISSING"));
  }
  // Corrupt artifact: one byte changed -> pinned hash mismatch, never unpickled.
  {
    const fs::path dir = CopyRelease("corrupt_artifact");
    {
      std::fstream f(dir / "models" / "stage1_gate.joblib", std::ios::in | std::ios::out | std::ios::binary);
      f.seekp(100);
      f.put('\x7f');
    }
    WorkerConfig c = aafr::test::RealWorkerConfig();
    c.model_dir = dir.u8string();
    ModelEventLog events;
    ModelWorkerClient client(c, &events);
    client.Start();
    CHECK(!client.WaitUntilReady(60'000));
    CHECK(Contains(client.Status().detail, "E_ARTIFACT_HASH_MISMATCH"));
  }
  // Unsupported metadata: a manifest whose gate threshold differs from the
  // model's metadata (0.51). The C++ contract check passes; the worker's
  // metadata validation refuses to serve a silently changed threshold.
  {
    std::ifstream in(aafr::test::RealWorkerConfig().manifest_path, std::ios::binary);
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const std::string from = "\"threshold\": 0.5100000000000001";
    const auto pos = text.find(from);
    CHECK(pos != std::string::npos);
    if (pos != std::string::npos) text.replace(pos, from.size(), "\"threshold\": 0.6");
    const fs::path dir = TempDir("bad_metadata");
    std::ofstream(dir / "manifest.json", std::ios::binary) << text;
    WorkerConfig c = aafr::test::RealWorkerConfig();
    c.manifest_path = (dir / "manifest.json").u8string();
    ModelEventLog events;
    ModelWorkerClient client(c, &events);
    client.Start();
    CHECK(!client.WaitUntilReady(60'000));
    const std::string detail = client.Status().detail;
    CHECK(Contains(detail, "E_METADATA_UNSUPPORTED"));
    CHECK(Contains(detail, "threshold"));
  }
}

}  // namespace

int main(int argc, char** argv) {
  std::vector<std::pair<const char*, void (*)()>> tests = {
      {"not_configured_without_interpreter", NotConfiguredWithoutInterpreter},
      {"valid_request_and_correlation", ValidRequestAndCorrelation},
      {"timeout_then_late_response_discarded", TimeoutThenLateResponseDiscarded},
      {"repeated_timeouts_restart_worker", RepeatedTimeoutsRestartWorker},
      {"crash_detected_and_recovered", CrashDetectedAndRecovered},
      {"restart_budget_prevents_loops", RestartBudgetPreventsLoops},
      {"malformed_and_unexpected_responses", MalformedAndUnexpectedResponses},
      {"oversized_output_kills_worker", OversizedOutputKillsWorker},
      {"startup_failures", StartupFailures},
      {"busy_while_in_flight", BusyWhileInFlight},
      {"real_model_loads_and_predicts", RealModelLoadsAndPredicts},
      {"real_model_artifact_failures", RealModelArtifactFailures},
  };
  if (argc > 1) {
    std::vector<std::pair<const char*, void (*)()>> selected;
    for (const auto& t : tests)
      if (std::string(t.first).find(argv[1]) != std::string::npos) selected.push_back(t);
    tests = selected;
  }
  const int rc = aafr::test::RunTests(tests);
  std::cout << skipped << " skipped\n";
  return rc;
}
