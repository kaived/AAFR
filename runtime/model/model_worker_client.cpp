#include "runtime/model/model_worker_client.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <set>
#include <sstream>

namespace aafr::model {

namespace {
using Clock = std::chrono::steady_clock;

double MsSince(Clock::time_point t) { return std::chrono::duration<double, std::milli>(Clock::now() - t).count(); }

std::string Fixed(double v, int precision = 1) {
  std::ostringstream o;
  o.setf(std::ios::fixed);
  o.precision(precision);
  o << v;
  return o.str();
}

bool IsFile(const std::string& path) {
  std::error_code ec;
  return !path.empty() && std::filesystem::is_regular_file(std::filesystem::u8path(path), ec);
}

bool IsDirectory(const std::string& path) {
  std::error_code ec;
  return !path.empty() && std::filesystem::is_directory(std::filesystem::u8path(path), ec);
}

std::optional<std::string> Env(const char* name) {
#ifdef _WIN32
  char* value = nullptr;
  std::size_t size = 0;
  if (_dupenv_s(&value, &size, name) != 0 || !value) return std::nullopt;
  std::string out(value);
  std::free(value);
  return out.empty() ? std::nullopt : std::optional<std::string>(out);
#else
  const char* value = std::getenv(name);
  if (!value || !*value) return std::nullopt;
  return std::string(value);
#endif
}

std::vector<std::string> Strings(const JsonValue* v) {
  std::vector<std::string> out;
  if (!v || !v->IsArray()) return out;
  for (const JsonValue& e : v->AsArray()) {
    if (e.IsString()) out.push_back(e.AsString());
  }
  return out;
}
}  // namespace

const char* ToString(WorkerState value) {
  switch (value) {
    case WorkerState::NotConfigured: return "not_configured";
    case WorkerState::Stopped: return "stopped";
    case WorkerState::Starting: return "starting";
    case WorkerState::Ready: return "ready";
    case WorkerState::Restarting: return "restarting";
    case WorkerState::Failed: return "failed";
  }
  return "unknown";
}

const char* ToString(InferenceStatus value) {
  switch (value) {
    case InferenceStatus::Ok: return "ok";
    case InferenceStatus::WorkerUnavailable: return "worker_unavailable";
    case InferenceStatus::Busy: return "busy";
    case InferenceStatus::Timeout: return "timeout";
    case InferenceStatus::WorkerError: return "worker_error";
    case InferenceStatus::InvalidResponse: return "invalid_response";
    case InferenceStatus::VersionMismatch: return "version_mismatch";
    case InferenceStatus::Stale: return "stale";
    case InferenceStatus::ExcessiveLatency: return "excessive_latency";
  }
  return "unknown";
}

std::vector<std::string> WorkerConfig::Validate() const {
  std::vector<std::string> issues;
  if (!IsFile(python_executable)) issues.push_back("python interpreter not found: '" + python_executable + "'");
  if (!IsFile(worker_script)) issues.push_back("worker script not found: '" + worker_script + "'");
  if (!IsDirectory(model_dir)) issues.push_back("model directory not found: '" + model_dir + "'");
  if (!IsFile(manifest_path)) issues.push_back("model manifest not found: '" + manifest_path + "'");
  if (!(request_timeout_ms > 0.0 && request_timeout_ms <= 60'000.0)) issues.push_back("request_timeout_ms must be within (0, 60000]");
  if (!(startup_timeout_ms > 0.0)) issues.push_back("startup_timeout_ms must be positive");
  if (max_message_bytes < 1024 || max_message_bytes > 4 * 1024 * 1024) issues.push_back("max_message_bytes must be within 1 KiB..4 MiB");
  if (max_restarts < 0) issues.push_back("max_restarts must be nonnegative");
  return issues;
}

struct ModelWorkerClient::Generation {
  std::uint64_t id{0};
  std::unique_ptr<ChildProcess> process;
  std::thread reader;
  std::thread err_reader;
  std::deque<std::string> stderr_tail;  // Guarded by the client mutex.
};

ModelWorkerClient::ModelWorkerClient(WorkerConfig config, ModelEventLog* events)
    : config_(std::move(config)), events_(events) {
  const auto issues = config_.Validate();
  if (!issues.empty()) {
    status_.state = WorkerState::NotConfigured;
    std::string text;
    for (const auto& i : issues) text += (text.empty() ? "" : "; ") + i;
    status_.detail = text;
    return;
  }
  ManifestLoadResult loaded = LoadManifest(config_.manifest_path);
  if (!loaded.manifest) {
    status_.state = WorkerState::Failed;
    status_.detail = loaded.errors.empty() ? "manifest unreadable" : loaded.errors.front();
    manifest_error_ = status_.detail;
    return;
  }
  const auto contract = ValidateManifestAgainstContract(*loaded.manifest);
  if (!contract.empty()) {
    status_.state = WorkerState::Failed;
    status_.detail = "feature contract mismatch: " + contract.front();
    manifest_error_ = status_.detail;
    return;
  }
  manifest_ = loaded.manifest;
  status_.state = WorkerState::Stopped;
  status_.detail = "not started";
  status_.model_name = manifest_->model_name;
  status_.model_version = manifest_->model_version;
  status_.feature_contract_version = manifest_->feature_contract_version;
}

ModelWorkerClient::~ModelWorkerClient() { Stop(); }

std::string ModelWorkerClient::ExpectedModelVersion() const { return manifest_ ? manifest_->model_version : ""; }
std::string ModelWorkerClient::ModelName() const { return manifest_ ? manifest_->model_name : ""; }

bool ModelWorkerClient::Start() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!manifest_ || status_.state == WorkerState::NotConfigured) return false;
    if (status_.state == WorkerState::Starting || status_.state == WorkerState::Ready) return true;
    if (status_.state == WorkerState::Failed && !manifest_error_.empty()) return false;
  }
  Spawn("start", false);
  std::lock_guard<std::mutex> lock(mutex_);
  return status_.state == WorkerState::Starting || status_.state == WorkerState::Ready;
}

