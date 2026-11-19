#include "routing/forwarding/loopback_forwarding.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <deque>
#include <functional>
#include <sstream>
#include <thread>

#include "routing/forwarding/socket_io.h"

namespace aafr::forwarding {
namespace {

using net::After;
using net::Clock;
using net::Connection;
using net::Deadline;
using net::ReadResult;
using net::RemainingMs;
using net::SocketHandle;
using net::kInvalidSocket;

constexpr std::size_t kMaxHeaderBytes = 1024;
constexpr std::size_t kMaxBodyBytes = ForwardingRequest::kMaxPayloadBytes + 256;
constexpr int kLogCapacity = 256;

std::int64_t SteadyNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
}

std::vector<std::string> Split(const std::string& text, char separator) {
  std::vector<std::string> parts;
  std::string part;
  std::istringstream stream(text);
  while (std::getline(stream, part, separator)) parts.push_back(part);
  return parts;
}

std::string JoinList(const std::vector<std::string>& items) {
  if (items.empty()) return "-";
  std::string text;
  for (const std::string& item : items) text += (text.empty() ? "" : ",") + item;
  return text;
}

std::vector<std::string> ParseList(const std::string& text) {
  return text == "-" ? std::vector<std::string>{} : Split(text, ',');
}

bool Token(const std::string& text) {
  if (text.empty() || text.size() > 64) return false;
  return std::all_of(text.begin(), text.end(), [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' ||
           c == '.';
  });
}

double ParseBudget(const std::string& text) {
  try {
    return std::stod(text);
  } catch (...) {
    return 0.0;
  }
}

}  // namespace

// ---- Directory: who exists, where they listen, who may forward to whom ----

class LoopbackDirectory {
 public:
  struct Link {
    bool enabled{true};
    double latency_ms{0.0};
    // V0.7.4 fault injection: an extra delay the forwarding graph does not
    // know about (see LoopbackForwardingNetwork::SetHiddenEdgeDelay).
    double hidden_delay_ms{0.0};
  };

  void SetPort(const std::string& id, std::uint16_t port) {
    std::lock_guard<std::mutex> lock(mutex_);
    ports_[id] = port;
  }
  std::uint16_t Port(const std::string& id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = ports_.find(id);
    return found == ports_.end() ? 0 : found->second;
  }
  void SetLink(const std::string& from, const std::string& to, Link link) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = links_.find({from, to});
    // A graph sync never clears an injected hidden delay.
    if (found != links_.end()) link.hidden_delay_ms = found->second.hidden_delay_ms;
    links_[{from, to}] = link;
  }
  bool SetHiddenDelay(const std::string& from, const std::string& to, double ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = links_.find({from, to});
    if (found == links_.end()) return false;
    found->second.hidden_delay_ms = ms;
    return true;
  }
  bool HasLink(const std::string& from, const std::string& to) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return links_.count({from, to}) != 0;
  }
  // nullopt-like: enabled false when absent.
  Link FindLink(const std::string& from, const std::string& to) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = links_.find({from, to});
    return found == links_.end() ? Link{false, 0.0, 0.0} : found->second;
  }
  void EnableLink(const std::string& from, const std::string& to, bool enabled) {
    std::lock_guard<std::mutex> lock(mutex_);
    links_[{from, to}].enabled = enabled;
  }
  void SetEgress(const EgressPolicy& policy) {
    std::lock_guard<std::mutex> lock(mutex_);
    egress_ = policy;
  }
  EgressPolicy Egress() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return egress_;
  }

  LoopbackOptions options;

 private:
  mutable std::mutex mutex_;
  std::map<std::string, std::uint16_t> ports_;
  std::map<std::pair<std::string, std::string>, Link> links_;
  EgressPolicy egress_;
};

// ---- One node: TCP server on 127.0.0.1 ----

class LoopbackNode {
 public:
  LoopbackNode(std::string id, NodeRole role, std::shared_ptr<LoopbackDirectory> directory)
      : id_(std::move(id)), role_(role), directory_(std::move(directory)) {}
  ~LoopbackNode() { Stop(); }

  bool Start(std::string* error) {
    if (running_) return true;
    std::string reason;
    listen_ = net::ListenLoopback(0, 16, &port_, &reason);
    if (listen_ == kInvalidSocket) {
      return Fail(error, reason.empty() ? "cannot listen on 127.0.0.1 for node '" + id_ + "'"
                                        : reason + " (node '" + id_ + "')");
    }
    stopping_ = false;
    running_ = true;
    acceptor_ = std::thread([this] { AcceptLoop(); });
    directory_->SetPort(id_, port_);
    return true;
  }

