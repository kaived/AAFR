#pragma once

// Phase 5B.2: hosting the non-actuating research runtime adapter inside an
// application process (the browser), and a read-only observability contract
// for displaying it. Plain C++17, no Qt.
//
// ResearchHost owns one ResearchRuntimeAdapter and one worker thread:
//   - Start() starts the worker, which loads the model and statistics off the
//     caller's thread (the browser's GUI thread is never blocked by it);
//   - RequestEvaluation() queues one controlled evaluation (coalesced: a
//     request while one is pending is counted and dropped). Nothing evaluates
//     on its own: there is no timer and no per-request hook;
//   - the worker builds the telemetry snapshot with the caller's
//     SnapshotSource (in the browser: the runtime's forwarding graph and
//     Status(), labelled controlled) and calls Evaluate() without probe
//     observations, because no probe tournament source exists yet;
//   - Stop() (also run by the destructor) finishes the running evaluation,
//     joins the worker and releases the adapter and the model.
// A missing XGBoost library or model leaves the host running with the
// adapter Unavailable; nothing throws.
//
// IResearchObservabilityProvider gives a copy of everything a viewer needs:
// host and adapter state, model identity, counters, the latest records and
// the request log. It has no method that evaluates or changes anything.

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "research/record_store.h"
#include "research/runtime_adapter.h"

namespace aafr::research {

enum class ResearchHostState {
  NotStarted,
  Loading,  // The worker is loading the model and statistics.
  Running,  // The adapter exists (it may itself be Unavailable).
  Stopped,
};

// Why a queued request did not reach the adapter, or that it did.
struct ResearchRequestLog {
  std::uint64_t id{0};
  std::string requested_by;
  std::string outcome;  // "evaluated", "skipped: ...", "coalesced", "refused: ...".
};

struct ResearchObservabilitySnapshot {
  ResearchHostState host_state{ResearchHostState::NotStarted};
  ResearchRuntimeStatus adapter;               // Valid when host_state is Running or Stopped.
  bool adapter_present{false};
  std::vector<ResearchRecommendation> recent;  // Newest last.
  std::vector<ResearchRequestLog> requests;    // Newest last.
  std::uint64_t requests_received{0};
  std::uint64_t requests_coalesced{0};
  std::uint64_t requests_skipped{0};
  bool evaluation_pending{false};
  // Phase 5B.3: the durable record store (absent when none is configured);
  // stored records span earlier sessions, newest last.
  std::optional<RecordStoreStatus> store;
  std::vector<StoredResearchRecord> stored;
  static constexpr bool kActuationEnabled = false;  // Decision only.
  // No research input is measured live telemetry in this build: origins are
  // controlled, recorded or synthetic only.
  static constexpr bool kMeasuredTelemetryAvailable = false;
};

class IResearchObservabilityProvider {
 public:
  virtual ~IResearchObservabilityProvider() = default;
  virtual ResearchObservabilitySnapshot ResearchSnapshot(std::size_t max_records) const = 0;
};

class ResearchHost final : public IResearchObservabilityProvider {
 public:
  // A snapshot for one evaluation, or nullopt with `why` when none can be
  // built now (e.g. the runtime has no active configuration yet).
  using SnapshotSource = std::function<std::optional<TelemetrySnapshot>(std::string* why)>;

  ResearchHost(ResearchRuntimeAdapter::Options options, SnapshotSource source);
  ~ResearchHost() override;
  ResearchHost(const ResearchHost&) = delete;
  ResearchHost& operator=(const ResearchHost&) = delete;

  // Phase 5B.3: keep every recommendation in a bounded record file (loaded
  // by the worker at start). Call before Start(); an empty path disables it.
  void SetRecordStore(const std::string& path, std::size_t capacity, const std::string& session_id);

  void Start();
  void Stop();
  void RequestEvaluation(const std::string& requested_by);
  // Blocks until no request is pending or running (tests; never on a GUI thread).
  void WaitIdle();

  ResearchObservabilitySnapshot ResearchSnapshot(std::size_t max_records) const override;

 private:
  void Run();
  void Log(std::uint64_t id, const std::string& by, std::string outcome);

  ResearchRuntimeAdapter::Options options_;
  SnapshotSource source_;
  mutable std::mutex mutex_;
  std::condition_variable wake_;
  std::condition_variable idle_;
  std::thread worker_;
  ResearchHostState state_{ResearchHostState::NotStarted};
  bool stop_{false};
  bool busy_{false};
  std::optional<std::pair<std::uint64_t, std::string>> pending_;
  std::unique_ptr<ResearchRuntimeAdapter> adapter_;  // Created and destroyed by the worker.
  std::unique_ptr<ResearchRecordStore> store_;       // Thread-safe itself.
  std::deque<ResearchRequestLog> log_;
  std::uint64_t next_id_{0};
  std::uint64_t received_{0}, coalesced_{0}, skipped_{0};
};

const char* ToString(ResearchHostState state);

}  // namespace aafr::research