void ModelWorkerClient::RetireGeneration() {
  std::shared_ptr<Generation> old;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    old = std::move(current_);
    current_.reset();
  }
  if (!old) return;
  if (old->process) old->process->Kill();
  if (old->reader.joinable()) old->reader.join();
  if (old->err_reader.joinable()) old->err_reader.join();
  if (old->process) old->process->Close();
}

void ModelWorkerClient::Spawn(const std::string& reason, bool is_restart) {
  RetireGeneration();
  auto gen = std::make_shared<Generation>();
  gen->process = std::make_unique<ChildProcess>();
  ProcessSpec spec;
  spec.executable = config_.python_executable;
  // -I: isolated mode (no PYTHON* variables, no user site, no script dir on
  // sys.path); -X utf8: UTF-8 I/O regardless of the console code page.
  spec.args = {"-I", "-X", "utf8", config_.worker_script, "--model-dir", config_.model_dir, "--manifest",
               config_.manifest_path, "--max-line-bytes", std::to_string(config_.max_message_bytes)};
  spec.memory_limit_bytes = config_.memory_limit_bytes;
  std::string error;
  const bool started = gen->process->Start(spec, &error);
  std::unique_lock<std::mutex> lock(mutex_);
  if (!started) {
    Fail("worker could not be started: " + error);
    lock.unlock();
    if (events_) events_->Emit(ModelEventType::ModelWorkerFailed, "spawn failed: " + error);
    return;
  }
  gen->id = ++status_.generation;
  status_.state = WorkerState::Starting;
  status_.detail = "loading model (" + reason + ")";
  status_.pid = gen->process->Pid();
  if (is_restart) ++status_.restarts;
  spawned_at_ = Clock::now();
  current_ = gen;
  gen->reader = std::thread([this, gen] { ReaderLoop(gen); });
  gen->err_reader = std::thread([this, gen] { StderrLoop(gen); });
  const std::string detail = "pid " + std::to_string(status_.pid) + " (" + reason + ")";
  lock.unlock();
  if (events_) {
    events_->Emit(is_restart ? ModelEventType::ModelWorkerRestarted : ModelEventType::ModelWorkerStarted, detail);
  }
}

void ModelWorkerClient::ReaderLoop(std::shared_ptr<Generation> gen) {
  std::string buffer;
  char chunk[8192];
  while (true) {
    const std::size_t n = gen->process->ReadStdout(chunk, sizeof(chunk));
    if (n == 0) break;
    buffer.append(chunk, n);
    bool overflow = false;
    std::size_t start = 0;
    while (true) {
      const std::size_t nl = buffer.find('\n', start);
      if (nl == std::string::npos) break;
      std::string line = buffer.substr(start, nl - start);
      if (!line.empty() && line.back() == '\r') line.pop_back();
      start = nl + 1;
      if (line.size() > config_.max_message_bytes) {
        overflow = true;
        break;
      }
      if (!line.empty()) HandleLine(gen, line);
    }
    buffer.erase(0, std::min(start, buffer.size()));
    if (overflow || buffer.size() > config_.max_message_bytes) {
      {
        std::lock_guard<std::mutex> lock(mutex_);
        if (gen == current_) {
          status_.last_error = "worker output line exceeded " + std::to_string(config_.max_message_bytes) + " bytes";
          if (!awaited_id_.empty()) {
            response_error_ = status_.last_error;
            cv_.notify_all();
          }
        }
      }
      gen->process->Kill();  // A corrupted protocol stream cannot be resynchronised safely.
      break;
    }
  }
  HandleExit(gen);
}

