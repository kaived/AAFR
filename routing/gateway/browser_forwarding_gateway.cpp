#include "routing/gateway/browser_forwarding_gateway.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <tlhelp32.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

#include <algorithm>
#include <chrono>
#include <random>
#include <sstream>

#include "network/telemetry/telemetry_pipeline.h"
#include "routing/gateway/proxy_http.h"

namespace aafr::gateway {
namespace {

namespace net = forwarding::net;
using net::After;
using net::Clock;
using net::ReadResult;

std::int64_t SteadyNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
}

double MsSince(std::int64_t start_ns) { return static_cast<double>(SteadyNs() - start_ns) / 1e6; }

std::string Fixed(double value, int digits) {
  std::ostringstream out;
  out.setf(std::ios::fixed);
  out.precision(digits);
  out << value;
  return out.str();
}

telemetry::EventType TelemetryType(GatewayEventType type) {
  using T = telemetry::EventType;
  switch (type) {
    case GatewayEventType::GatewayStarted: return T::GatewayStarted;
    case GatewayEventType::GatewayStopped: return T::GatewayStopped;
    case GatewayEventType::BindingsChanged: return T::BrowserProxyConfigured;
    case GatewayEventType::BrowserConnectionAccepted: return T::BrowserConnectionAccepted;
    case GatewayEventType::ConnectionRejected: return T::BrowserConnectionRejected;
    case GatewayEventType::ConnectionPinned: return T::ConnectionPinned;
    case GatewayEventType::RelayPathEstablished: return T::RelayPathEstablished;
    case GatewayEventType::ForwardingStarted: return T::BrowserForwardingStarted;
    case GatewayEventType::ForwardingCompleted: return T::BrowserForwardingCompleted;
    case GatewayEventType::ForwardingFailed: return T::BrowserForwardingFailed;
    case GatewayEventType::ConnectionCancelled: return T::BrowserConnectionCancelled;
    case GatewayEventType::RelayPathChanged: return T::RelayPathChanged;
    case GatewayEventType::ConfigurationDraining: return T::ConfigurationDraining;
    case GatewayEventType::ConfigurationRetired: return T::ConfigurationRetired;
    case GatewayEventType::ProxyBypassDetected: return T::ProxyBypassDetected;
  }
  return T::PipelineNotice;
}

// Answers and closes without discarding unread request bytes, which would
// make the peer see a reset instead of the answer.
void RefuseGracefully(net::SocketHandle client, const std::string& answer) {
  net::Connection c(client);
  c.SendAll(answer);
  net::ShutdownWrite(client);
  const auto until = After(100.0);
  std::string discard;
  while (c.ReadSome(discard, 65536, until, {}) == ReadResult::Ok) discard.clear();
}

bool LoopbackPeer(const std::string& address) {
  net::AddressClass cls;
  return net::ClassifyLiteral(address, &cls) && cls == net::AddressClass::Loopback;
}

#ifdef _WIN32
std::uint32_t CurrentPid() { return static_cast<std::uint32_t>(GetCurrentProcessId()); }

// Owner of the TCP connection 127.0.0.1:client_port -> 127.0.0.1:server_port.
std::uint32_t SocketOwnerPid(std::uint16_t client_port, std::uint16_t server_port) {
  ULONG size = 0;
  if (GetExtendedTcpTable(nullptr, &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_CONNECTIONS, 0) !=
      ERROR_INSUFFICIENT_BUFFER) {
    return 0;
  }
  std::vector<unsigned char> buffer(size + 4096);
  size = static_cast<ULONG>(buffer.size());
  if (GetExtendedTcpTable(buffer.data(), &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_CONNECTIONS, 0) != NO_ERROR) {
    return 0;
  }
  const auto* table = reinterpret_cast<const MIB_TCPTABLE_OWNER_PID*>(buffer.data());
  for (DWORD i = 0; i < table->dwNumEntries; ++i) {
    const MIB_TCPROW_OWNER_PID& row = table->table[i];
    if (ntohs(static_cast<u_short>(row.dwLocalPort)) == client_port &&
        ntohs(static_cast<u_short>(row.dwRemotePort)) == server_port &&
        row.dwLocalAddr == htonl(INADDR_LOOPBACK)) {
      return row.dwOwningPid;
    }
  }
  return 0;
}

std::uint32_t ParentPid(std::uint32_t pid) {
  HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snapshot == INVALID_HANDLE_VALUE) return 0;
  PROCESSENTRY32W entry{};
  entry.dwSize = sizeof(entry);
  std::uint32_t parent = 0;
  if (Process32FirstW(snapshot, &entry)) {
    do {
      if (entry.th32ProcessID == pid) {
        parent = entry.th32ParentProcessID;
        break;
      }
    } while (Process32NextW(snapshot, &entry));
  }
  CloseHandle(snapshot);
  return parent;
}
#else
std::uint32_t CurrentPid() { return static_cast<std::uint32_t>(getpid()); }
#endif

}  // namespace

const char* ToString(GatewayMode value) {
  switch (value) {
    case GatewayMode::Controlled: return "controlled";
    case GatewayMode::Diagnostic: return "diagnostic";
  }
  return "unknown";
}

const char* ToString(ConnectionKind value) {
  switch (value) {
    case ConnectionKind::HttpForward: return "http";
    case ConnectionKind::ConnectTunnel: return "connect";
    case ConnectionKind::Unknown: return "unknown";
  }
  return "unknown";
}

const char* ToString(ConnectionState value) {
  switch (value) {
    case ConnectionState::Reading: return "reading";
    case ConnectionState::Establishing: return "establishing";
    case ConnectionState::Forwarding: return "forwarding";
    case ConnectionState::Completed: return "completed";
    case ConnectionState::Failed: return "failed";
    case ConnectionState::Rejected: return "rejected";
    case ConnectionState::Cancelled: return "cancelled";
  }
  return "unknown";
}