  void Stop() {
    if (!running_) return;
    directory_->SetPort(id_, 0);
    stopping_ = true;
    if (acceptor_.joinable()) acceptor_.join();
    std::vector<Worker> workers;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      workers.swap(workers_);
    }
    for (Worker& worker : workers) {
      if (worker.thread.joinable()) worker.thread.join();
    }
    net::CloseSocket(listen_);
    listen_ = kInvalidSocket;
    port_ = 0;
    running_ = false;
  }

  std::uint16_t port() const { return port_; }
  bool running() const { return running_; }
  std::uint64_t handled() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return handled_;
  }
  std::uint64_t streams_handled() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return streams_handled_;
  }
  bool Logged(const std::string& request_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return std::find(log_.begin(), log_.end(), request_id) != log_.end();
  }
  std::size_t active() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::size_t count = 0;
    for (const Worker& worker : workers_) count += worker.done->load() ? 0 : 1;
    return count;
  }
  std::size_t active_streams() const { return active_streams_.load(); }
  std::vector<StreamTraversal> Traversals(const std::string& connection_id) const {
    std::vector<StreamTraversal> out;
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& record : stream_log_) {
      if (record->connection_id != connection_id) continue;
      StreamTraversal copy = record->Snapshot();
      out.push_back(std::move(copy));
    }
    return out;
  }

 private:
  struct Worker {
    std::thread thread;
    std::shared_ptr<std::atomic<bool>> done;
  };
  // A tunnel record that the pumping thread updates while others read it.
  struct LiveTraversal {
    std::string connection_id;
    std::string node;
    NodeRole role{NodeRole::Relay};
    std::int64_t opened_ns{0};
    mutable std::mutex mutex;
    std::string next_hop;
    std::string status;
    bool open{false};
    std::int64_t closed_ns{0};
    std::string end;
    std::atomic<std::uint64_t> toward_destination{0};
    std::atomic<std::uint64_t> toward_client{0};

    StreamTraversal Snapshot() const {
      StreamTraversal t;
      t.connection_id = connection_id;
      t.node = node;
      t.role = role;
      t.opened_ns = opened_ns;
      t.bytes_toward_destination = toward_destination.load();
      t.bytes_toward_client = toward_client.load();
      std::lock_guard<std::mutex> lock(mutex);
      t.next_hop = next_hop;
      t.status = status;
      t.open = open;
      t.closed_ns = closed_ns;
      t.end = end;
      return t;
    }
    void Finish(const std::string& how) {
      std::lock_guard<std::mutex> lock(mutex);
      open = false;
      closed_ns = SteadyNs();
      if (end.empty()) end = how;
    }
  };

  static bool Fail(std::string* error, const std::string& message) {
    if (error) *error = message;
    return false;
  }

  void Reap() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = workers_.begin(); it != workers_.end();) {
      if (it->done->load()) {
        it->thread.join();
        it = workers_.erase(it);
      } else {
        ++it;
      }
    }
  }

  void AcceptLoop() {
    while (!stopping_) {
      if (net::WaitSocket(listen_, false, 50) != net::WaitResult::Ready) continue;
      SocketHandle client = net::Accept(listen_, nullptr, nullptr);
      if (client == kInvalidSocket) continue;
      Reap();
      std::lock_guard<std::mutex> lock(mutex_);
      if (workers_.size() >= LoopbackForwardingNetwork::kMaxConnections + LoopbackForwardingNetwork::kMaxStreams) {
        Connection refused(client);
        refused.SendAll("AAFR1 RSP - BUSY " + id_ + "\n\n");
        continue;
      }
      auto done = std::make_shared<std::atomic<bool>>(false);
      workers_.push_back({std::thread([this, client, done] {
                            Handle(client);
                            done->store(true);
                          }),
                          done});
    }
  }

  void Record(const std::string& request_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    ++handled_;
    log_.push_back(request_id);
    while (log_.size() > kLogCapacity) log_.pop_front();
  }

  void Handle(SocketHandle client) {
    Connection in(client);
    const auto interrupted = [this] { return stopping_.load(); };
    std::string header;
    const Deadline read_deadline = After(2000.0);
    if (in.ReadLine(header, kMaxHeaderBytes, read_deadline, interrupted) != ReadResult::Ok) {
      in.SendAll("AAFR1 RSP - BAD_REQUEST " + id_ + "\n\n");
      return;
    }
    const std::vector<std::string> parts = Split(header, ' ');
    if (parts.size() >= 2 && parts[0] == "AAFR1" && parts[1] == "TUN") {
      HandleTunnel(in, parts);
      return;
    }
    std::string payload;
    if (in.ReadLine(payload, kMaxBodyBytes, read_deadline, interrupted) != ReadResult::Ok) {
      in.SendAll("AAFR1 RSP - BAD_REQUEST " + id_ + "\n\n");
      return;
    }
    if (parts.size() != 6 || parts[0] != "AAFR1" || parts[1] != "REQ" || !Token(parts[2])) {
      in.SendAll("AAFR1 RSP - BAD_REQUEST " + id_ + "\n\n");
      return;
    }
    const std::string& request_id = parts[2];
    const double budget_ms = ParseBudget(parts[3]);
    const std::vector<std::string> route = ParseList(parts[4]);
    std::vector<std::string> trace = ParseList(parts[5]);
    const auto reply = [&](const std::string& status, const std::string& body) {
      in.SendAll("AAFR1 RSP " + request_id + " " + status + " " + JoinList(trace) + "\n" + body + "\n");
    };
    if (!(budget_ms > 0.0) || budget_ms > 60000.0 ||
        !std::all_of(route.begin(), route.end(), Token) || !std::all_of(trace.begin(), trace.end(), Token)) {
      reply("BAD_REQUEST", "");
      return;
    }
    const Deadline deadline = After(budget_ms);
    const bool loop = std::find(trace.begin(), trace.end(), id_) != trace.end();
    trace.push_back(id_);
    if (loop) return reply("LOOP_DETECTED", "");
    if (trace.size() > LoopbackForwardingNetwork::kMaxHops + 1) return reply("HOP_LIMIT", "");
    Record(request_id);

    if (route.empty()) {
      if (role_ == NodeRole::Destination) return reply("OK", "served-by=" + id_ + ";echo=" + payload);
      return reply("NO_ROUTE", "");
    }
    if (role_ == NodeRole::Destination) return reply("BAD_ROUTE", "");
    const std::string& next = route.front();
    const LoopbackDirectory::Link link = directory_->FindLink(id_, next);
    if (!directory_->HasLink(id_, next)) return reply("NOT_A_NEIGHBOUR", "");
    if (!link.enabled) return reply("LINK_DOWN", "");
    const std::uint16_t port = directory_->Port(next);
    if (port == 0) return reply("NEXT_HOP_UNAVAILABLE", "");

    InjectDelay(link, deadline);
    const double remaining = RemainingMs(deadline);
    if (remaining <= 0.0) return reply("TIMEOUT", "");
    SocketHandle upstream = net::ConnectLoopback(port, std::min(remaining, 1000.0));
    if (upstream == kInvalidSocket) return reply("NEXT_HOP_UNAVAILABLE", "");
    Connection out(upstream);
    const std::vector<std::string> rest(route.begin() + 1, route.end());
    char budget[32];
    std::snprintf(budget, sizeof(budget), "%.0f", std::max(1.0, RemainingMs(deadline)));
    if (!out.SendAll("AAFR1 REQ " + request_id + " " + budget + " " + JoinList(rest) + " " + JoinList(trace) + "\n" +
                     payload + "\n")) {
      return reply("NEXT_HOP_FAILED", "");
    }
    std::string response;
    std::string body;
    const ReadResult first = out.ReadLine(response, kMaxHeaderBytes, deadline, interrupted);
    const ReadResult second =
        first == ReadResult::Ok ? out.ReadLine(body, kMaxBodyBytes, deadline, interrupted) : first;
    if (second == ReadResult::Timeout) return reply("TIMEOUT", "");
    if (second != ReadResult::Ok || response.rfind("AAFR1 RSP " + request_id + " ", 0) != 0) {
      return reply("NEXT_HOP_FAILED", "");
    }
    in.SendAll(response + "\n" + body + "\n");  // Relay the downstream answer unchanged.
  }

  void InjectDelay(const LoopbackDirectory::Link& link, Deadline deadline) {
    const double configured = directory_->options.inject_edge_latency ? link.latency_ms : 0.0;
    const double total = configured + link.hidden_delay_ms;
    if (!(total > 0.0)) return;
    const Deadline wake = After(std::min(total, directory_->options.max_injected_delay_ms));
    while (Clock::now() < wake && !stopping_ && RemainingMs(deadline) > 0.0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
  }

  // ---- V0.7.3 tunnels ----

  void HandleTunnel(Connection& in, const std::vector<std::string>& parts) {
    // AAFR1 TUN <conn_id> <budget_ms> <route|-> <trace|-> <host:port>
    if (parts.size() != 7 || !Token(parts[2])) {
      in.SendAll("AAFR1 TUNRSP - BAD_REQUEST " + id_ + " -\n");
      return;
    }
    const std::string& connection_id = parts[2];
    const double budget_ms = ParseBudget(parts[3]);
    const std::vector<std::string> route = ParseList(parts[4]);
    std::vector<std::string> trace = ParseList(parts[5]);
    std::string target_host;
    std::uint16_t target_port = 0;
    const bool target_ok = ParseTarget(parts[6], &target_host, &target_port);

    auto record = std::make_shared<LiveTraversal>();
    record->connection_id = connection_id;
    record->node = id_;
    record->role = role_;
    record->opened_ns = SteadyNs();
    const auto reply = [&](const std::string& status, const std::string& egress) {
      {
        std::lock_guard<std::mutex> lock(record->mutex);
        record->status = status;
      }
      in.SendAll("AAFR1 TUNRSP " + connection_id + " " + status + " " + JoinList(trace) + " " +
                 (egress.empty() ? "-" : egress) + "\n");
    };
    const auto refuse = [&](const std::string& status) {
      reply(status, "");
      record->Finish("not_established");
    };
    if (!target_ok || !(budget_ms > 0.0) || budget_ms > 60000.0 || !std::all_of(route.begin(), route.end(), Token) ||
        !std::all_of(trace.begin(), trace.end(), Token)) {
      return refuse("BAD_REQUEST");
    }
    const Deadline deadline = After(budget_ms);
    const bool loop = std::find(trace.begin(), trace.end(), id_) != trace.end();
    trace.push_back(id_);
    if (loop) return refuse("LOOP_DETECTED");
    if (trace.size() > LoopbackForwardingNetwork::kMaxHops + 1) return refuse("HOP_LIMIT");
    // One stream slot for the tunnel's lifetime.
    if (active_streams_.fetch_add(1) >= LoopbackForwardingNetwork::kMaxStreams) {
      active_streams_.fetch_sub(1);
      return refuse("BUSY");
    }
    struct Slot {
      std::atomic<std::size_t>& count;
      ~Slot() { count.fetch_sub(1); }
    } slot{active_streams_};
    Record(connection_id);
    {
      std::lock_guard<std::mutex> lock(mutex_);
      ++streams_handled_;
      stream_log_.push_back(record);
      while (stream_log_.size() > LoopbackForwardingNetwork::kStreamLogCapacity) stream_log_.pop_front();
    }

    Connection out;
    std::string egress;
    if (route.empty()) {
      if (role_ != NodeRole::Destination) return refuse("NO_ROUTE");
      // The egress: policy, resolution, per-address policy, connect.
      const EgressPolicy policy = directory_->Egress();
      std::string why;
      if (!policy.PermitsPort(target_port, &why)) return refuse("DEST_FORBIDDEN");
      net::AddressClass literal_class;
      if (net::ClassifyLiteral(target_host, &literal_class) && !policy.PermitsClass(literal_class, &why)) {
        return refuse("DEST_FORBIDDEN");
      }
      const double remaining = RemainingMs(deadline);
      if (remaining <= 0.0) return refuse("TIMEOUT");
      net::ConnectError error = net::ConnectError::None;
      SocketHandle s = net::ConnectHost(
          target_host, target_port, std::min(remaining, policy.connect_timeout_ms),
          [&policy](net::AddressClass cls, std::string* reason) { return policy.PermitsClass(cls, reason); }, &error,
          &egress, &why);
      if (s == kInvalidSocket) {
        switch (error) {
          case net::ConnectError::ResolveFailed: return refuse("DNS_FAILED");
          case net::ConnectError::NotPermitted: return refuse("DEST_FORBIDDEN");
          case net::ConnectError::Timeout: return refuse("DEST_TIMEOUT");
          default: return refuse("DEST_UNREACHABLE");
        }
      }
      out = Connection(s);
      reply("OK", egress);
    } else {
      if (role_ == NodeRole::Destination) return refuse("BAD_ROUTE");
      const std::string& next = route.front();
      {
        std::lock_guard<std::mutex> lock(record->mutex);
        record->next_hop = next;
      }
      if (!directory_->HasLink(id_, next)) return refuse("NOT_A_NEIGHBOUR");
      const LoopbackDirectory::Link link = directory_->FindLink(id_, next);
      if (!link.enabled) return refuse("LINK_DOWN");
      const std::uint16_t port = directory_->Port(next);
      if (port == 0) return refuse("NEXT_HOP_UNAVAILABLE");
      InjectDelay(link, deadline);
      const double remaining = RemainingMs(deadline);
      if (remaining <= 0.0) return refuse("TIMEOUT");
      SocketHandle upstream = net::ConnectLoopback(port, std::min(remaining, 1000.0));
      if (upstream == kInvalidSocket) return refuse("NEXT_HOP_UNAVAILABLE");
      out = Connection(upstream);
      const std::vector<std::string> rest(route.begin() + 1, route.end());
      char budget[32];
      std::snprintf(budget, sizeof(budget), "%.0f", std::max(1.0, RemainingMs(deadline)));
      if (!out.SendAll("AAFR1 TUN " + connection_id + " " + budget + " " + JoinList(rest) + " " + JoinList(trace) +
                       " " + parts[6] + "\n")) {
        return refuse("NEXT_HOP_FAILED");
      }
      std::string response;
      const ReadResult read = out.ReadLine(response, kMaxHeaderBytes, deadline, [this] { return stopping_.load(); });
      if (read == ReadResult::Timeout) return refuse("TIMEOUT");
      const std::vector<std::string> answer = Split(response, ' ');
      if (read != ReadResult::Ok || answer.size() != 6 || answer[0] != "AAFR1" || answer[1] != "TUNRSP" ||
          answer[2] != connection_id) {
        return refuse("NEXT_HOP_FAILED");
      }
      {
        std::lock_guard<std::mutex> lock(record->mutex);
        record->status = answer[3];
      }
      in.SendAll(response + "\n");  // Relay the downstream answer unchanged.
      if (answer[3] != "OK") {
        record->Finish("not_established");
        return;
      }
    }
    {
      std::lock_guard<std::mutex> lock(record->mutex);
      record->open = true;
    }
    // Opaque bidirectional copy. Anything either side sent early is flushed
    // first.
    net::SetIoTimeouts(in.socket(), directory_->options.stream_send_timeout_ms);
    net::SetIoTimeouts(out.socket(), directory_->options.stream_send_timeout_ms);
    net::SetNoDelay(in.socket());
    net::SetNoDelay(out.socket());
    const std::string early_client = in.TakeBuffered();
    const std::string early_server = out.TakeBuffered();
    if ((!early_client.empty() && !out.SendAll(early_client)) || (!early_server.empty() && !in.SendAll(early_server))) {
      record->Finish("error");
      return;
    }
    record->toward_destination += early_client.size();
    record->toward_client += early_server.size();
    const net::PumpResult pumped =
        net::Pump(in.socket(), out.socket(), directory_->options.stream_idle_timeout_ms,
                  [this] { return stopping_.load(); }, &record->toward_destination, &record->toward_client);
    record->Finish(net::ToString(pumped.end));
    if (pumped.end != net::PumpEnd::BothClosed) {
      // An abort (reset, relay shutdown, idle timeout) tears the whole chain
      // down in both directions rather than leaving half-open tunnels.
      net::AbortClose(in.Release());
      net::AbortClose(out.Release());
    }
  }

  const std::string id_;
  const NodeRole role_;
  std::shared_ptr<LoopbackDirectory> directory_;
  SocketHandle listen_{kInvalidSocket};
  std::uint16_t port_{0};
  std::atomic<bool> stopping_{false};
  bool running_{false};
  std::thread acceptor_;
  mutable std::mutex mutex_;
  std::vector<Worker> workers_;
  std::deque<std::string> log_;
  std::uint64_t handled_{0};
  std::uint64_t streams_handled_{0};
  std::atomic<std::size_t> active_streams_{0};
  std::deque<std::shared_ptr<LiveTraversal>> stream_log_;
};