void ModelWorkerClient::StderrLoop(std::shared_ptr<Generation> gen) {
  std::string buffer;
  char chunk[4096];
  while (true) {
    const std::size_t n = gen->process->ReadStderr(chunk, sizeof(chunk));
    if (n == 0) break;
    buffer.append(chunk, n);
    std::size_t nl;
    while ((nl = buffer.find('\n')) != std::string::npos) {
      std::string line = buffer.substr(0, std::min<std::size_t>(nl, 500));
      if (!line.empty() && line.back() == '\r') line.pop_back();
      buffer.erase(0, nl + 1);
      std::lock_guard<std::mutex> lock(mutex_);
      gen->stderr_tail.push_back(line);
      while (gen->stderr_tail.size() > 40) gen->stderr_tail.pop_front();
    }
    if (buffer.size() > 4096) buffer.erase(0, buffer.size() - 4096);
  }
}

void ModelWorkerClient::HandleLine(const std::shared_ptr<Generation>& gen, const std::string& line) {
  const JsonParseResult parsed = ParseJson(line);
  std::vector<std::pair<ModelEventType, std::string>> emit;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (gen != current_) return;
    if (!parsed.value || !parsed.value->IsObject()) {
      status_.last_error = "malformed worker output: " + (parsed.error.empty() ? std::string("not an object") : parsed.error);
      if (!awaited_id_.empty()) {
        response_error_ = status_.last_error;
        cv_.notify_all();
      }
      return;
    }
    const JsonValue& msg = *parsed.value;
    const std::string type = msg.GetString("type").value_or("");
    if (type == "hello") {
      status_.python_version = msg.GetString("python_version").value_or("");
      status_.worker_version = msg.GetString("worker_version").value_or("");
      if (msg.GetNumber("protocol_version").value_or(-1) != kProtocolVersion) {
        Fail("worker speaks protocol " + Fixed(msg.GetNumber("protocol_version").value_or(-1), 0));
        emit.emplace_back(ModelEventType::ModelWorkerFailed, status_.detail);
        gen->process->Kill();
      }
    } else if (type == "ready") {
      std::vector<std::string> problems;
      if (msg.GetNumber("protocol_version").value_or(-1) != kProtocolVersion) problems.push_back("protocol version");
      if (msg.GetString("model_version").value_or("") != manifest_->model_version) {
        problems.push_back("model_version '" + msg.GetString("model_version").value_or("") + "' != manifest '" +
                           manifest_->model_version + "'");
      }
      if (msg.GetString("feature_contract_version").value_or("") != kFeatureContractVersion) {
        problems.push_back("feature contract '" + msg.GetString("feature_contract_version").value_or("") + "'");
      }
      const auto raw = Strings(msg.Find("raw_features"));
      const auto& contract = RawFeatureContract();
      bool same = raw.size() == contract.size();
      for (std::size_t i = 0; same && i < raw.size(); ++i) same = raw[i] == contract[i].name;
      if (!same) problems.push_back("worker raw feature list differs from " + std::string(kFeatureContractVersion));
      const auto labels = Strings(msg.Find("labels"));
      for (const auto& l : labels) {
        if (!MapModelLabel(l)) problems.push_back("worker label '" + l + "' has no AAFR mapping");
      }
      const auto threshold = msg.GetNumber("gate_threshold");
      if (!threshold || !manifest_->gate_threshold || std::fabs(*threshold - *manifest_->gate_threshold) > 1e-12) {
        problems.push_back("gate threshold differs from the manifest");
      }
      status_.load_ms = msg.GetNumber("load_ms");
      status_.verify_ms = msg.GetNumber("verify_ms");
      status_.dependency_import_ms = msg.GetNumber("dependency_import_ms");
      status_.labels = labels;
      status_.gate_threshold = threshold;
      status_.load_warnings = Strings(msg.Find("load_warnings"));
      status_.library_versions.clear();
      if (const JsonValue* lv = msg.Find("library_versions"); lv && lv->IsObject()) {
        for (const auto& e : lv->AsObject()) {
          if (e.second.IsString()) status_.library_versions[e.first] = e.second.AsString();
        }
      }
      emit.emplace_back(ModelEventType::ModelLoaded,
                        manifest_->model_version + " loaded in " + Fixed(status_.load_ms.value_or(0.0)) +
                            " ms (artifacts verified in " + Fixed(status_.verify_ms.value_or(0.0)) + " ms)");
      if (!problems.empty()) {
        std::string text;
        for (const auto& p : problems) text += (text.empty() ? "" : "; ") + p;
        Fail("worker does not match the AAFR contract: " + text);
        manifest_error_ = status_.detail;  // Not retried automatically.
        emit.emplace_back(ModelEventType::ModelWorkerFailed, status_.detail);
        gen->process->Kill();
      } else {
        status_.state = WorkerState::Ready;
        status_.startup_ms = MsSince(spawned_at_);
        if (restart_requested_at_) {
          status_.last_restart_ms = MsSince(*restart_requested_at_);
          restart_requested_at_.reset();
        }
        status_.detail = "ready";
        consecutive_timeouts_ = 0;
        emit.emplace_back(ModelEventType::ModelReady,
                          manifest_->model_version + " ready after " + Fixed(*status_.startup_ms) + " ms");
        cv_.notify_all();
      }
    } else if (type == "fatal") {
      const std::string code = msg.GetString("error_code").value_or("E_UNKNOWN");
      Fail("worker startup failed (" + code + "): " + msg.GetString("message").value_or(""));
      manifest_error_ = status_.detail;  // Artifact/dependency problems are not retried automatically.
      emit.emplace_back(ModelEventType::ModelWorkerFailed, status_.detail);
    } else if (type == "prediction" || type == "pong" || type == "bye") {
      const JsonValue* rid = msg.Find("request_id");
      const std::string id = rid && rid->IsString() ? rid->AsString() : "";
      if (!awaited_id_.empty() && id == awaited_id_) {
        response_ = msg;
        cv_.notify_all();
      } else if (!awaited_id_.empty() && id.empty() && msg.GetString("status").value_or("") == "error") {
        // The worker could not read our request well enough to echo its id.
        response_ = msg;
        cv_.notify_all();
      } else if (type != "bye") {
        ++status_.stale_discarded;
        emit.emplace_back(ModelEventType::StaleInferenceDiscarded,
                          "response for " + (id.empty() ? std::string("<no id>") : id) +
                              (awaited_id_.empty() ? " arrived after its deadline" : " while awaiting " + awaited_id_));
      }
    } else {
      status_.last_error = "unexpected worker message type '" + type + "'";
    }
  }
  if (events_) {
    for (const auto& e : emit) events_->Emit(e.first, e.second);
  }
}

