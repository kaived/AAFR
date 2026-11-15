#include "routing/forwarding/loopback_forwarding.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>
#endif

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <deque>
#include <functional>
#include <sstream>
#include <thread>

namespace aafr::forwarding {
namespace {

using Clock = std::chrono::steady_clock;
using Deadline = Clock::time_point;

#ifdef _WIN32
using Socket = SOCKET;
const Socket kInvalidSocket = INVALID_SOCKET;
void CloseSocket(Socket s) { closesocket(s); }
void SetNonBlocking(Socket s, bool on) {
  u_long mode = on ? 1 : 0;
  ioctlsocket(s, FIONBIO, &mode);
}
bool ConnectPending() {
  const int error = WSAGetLastError();
  return error == WSAEWOULDBLOCK || error == WSAEINPROGRESS;
}
constexpr int kSendFlags = 0;
struct SocketRuntime {
  SocketRuntime() {
    WSADATA data;
    ok = WSAStartup(MAKEWORD(2, 2), &data) == 0;
  }
  ~SocketRuntime() {
    if (ok) WSACleanup();
  }
  bool ok{false};
};
bool EnsureSocketRuntime() {
  static SocketRuntime runtime;
  return runtime.ok;
}
#else
using Socket = int;
const Socket kInvalidSocket = -1;
void CloseSocket(Socket s) { ::close(s); }
void SetNonBlocking(Socket s, bool on) {
  const int flags = fcntl(s, F_GETFL, 0);
  fcntl(s, F_SETFL, on ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK));
}
bool ConnectPending() { return errno == EINPROGRESS; }
constexpr int kSendFlags = MSG_NOSIGNAL;
bool EnsureSocketRuntime() { return true; }
#endif

constexpr std::size_t kMaxHeaderBytes = 1024;
constexpr std::size_t kMaxBodyBytes = ForwardingRequest::kMaxPayloadBytes + 256;
constexpr int kPollSliceMs = 20;
constexpr int kLogCapacity = 256;

double RemainingMs(Deadline deadline) {
  return std::chrono::duration<double, std::milli>(deadline - Clock::now()).count();
}

Deadline After(double ms) {
  return Clock::now() + std::chrono::microseconds(static_cast<long long>(ms * 1000.0));
}

enum class WaitResult { Ready, Timeout, Error };

WaitResult WaitSocket(Socket s, bool for_write, int timeout_ms) {
  fd_set ready;
  fd_set failed;
  FD_ZERO(&ready);
  FD_ZERO(&failed);
  FD_SET(s, &ready);
  FD_SET(s, &failed);
  timeval tv{};
  tv.tv_sec = timeout_ms / 1000;
  tv.tv_usec = (timeout_ms % 1000) * 1000;
  const int count = select(static_cast<int>(s + 1), for_write ? nullptr : &ready, for_write ? &ready : nullptr,
                           &failed, &tv);
  if (count < 0) return WaitResult::Error;
  if (count == 0) return WaitResult::Timeout;
  if (FD_ISSET(s, &failed) && !FD_ISSET(s, &ready)) return WaitResult::Error;
  return WaitResult::Ready;
}

// Connects to 127.0.0.1:port within timeout_ms; kInvalidSocket on failure.
Socket ConnectLoopback(std::uint16_t port, double timeout_ms) {
  Socket s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (s == kInvalidSocket) return kInvalidSocket;
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(port);
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  SetNonBlocking(s, true);
  if (::connect(s, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
    if (!ConnectPending() ||
        WaitSocket(s, true, static_cast<int>(std::max(1.0, timeout_ms))) != WaitResult::Ready) {
      CloseSocket(s);
      return kInvalidSocket;
    }
    int error = 0;
    socklen_t length = sizeof(error);
    getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&error), &length);
    if (error != 0) {
      CloseSocket(s);
      return kInvalidSocket;
    }
  }
  SetNonBlocking(s, false);
  return s;
}

enum class ReadResult { Ok, Timeout, Closed, TooLong, Cancelled };

// Line-oriented reader that never blocks longer than one poll slice, so
// cancellation, shutdown and deadlines are observed promptly.
class Connection {
 public:
  explicit Connection(Socket s) : socket_(s) {}
  ~Connection() {
    if (socket_ != kInvalidSocket) CloseSocket(socket_);
  }
  Connection(const Connection&) = delete;
  Connection& operator=(const Connection&) = delete;

  ReadResult ReadLine(std::string& line, std::size_t max, Deadline deadline,
                      const std::function<bool()>& interrupted) {
    for (;;) {
      const auto newline = buffer_.find('\n');
      if (newline != std::string::npos) {
        line = buffer_.substr(0, newline);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        buffer_.erase(0, newline + 1);
        return line.size() > max ? ReadResult::TooLong : ReadResult::Ok;
      }
      if (buffer_.size() > max) return ReadResult::TooLong;
      if (interrupted && interrupted()) return ReadResult::Cancelled;
      const double remaining = RemainingMs(deadline);
      if (remaining <= 0.0) return ReadResult::Timeout;
      const int slice = static_cast<int>(std::min<double>(kPollSliceMs, std::max(1.0, remaining)));
      const WaitResult waited = WaitSocket(socket_, false, slice);
      if (waited == WaitResult::Timeout) continue;
      if (waited == WaitResult::Error) return ReadResult::Closed;
      char chunk[1024];
      const int received = static_cast<int>(::recv(socket_, chunk, sizeof(chunk), 0));
      if (received <= 0) return ReadResult::Closed;
      buffer_.append(chunk, static_cast<std::size_t>(received));
    }
  }