// ---- Network ----

LoopbackForwardingNetwork::LoopbackForwardingNetwork(const ForwardingGraph& graph, LoopbackOptions options)
    : directory_(std::make_shared<LoopbackDirectory>()), graph_version_(graph.version()) {
  directory_->options = options;
  for (const ForwardingNode& node : graph.nodes()) {
    nodes_[node.id] = std::make_unique<LoopbackNode>(node.id, node.role, directory_);
    offline_initially_[node.id] = !node.available;
  }
  for (const ForwardingEdge& edge : graph.edges()) {
    directory_->SetLink(edge.from, edge.to, {edge.available, edge.latency_ms.value.value_or(0.0)});
  }
}

LoopbackForwardingNetwork::~LoopbackForwardingNetwork() { Stop(); }

bool LoopbackForwardingNetwork::Start(std::string* error) {
  if (running_) return true;
  for (auto& entry : nodes_) {
    if (offline_initially_[entry.first]) continue;
    if (!entry.second->Start(error)) {
      Stop();
      return false;
    }
  }
  running_ = true;
  return true;
}

void LoopbackForwardingNetwork::Stop() {
  for (auto& entry : nodes_) entry.second->Stop();
  running_ = false;
}

bool LoopbackForwardingNetwork::SetNodeOnline(const std::string& id, bool online) {
  const auto found = nodes_.find(id);
  if (found == nodes_.end()) return false;
  offline_initially_[id] = !online;
  if (!online) {
    found->second->Stop();
    return true;
  }
  return !running_ || found->second->Start(nullptr);
}

