#pragma once

// AAFRBrowserForwardingGateway: the V0.7.3 local forwarding gateway between
// Qt WebEngine and the controlled relays. Plain C++17 over sockets; no Qt.
//
//   browser --(explicit HTTP proxy: CONNECT or absolute-form GET)--> gateway
//     -> confirmed active configuration (pinned for the connection's life)
//     -> IStreamForwardingExecutor::OpenStream along that configuration's path
//     -> relays (entry -> ... -> Destination node = egress) -> real server
//
// The gateway owns no forwarding decision. For every accepted connection it
// asks its ConfigurationSource for the runtime's confirmed active
// configuration, checks that the configuration is still usable, pins the
// connection to that configuration's generation, opens a tunnel along its
// path and only then answers the browser (for CONNECT, "200 Connection
// Established" is sent only after the tunnel's path is confirmed by the
// relays' own trace and logs). It never falls back to a direct connection:
// without a confirmed configuration the browser gets 503.
//
// Connection pinning: a connection keeps the path it was opened on for its
// whole life. When the source reports a new generation, new connections use
// it; connections on older generations drain. Optional bounded draining
// (drain_timeout_ms > 0) cancels connections that are still pinned to a
// superseded generation that long after it was superseded; the default (0)
// never terminates a connection because the algorithm changed.
//
// HTTPS: CONNECT bytes are relayed opaquely end to end. The gateway never
// decrypts, never terminates TLS, and needs no certificate; the browser
// validates the real server's certificate as usual.
//
// Plain HTTP: one request per browser connection (the forwarded request and
// the returned response carry "Connection: close"); see proxy_http.h.
//
// Security: binds 127.0.0.1 only; refuses peers that are not loopback and,
// when restrict_to_process is set (Windows), peers whose socket is not owned
// by this process or one of its children (the browser's network service);
// applies the EgressPolicy before forwarding; bounds clients, head sizes and
// every wait; logs no header values, paths, query strings, cookies,
// credentials or payload bytes.

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "routing/forwarding/egress_policy.h"
#include "routing/forwarding/forwarding_executor.h"
#include "routing/forwarding/loopback_forwarding.h"

namespace aafr::telemetry {
class TelemetryPipeline;
}