void ModelWorkerClient::HandleExit(const std::shared_ptr<Generation>& gen) {
  const std::optional<int> code = gen->process->Wait(3000);
  std::string event_detail;
  ModelEventType event_type = ModelEventType::ModelWorkerFailed;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (gen != current_) return;
    const std::string exit_text = code ? "exit code " + std::to_string(*code) : std::string("exit code unknown");
    if (!awaited_id_.empty()) {
      response_error_ = "worker exited (" + exit_text + ")";
      cv_.notify_all();
    }
    switch (status_.state) {
      case WorkerState::Starting: {
        std::string tail;
        for (auto it = gen->stderr_tail.rbegin(); it != gen->stderr_tail.rend() && tail.size() < 400; ++it) {
          if (!it->empty()) {
            tail = *it;
            break;
          }
        }
        Fail("worker exited during startup (" + exit_text + ")" + (tail.empty() ? "" : ": " + tail));
        event_detail = status_.detail;
        break;
      }
      case WorkerState::Ready:
        ++status_.failures;
        ScheduleRestart("worker exited unexpectedly (" + exit_text + ")");
        event_detail = status_.detail;
        break;
      default:
        break;  // Stopped, Restarting or Failed: the exit was expected.
    }
    status_.pid = 0;
  }
  if (events_ && !event_detail.empty()) events_->Emit(event_type, event_detail);
}

void ModelWorkerClient::Fail(const std::string& reason) {
  status_.state = WorkerState::Failed;
  status_.detail = reason;
  status_.last_error = reason;
  cv_.notify_all();
}