bool LoopbackForwardingNetwork::SetEdgeEnabled(const std::string& from, const std::string& to, bool enabled) {
  if (!directory_->HasLink(from, to)) return false;
  directory_->EnableLink(from, to, enabled);
  return true;
}

bool LoopbackForwardingNetwork::SyncWith(const ForwardingGraph& graph, std::string* error) {
  const auto fail = [error](const std::string& reason) {
    if (error) *error = reason;
    return false;
  };
  if (graph.nodes().size() != nodes_.size()) return fail("the graph has a different node set");
  for (const ForwardingNode& node : graph.nodes()) {
    if (nodes_.count(node.id) == 0) return fail("unknown node " + node.id);
  }
  for (const ForwardingEdge& edge : graph.edges()) {
    if (!directory_->HasLink(edge.from, edge.to)) return fail("unknown edge " + edge.from + "->" + edge.to);
  }
  for (const ForwardingEdge& edge : graph.edges()) {
    directory_->SetLink(edge.from, edge.to, {edge.available, edge.latency_ms.value.value_or(0.0)});
  }
  graph_version_ = graph.version();
  return true;
}

std::uint16_t LoopbackForwardingNetwork::PortOf(const std::string& id) const { return directory_->Port(id); }

std::uint64_t LoopbackForwardingNetwork::HandledCount(const std::string& id) const {
  const auto found = nodes_.find(id);
  return found == nodes_.end() ? 0 : found->second->handled();
}