  bool SendAll(const std::string& data) {
    std::size_t sent = 0;
    while (sent < data.size()) {
      const int n = static_cast<int>(::send(socket_, data.data() + sent, static_cast<int>(data.size() - sent), kSendFlags));
      if (n <= 0) return false;
      sent += static_cast<std::size_t>(n);
    }
    return true;
  }

 private:
  Socket socket_;
  std::string buffer_;
};

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

}  // namespace

// ---- Directory: who exists, where they listen, who may forward to whom ----

class LoopbackDirectory {
 public:
  struct Link {
    bool enabled{true};
    double latency_ms{0.0};
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
    links_[{from, to}] = link;
  }
  bool HasLink(const std::string& from, const std::string& to) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return links_.count({from, to}) != 0;
  }
  // nullopt-like: enabled false when absent.
  Link FindLink(const std::string& from, const std::string& to) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = links_.find({from, to});
    return found == links_.end() ? Link{false, 0.0} : found->second;
  }
  void EnableLink(const std::string& from, const std::string& to, bool enabled) {
    std::lock_guard<std::mutex> lock(mutex_);
    links_[{from, to}].enabled = enabled;
  }

  LoopbackOptions options;

 private:
  mutable std::mutex mutex_;
  std::map<std::string, std::uint16_t> ports_;
  std::map<std::pair<std::string, std::string>, Link> links_;
};

// ---- One node: TCP server on 127.0.0.1 ----

class LoopbackNode {
 public:
  LoopbackNode(std::string id, NodeRole role, std::shared_ptr<LoopbackDirectory> directory)
      : id_(std::move(id)), role_(role), directory_(std::move(directory)) {}
  ~LoopbackNode() { Stop(); }

  bool Start(std::string* error) {
    if (running_) return true;
    if (!EnsureSocketRuntime()) return Fail(error, "socket runtime unavailable");
    listen_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listen_ == kInvalidSocket) return Fail(error, "cannot create a socket");
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = 0;  // Ephemeral port.
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);  // Never a public interface.
    socklen_t length = sizeof(address);
    if (::bind(listen_, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0 ||
        ::listen(listen_, 16) != 0 ||
        ::getsockname(listen_, reinterpret_cast<sockaddr*>(&address), &length) != 0) {
      CloseSocket(listen_);
      listen_ = kInvalidSocket;
      return Fail(error, "cannot listen on 127.0.0.1 for node '" + id_ + "'");
    }
    port_ = ntohs(address.sin_port);
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
    CloseSocket(listen_);
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

 private:
  struct Worker {
    std::thread thread;
    std::shared_ptr<std::atomic<bool>> done;
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
      if (WaitSocket(listen_, false, 50) != WaitResult::Ready) continue;
      Socket client = ::accept(listen_, nullptr, nullptr);
      if (client == kInvalidSocket) continue;
      Reap();
      std::lock_guard<std::mutex> lock(mutex_);
      if (workers_.size() >= LoopbackForwardingNetwork::kMaxConnections) {
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

  void Handle(Socket client) {
    Connection in(client);
    const auto interrupted = [this] { return stopping_.load(); };
    std::string header;
    std::string payload;
    const Deadline read_deadline = After(2000.0);
    if (in.ReadLine(header, kMaxHeaderBytes, read_deadline, interrupted) != ReadResult::Ok ||
        in.ReadLine(payload, kMaxBodyBytes, read_deadline, interrupted) != ReadResult::Ok) {
      in.SendAll("AAFR1 RSP - BAD_REQUEST " + id_ + "\n\n");
      return;
    }
    const std::vector<std::string> parts = Split(header, ' ');
    if (parts.size() != 6 || parts[0] != "AAFR1" || parts[1] != "REQ" || !Token(parts[2])) {
      in.SendAll("AAFR1 RSP - BAD_REQUEST " + id_ + "\n\n");
      return;
    }
    const std::string& request_id = parts[2];
    double budget_ms = 0.0;
    try {
      budget_ms = std::stod(parts[3]);
    } catch (...) {
      budget_ms = 0.0;
    }
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

    if (directory_->options.inject_edge_latency) {
      const Deadline wake = After(std::min(link.latency_ms, directory_->options.max_injected_delay_ms));
      while (Clock::now() < wake && !stopping_ && RemainingMs(deadline) > 0.0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
      }
    }
    const double remaining = RemainingMs(deadline);
    if (remaining <= 0.0) return reply("TIMEOUT", "");
    Socket upstream = ConnectLoopback(port, std::min(remaining, 1000.0));
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

  const std::string id_;
  const NodeRole role_;
  std::shared_ptr<LoopbackDirectory> directory_;
  Socket listen_{kInvalidSocket};
  std::uint16_t port_{0};
  std::atomic<bool> stopping_{false};
  bool running_{false};
  std::thread acceptor_;
  mutable std::mutex mutex_;
  std::vector<Worker> workers_;
  std::deque<std::string> log_;
  std::uint64_t handled_{0};
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

// ---- Executor ----

ForwardingCapability LoopbackForwardingExecutor::Capability() const {
  ForwardingCapability capability;
  capability.executor = "loopback_tcp";
  capability.operations = {netstate::ForwardingOperation::SelectPath};
  capability.protocols = {netstate::TransportProtocol::Tcp};
  capability.real_network = true;
  capability.loopback_only = true;
  capability.max_hops = LoopbackForwardingNetwork::kMaxHops;
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
  Socket s = ConnectLoopback(port, std::min(request.timeout_ms, 1000.0));
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

}  // namespace aafr::forwarding