const char* ToString(GatewayEventType value) {
  switch (value) {
    case GatewayEventType::GatewayStarted: return "GatewayStarted";
    case GatewayEventType::GatewayStopped: return "GatewayStopped";
    case GatewayEventType::BindingsChanged: return "BrowserProxyConfigured";
    case GatewayEventType::BrowserConnectionAccepted: return "BrowserConnectionAccepted";
    case GatewayEventType::ConnectionRejected: return "ConnectionRejected";
    case GatewayEventType::ConnectionPinned: return "ConnectionPinned";
    case GatewayEventType::RelayPathEstablished: return "RelayPathEstablished";
    case GatewayEventType::ForwardingStarted: return "ForwardingStarted";
    case GatewayEventType::ForwardingCompleted: return "ForwardingCompleted";
    case GatewayEventType::ForwardingFailed: return "ForwardingFailed";
    case GatewayEventType::ConnectionCancelled: return "ConnectionCancelled";
    case GatewayEventType::RelayPathChanged: return "RelayPathChanged";
    case GatewayEventType::ConfigurationDraining: return "ConfigurationDraining";
    case GatewayEventType::ConfigurationRetired: return "ConfigurationRetired";
    case GatewayEventType::ProxyBypassDetected: return "ProxyBypassDetected";
  }
  return "unknown";
}

std::vector<std::string> GatewayConfig::Validate() const {
  std::vector<std::string> issues;
  if (max_clients == 0 || max_clients > 1024) issues.push_back("max_clients must be in 1..1024");
  if (max_head_bytes < 1024 || max_head_bytes > 256 * 1024) issues.push_back("max_head_bytes must be in 1 KiB..256 KiB");
  const auto positive = [&](double v, const char* name) {
    if (!(v > 0.0) || v > 3'600'000.0) issues.push_back(std::string(name) + " must be in (0, 3600000] ms");
  };
  positive(head_timeout_ms, "head_timeout_ms");
  positive(establish_timeout_ms, "establish_timeout_ms");
  positive(idle_timeout_ms, "idle_timeout_ms");
  if (configuration_wait_ms < 0.0) issues.push_back("configuration_wait_ms must be >= 0");
  if (drain_timeout_ms < 0.0) issues.push_back("drain_timeout_ms must be >= 0");
  if (send_timeout_ms <= 0) issues.push_back("send_timeout_ms must be positive");
  for (const char c : session_id) {
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) {
      issues.push_back("session_id may contain only letters and digits");
      break;
    }
  }
  return issues;
}

// ---- One browser connection ----

struct AAFRBrowserForwardingGateway::Live {
  mutable std::mutex mutex;
  GatewayConnectionRecord record;
  std::atomic<bool> cancel{false};
  std::atomic<std::uint64_t> up{0};
  std::atomic<std::uint64_t> down{0};
  std::string cancel_reason;
  bool finished{false};

  GatewayConnectionRecord Snapshot() const {
    std::lock_guard<std::mutex> lock(mutex);
    GatewayConnectionRecord copy = record;
    copy.bytes_from_client = up.load();
    copy.bytes_to_client = down.load();
    return copy;
  }
};

AAFRBrowserForwardingGateway::AAFRBrowserForwardingGateway(GatewayConfig config, telemetry::TelemetryPipeline* pipeline)
    : config_(std::move(config)), pipeline_(pipeline), mode_(config_.mode), listener_(net::kInvalidSocket) {
  session_id_ = config_.session_id;
  if (session_id_.empty()) {
    std::random_device device;
    std::ostringstream id;
    id << std::hex << (static_cast<std::uint64_t>(device()) << 16 ^ device());
    session_id_ = id.str().substr(0, 8);
  }
  own_pid_ = CurrentPid();
}

AAFRBrowserForwardingGateway::~AAFRBrowserForwardingGateway() { Stop(); }

std::string AAFRBrowserForwardingGateway::NextId() {
  return "gw-" + session_id_ + "-" + std::to_string(++id_seq_);
}

bool AAFRBrowserForwardingGateway::Bind(std::string* error) {
  std::lock_guard<std::mutex> lock(bind_mutex_);
  if (listener_ != net::kInvalidSocket) return true;
  const auto issues = config_.Validate();
  if (!issues.empty()) {
    if (error) *error = issues.front();
    return false;
  }
  std::uint16_t bound = 0;
  std::string reason;
  const net::SocketHandle s =
      net::ListenLoopback(config_.port, static_cast<int>(std::min<std::size_t>(config_.max_clients, 128)), &bound, &reason);
  if (s == net::kInvalidSocket) {
    if (error) *error = reason;
    std::lock_guard<std::mutex> state(mutex_);
    last_error_ = reason;
    return false;
  }
  listener_ = s;
  port_ = bound;
  return true;
}

bool AAFRBrowserForwardingGateway::Start(std::string* error) {
  if (!Bind(error)) return false;
  std::lock_guard<std::mutex> lock(bind_mutex_);
  if (running_) return true;
  stopping_ = false;
  running_ = true;
  acceptor_ = std::thread([this] { AcceptLoop(); });
  watcher_ = std::thread([this] { WatchLoop(); });
  Emit(GatewayEventType::GatewayStarted, "", 0,
       "listening on 127.0.0.1:" + std::to_string(port_.load()) + " (" + ToString(mode_.load()) + ")");
  return true;
}