bool LoopbackForwardingNetwork::NodeLogged(const std::string& id, const std::string& request_id) const {
  const auto found = nodes_.find(id);
  return found != nodes_.end() && found->second->Logged(request_id);
}

std::size_t LoopbackForwardingNetwork::ActiveConnections() const {
  std::size_t total = 0;
  for (const auto& entry : nodes_) total += entry.second->active();
  return total;
}

bool LoopbackForwardingNetwork::InjectsLatency() const { return directory_->options.inject_edge_latency; }

bool LoopbackForwardingNetwork::SetHiddenEdgeDelay(const std::string& from, const std::string& to, double delay_ms) {
  if (!std::isfinite(delay_ms) || delay_ms < 0.0) return false;
  return directory_->SetHiddenDelay(from, to, delay_ms);
}

void LoopbackForwardingNetwork::SetEgressPolicy(const EgressPolicy& policy) { directory_->SetEgress(policy); }

EgressPolicy LoopbackForwardingNetwork::egress_policy() const { return directory_->Egress(); }

std::vector<StreamTraversal> LoopbackForwardingNetwork::StreamTraversals(const std::string& connection_id) const {
  std::vector<StreamTraversal> out;
  for (const auto& entry : nodes_) {
    std::vector<StreamTraversal> part = entry.second->Traversals(connection_id);
    out.insert(out.end(), part.begin(), part.end());
  }
  std::sort(out.begin(), out.end(),
            [](const StreamTraversal& a, const StreamTraversal& b) { return a.opened_ns < b.opened_ns; });
  return out;
}

