#include "research/research_host.h"

#include <algorithm>

namespace aafr::research {
namespace {
constexpr std::size_t kRequestLogSize = 32;
// Lock wait for the shutdown flush: short, since browser shutdown waits on it.
constexpr int kShutdownFlushLockMs = 2000;
}

ResearchHost::ResearchHost(ResearchRuntimeAdapter::Options options, SnapshotSource source)
    : options_(std::move(options)), source_(std::move(source)) {}

ResearchHost::~ResearchHost() {
  Stop();
  adapter_.reset();
}

void ResearchHost::SetRecordStore(const std::string& path, std::size_t capacity, const std::string& session_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (state_ != ResearchHostState::NotStarted) return;  // Fixed once the worker runs.
  store_ = path.empty() ? nullptr : std::make_unique<ResearchRecordStore>(path, capacity, session_id);
  store_status_ = store_ ? std::optional<RecordStoreStatus>(store_->Status()) : std::nullopt;
}

void ResearchHost::Start() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (state_ != ResearchHostState::NotStarted) return;
  state_ = ResearchHostState::Loading;
  worker_ = std::thread([this] { Run(); });
}

void ResearchHost::Stop() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ == ResearchHostState::NotStarted) {
      state_ = ResearchHostState::Stopped;
      return;
    }
    stop_ = true;
  }
  wake_.notify_all();
  if (worker_.joinable()) worker_.join();
  std::lock_guard<std::mutex> lock(mutex_);
  state_ = ResearchHostState::Stopped;
  if (pending_) {
    Log(pending_->first, pending_->second, "dropped: host stopped");
    pending_.reset();
  }
  idle_.notify_all();
}

void ResearchHost::Log(std::uint64_t id, const std::string& by, std::string outcome) {
  log_.push_back(ResearchRequestLog{id, by, std::move(outcome)});
  while (log_.size() > kRequestLogSize) log_.pop_front();
}

void ResearchHost::RequestEvaluation(const std::string& requested_by) {
  std::lock_guard<std::mutex> lock(mutex_);
  ++received_;
  const std::uint64_t id = ++next_id_;
  if (state_ == ResearchHostState::NotStarted || state_ == ResearchHostState::Stopped || stop_) {
    Log(id, requested_by, "refused: research host not running");
    return;
  }
  if (pending_) {
    ++coalesced_;
    Log(id, requested_by, "coalesced with pending request #" + std::to_string(pending_->first));
    return;
  }
  pending_ = std::make_pair(id, requested_by);
  wake_.notify_all();
}

void ResearchHost::WaitIdle() {
  std::unique_lock<std::mutex> lock(mutex_);
  idle_.wait(lock, [this] {
    return state_ == ResearchHostState::NotStarted || state_ == ResearchHostState::Stopped ||
           (state_ == ResearchHostState::Running && !pending_ && !busy_);
  });
}

void ResearchHost::Run() {
  // Load off the caller's thread; the adapter reports Unavailable itself
  // when the library or the artifacts are missing.
  auto adapter = std::make_unique<ResearchRuntimeAdapter>(options_);
  if (store_) store_->Load();  // Records of earlier sessions, read off the caller's thread.
  {
    std::lock_guard<std::mutex> lock(mutex_);
    adapter_ = std::move(adapter);
  }
  RefreshPublishedSnapshot();
  {
    std::lock_guard<std::mutex> lock(mutex_);
    state_ = ResearchHostState::Running;
  }
  idle_.notify_all();
  while (true) {
    std::pair<std::uint64_t, std::string> request;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      wake_.wait(lock, [this] { return stop_ || pending_.has_value(); });
      if (stop_) break;
      request = *pending_;
      pending_.reset();
      busy_ = true;
    }
    std::string why;
    std::optional<TelemetrySnapshot> snapshot;
    if (source_) snapshot = source_(&why);
    else why = "no telemetry source";
    std::string outcome;
    if (!snapshot) {
      outcome = "skipped: " + (why.empty() ? std::string("no telemetry snapshot") : why);
    } else {
      // No probe tournament source exists yet: observations are absent.
      const ResearchEvaluation e = adapter_->Evaluate(*snapshot, std::nullopt);
      if (e.produced) {
        outcome = std::string("evaluated: recommendation #") + std::to_string(e.recommendation->sequence) + " " +
                  ToString(e.recommendation->verdict) + " (not applied)";
        if (store_ && !store_->Append(*e.recommendation)) outcome += "; not stored: " + store_->Status().last_error;
      } else {
        outcome = "refused: " + (e.errors.empty() ? std::string(ToString(e.state)) : e.errors.front());
      }
    }
    RefreshPublishedSnapshot([&] {
      if (!snapshot) ++skipped_;
      Log(request.first, request.second, std::move(outcome));
      busy_ = false;
    });
    idle_.notify_all();
  }
  // Shutdown: write records a busy or failed write left in memory, waiting
  // only briefly for another instance's lock (Stop() joins this thread).
  if (store_) store_->Flush(kShutdownFlushLockMs);
  RefreshPublishedSnapshot();
}

void ResearchHost::RefreshPublishedSnapshot(const std::function<void()>& on_publish) {
  auto status = store_ ? std::optional<RecordStoreStatus>(store_->Status()) : std::nullopt;
  auto records = store_ ? store_->Records() : std::vector<StoredResearchRecord>{};
  auto adapter = adapter_ ? std::optional<ResearchRuntimeStatus>(adapter_->Status()) : std::nullopt;
  auto recent = adapter_ ? adapter_->History(options_.history) : std::vector<ResearchRecommendation>{};
  std::lock_guard<std::mutex> lock(mutex_);
  store_status_ = std::move(status);
  stored_records_ = std::move(records);
  published_adapter_ = std::move(adapter);
  published_recent_ = std::move(recent);
  if (on_publish) on_publish();
}

ResearchObservabilitySnapshot ResearchHost::ResearchSnapshot(std::size_t max_records) const {
  ResearchObservabilitySnapshot s;
  std::lock_guard<std::mutex> lock(mutex_);
  s.host_state = state_;
  s.requests.assign(log_.begin(), log_.end());
  s.requests_received = received_;
  s.requests_coalesced = coalesced_;
  s.requests_skipped = skipped_;
  s.evaluation_pending = pending_.has_value() || busy_;
  if (published_adapter_) {
    s.adapter_present = true;
    s.adapter = *published_adapter_;
    const std::size_t n = std::min(max_records, published_recent_.size());
    s.recent.assign(published_recent_.end() - static_cast<long>(n), published_recent_.end());
  }
  if (store_status_) {
    s.store = store_status_;
    const std::size_t n = std::min(max_records, stored_records_.size());
    s.stored.assign(stored_records_.end() - static_cast<long>(n), stored_records_.end());
  }
  return s;
}

const char* ToString(ResearchHostState state) {
  switch (state) {
    case ResearchHostState::NotStarted: return "not started";
    case ResearchHostState::Loading: return "loading";
    case ResearchHostState::Running: return "running";
    case ResearchHostState::Stopped: return "stopped";
  }
  return "unknown";
}

}  // namespace aafr::research