void AAFRBrowserForwardingGateway::Stop() {
  {
    std::lock_guard<std::mutex> lock(bind_mutex_);
    if (!running_ && listener_ == net::kInvalidSocket) return;
    stopping_ = true;
  }
  if (acceptor_.joinable()) acceptor_.join();
  if (watcher_.joinable()) watcher_.join();
  {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& live : live_) live->cancel = true;
  }
  std::vector<WorkerThread> workers;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    workers.swap(workers_);
  }
  for (WorkerThread& worker : workers) {
    if (worker.thread.joinable()) worker.thread.join();
  }
  const bool was_running = running_.exchange(false);
  {
    std::lock_guard<std::mutex> lock(bind_mutex_);
    net::CloseSocket(listener_);
    listener_ = net::kInvalidSocket;
  }
  if (was_running) Emit(GatewayEventType::GatewayStopped, "", 0, "stopped");
}

void AAFRBrowserForwardingGateway::SetBindings(GatewayBindings bindings) {
  {
    std::lock_guard<std::mutex> lock(bindings_mutex_);
    bindings_ = std::make_shared<const GatewayBindings>(std::move(bindings));
  }
  Emit(GatewayEventType::BindingsChanged, "", 0, "bound to the AAFR runtime's confirmed configuration");
}

void AAFRBrowserForwardingGateway::Detach() {
  {
    std::lock_guard<std::mutex> lock(bindings_mutex_);
    bindings_.reset();
  }
  // Cancel everything and wait for the workers: they may hold the executor.
  {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& live : live_) {
      live->cancel = true;
      std::lock_guard<std::mutex> l(live->mutex);
      if (live->cancel_reason.empty()) live->cancel_reason = "gateway_detached";
    }
  }
  for (;;) {
    std::vector<WorkerThread> finished;
    bool busy = false;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      for (auto it = workers_.begin(); it != workers_.end();) {
        if (it->done->load()) {
          finished.push_back(std::move(*it));
          it = workers_.erase(it);
        } else {
          busy = true;
          ++it;
        }
      }
      for (const auto& live : live_) live->cancel = true;  // Late arrivals too.
    }
    for (WorkerThread& w : finished) {
      if (w.thread.joinable()) w.thread.join();
    }
    if (!busy) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    current_.reset();
  }
  Emit(GatewayEventType::BindingsChanged, "", 0, "detached; connections answer 503 until rebound");
}

void AAFRBrowserForwardingGateway::SetTelemetry(telemetry::TelemetryPipeline* pipeline) { pipeline_ = pipeline; }

void AAFRBrowserForwardingGateway::SetMode(GatewayMode mode) { mode_ = mode; }

void AAFRBrowserForwardingGateway::ReportBypass(const std::string& detail) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    ++counters_.bypass_detected;
  }
  Emit(GatewayEventType::ProxyBypassDetected, "", 0, detail);
}

void AAFRBrowserForwardingGateway::Emit(GatewayEventType type, const std::string& connection_id,
                                        std::uint64_t generation, const std::string& detail) {
  GatewayEvent event;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    event.sequence = ++event_seq_;
    event.monotonic_ns = SteadyNs();
    event.type = type;
    event.connection_id = connection_id;
    event.generation = generation;
    event.detail = detail;
    events_.push_back(event);
    while (events_.size() > config_.event_history) events_.pop_front();
  }
  telemetry::TelemetryPipeline* pipeline = pipeline_.load();
  if (!pipeline) return;
  telemetry::TelemetryEvent t;
  t.type = TelemetryType(type);
  t.source = telemetry::Source::BrowserGateway;
  t.scope = telemetry::MeasurementScope::Browser;
  t.attribution = telemetry::AttributionQuality::Unattributed;
  t.attributes.push_back({"detail", detail});
  if (!connection_id.empty()) t.attributes.push_back({"connection_id", connection_id});
  if (generation != 0) t.attributes.push_back({"configuration_generation", std::to_string(generation)});
  t.attributes.push_back({"measured_by", "browser_gateway"});
  pipeline->Publish(std::move(t));
}

bool AAFRBrowserForwardingGateway::ClientAllowed(net::SocketHandle client, const std::string& peer,
                                                 std::uint16_t peer_port, std::uint32_t* pid, std::string* reason) {
  (void)client;
  if (!LoopbackPeer(peer)) {
    *reason = "non_loopback_client";
    return false;
  }
#ifdef _WIN32
  *pid = SocketOwnerPid(peer_port, port_.load());
  if (!config_.restrict_to_process) return true;
  if (*pid == own_pid_) return true;
  if (*pid != 0 && ParentPid(*pid) == own_pid_) return true;
  *reason = "foreign_process";
  return false;
#else
  (void)peer_port;
  *pid = 0;
  return true;
#endif
}

void AAFRBrowserForwardingGateway::AcceptLoop() {
  while (!stopping_) {
    net::SocketHandle listener;
    {
      std::lock_guard<std::mutex> lock(bind_mutex_);
      listener = listener_;
    }
    if (net::WaitSocket(listener, false, 50) != net::WaitResult::Ready) continue;
    std::string peer;
    std::uint16_t peer_port = 0;
    const net::SocketHandle client = net::Accept(listener, &peer, &peer_port);
    if (client == net::kInvalidSocket) continue;
    std::uint32_t pid = 0;
    std::string why;
    if (!ClientAllowed(client, peer, peer_port, &pid, &why)) {
      net::CloseSocket(client);
      {
        std::lock_guard<std::mutex> lock(mutex_);
        ++counters_.refused_clients;
      }
      Emit(GatewayEventType::ConnectionRejected, "", 0, "client refused: " + why);
      continue;
    }
    auto live = std::make_shared<Live>();
    live->record.connection_id = NextId();
    live->record.accepted_ns = SteadyNs();
    live->record.client_pid = pid;
    std::unique_lock<std::mutex> lock(mutex_);
    // Reap finished workers.
    for (auto it = workers_.begin(); it != workers_.end();) {
      if (it->done->load()) {
        it->thread.join();
        it = workers_.erase(it);
      } else {
        ++it;
      }
    }
    if (workers_.size() >= config_.max_clients) {
      ++counters_.refused_clients;
      lock.unlock();  // the refusal drains for up to 100 ms
      RefuseGracefully(client, GatewayErrorResponse(503, "gateway_at_capacity"));
      continue;
    }
    ++counters_.accepted;
    live_.push_back(live);
    auto done = std::make_shared<std::atomic<bool>>(false);
    workers_.push_back({std::thread([this, live, client, done] {
                          Serve(live, client);
                          done->store(true);
                        }),
                        done});
  }
}