void ModelWorkerClient::ScheduleRestart(const std::string& reason) {
  if (!config_.auto_restart) {
    Fail(reason + "; automatic restart disabled");
    return;
  }
  const auto now = Clock::now();
  const auto window = std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double, std::milli>(config_.restart_window_ms));
  while (!restart_times_.empty() && now - restart_times_.front() > window) restart_times_.pop_front();
  if (static_cast<int>(restart_times_.size()) >= config_.max_restarts) {
    Fail(reason + "; restart budget exhausted (" + std::to_string(config_.max_restarts) + " in " +
         Fixed(config_.restart_window_ms / 1000.0, 0) + " s)");
    return;
  }
  status_.state = WorkerState::Restarting;
  status_.detail = reason + "; restart in " + Fixed(config_.restart_backoff_ms, 0) + " ms";
  status_.last_error = reason;
  restart_due_ = now + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double, std::milli>(config_.restart_backoff_ms));
  restart_requested_at_ = now;
  cv_.notify_all();
}

void ModelWorkerClient::MaintainWorker() {
  std::string reason;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (status_.state == WorkerState::Starting && MsSince(spawned_at_) > config_.startup_timeout_ms) {
      Fail("worker did not become ready within " + Fixed(config_.startup_timeout_ms, 0) + " ms");
      if (current_ && current_->process) current_->process->Kill();
      return;
    }
    if (status_.state != WorkerState::Restarting || Clock::now() < restart_due_) return;
    restart_times_.push_back(Clock::now());
    reason = "automatic restart: " + status_.last_error;
  }
  Spawn(reason, true);
}

bool ModelWorkerClient::WaitUntilReady(double timeout_ms) {
  std::unique_lock<std::mutex> lock(mutex_);
  const auto deadline = Clock::now() + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double, std::milli>(timeout_ms));
  cv_.wait_until(lock, deadline, [&] {
    return status_.state == WorkerState::Ready || status_.state == WorkerState::Failed ||
           status_.state == WorkerState::NotConfigured || status_.state == WorkerState::Stopped;
  });
  if (status_.state == WorkerState::Starting && Clock::now() >= deadline) return false;
  return status_.state == WorkerState::Ready;
}

void ModelWorkerClient::Stop() {
  std::shared_ptr<Generation> gen;
  bool had_worker = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    gen = current_;
    had_worker = gen != nullptr;
    if (status_.state != WorkerState::NotConfigured && (status_.state != WorkerState::Failed || had_worker)) {
      if (status_.state != WorkerState::Failed) {
        status_.state = WorkerState::Stopped;
        status_.detail = "stopped";
      }
    }
    cv_.notify_all();
  }
  if (gen && gen->process) {
    gen->process->Write("{\"type\":\"shutdown\"}\n");
    gen->process->CloseStdin();
    gen->process->Wait(2000);
  }
  RetireGeneration();
  if (had_worker && events_) events_->Emit(ModelEventType::ModelWorkerStopped, "worker stopped");
}

bool ModelWorkerClient::Restart(const std::string& reason) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!manifest_) return false;
    restart_times_.clear();
    manifest_error_.clear();
    restart_requested_at_ = Clock::now();
  }
  Spawn("manual restart: " + reason, true);
  std::lock_guard<std::mutex> lock(mutex_);
  return status_.state == WorkerState::Starting || status_.state == WorkerState::Ready;
}

void ModelWorkerClient::KillWorkerForTesting() {
  std::shared_ptr<Generation> gen;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    gen = current_;
  }
  if (gen && gen->process) gen->process->Kill();
}

bool ModelWorkerClient::Ping(double timeout_ms) {
  std::unique_lock<std::timed_mutex> busy(infer_mutex_, std::defer_lock);
  if (!busy.try_lock_for(std::chrono::duration<double, std::milli>(timeout_ms))) return false;
  std::shared_ptr<Generation> gen;
  const std::string id = "ping-" + std::to_string(++sequence_);
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (status_.state != WorkerState::Ready || !current_) return false;
    gen = current_;
    awaited_id_ = id;
    response_.reset();
    response_error_.clear();
  }
  if (!gen->process->Write("{\"type\":\"ping\",\"request_id\":" + JsonQuote(id) + "}\n")) return false;
  std::unique_lock<std::mutex> lock(mutex_);
  const bool got = cv_.wait_for(lock, std::chrono::duration<double, std::milli>(timeout_ms),
                                [&] { return response_.has_value() || !response_error_.empty(); });
  awaited_id_.clear();
  return got && response_ && response_->GetString("type").value_or("") == "pong";
}