namespace aafr::gateway {

enum class GatewayMode {
  Controlled,  // Forward through the confirmed configuration.
  Diagnostic,  // As Controlled, plus per-connection relay evidence in telemetry.
};
const char* ToString(GatewayMode value);

// The confirmed configuration a connection is pinned to.
struct PinnedConfiguration {
  std::uint64_t generation{0};  // The runtime's config_version.
  std::string algorithm;
  forwarding::Path path;
  std::string graph_id;
  std::uint64_t graph_version{0};  // Graph version the configuration was confirmed on.
  std::string decision_id;
  std::string activation_request_id;
};

// What the gateway needs from the rest of AAFR. All callbacks may be called
// from gateway worker threads.
struct GatewayBindings {
  // Required: the confirmed active configuration, or nullopt.
  std::function<std::optional<PinnedConfiguration>()> configuration;
  // Optional: whether a pinned configuration can still carry a new
  // connection (e.g. its path is valid on the current graph); false with a
  // reason refuses the connection (503). `graph_version` receives the
  // version the relays should check the tunnel against (0: unchecked).
  std::function<bool(const PinnedConfiguration&, std::uint64_t* graph_version, std::string* reason)> usable;
  // Required: the executor that opens tunnels.
  forwarding::IStreamForwardingExecutor* executor{nullptr};
  // Optional: relay-level evidence for a connection id (StreamTraversal).
  std::function<std::vector<forwarding::StreamTraversal>(const std::string&)> relay_evidence;
  // Optional: told about every finished connection (outcome observation).
  std::function<void(const struct GatewayConnectionRecord&)> on_finished;
};

struct GatewayConfig {
  std::uint16_t port{0};  // 0: ephemeral. Always 127.0.0.1.
  GatewayMode mode{GatewayMode::Controlled};
  std::string session_id;  // Letters and digits; generated when empty.
  std::size_t max_clients{64};
  std::size_t max_head_bytes{32 * 1024};
  double head_timeout_ms{15'000.0};
  double establish_timeout_ms{10'000.0};
  // Wait this long for a first confirmed configuration before answering 503
  // (the runtime verifies the ten algorithms at start-up).
  double configuration_wait_ms{15'000.0};
  double idle_timeout_ms{120'000.0};
  int send_timeout_ms{10'000};
  // 0: superseded configurations drain without a bound.
  double drain_timeout_ms{0.0};
  // Windows: only sockets owned by this process (or its children).
  bool restrict_to_process{false};
  forwarding::EgressPolicy egress;
  std::size_t record_history{512};
  std::size_t event_history{1024};

  std::vector<std::string> Validate() const;
};

enum class ConnectionKind { HttpForward, ConnectTunnel, Unknown };
enum class ConnectionState { Reading, Establishing, Forwarding, Completed, Failed, Rejected, Cancelled };
const char* ToString(ConnectionKind value);
const char* ToString(ConnectionState value);

struct GatewayConnectionRecord {
  std::string connection_id;
  ConnectionKind kind{ConnectionKind::Unknown};
  ConnectionState state{ConnectionState::Reading};
  std::string method;
  std::string target;           // host:port only (no path, no query).
  std::uint32_t client_pid{0};  // Owning process of the browser socket (Windows), 0 if unknown.
  // Pinned configuration.
  std::uint64_t generation{0};
  std::string algorithm;
  std::string decision_id;
  forwarding::Path expected_path;
  // Evidence.
  forwarding::Path observed_path;  // Trace returned by the relays.
  bool path_confirmed{false};
  std::string confirmation_evidence;
  std::vector<forwarding::StreamTraversal> relay_traversals;  // Filled when finished.
  std::string egress_address;
  // Gateway-observed timings (steady clock).
  std::int64_t accepted_ns{0};
  std::int64_t finished_ns{0};
  std::optional<double> head_ms;        // Accept -> complete request head.
  std::optional<double> establish_ms;   // Tunnel request -> confirmed tunnel (executor-measured).
  std::optional<double> first_byte_ms;  // Request forwarded -> first response byte.
  std::optional<double> duration_ms;    // Accept -> close.
  std::uint64_t bytes_from_client{0};
  std::uint64_t bytes_to_client{0};
  int http_status{0};               // Upstream status (HTTP) or the gateway's own error status.
  std::string forwarding_status;    // Executor response status (OK, NEXT_HOP_UNAVAILABLE, ...).
  bool relay_failure{false};
  bool superseded{false};           // Its generation is no longer current.
  std::string end;                  // How the byte exchange ended.
  std::string failure_reason;
};

enum class GatewayEventType {
  GatewayStarted,
  GatewayStopped,
  BindingsChanged,
  BrowserConnectionAccepted,
  ConnectionRejected,
  ConnectionPinned,
  RelayPathEstablished,
  ForwardingStarted,
  ForwardingCompleted,
  ForwardingFailed,
  ConnectionCancelled,
  RelayPathChanged,
  ConfigurationDraining,
  ConfigurationRetired,
  ProxyBypassDetected,
};
const char* ToString(GatewayEventType value);

struct GatewayEvent {
  std::uint64_t sequence{0};
  std::int64_t monotonic_ns{0};
  GatewayEventType type{GatewayEventType::GatewayStarted};
  std::string connection_id;
  std::uint64_t generation{0};
  std::string detail;
};

struct GatewayCounters {
  std::uint64_t accepted{0};
  std::uint64_t refused_clients{0};  // Non-loopback, foreign process or over capacity.
  std::uint64_t rejected{0};         // Malformed or forbidden requests, no configuration.
  std::uint64_t forwarded{0};        // Tunnel established over a confirmed path.
  std::uint64_t completed{0};
  std::uint64_t failed{0};
  std::uint64_t relay_failures{0};
  std::uint64_t cancelled{0};
  std::uint64_t connect_tunnels{0};
  std::uint64_t http_requests{0};
  std::uint64_t bytes_from_clients{0};
  std::uint64_t bytes_to_clients{0};
  std::uint64_t bypass_detected{0};
  std::uint64_t path_changes{0};
  std::uint64_t retired_generations{0};
};

struct GatewayStatus {
  bool running{false};
  bool bound{false};
  GatewayMode mode{GatewayMode::Controlled};
  std::string listen_address;  // 127.0.0.1:<port>
  std::uint16_t port{0};
  bool has_bindings{false};
  std::optional<PinnedConfiguration> current;
  std::size_t active_connections{0};
  std::size_t draining_connections{0};  // Pinned to a superseded generation.
  std::map<std::uint64_t, std::size_t> active_by_generation;
  GatewayCounters counters;
  std::string last_error;
};

class AAFRBrowserForwardingGateway {
 public:
  explicit AAFRBrowserForwardingGateway(GatewayConfig config = {}, telemetry::TelemetryPipeline* pipeline = nullptr);
  ~AAFRBrowserForwardingGateway();
  AAFRBrowserForwardingGateway(const AAFRBrowserForwardingGateway&) = delete;
  AAFRBrowserForwardingGateway& operator=(const AAFRBrowserForwardingGateway&) = delete;

  // Binds 127.0.0.1:<port> without accepting yet, so the port can be given
  // to Chromium before Qt WebEngine initialises. Idempotent.
  bool Bind(std::string* error = nullptr);
  // Bind() if needed, then accepts on a worker thread.
  bool Start(std::string* error = nullptr);
  // Cancels every connection, joins all threads, closes the listener.
  void Stop();

  // Replaces the bindings (e.g. when the runtime starts). Connections
  // already forwarding keep the executor they were opened with, so a caller
  // that destroys the executor must call Detach() first.
  void SetBindings(GatewayBindings bindings);
  // Clears the bindings, cancels every connection and waits until none is
  // left. The gateway keeps listening and answers 503 until rebound.
  void Detach();

  // Recorded when the browser side sees a request that will not use the
  // gateway (e.g. a loopback URL while loopback is bypassed).
  void ReportBypass(const std::string& detail);
  void SetTelemetry(telemetry::TelemetryPipeline* pipeline);
  void SetMode(GatewayMode mode);

  std::uint16_t port() const { return port_.load(); }
  GatewayStatus Status() const;
  std::vector<GatewayConnectionRecord> Connections(std::size_t max_records) const;
  std::optional<GatewayConnectionRecord> FindConnection(const std::string& connection_id) const;
  std::vector<GatewayEvent> Events(std::size_t max_events) const;
  const GatewayConfig& config() const { return config_; }
  const std::string& session_id() const { return session_id_; }

 private:
  struct Live;
  void AcceptLoop();
  void WatchLoop();
  void Serve(std::shared_ptr<Live> live, forwarding::net::SocketHandle client);
  void CheckGenerations();
  // Adopts a newer confirmed configuration (RelayPathChanged). mutex_ held.
  void ObserveLocked(const PinnedConfiguration& configuration, std::vector<GatewayEvent>* pending);
  void Emit(GatewayEventType type, const std::string& connection_id, std::uint64_t generation,
            const std::string& detail);
  void Finish(const std::shared_ptr<Live>& live);
  bool ClientAllowed(forwarding::net::SocketHandle client, const std::string& peer, std::uint16_t peer_port,
                     std::uint32_t* pid, std::string* reason);
  std::string NextId();

  const GatewayConfig config_;
  std::string session_id_;
  std::atomic<telemetry::TelemetryPipeline*> pipeline_{nullptr};
  std::atomic<GatewayMode> mode_;

  mutable std::mutex bind_mutex_;
  forwarding::net::SocketHandle listener_;
  std::atomic<std::uint16_t> port_{0};
  std::atomic<bool> running_{false};
  std::atomic<bool> stopping_{false};
  std::thread acceptor_;
  std::thread watcher_;

  mutable std::mutex bindings_mutex_;
  std::shared_ptr<const GatewayBindings> bindings_;

  mutable std::mutex mutex_;
  std::vector<std::shared_ptr<Live>> live_;
  struct WorkerThread {
    std::thread thread;
    std::shared_ptr<std::atomic<bool>> done;
  };
  std::vector<WorkerThread> workers_;
  std::deque<GatewayConnectionRecord> history_;
  std::deque<GatewayEvent> events_;
  std::uint64_t event_seq_{0};
  GatewayCounters counters_;
  std::string last_error_;
  std::optional<PinnedConfiguration> current_;
  // When each superseded generation was superseded (steady ns).
  std::map<std::uint64_t, std::int64_t> superseded_at_;
  std::map<std::uint64_t, PinnedConfiguration> generations_;
  std::set<std::uint64_t> draining_announced_;
  std::atomic<std::uint64_t> id_seq_{0};
  std::uint32_t own_pid_{0};
};

}  // namespace aafr::gateway