std::size_t LoopbackForwardingNetwork::ActiveStreams() const {
  std::size_t total = 0;
  for (const auto& entry : nodes_) total += entry.second->active_streams();
  return total;
}

std::uint64_t LoopbackForwardingNetwork::StreamsHandled(const std::string& id) const {
  const auto found = nodes_.find(id);
  return found == nodes_.end() ? 0 : found->second->streams_handled();
}

// ---- Executor ----

ForwardingCapability LoopbackForwardingExecutor::Capability() const {
  ForwardingCapability capability;
  capability.executor = "loopback_tcp";
  capability.operations = {netstate::ForwardingOperation::SelectPath};
  capability.protocols = {netstate::TransportProtocol::Tcp};
  capability.real_network = true;
  capability.loopback_only = true;
  capability.max_hops = LoopbackForwardingNetwork::kMaxHops;
  capability.byte_streams = true;
  return capability;
}

ForwardingExecutionResult LoopbackForwardingExecutor::Execute(const ForwardingRequest& request,
                                                              const ForwardingExecutionContext& context) {
  ForwardingExecutionResult result;
  result.request_id = request.request_id;
  result.requested_path = request.path;
  result.provenance = Provenance::Measured;
  const auto reject = [&](const std::string& status, const std::string& message) {
    result.status = ForwardingStatus::RequestRejected;
    result.response_status = status;
    result.message = message;
    return result;
  };
  const auto issues = ValidateRequest(request, LoopbackForwardingNetwork::kMaxHops);
  if (!issues.empty()) return reject("REJECTED", issues.front());
  if (!network_.Running()) return reject("NOT_STARTED", "the loopback relays are not running");
  if (request.graph_version != 0 && request.graph_version != network_.graph_version()) {
    return reject("STALE_GRAPH", "the relays were built from a different graph version");
  }
  const auto start = Clock::now();
  const Deadline deadline = After(request.timeout_ms);
  const auto elapsed = [&start] { return std::chrono::duration<double, std::milli>(Clock::now() - start).count(); };
  const auto interrupted = [&context] { return context.cancel && context.cancel->load(); };
  const auto finish = [&](ForwardingStatus status, const std::string& response_status, const std::string& message) {
    result.status = status;
    result.response_status = response_status;
    result.message = message;
    result.end_to_end_ms = elapsed();
    return result;
  };

  const std::uint16_t port = network_.PortOf(request.path.front());
  if (port == 0) {
    result.failed_at = request.path.front();
    return finish(ForwardingStatus::RelayUnavailable, "ENTRY_UNAVAILABLE", "the entry node is not running");
  }
  SocketHandle s = net::ConnectLoopback(port, std::min(request.timeout_ms, 1000.0));
  if (s == kInvalidSocket) {
    result.failed_at = request.path.front();
    return finish(ForwardingStatus::RelayUnavailable, "ENTRY_UNAVAILABLE", "cannot connect to the entry node");
  }
  Connection connection(s);
  const std::vector<std::string> route(request.path.begin() + 1, request.path.end());
  char budget[32];
  std::snprintf(budget, sizeof(budget), "%.0f", std::max(1.0, request.timeout_ms));
  if (!connection.SendAll("AAFR1 REQ " + request.request_id + " " + budget + " " + JoinList(route) + " -\n" +
                          request.payload + "\n")) {
    return finish(ForwardingStatus::Failed, "SEND_FAILED", "could not send the request");
  }
  std::string header;
  std::string body;
  ReadResult read = connection.ReadLine(header, kMaxHeaderBytes, deadline, interrupted);
  if (read == ReadResult::Ok) read = connection.ReadLine(body, kMaxBodyBytes, deadline, interrupted);
  if (read == ReadResult::Cancelled) return finish(ForwardingStatus::Cancelled, "CANCELLED", "cancelled by the caller");
  if (read == ReadResult::Timeout) return finish(ForwardingStatus::Timeout, "TIMEOUT", "no response before the timeout");
  if (read != ReadResult::Ok) return finish(ForwardingStatus::Failed, "CONNECTION_CLOSED", "the connection closed early");

  const std::vector<std::string> parts = Split(header, ' ');
  if (parts.size() != 5 || parts[0] != "AAFR1" || parts[1] != "RSP" || parts[2] != request.request_id) {
    return finish(ForwardingStatus::Failed, "MALFORMED_RESPONSE", "the response did not match the request");
  }
  result.confirmed_path = ParseList(parts[4]);
  result.response_body = body;
  const std::string& status = parts[3];
  if (status != "OK") {
    const std::size_t reached = result.confirmed_path.size();
    if (status == "NEXT_HOP_UNAVAILABLE" || status == "LINK_DOWN") {
      result.failed_at = reached < request.path.size() ? request.path[reached] : std::string();
      return finish(ForwardingStatus::RelayUnavailable, status, "forwarding stopped after " + PathText(result.confirmed_path));
    }
    result.failed_at = result.confirmed_path.empty() ? std::string() : result.confirmed_path.back();
    return finish(status == "TIMEOUT" ? ForwardingStatus::Timeout : ForwardingStatus::Failed, status,
                  "a node answered " + status);
  }
  bool logs_match = true;
  for (const std::string& node : result.confirmed_path) {
    logs_match = logs_match && network_.NodeLogged(node, request.request_id);
  }
  result.path_confirmed = result.confirmed_path == request.path && logs_match;
  result.evidence = "response trace from " + std::to_string(result.confirmed_path.size()) + " node(s); node logs " +
                    (logs_match ? "match" : "do not match") +
                    (network_.InjectsLatency() ? "; duration includes injected (simulated) edge delay" : "");
  return finish(ForwardingStatus::Delivered, "OK",
                result.path_confirmed ? "delivered over the requested path" : "delivered, but the path differs from the request");
}