void AAFRBrowserForwardingGateway::WatchLoop() {
  while (!stopping_) {
    CheckGenerations();
    for (int i = 0; i < 10 && !stopping_; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
}

void AAFRBrowserForwardingGateway::ObserveLocked(const PinnedConfiguration& configuration,
                                                 std::vector<GatewayEvent>* pending) {
  generations_[configuration.generation] = configuration;
  if (current_ && current_->generation == configuration.generation) return;
  if (current_ && configuration.generation < current_->generation) return;  // Older news.
  if (current_) {
    superseded_at_[current_->generation] = SteadyNs();
    ++counters_.path_changes;
    GatewayEvent e;
    e.type = GatewayEventType::RelayPathChanged;
    e.generation = configuration.generation;
    e.detail = "new connections: generation " + std::to_string(configuration.generation) + " " +
               configuration.algorithm + " on " + forwarding::PathText(configuration.path) + " (was generation " +
               std::to_string(current_->generation) + " " + current_->algorithm + " on " +
               forwarding::PathText(current_->path) + ")";
    pending->push_back(e);
  }
  current_ = configuration;
}

void AAFRBrowserForwardingGateway::CheckGenerations() {
  std::shared_ptr<const GatewayBindings> bindings;
  {
    std::lock_guard<std::mutex> lock(bindings_mutex_);
    bindings = bindings_;
  }
  std::optional<PinnedConfiguration> latest;
  if (bindings && bindings->configuration) latest = bindings->configuration();
  std::vector<GatewayEvent> pending;
  std::vector<std::shared_ptr<Live>> to_cancel;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (latest) ObserveLocked(*latest, &pending);
    const std::int64_t now = SteadyNs();
    for (auto it = superseded_at_.begin(); it != superseded_at_.end();) {
      const std::uint64_t generation = it->first;
      std::size_t pinned = 0;
      for (const auto& live : live_) {
        std::lock_guard<std::mutex> l(live->mutex);
        if (live->finished || live->record.generation != generation) continue;
        live->record.superseded = true;
        ++pinned;
        if (config_.drain_timeout_ms > 0.0 &&
            static_cast<double>(now - it->second) / 1e6 > config_.drain_timeout_ms && !live->cancel) {
          if (live->cancel_reason.empty()) live->cancel_reason = "drain_timeout";
          to_cancel.push_back(live);
        }
      }
      if (pinned > 0 && draining_announced_.insert(generation).second) {
        GatewayEvent e;
        e.type = GatewayEventType::ConfigurationDraining;
        e.generation = generation;
        e.detail = "generation " + std::to_string(generation) + " superseded; " + std::to_string(pinned) +
                   " pinned connection(s) draining";
        pending.push_back(e);
      }
      if (pinned == 0) {
        draining_announced_.erase(generation);
        GatewayEvent e;
        e.type = GatewayEventType::ConfigurationRetired;
        e.generation = generation;
        e.detail = "generation " + std::to_string(generation) + " has no connection left; retired";
        pending.push_back(e);
        ++counters_.retired_generations;
        generations_.erase(generation);
        it = superseded_at_.erase(it);
        continue;
      }
      ++it;
    }
  }
  for (const auto& live : to_cancel) live->cancel = true;
  for (const GatewayEvent& e : pending) Emit(e.type, e.connection_id, e.generation, e.detail);
}

void AAFRBrowserForwardingGateway::Finish(const std::shared_ptr<Live>& live) {
  // The record becomes visible as finished only once it is complete: relay
  // evidence attached and the runtime told, so a reader never sees a
  // half-finished record.
  GatewayConnectionRecord record;
  const std::int64_t finished_ns = SteadyNs();
  {
    std::lock_guard<std::mutex> lock(live->mutex);
    record = live->record;
  }
  record.finished_ns = finished_ns;
  record.duration_ms = static_cast<double>(finished_ns - record.accepted_ns) / 1e6;
  record.bytes_from_client = live->up.load();
  record.bytes_to_client = live->down.load();
  std::shared_ptr<const GatewayBindings> bindings;
  {
    std::lock_guard<std::mutex> lock(bindings_mutex_);
    bindings = bindings_;
  }
  // Relay-level evidence: wait briefly for the relays to close their side.
  if (bindings && bindings->relay_evidence && !record.observed_path.empty()) {
    for (int i = 0; i < 50; ++i) {
      record.relay_traversals = bindings->relay_evidence(record.connection_id);
      const bool open = std::any_of(record.relay_traversals.begin(), record.relay_traversals.end(),
                                    [](const forwarding::StreamTraversal& t) { return t.open; });
      if (!open || stopping_) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  }
  if (bindings && bindings->on_finished && record.generation != 0) bindings->on_finished(record);
  {
    std::lock_guard<std::mutex> lock(mutex_);
    {
      std::lock_guard<std::mutex> l(live->mutex);
      live->record = record;
      live->finished = true;
    }
    live_.erase(std::remove(live_.begin(), live_.end(), live), live_.end());
    history_.push_back(record);
    while (history_.size() > config_.record_history) history_.pop_front();
    counters_.bytes_from_clients += record.bytes_from_client;
    counters_.bytes_to_clients += record.bytes_to_client;
    switch (record.state) {
      case ConnectionState::Completed: ++counters_.completed; break;
      case ConnectionState::Failed: ++counters_.failed; break;
      case ConnectionState::Rejected: ++counters_.rejected; break;
      case ConnectionState::Cancelled: ++counters_.cancelled; break;
      default: break;
    }
    if (record.relay_failure) ++counters_.relay_failures;
    if (record.state == ConnectionState::Failed || record.state == ConnectionState::Rejected) {
      last_error_ = record.connection_id + ": " + record.failure_reason;
    }
  }

  // Telemetry summary (gateway-observed measurements; no path or payload).
  const bool diagnostic = mode_.load() == GatewayMode::Diagnostic;
  std::string detail = std::string(ToString(record.kind)) + " " + ToString(record.state);
  if (!record.observed_path.empty()) detail += " via " + forwarding::PathText(record.observed_path);
  if (!record.failure_reason.empty()) detail += " (" + record.failure_reason + ")";
  const GatewayEventType type = record.state == ConnectionState::Completed  ? GatewayEventType::ForwardingCompleted
                                : record.state == ConnectionState::Cancelled ? GatewayEventType::ConnectionCancelled
                                : record.state == ConnectionState::Rejected  ? GatewayEventType::ConnectionRejected
                                                                             : GatewayEventType::ForwardingFailed;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    GatewayEvent event;
    event.sequence = ++event_seq_;
    event.monotonic_ns = SteadyNs();
    event.type = type;
    event.connection_id = record.connection_id;
    event.generation = record.generation;
    event.detail = detail;
    events_.push_back(event);
    while (events_.size() > config_.event_history) events_.pop_front();
  }
  telemetry::TelemetryPipeline* pipeline = pipeline_.load();
  if (!pipeline) return;
  telemetry::TelemetryEvent t;
  t.type = TelemetryType(type);
  t.source = telemetry::Source::BrowserGateway;
  t.scope = telemetry::MeasurementScope::Browser;
  t.attribution = telemetry::AttributionQuality::Unattributed;
  t.metric = "gateway_connection_duration";
  if (record.duration_ms) t.value = *record.duration_ms;
  t.unit = "ms";
  t.attributes = {{"connection_id", record.connection_id},
                  {"kind", ToString(record.kind)},
                  {"state", ToString(record.state)},
                  {"configuration_generation", std::to_string(record.generation)},
                  {"algorithm", record.algorithm},
                  {"expected_path", forwarding::PathText(record.expected_path)},
                  {"observed_path", forwarding::PathText(record.observed_path)},
                  {"path_confirmed", record.path_confirmed ? "true" : "false"},
                  {"bytes_from_client", std::to_string(record.bytes_from_client)},
                  {"bytes_to_client", std::to_string(record.bytes_to_client)},
                  {"forwarding_status", record.forwarding_status},
                  {"measured_by", "browser_gateway"},
                  {"provenance", "measured"}};
  if (record.establish_ms) t.attributes.push_back({"establish_ms", Fixed(*record.establish_ms, 3)});
  if (record.first_byte_ms) t.attributes.push_back({"first_byte_ms", Fixed(*record.first_byte_ms, 3)});
  if (!record.failure_reason.empty()) t.attributes.push_back({"failure_reason", record.failure_reason});
  if (diagnostic) {
    t.attributes.push_back({"target", record.target});
    std::string relays;
    for (const auto& r : record.relay_traversals) {
      relays += (relays.empty() ? "" : ";") + r.node + ":" + r.status + ":" + std::to_string(r.bytes_toward_destination) +
                "/" + std::to_string(r.bytes_toward_client) + ":" + r.end;
    }
    t.attributes.push_back({"relay_traversals", relays});
  }
  pipeline->Publish(std::move(t));
}

void AAFRBrowserForwardingGateway::Serve(std::shared_ptr<Live> live, net::SocketHandle client) {
  net::Connection browser(client);
  net::SetIoTimeouts(client, config_.send_timeout_ms);
  net::SetNoDelay(client);
  const auto interrupted = [this, &live] { return stopping_.load() || live->cancel.load(); };
  std::string connection_id;
  {
    std::lock_guard<std::mutex> lock(live->mutex);
    connection_id = live->record.connection_id;
  }
  const auto set = [&live](const std::function<void(GatewayConnectionRecord&)>& f) {
    std::lock_guard<std::mutex> lock(live->mutex);
    f(live->record);
  };
  const auto cancelled_reason = [&live]() {
    std::lock_guard<std::mutex> lock(live->mutex);
    return live->cancel_reason.empty() ? std::string("cancelled") : live->cancel_reason;
  };
  // Ends the connection with a gateway-generated answer.
  const auto reject = [&](ConnectionState state, int status, const std::string& reason) {
    if (status != 0) {
      browser.SendAll(GatewayErrorResponse(status, reason));
      net::ShutdownWrite(browser.socket());
      const auto until = After(100.0);
      std::string discard;
      while (browser.ReadSome(discard, 65536, until, {}) == ReadResult::Ok) discard.clear();
    }
    set([&](GatewayConnectionRecord& r) {
      r.state = state;
      r.http_status = status;
      r.failure_reason = reason;
    });
    Finish(live);
  };
  Emit(GatewayEventType::BrowserConnectionAccepted, connection_id, 0, "browser connection accepted");

  // 1. Request head.
  std::string head;
  const ReadResult read = browser.ReadHead(head, config_.max_head_bytes, After(config_.head_timeout_ms), interrupted);
  if (read == ReadResult::Cancelled) return reject(ConnectionState::Cancelled, 0, cancelled_reason());
  if (read == ReadResult::TooLong) return reject(ConnectionState::Rejected, 431, "request_head_too_large");
  if (read == ReadResult::Timeout) return reject(ConnectionState::Rejected, 408, "request_head_timeout");
  if (read != ReadResult::Ok) return reject(ConnectionState::Rejected, 0, "client_closed_before_request");
  set([&](GatewayConnectionRecord& r) { r.head_ms = MsSince(r.accepted_ns); });
  ProxyRequest request;
  const ParseOutcome parsed = ParseProxyRequestHead(head, &request);
  if (!parsed.ok) return reject(ConnectionState::Rejected, parsed.status, parsed.reason);
  const bool tunnel = request.kind == ProxyRequestKind::Connect;
  set([&](GatewayConnectionRecord& r) {
    r.kind = tunnel ? ConnectionKind::ConnectTunnel : ConnectionKind::HttpForward;
    r.method = request.method;
    r.target = forwarding::FormatTarget(request.target_host, request.target_port);
  });
  {
    std::lock_guard<std::mutex> lock(mutex_);
    ++(tunnel ? counters_.connect_tunnels : counters_.http_requests);
  }

  // 2. Destination policy on what the browser asked for (the egress checks
  //    every resolved address again).
  {
    std::string why;
    net::AddressClass cls;
    if (!config_.egress.PermitsPort(request.target_port, &why) ||
        (net::ClassifyLiteral(request.target_host, &cls) && !config_.egress.PermitsClass(cls, &why))) {
      return reject(ConnectionState::Rejected, 403, "destination_forbidden");
    }
  }

  // 3. The confirmed configuration (waiting briefly while AAFR starts).
  std::shared_ptr<const GatewayBindings> bindings;
  std::optional<PinnedConfiguration> pinned;
  const auto wait_until = After(config_.configuration_wait_ms);
  for (;;) {
    {
      std::lock_guard<std::mutex> lock(bindings_mutex_);
      bindings = bindings_;
    }
    if (bindings && bindings->configuration && bindings->executor) pinned = bindings->configuration();
    if (pinned || interrupted() || Clock::now() >= wait_until) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
  }
  if (interrupted()) return reject(ConnectionState::Cancelled, 503, cancelled_reason());
  if (!pinned) return reject(ConnectionState::Rejected, 503, "no_confirmed_configuration");
  std::uint64_t graph_version = 0;
  if (bindings->usable) {
    std::string why;
    if (!bindings->usable(*pinned, &graph_version, &why)) {
      return reject(ConnectionState::Rejected, 503, "configuration_unusable: " + why);
    }
  }
  set([&](GatewayConnectionRecord& r) {
    r.state = ConnectionState::Establishing;
    r.generation = pinned->generation;
    r.algorithm = pinned->algorithm;
    r.decision_id = pinned->decision_id;
    r.expected_path = pinned->path;
  });
  {
    std::vector<GatewayEvent> pending;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      ObserveLocked(*pinned, &pending);
      if (current_ && current_->generation != pinned->generation) {
        std::lock_guard<std::mutex> l(live->mutex);
        live->record.superseded = true;
      }
    }
    for (const GatewayEvent& e : pending) Emit(e.type, e.connection_id, e.generation, e.detail);
  }
  Emit(GatewayEventType::ConnectionPinned, connection_id, pinned->generation,
       "pinned to generation " + std::to_string(pinned->generation) + " (" + pinned->algorithm + " on " +
           forwarding::PathText(pinned->path) + ")");

  // 4. The tunnel along the pinned path.
  forwarding::StreamOpenRequest open;
  open.connection_id = connection_id;
  open.path = pinned->path;
  open.target_host = request.target_host;
  open.target_port = request.target_port;
  open.timeout_ms = config_.establish_timeout_ms;
  open.graph_version = graph_version;
  forwarding::ForwardingExecutionContext context;
  context.cancel = &live->cancel;
  context.execution_id = connection_id;
  context.scenario_id = "browser_gateway";
  forwarding::StreamOpenResult opened = bindings->executor->OpenStream(open, context);
  set([&](GatewayConnectionRecord& r) {
    r.observed_path = opened.confirmed_path;
    r.path_confirmed = opened.path_confirmed;
    r.confirmation_evidence = opened.evidence;
    r.establish_ms = opened.establish_ms;
    r.forwarding_status = opened.response_status;
    r.relay_failure = opened.relay_failure;
    r.egress_address = opened.egress_address;
  });
  if (!opened.path_confirmed) {
    if (opened.status == forwarding::ForwardingStatus::Cancelled || interrupted()) {
      return reject(ConnectionState::Cancelled, 0, cancelled_reason());
    }
    const std::string& s = opened.response_status;
    int status = 502;
    std::string reason = "forwarding_failed: " + s;
    if (opened.relay_failure) reason = "relay_unavailable: " + s + (opened.failed_at.empty() ? "" : " at " + opened.failed_at);
    else if (s == "DNS_FAILED") reason = "dns_failed";
    else if (s == "DEST_FORBIDDEN") status = 403, reason = "destination_forbidden";
    else if (s == "DEST_UNREACHABLE") reason = "destination_unreachable";
    else if (s == "DEST_TIMEOUT" || s == "TIMEOUT") status = 504, reason = s == "TIMEOUT" ? "relay_timeout" : "destination_timeout";
    else if (s == "BUSY") status = 503, reason = "relay_busy";
    else if (s == "STALE_GRAPH") status = 503, reason = "stale_configuration";
    else if (s == "PATH_NOT_CONFIRMED") reason = "path_not_confirmed";
    return reject(ConnectionState::Failed, status, reason);
  }
  Emit(GatewayEventType::RelayPathEstablished, connection_id, pinned->generation,
       "observed " + forwarding::PathText(opened.confirmed_path) + " in " + Fixed(opened.establish_ms, 2) + " ms; " +
           opened.evidence);
  {
    std::lock_guard<std::mutex> lock(mutex_);
    ++counters_.forwarded;
  }
  net::Connection& upstream = opened.stream;
  net::SetIoTimeouts(upstream.socket(), config_.send_timeout_ms);
  set([&](GatewayConnectionRecord& r) { r.state = ConnectionState::Forwarding; });
  Emit(GatewayEventType::ForwardingStarted, connection_id, pinned->generation,
       std::string(tunnel ? "CONNECT tunnel" : "HTTP request") + " over " + forwarding::PathText(opened.confirmed_path));

  const std::int64_t forward_start = SteadyNs();
  const auto first_byte = [&] { set([&](GatewayConnectionRecord& r) { r.first_byte_ms = MsSince(forward_start); }); };
  if (tunnel) {
    // 5a. CONNECT: answer only now that the path is confirmed, then relay
    //     opaque bytes (TLS stays end to end).
    if (!browser.SendAll(std::string("HTTP/1.1 200 Connection Established\r\n\r\n"))) {
      set([&](GatewayConnectionRecord& r) {
        r.state = ConnectionState::Cancelled;
        r.failure_reason = "client_closed";
      });
      return Finish(live);
    }
    const std::string early_client = browser.TakeBuffered();
    const std::string early_server = upstream.TakeBuffered();
    if (!early_client.empty()) {
      upstream.SendAll(early_client);
      live->up += early_client.size();
    }
    if (!early_server.empty()) {
      first_byte();
      browser.SendAll(early_server);
      live->down += early_server.size();
    }
    const net::PumpResult pumped = net::Pump(browser.socket(), upstream.socket(), config_.idle_timeout_ms, interrupted,
                                             &live->up, &live->down, early_server.empty() ? first_byte : std::function<void()>());
    set([&](GatewayConnectionRecord& r) {
      r.end = std::string(net::ToString(pumped.end)) + (pumped.first_closed.empty() ? "" : " (first: " +
              std::string(pumped.first_closed == "a" ? "browser" : "destination") + ")");
      if (pumped.end == net::PumpEnd::Interrupted || pumped.end == net::PumpEnd::Error) {
        // Tear the relay chain down instead of half-closing it.
        net::AbortClose(upstream.Release());
      }
      if (pumped.end == net::PumpEnd::Interrupted) {
        r.state = ConnectionState::Cancelled;
        r.failure_reason = live->cancel_reason.empty() ? "cancelled" : live->cancel_reason;
      } else {
        // Errors after a confirmed tunnel are ordinary connection ends
        // (resets, aborted page loads); the tunnel itself worked.
        r.state = ConnectionState::Completed;
      }
    });
    return Finish(live);
  }

  // 5b. Plain HTTP: one request, rebuilt in origin-form. While waiting for
  //     the response, a browser that gives up (stop, tab closed) cancels
  //     the connection at once instead of when the response arrives.
  const auto browser_gone = [&] {
    if (interrupted()) return true;
    if (!net::PeerClosed(browser.socket())) return false;
    std::lock_guard<std::mutex> lock(live->mutex);
    if (live->cancel_reason.empty()) live->cancel_reason = "browser_closed";
    live->cancel = true;
    return true;
  };
  const std::string origin_head = BuildOriginRequestHead(request);
  if (!upstream.SendAll(origin_head)) {
    return reject(ConnectionState::Failed, 502, "upstream_send_failed");
  }
  std::uint64_t body_left = request.content_length.value_or(0);
  while (body_left > 0) {
    std::string chunk;
    const ReadResult r =
        browser.ReadSome(chunk, static_cast<std::size_t>(std::min<std::uint64_t>(body_left, 64 * 1024)),
                         After(config_.idle_timeout_ms), interrupted);
    if (r == ReadResult::Cancelled) return reject(ConnectionState::Cancelled, 0, cancelled_reason());
    if (r != ReadResult::Ok) return reject(ConnectionState::Failed, r == ReadResult::Timeout ? 408 : 0, "request_body_incomplete");
    if (!upstream.SendAll(chunk)) return reject(ConnectionState::Failed, 502, "upstream_send_failed");
    body_left -= chunk.size();
    live->up += chunk.size();
  }
  // Response head(s): interim 1xx heads are passed on, then the final one.
  ResponseHead response;
  for (;;) {
    std::string response_head;
    const ReadResult r = upstream.ReadHead(response_head, 64 * 1024, After(config_.idle_timeout_ms), browser_gone);
    if (r == ReadResult::Cancelled) return reject(ConnectionState::Cancelled, 0, cancelled_reason());
    if (r == ReadResult::Timeout) return reject(ConnectionState::Failed, 504, "response_timeout");
    if (r != ReadResult::Ok) return reject(ConnectionState::Failed, 502, "upstream_closed_before_response");
    set([&](GatewayConnectionRecord& rec) {
      if (!rec.first_byte_ms) rec.first_byte_ms = MsSince(forward_start);
    });
    std::string why;
    if (!ParseResponseHead(response_head, &response, &why)) return reject(ConnectionState::Failed, 502, "bad_response_head: " + why);
    if (response.status == 101) return reject(ConnectionState::Failed, 502, "unexpected_protocol_switch");
    const std::string client_head = BuildClientResponseHead(response);
    if (!browser.SendAll(client_head)) {
      set([&](GatewayConnectionRecord& rec) {
        rec.state = ConnectionState::Cancelled;
        rec.failure_reason = "client_closed";
      });
      return Finish(live);
    }
    live->down += client_head.size();
    if (response.status >= 200) break;
  }
  set([&](GatewayConnectionRecord& rec) { rec.http_status = response.status; });
  std::uint64_t length = 0;
  const BodyFraming framing = ResponseFraming(response, request.method, &length);
  if (framing == BodyFraming::Invalid) {
    set([&](GatewayConnectionRecord& rec) {
      rec.state = ConnectionState::Failed;
      rec.failure_reason = "ambiguous_response_framing";
    });
    return Finish(live);
  }
  ChunkedTracker chunked;
  std::string outcome = "complete";
  bool finished_body = framing == BodyFraming::None;
  while (!finished_body) {
    std::string chunk;
    const std::size_t want = framing == BodyFraming::ContentLength
                                 ? static_cast<std::size_t>(std::min<std::uint64_t>(length, 64 * 1024))
                                 : 64 * 1024;
    const ReadResult r = upstream.ReadSome(chunk, want, After(config_.idle_timeout_ms), browser_gone);
    if (r == ReadResult::Closed) {
      outcome = framing == BodyFraming::UntilClose ? "complete" : "upstream_closed_early";
      break;
    }
    if (r == ReadResult::Cancelled) {
      outcome = "cancelled";
      break;
    }
    if (r != ReadResult::Ok) {
      outcome = "response_idle_timeout";
      break;
    }
    std::size_t use = chunk.size();
    if (framing == BodyFraming::Chunked) {
      use = chunked.Feed(chunk.data(), chunk.size());
      finished_body = chunked.done();
      if (chunked.failed()) {
        outcome = "malformed_chunked_response";
        finished_body = true;
      }
    } else if (framing == BodyFraming::ContentLength) {
      length -= use;
      finished_body = length == 0;
    }
    if (!browser.SendAll(chunk.data(), use)) {
      outcome = "client_closed";
      break;
    }
    live->down += use;
  }
  set([&](GatewayConnectionRecord& rec) {
    rec.end = std::string(ToString(framing)) + ": " + outcome;
    if (outcome == "complete") rec.state = ConnectionState::Completed;
    else if (outcome == "cancelled" || outcome == "client_closed") {
      rec.state = ConnectionState::Cancelled;
      rec.failure_reason = outcome == "cancelled" ? (live->cancel_reason.empty() ? "cancelled" : live->cancel_reason) : outcome;
    } else {
      rec.state = ConnectionState::Failed;
      rec.failure_reason = outcome;
    }
  });
  if (outcome != "complete") net::AbortClose(upstream.Release());
  browser.Close();
  upstream.Close();
  Finish(live);
}