InferenceResponse ModelWorkerClient::Infer(const InferenceRequest& request) {
  InferenceResponse out;
  out.request_id = request.request_id;
  MaintainWorker();
  const auto started = Clock::now();
  std::unique_lock<std::timed_mutex> busy(infer_mutex_, std::defer_lock);
  if (!busy.try_lock_for(std::chrono::duration<double, std::milli>(config_.request_timeout_ms))) {
    out.status = InferenceStatus::Busy;
    out.message = "another inference request is in flight";
    return out;
  }
  // Serialize and bound the request before touching the worker.
  JsonValue msg = JsonValue::MakeObject();
  msg.Set("type", JsonValue::String("predict"));
  msg.Set("protocol_version", JsonValue::Number(kProtocolVersion));
  msg.Set("request_id", JsonValue::String(request.request_id));
  msg.Set("model_version", JsonValue::String(ExpectedModelVersion()));
  msg.Set("feature_contract_version", JsonValue::String(kFeatureContractVersion));
  msg.Set("evaluation_id", JsonValue::String(request.evaluation_id));
  msg.Set("network_state_id", JsonValue::Number(static_cast<double>(request.network_state_id)));
  msg.Set("graph_version", JsonValue::Number(static_cast<double>(request.graph_version)));
  msg.Set("state_version", JsonValue::String(request.state_version));
  JsonValue features = JsonValue::MakeObject();
  for (const auto& f : request.features) {
    if (!std::isfinite(f.second)) {
      out.status = InferenceStatus::InvalidResponse;
      out.error_code = "E_INVALID_REQUEST";
      out.message = "feature '" + f.first + "' is not finite; request not sent";
      return out;
    }
    features.Set(f.first, JsonValue::Number(f.second));
  }
  msg.Set("features", features);
  const std::string line = msg.Dump() + "\n";
  if (line.size() > config_.max_message_bytes || request.request_id.empty() || request.request_id.size() > 128) {
    out.status = InferenceStatus::InvalidResponse;
    out.error_code = "E_INVALID_REQUEST";
    out.message = "request id or size outside the protocol bounds; request not sent";
    return out;
  }
  std::shared_ptr<Generation> gen;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (status_.state != WorkerState::Ready || !current_) {
      out.status = InferenceStatus::WorkerUnavailable;
      out.message = std::string("worker ") + ToString(status_.state) + (status_.detail.empty() ? "" : ": " + status_.detail);
      return out;
    }
    gen = current_;
    awaited_id_ = request.request_id;
    response_.reset();
    response_error_.clear();
    ++status_.requests;
  }
  if (events_) events_->Emit(ModelEventType::InferenceRequested, "evaluation " + request.evaluation_id, request.request_id);
  if (!gen->process->Write(line)) {
    std::lock_guard<std::mutex> lock(mutex_);
    awaited_id_.clear();
    ++status_.failures;
    out.status = InferenceStatus::WorkerUnavailable;
    out.message = "could not write to the worker";
    status_.last_error = out.message;
  } else {
    std::unique_lock<std::mutex> lock(mutex_);
    const auto deadline =
        started + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double, std::milli>(config_.request_timeout_ms));
    const bool done = cv_.wait_until(lock, deadline, [&] {
      return response_.has_value() || !response_error_.empty() || status_.state != WorkerState::Ready;
    });
    std::optional<JsonValue> response = std::move(response_);
    const std::string error = response_error_;
    awaited_id_.clear();
    response_.reset();
    response_error_.clear();
    if (!done) {
      ++status_.timeouts;
      ++consecutive_timeouts_;
      out.status = InferenceStatus::Timeout;
      out.message = "no response within " + Fixed(config_.request_timeout_ms, 0) + " ms";
      status_.last_error = out.message;
      if (consecutive_timeouts_ >= config_.timeouts_before_restart) {
        consecutive_timeouts_ = 0;
        ScheduleRestart(std::to_string(config_.timeouts_before_restart) + " consecutive inference timeouts");
        if (current_ && current_->process) current_->process->Kill();
      }
    } else if (!response) {
      ++status_.failures;
      out.status = error.find("exited") != std::string::npos || status_.state != WorkerState::Ready
                       ? InferenceStatus::WorkerUnavailable
                       : InferenceStatus::InvalidResponse;
      out.message = error.empty() ? std::string("worker ") + ToString(status_.state) : error;
      status_.last_error = out.message;
    } else {
      consecutive_timeouts_ = 0;
      lock.unlock();
      out = Parse(*response, request);
      lock.lock();
      if (out.status == InferenceStatus::Ok) {
        ++status_.completed;
        status_.last_inference_ms = out.inference_ms;
      } else {
        ++status_.failures;
        status_.last_error = std::string(ToString(out.status)) + ": " + out.message;
        if (out.status == InferenceStatus::Stale) ++status_.stale_discarded;
      }
    }
    out.round_trip_ms = MsSince(started);
    status_.last_round_trip_ms = out.round_trip_ms;
  }
  out.round_trip_ms = MsSince(started);
  if (events_) {
    switch (out.status) {
      case InferenceStatus::Ok:
        events_->Emit(ModelEventType::InferenceCompleted,
                      out.model_label + " (gate " + Fixed(out.gate_probability.value_or(-1), 3) + ", " +
                          Fixed(out.round_trip_ms, 2) + " ms round trip)",
                      request.request_id);
        break;
      case InferenceStatus::Timeout:
        events_->Emit(ModelEventType::InferenceTimedOut, out.message, request.request_id);
        break;
      case InferenceStatus::Stale:
        events_->Emit(ModelEventType::StaleInferenceDiscarded, out.message, request.request_id);
        break;
      default:
        events_->Emit(ModelEventType::InferenceFailed, std::string(ToString(out.status)) + ": " + out.message,
                      request.request_id);
    }
  }
  return out;
}