StreamOpenResult LoopbackForwardingExecutor::OpenStream(const StreamOpenRequest& request,
                                                        const ForwardingExecutionContext& context) {
  StreamOpenResult result;
  result.connection_id = request.connection_id;
  result.requested_path = request.path;
  const auto start = Clock::now();
  const auto finish = [&](ForwardingStatus status, const std::string& response_status, const std::string& message) {
    result.status = status;
    result.response_status = response_status;
    result.message = message;
    result.establish_ms = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    if (!result.path_confirmed) result.stream.Close();
    return std::move(result);
  };
  const auto issues = ValidateStreamRequest(request, LoopbackForwardingNetwork::kMaxHops);
  if (!issues.empty()) return finish(ForwardingStatus::RequestRejected, "REJECTED", issues.front());
  if (!network_.Running()) return finish(ForwardingStatus::RequestRejected, "NOT_STARTED", "the loopback relays are not running");
  if (request.graph_version != 0 && request.graph_version != network_.graph_version()) {
    return finish(ForwardingStatus::RequestRejected, "STALE_GRAPH", "the relays were built from a different graph version");
  }
  const Deadline deadline = After(request.timeout_ms);
  const auto interrupted = [&context] { return context.cancel && context.cancel->load(); };
  const std::uint16_t port = network_.PortOf(request.path.front());
  if (port == 0) {
    result.failed_at = request.path.front();
    result.relay_failure = true;
    return finish(ForwardingStatus::RelayUnavailable, "ENTRY_UNAVAILABLE", "the entry node is not running");
  }
  SocketHandle s = net::ConnectLoopback(port, std::min(request.timeout_ms, 1000.0));
  if (s == kInvalidSocket) {
    result.failed_at = request.path.front();
    result.relay_failure = true;
    return finish(ForwardingStatus::RelayUnavailable, "ENTRY_UNAVAILABLE", "cannot connect to the entry node");
  }
  result.stream = Connection(s);
  const std::vector<std::string> route(request.path.begin() + 1, request.path.end());
  char budget[32];
  std::snprintf(budget, sizeof(budget), "%.0f", std::max(1.0, request.timeout_ms));
  if (!result.stream.SendAll("AAFR1 TUN " + request.connection_id + " " + budget + " " + JoinList(route) + " - " +
                             FormatTarget(request.target_host, request.target_port) + "\n")) {
    return finish(ForwardingStatus::Failed, "SEND_FAILED", "could not send the tunnel request");
  }
  std::string header;
  const ReadResult read = result.stream.ReadLine(header, kMaxHeaderBytes, deadline, interrupted);
  if (read == ReadResult::Cancelled) return finish(ForwardingStatus::Cancelled, "CANCELLED", "cancelled by the caller");
  if (read == ReadResult::Timeout) return finish(ForwardingStatus::Timeout, "TIMEOUT", "the tunnel was not established in time");
  if (read != ReadResult::Ok) return finish(ForwardingStatus::Failed, "CONNECTION_CLOSED", "the entry node closed the tunnel");
  const std::vector<std::string> parts = Split(header, ' ');
  if (parts.size() != 6 || parts[0] != "AAFR1" || parts[1] != "TUNRSP" || parts[2] != request.connection_id) {
    return finish(ForwardingStatus::Failed, "MALFORMED_RESPONSE", "the tunnel response did not match the request");
  }
  result.confirmed_path = ParseList(parts[4]);
  if (parts[5] != "-") result.egress_address = parts[5];
  const std::string& status = parts[3];
  if (status != "OK") {
    const std::size_t reached = result.confirmed_path.size();
    if (status == "NEXT_HOP_UNAVAILABLE" || status == "LINK_DOWN" || status == "NEXT_HOP_FAILED") {
      result.relay_failure = true;
      result.failed_at = reached < request.path.size() ? request.path[reached] : std::string();
      return finish(ForwardingStatus::RelayUnavailable, status,
                    "the tunnel stopped after " + PathText(result.confirmed_path));
    }
    result.failed_at = result.confirmed_path.empty() ? std::string() : result.confirmed_path.back();
    if (status == "TIMEOUT" || status == "DEST_TIMEOUT") {
      result.relay_failure = status == "TIMEOUT";
      return finish(ForwardingStatus::Timeout, status, "a node answered " + status);
    }
    return finish(ForwardingStatus::Failed, status, "a node answered " + status);
  }
  bool logs_match = true;
  for (const std::string& node : result.confirmed_path) {
    logs_match = logs_match && network_.NodeLogged(node, request.connection_id);
  }
  result.path_confirmed = result.confirmed_path == request.path && logs_match;
  result.evidence = "tunnel trace from " + std::to_string(result.confirmed_path.size()) + " node(s); node logs " +
                    (logs_match ? "match" : "do not match") +
                    (network_.InjectsLatency() ? "; set-up includes injected (simulated) edge delay" : "");
  if (!result.path_confirmed) {
    return finish(ForwardingStatus::Failed, "PATH_NOT_CONFIRMED",
                  "the tunnel did not traverse the requested path; it was closed unused");
  }
  return finish(ForwardingStatus::Delivered, "OK", "tunnel established over the requested path");
}

}  // namespace aafr::forwarding
