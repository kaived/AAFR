#pragma once

// V0.7.2 expert-model test fixtures: a scripted IModelInference fake, a
// complete configured telemetry profile, and the configured paths of the
// real worker (from CMake). Header-only; include from one translation unit.

#include <atomic>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "runtime/model/model_feature_adapter.h"
#include "runtime/model/model_worker_client.h"

namespace aafr::test {

// A mid-range value for each of the 23 telemetry/latency/dynamics features
// (inside the training ranges), in contract units.
inline std::map<std::string, double> ConfiguredTelemetryProfile() {
  return {
      {"latency", 20.0},
      {"jitter", 2.0},
      {"loss", 0.01},
      {"throughput", 40.0},
      {"mean_link_utilization", 1.5},
      {"max_link_utilization", 12.0},
      {"link_utilization_std", 2.5},
      {"avg_queue_delay_ms", 1.0},
      {"max_queue_depth", 8.0},
      {"avg_queue_occupancy", 0.01},
      {"offered_load_mbps", 42.0},
      {"packet_delivery_ratio", 0.98},
      {"jains_fairness_index", 0.95},
      {"latency_p50", 18.0},
      {"latency_p90", 30.0},
      {"latency_p95", 35.0},
      {"latency_p99", 45.0},
      {"latency_min", 5.0},
      {"latency_max", 60.0},
      {"latency_std", 6.0},
      {"link_failure_events", 0.0},
      {"link_recovery_events", 0.0},
      {"reroute_count", 0.0},
  };
}

// Scripted model: each Infer() returns the next scripted response (the last
// one repeats). A hook can run during inference (to change the world).
class FakeModel final : public model::IModelInference {
 public:
  struct Step {
    model::InferenceStatus status{model::InferenceStatus::Ok};
    std::string label{"Dijkstra"};
    double gate{0.2};
    std::vector<std::pair<std::string, double>> probabilities;
    std::string error_code;
  };

  FakeModel() { steps_.push_back(Step{}); }
  void Script(std::vector<Step> steps) {
    std::lock_guard<std::mutex> lock(mutex_);
    steps_.assign(steps.begin(), steps.end());
  }
  void Always(Step step) { Script({step}); }
  static Step Label(const std::string& label) {
    Step s;
    s.label = label;
    if (label != "Dijkstra") {
      s.gate = 0.7;
      s.probabilities = {{"ACO", 0.1}, {"GWO", 0.1}, {"PSO", 0.1}, {"WOA", 0.1}};
      for (auto& p : s.probabilities) {
        if (p.first == label) p.second = 0.7;
      }
    }
    return s;
  }
  static Step Failure(model::InferenceStatus status, const std::string& code = "") {
    Step s;
    s.status = status;
    s.error_code = code;
    return s;
  }

  model::InferenceResponse Infer(const model::InferenceRequest& request) override {
    ++calls;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      last_request = request;
    }
    if (during_inference) during_inference(request);
    Step s;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      s = steps_.front();
      if (steps_.size() > 1) steps_.pop_front();
    }
    model::InferenceResponse r;
    r.request_id = request.request_id;
    r.status = s.status;
    r.error_code = s.error_code;
    r.message = s.status == model::InferenceStatus::Ok ? "" : std::string("scripted ") + model::ToString(s.status);
    if (s.status == model::InferenceStatus::Ok) {
      r.model_version = ExpectedModelVersion();
      r.feature_contract_version = model::kFeatureContractVersion;
      r.model_label = s.label;
      r.gate_probability = s.gate;
      r.gate_threshold = 0.51;
      r.stage2_ran = s.gate >= 0.51;
      r.label_source = r.stage2_ran ? "stage2_argmax" : "stage1_below_threshold";
      r.stage2_probabilities = s.probabilities;
      r.inference_ms = 1.0;
      r.round_trip_ms = 2.0;
    }
    return r;
  }
  model::WorkerStatus Status() const override {
    model::WorkerStatus s;
    s.state = model::WorkerState::Ready;
    s.model_version = ExpectedModelVersion();
    return s;
  }
  std::string ExpectedModelVersion() const override { return "fake-model-1"; }
  std::string ModelName() const override { return "fake_model"; }

  std::atomic<int> calls{0};
  model::InferenceRequest last_request;
  std::function<void(const model::InferenceRequest&)> during_inference;

 private:
  std::mutex mutex_;
  std::deque<Step> steps_;
};

// ---- Real worker configuration (compile definitions from CMake) ----

inline std::string Define(const char* value) { return value ? std::string(value) : std::string(); }

inline model::WorkerConfig RealWorkerConfig() {
  model::WorkerConfig c;
#ifdef AAFR_TEST_MODEL_PYTHON
  c.python_executable = AAFR_TEST_MODEL_PYTHON;
#endif
#ifdef AAFR_TEST_MODEL_WORKER
  c.worker_script = AAFR_TEST_MODEL_WORKER;
#endif
#ifdef AAFR_TEST_MODEL_DIR
  c.model_dir = AAFR_TEST_MODEL_DIR;
#endif
#ifdef AAFR_TEST_MODEL_MANIFEST
  c.manifest_path = AAFR_TEST_MODEL_MANIFEST;
#endif
  return c;
}

inline std::string FakeWorkerScript() {
#ifdef AAFR_TEST_FAKE_WORKER
  return AAFR_TEST_FAKE_WORKER;
#else
  return "";
#endif
}

}  // namespace aafr::test