InferenceResponse ModelWorkerClient::Parse(const JsonValue& m, const InferenceRequest& request) {
  InferenceResponse r;
  r.request_id = request.request_id;
  const auto invalid = [&](const std::string& why) {
    r.status = InferenceStatus::InvalidResponse;
    r.message = why;
    return r;
  };
  if (m.GetString("type").value_or("") != "prediction") return invalid("response type is not 'prediction'");
  if (m.GetNumber("protocol_version").value_or(-1) != kProtocolVersion) return invalid("response protocol_version mismatch");
  const std::string status = m.GetString("status").value_or("");
  if (status == "error") {
    r.error_code = m.GetString("error_code").value_or("E_UNKNOWN");
    r.message = m.GetString("message").value_or("");
    r.status = (r.error_code == "E_MODEL_VERSION_MISMATCH" || r.error_code == "E_FEATURE_CONTRACT_MISMATCH")
                   ? InferenceStatus::VersionMismatch
                   : InferenceStatus::WorkerError;
    return r;
  }
  if (status != "ok") return invalid("unknown response status '" + status + "'");
  if (m.GetString("request_id").value_or("") != request.request_id) return invalid("request_id does not match");
  r.model_version = m.GetString("model_version").value_or("");
  r.feature_contract_version = m.GetString("feature_contract_version").value_or("");
  if (r.model_version != ExpectedModelVersion() || r.feature_contract_version != kFeatureContractVersion) {
    r.status = InferenceStatus::VersionMismatch;
    r.message = "response model '" + r.model_version + "' / contract '" + r.feature_contract_version + "'";
    return r;
  }
  // The evaluation context must be echoed exactly; otherwise the answer
  // belongs to another evaluation.
  const JsonValue* echo = m.Find("echo");
  if (!echo || !echo->IsObject() ||
      echo->GetNumber("network_state_id").value_or(-1) != static_cast<double>(request.network_state_id) ||
      echo->GetNumber("graph_version").value_or(-1) != static_cast<double>(request.graph_version) ||
      echo->GetString("evaluation_id").value_or("") != request.evaluation_id) {
    r.status = InferenceStatus::Stale;
    r.message = "response does not echo the request's evaluation context";
    return r;
  }
  r.model_label = m.GetString("model_label").value_or("");
  r.label_source = m.GetString("label_source").value_or("");
  r.gate_probability = m.GetNumber("gate_probability");
  r.gate_threshold = m.GetNumber("gate_threshold");
  r.stage2_ran = m.GetBool("stage2_ran").value_or(false);
  r.probabilities_calibrated = m.GetBool("probabilities_calibrated").value_or(false);
  r.inference_ms = m.GetNumber("inference_ms");
  r.out_of_training_range = Strings(m.Find("out_of_training_range"));
  r.distribution_warnings = Strings(m.Find("distribution_warnings"));
  if (r.model_label.empty()) return invalid("model_label missing");
  if (!r.gate_probability || *r.gate_probability < 0.0 || *r.gate_probability > 1.0) {
    return invalid("gate_probability missing or outside [0, 1]");
  }
  if (!r.gate_threshold || !manifest_->gate_threshold || std::fabs(*r.gate_threshold - *manifest_->gate_threshold) > 1e-12) {
    return invalid("gate_threshold differs from the manifest");
  }
  if (!m.GetBool("stage2_ran")) return invalid("stage2_ran missing");
  if (!r.inference_ms || *r.inference_ms < 0.0) return invalid("inference_ms missing or negative");
  const JsonValue* probs = m.Find("stage2_probabilities");
  if (r.stage2_ran) {
    if (r.label_source != "stage2_argmax") return invalid("label_source inconsistent with stage2_ran");
    if (*r.gate_probability < *r.gate_threshold) return invalid("stage 2 ran although the gate is below its threshold");
    if (!probs || !probs->IsObject()) return invalid("stage2_probabilities missing");
    double sum = 0.0, best = -1.0;
    std::string best_label;
    std::set<std::string> keys;
    for (const auto& e : probs->AsObject()) {
      if (!e.second.IsNumber() || e.second.AsNumber() < 0.0 || e.second.AsNumber() > 1.0) {
        return invalid("stage 2 probability for '" + e.first + "' outside [0, 1]");
      }
      r.stage2_probabilities.emplace_back(e.first, e.second.AsNumber());
      sum += e.second.AsNumber();
      keys.insert(e.first);
      if (e.second.AsNumber() > best) {
        best = e.second.AsNumber();
        best_label = e.first;
      }
    }
    const std::set<std::string> expected(manifest_->stage2_classes.begin(), manifest_->stage2_classes.end());
    if (keys != expected) return invalid("stage 2 probability classes differ from the manifest");
    if (std::fabs(sum - 1.0) > 1e-3) return invalid("stage 2 probabilities do not sum to 1");
    if (best_label != r.model_label) return invalid("model_label is not the stage 2 argmax");
  } else {
    if (r.label_source != "stage1_below_threshold") return invalid("label_source inconsistent with stage2_ran");
    if (*r.gate_probability >= *r.gate_threshold) return invalid("gate at or above threshold but stage 2 did not run");
    if (probs && !probs->IsNull()) return invalid("stage2_probabilities present although stage 2 did not run");
  }
  if (*r.inference_ms > config_.max_model_inference_ms) {
    r.status = InferenceStatus::ExcessiveLatency;
    r.message = "model inference took " + Fixed(*r.inference_ms, 2) + " ms (budget " +
                Fixed(config_.max_model_inference_ms, 0) + " ms)";
    return r;
  }
  r.status = InferenceStatus::Ok;
  return r;
}