GatewayStatus AAFRBrowserForwardingGateway::Status() const {
  GatewayStatus status;
  status.running = running_.load();
  status.port = port_.load();
  status.bound = status.port != 0;
  status.mode = mode_.load();
  status.listen_address = status.bound ? "127.0.0.1:" + std::to_string(status.port) : std::string();
  {
    std::lock_guard<std::mutex> lock(bindings_mutex_);
    status.has_bindings = bindings_ != nullptr;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  status.current = current_;
  status.counters = counters_;
  status.last_error = last_error_;
  for (const auto& live : live_) {
    std::lock_guard<std::mutex> l(live->mutex);
    if (live->finished) continue;
    ++status.active_connections;
    if (live->record.generation != 0) ++status.active_by_generation[live->record.generation];
    if (live->record.generation != 0 && current_ && live->record.generation != current_->generation) {
      ++status.draining_connections;
    }
  }
  return status;
}

std::vector<GatewayConnectionRecord> AAFRBrowserForwardingGateway::Connections(std::size_t max_records) const {
  std::vector<GatewayConnectionRecord> out;
  std::vector<std::shared_ptr<Live>> live;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    const std::size_t from = history_.size() > max_records ? history_.size() - max_records : 0;
    out.assign(history_.begin() + static_cast<std::ptrdiff_t>(from), history_.end());
    live = live_;
  }
  for (const auto& l : live) {
    GatewayConnectionRecord r = l->Snapshot();
    out.push_back(std::move(r));
  }
  // Oldest first by acceptance, live and finished alike.
  std::stable_sort(out.begin(), out.end(), [](const GatewayConnectionRecord& x, const GatewayConnectionRecord& y) {
    return x.accepted_ns < y.accepted_ns;
  });
  if (out.size() > max_records) out.erase(out.begin(), out.begin() + static_cast<std::ptrdiff_t>(out.size() - max_records));
  return out;
}

std::optional<GatewayConnectionRecord> AAFRBrowserForwardingGateway::FindConnection(const std::string& connection_id) const {
  std::vector<std::shared_ptr<Live>> live;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = history_.rbegin(); it != history_.rend(); ++it) {
      if (it->connection_id == connection_id) return *it;
    }
    live = live_;
  }
  for (const auto& l : live) {
    GatewayConnectionRecord r = l->Snapshot();
    if (r.connection_id == connection_id) return r;
  }
  return std::nullopt;
}

std::vector<GatewayEvent> AAFRBrowserForwardingGateway::Events(std::size_t max_events) const {
  std::lock_guard<std::mutex> lock(mutex_);
  const std::size_t from = events_.size() > max_events ? events_.size() - max_events : 0;
  return std::vector<GatewayEvent>(events_.begin() + static_cast<std::ptrdiff_t>(from), events_.end());
}

}  // namespace aafr::gateway