WorkerStatus ModelWorkerClient::Status() const {
  std::lock_guard<std::mutex> lock(mutex_);
  WorkerStatus s = status_;
  if (current_) {
    s.stderr_tail.assign(current_->stderr_tail.begin(), current_->stderr_tail.end());
    if (current_->process) s.resources = current_->process->Resources();
  }
  return s;
}

WorkerConfig WorkerConfigFromEnvironment() {
  WorkerConfig c;
#ifdef AAFR_DEFAULT_MODEL_PYTHON
  c.python_executable = AAFR_DEFAULT_MODEL_PYTHON;
#endif
#ifdef AAFR_DEFAULT_MODEL_WORKER
  c.worker_script = AAFR_DEFAULT_MODEL_WORKER;
#endif
#ifdef AAFR_DEFAULT_MODEL_DIR
  c.model_dir = AAFR_DEFAULT_MODEL_DIR;
#endif
#ifdef AAFR_DEFAULT_MODEL_MANIFEST
  c.manifest_path = AAFR_DEFAULT_MODEL_MANIFEST;
#endif
  if (auto v = Env("AAFR_MODEL_PYTHON")) c.python_executable = *v;
  if (auto v = Env("AAFR_MODEL_WORKER")) c.worker_script = *v;
  if (auto v = Env("AAFR_MODEL_DIR")) c.model_dir = *v;
  if (auto v = Env("AAFR_MODEL_MANIFEST")) c.manifest_path = *v;
  if (auto v = Env("AAFR_MODEL_TIMEOUT_MS")) {
    char* end = nullptr;
    const double ms = std::strtod(v->c_str(), &end);
    if (end && *end == '\0' && ms > 0.0) c.request_timeout_ms = ms;
  }
  return c;
}

}  // namespace aafr::model
