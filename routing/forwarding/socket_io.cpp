#include "routing/forwarding/socket_io.h"

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
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <cerrno>
#endif

#include <algorithm>
#include <cstring>
#include <vector>

namespace aafr::forwarding::net {

#ifdef _WIN32
const SocketHandle kInvalidSocket = static_cast<SocketHandle>(INVALID_SOCKET);
namespace {
SOCKET Native(SocketHandle s) { return static_cast<SOCKET>(s); }
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
}  // namespace
bool EnsureSocketRuntime() {
  static SocketRuntime runtime;
  return runtime.ok;
}
void CloseSocket(SocketHandle s) {
  if (s != kInvalidSocket) closesocket(Native(s));
}
void SetNonBlocking(SocketHandle s, bool on) {
  u_long mode = on ? 1 : 0;
  ioctlsocket(Native(s), FIONBIO, &mode);
}
void SetIoTimeouts(SocketHandle s, int send_timeout_ms) {
  DWORD value = static_cast<DWORD>(std::max(1, send_timeout_ms));
  setsockopt(Native(s), SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&value), sizeof(value));
}
void ShutdownWrite(SocketHandle s) { shutdown(Native(s), SD_SEND); }
#else
const SocketHandle kInvalidSocket = -1;
namespace {
int Native(SocketHandle s) { return s; }
bool ConnectPending() { return errno == EINPROGRESS; }
constexpr int kSendFlags = MSG_NOSIGNAL;
}  // namespace
bool EnsureSocketRuntime() { return true; }
void CloseSocket(SocketHandle s) {
  if (s != kInvalidSocket) ::close(s);
}
void SetNonBlocking(SocketHandle s, bool on) {
  const int flags = fcntl(s, F_GETFL, 0);
  fcntl(s, F_SETFL, on ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK));
}
void SetIoTimeouts(SocketHandle s, int send_timeout_ms) {
  timeval tv{};
  tv.tv_sec = send_timeout_ms / 1000;
  tv.tv_usec = (send_timeout_ms % 1000) * 1000;
  setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}
void ShutdownWrite(SocketHandle s) { shutdown(s, SHUT_WR); }
#endif

void AbortClose(SocketHandle s) {
  if (s == kInvalidSocket) return;
  linger l{};
  l.l_onoff = 1;
  l.l_linger = 0;
  setsockopt(Native(s), SOL_SOCKET, SO_LINGER, reinterpret_cast<const char*>(&l), sizeof(l));
  CloseSocket(s);
}

void SetNoDelay(SocketHandle s) {
  int on = 1;
  setsockopt(Native(s), IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&on), sizeof(on));
}

double RemainingMs(Deadline deadline) {
  return std::chrono::duration<double, std::milli>(deadline - Clock::now()).count();
}

Deadline After(double ms) { return Clock::now() + std::chrono::microseconds(static_cast<long long>(ms * 1000.0)); }

WaitResult WaitSocket(SocketHandle s, bool for_write, int timeout_ms) {
  fd_set ready;
  fd_set failed;
  FD_ZERO(&ready);
  FD_ZERO(&failed);
  FD_SET(Native(s), &ready);
  FD_SET(Native(s), &failed);
  timeval tv{};
  tv.tv_sec = timeout_ms / 1000;
  tv.tv_usec = (timeout_ms % 1000) * 1000;
  const int count = select(static_cast<int>(Native(s) + 1), for_write ? nullptr : &ready, for_write ? &ready : nullptr,
                           &failed, &tv);
  if (count < 0) return WaitResult::Error;
  if (count == 0) return WaitResult::Timeout;
  if (FD_ISSET(Native(s), &failed) && !FD_ISSET(Native(s), &ready)) return WaitResult::Error;
  return WaitResult::Ready;
}

namespace {

// Non-blocking connect to one address within timeout_ms.
SocketHandle ConnectAddress(const sockaddr* address, int length, int family, double timeout_ms, ConnectError* error) {
  SocketHandle s = static_cast<SocketHandle>(::socket(family, SOCK_STREAM, IPPROTO_TCP));
  if (s == kInvalidSocket) {
    *error = ConnectError::Failed;
    return kInvalidSocket;
  }
  SetNonBlocking(s, true);
  if (::connect(Native(s), address, length) != 0) {
    if (!ConnectPending()) {
      CloseSocket(s);
      *error = ConnectError::Refused;
      return kInvalidSocket;
    }
    const WaitResult waited = WaitSocket(s, true, static_cast<int>(std::max(1.0, timeout_ms)));
    if (waited != WaitResult::Ready) {
      CloseSocket(s);
      *error = waited == WaitResult::Timeout ? ConnectError::Timeout : ConnectError::Refused;
      return kInvalidSocket;
    }
    int so_error = 0;
    socklen_t so_length = sizeof(so_error);
    getsockopt(Native(s), SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&so_error), &so_length);
    if (so_error != 0) {
      CloseSocket(s);
      *error = ConnectError::Refused;
      return kInvalidSocket;
    }
  }
  SetNonBlocking(s, false);
  *error = ConnectError::None;
  return s;
}

std::string NumericAddress(const sockaddr* address, socklen_t length) {
  char host[NI_MAXHOST] = {0};
  if (getnameinfo(address, length, host, sizeof(host), nullptr, 0, NI_NUMERICHOST) != 0) return std::string();
  return host;
}

bool ClassifyV4(const unsigned char* b, AddressClass* out) {
  if (b[0] == 127) *out = AddressClass::Loopback;
  else if (b[0] == 0) *out = AddressClass::Unspecified;
  else if (b[0] == 10 || (b[0] == 172 && (b[1] & 0xF0) == 16) || (b[0] == 192 && b[1] == 168) ||
           (b[0] == 100 && (b[1] & 0xC0) == 64))
    *out = AddressClass::Private;
  else if (b[0] == 169 && b[1] == 254) *out = AddressClass::LinkLocal;
  else if ((b[0] & 0xF0) == 224 || (b[0] == 255 && b[1] == 255 && b[2] == 255 && b[3] == 255))
    *out = AddressClass::Multicast;
  else if ((b[0] & 0xF0) == 240 || (b[0] == 198 && (b[1] & 0xFE) == 18) || (b[0] == 192 && b[1] == 0 && b[2] == 0))
    *out = AddressClass::Reserved;
  else *out = AddressClass::Public;
  return true;
}

bool ClassifyV6(const unsigned char* b, AddressClass* out) {
  static const unsigned char kZero[16] = {0};
  if (std::memcmp(b, kZero, 15) == 0 && b[15] == 1) *out = AddressClass::Loopback;
  else if (std::memcmp(b, kZero, 16) == 0) *out = AddressClass::Unspecified;
  else if (std::memcmp(b, kZero, 10) == 0 && b[10] == 0xFF && b[11] == 0xFF) return ClassifyV4(b + 12, out);
  else if (b[0] == 0xFF) *out = AddressClass::Multicast;
  else if ((b[0] & 0xFE) == 0xFC) *out = AddressClass::Private;
  else if (b[0] == 0xFE && (b[1] & 0xC0) == 0x80) *out = AddressClass::LinkLocal;
  else if (b[0] == 0x20 && b[1] == 0x01 && b[2] == 0x0d && b[3] == 0xb8) *out = AddressClass::Reserved;
  else *out = AddressClass::Public;
  return true;
}

bool ClassifySockaddr(const sockaddr* address, AddressClass* out) {
  if (address->sa_family == AF_INET) {
    const auto* v4 = reinterpret_cast<const sockaddr_in*>(address);
    return ClassifyV4(reinterpret_cast<const unsigned char*>(&v4->sin_addr), out);
  }
  if (address->sa_family == AF_INET6) {
    const auto* v6 = reinterpret_cast<const sockaddr_in6*>(address);
    return ClassifyV6(reinterpret_cast<const unsigned char*>(&v6->sin6_addr), out);
  }
  return false;
}

}  // namespace

SocketHandle ConnectLoopback(std::uint16_t port, double timeout_ms) {
  if (!EnsureSocketRuntime()) return kInvalidSocket;
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(port);
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  ConnectError error = ConnectError::None;
  return ConnectAddress(reinterpret_cast<const sockaddr*>(&address), sizeof(address), AF_INET, timeout_ms, &error);
}

SocketHandle ListenLoopback(std::uint16_t port, int backlog, std::uint16_t* bound_port, std::string* error) {
  const auto fail = [error](const std::string& reason) {
    if (error) *error = reason;
    return kInvalidSocket;
  };
  if (!EnsureSocketRuntime()) return fail("socket runtime unavailable");
  SocketHandle s = static_cast<SocketHandle>(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
  if (s == kInvalidSocket) return fail("cannot create a socket");
#ifdef _WIN32
  // Another process may not bind the same port while we hold it.
  int exclusive = 1;
  setsockopt(Native(s), SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));
#endif
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(port);
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);  // Never a public interface.
  socklen_t length = sizeof(address);
  if (::bind(Native(s), reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0 ||
      ::listen(Native(s), backlog) != 0 ||
      ::getsockname(Native(s), reinterpret_cast<sockaddr*>(&address), &length) != 0) {
    CloseSocket(s);
    return fail("cannot listen on 127.0.0.1:" + std::to_string(port));
  }
  if (bound_port) *bound_port = ntohs(address.sin_port);
  return s;
}

SocketHandle Accept(SocketHandle listener, std::string* peer_address, std::uint16_t* peer_port) {
  sockaddr_storage address{};
  socklen_t length = sizeof(address);
  SocketHandle s = static_cast<SocketHandle>(::accept(Native(listener), reinterpret_cast<sockaddr*>(&address), &length));
  if (s == kInvalidSocket) return s;
#ifdef _WIN32
  // Accepted sockets inherit the listener's mode; make sure it is blocking.
  SetNonBlocking(s, false);
#endif
  if (peer_address) *peer_address = NumericAddress(reinterpret_cast<const sockaddr*>(&address), length);
  if (peer_port) {
    if (address.ss_family == AF_INET) *peer_port = ntohs(reinterpret_cast<const sockaddr_in*>(&address)->sin_port);
    else if (address.ss_family == AF_INET6) *peer_port = ntohs(reinterpret_cast<const sockaddr_in6*>(&address)->sin6_port);
    else *peer_port = 0;
  }
  return s;
}

const char* ToString(AddressClass value) {
  switch (value) {
    case AddressClass::Loopback: return "loopback";
    case AddressClass::Private: return "private";
    case AddressClass::LinkLocal: return "link_local";
    case AddressClass::Public: return "public";
    case AddressClass::Unspecified: return "unspecified";
    case AddressClass::Multicast: return "multicast";
    case AddressClass::Reserved: return "reserved";
  }
  return "unknown";
}

bool ClassifyLiteral(const std::string& text, AddressClass* out) {
  if (!EnsureSocketRuntime()) return false;
  unsigned char buffer[16];
  if (inet_pton(AF_INET, text.c_str(), buffer) == 1) return ClassifyV4(buffer, out);
  if (inet_pton(AF_INET6, text.c_str(), buffer) == 1) return ClassifyV6(buffer, out);
  return false;
}

const char* ToString(ConnectError value) {
  switch (value) {
    case ConnectError::None: return "none";
    case ConnectError::ResolveFailed: return "resolve_failed";
    case ConnectError::NotPermitted: return "not_permitted";
    case ConnectError::Refused: return "refused";
    case ConnectError::Timeout: return "timeout";
    case ConnectError::Failed: return "failed";
  }
  return "unknown";
}

SocketHandle ConnectHost(const std::string& host, std::uint16_t port, double timeout_ms,
                         const std::function<bool(AddressClass, std::string*)>& permit, ConnectError* error,
                         std::string* reached_address, std::string* reason) {
  ConnectError local_error = ConnectError::None;
  ConnectError* err = error ? error : &local_error;
  if (!EnsureSocketRuntime() || port == 0) {
    *err = ConnectError::Failed;
    return kInvalidSocket;
  }
  const Deadline deadline = After(timeout_ms);
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;
  addrinfo* results = nullptr;
  const std::string service = std::to_string(port);
  if (getaddrinfo(host.c_str(), service.c_str(), &hints, &results) != 0 || !results) {
    *err = ConnectError::ResolveFailed;
    return kInvalidSocket;
  }
  // Every resolved address passes the permit check first (no DNS-rebinding
  // bypass). The permitted ones are tried Happy-Eyeballs style (RFC 8305):
  // families interleaved, the next attempt started 250 ms after the previous
  // one or as soon as it fails, the first connection to complete wins. A
  // sequential loop would pay the full failure time of an unreachable
  // address first, e.g. about 2 s on Windows for "localhost" -> ::1 when the
  // service listens on 127.0.0.1 only.
  struct Candidate {
    sockaddr_storage address{};
    socklen_t length{0};
    int family{0};
  };
  std::vector<Candidate> v6;
  std::vector<Candidate> v4;
  bool any_permitted = false;
  for (addrinfo* ai = results; ai; ai = ai->ai_next) {
    AddressClass cls = AddressClass::Reserved;
    if (!ClassifySockaddr(ai->ai_addr, &cls)) continue;
    std::string why;
    if (permit && !permit(cls, &why)) {
      if (reason && reason->empty()) *reason = why;
      continue;
    }
    any_permitted = true;
    Candidate c;
    std::memcpy(&c.address, ai->ai_addr, ai->ai_addrlen);
    c.length = static_cast<socklen_t>(ai->ai_addrlen);
    c.family = ai->ai_family;
    (ai->ai_family == AF_INET6 ? v6 : v4).push_back(c);
  }
  const int first_family = results->ai_family;
  freeaddrinfo(results);
  if (!any_permitted) {
    *err = ConnectError::NotPermitted;
    return kInvalidSocket;
  }
  std::vector<Candidate> order;
  {
    std::vector<Candidate>& a = first_family == AF_INET6 ? v6 : v4;
    std::vector<Candidate>& b = first_family == AF_INET6 ? v4 : v6;
    for (std::size_t i = 0; i < std::max(a.size(), b.size()); ++i) {
      if (i < a.size()) order.push_back(a[i]);
      if (i < b.size()) order.push_back(b[i]);
    }
  }
  constexpr double kAttemptDelayMs = 250.0;
  struct Pending {
    SocketHandle socket;
    std::size_t index;
  };
  std::vector<Pending> pending;
  std::size_t next = 0;
  Deadline next_start = Clock::now();
  ConnectError last = ConnectError::Refused;
  SocketHandle winner = kInvalidSocket;
  std::size_t winner_index = 0;
  const auto start_next = [&]() {
    while (next < order.size()) {
      const Candidate& c = order[next++];
      SocketHandle sock = static_cast<SocketHandle>(::socket(c.family, SOCK_STREAM, IPPROTO_TCP));
      if (sock == kInvalidSocket) continue;
      SetNonBlocking(sock, true);
      if (::connect(Native(sock), reinterpret_cast<const sockaddr*>(&c.address), c.length) == 0) {
        winner = sock;
        winner_index = next - 1;
        return;
      }
      if (!ConnectPending()) {
        CloseSocket(sock);
        last = ConnectError::Refused;
        continue;  // failed at once: try the next address immediately
      }
      pending.push_back({sock, next - 1});
      next_start = After(kAttemptDelayMs);
      return;
    }
  };
  start_next();
  while (winner == kInvalidSocket) {
    if (pending.empty()) {
      if (next >= order.size()) break;
      start_next();
      continue;
    }
    const double remaining = RemainingMs(deadline);
    if (remaining <= 0.0) {
      last = ConnectError::Timeout;
      break;
    }
    const double until_next = next < order.size() ? std::max(0.0, RemainingMs(next_start)) : remaining;
    const int wait_ms = static_cast<int>(std::max(1.0, std::min({remaining, until_next, 20.0})));
    fd_set writable;
    fd_set failed;
    FD_ZERO(&writable);
    FD_ZERO(&failed);
    SocketHandle highest = 0;
    for (const Pending& p : pending) {
      FD_SET(Native(p.socket), &writable);
      FD_SET(Native(p.socket), &failed);
      highest = std::max(highest, p.socket);
    }
    timeval tv{};
    tv.tv_sec = wait_ms / 1000;
    tv.tv_usec = (wait_ms % 1000) * 1000;
    const int count = select(static_cast<int>(Native(highest) + 1), nullptr, &writable, &failed, &tv);
    if (count > 0) {
      for (auto it = pending.begin(); it != pending.end();) {
        const bool done = FD_ISSET(Native(it->socket), &writable) || FD_ISSET(Native(it->socket), &failed);
        if (!done) {
          ++it;
          continue;
        }
        int so_error = 0;
        socklen_t so_length = sizeof(so_error);
        getsockopt(Native(it->socket), SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&so_error), &so_length);
        if (so_error == 0 && FD_ISSET(Native(it->socket), &writable) && winner == kInvalidSocket) {
          winner = it->socket;
          winner_index = it->index;
          it = pending.erase(it);
          continue;
        }
        CloseSocket(it->socket);
        last = ConnectError::Refused;
        it = pending.erase(it);
        if (pending.empty()) next_start = Clock::now();  // nothing in flight: next address now
      }
    }
    if (winner == kInvalidSocket && next < order.size() && Clock::now() >= next_start) start_next();
  }
  for (const Pending& p : pending) CloseSocket(p.socket);
  if (winner == kInvalidSocket) {
    *err = last;
    return kInvalidSocket;
  }
  SetNonBlocking(winner, false);
  if (reached_address) {
    const Candidate& c = order[winner_index];
    *reached_address = NumericAddress(reinterpret_cast<const sockaddr*>(&c.address), c.length);
  }
  *err = ConnectError::None;
  return winner;
}

// ---- Connection ----

Connection::~Connection() { Close(); }

Connection::Connection(Connection&& other) noexcept : socket_(other.socket_), buffer_(std::move(other.buffer_)) {
  other.socket_ = kInvalidSocket;
}

Connection& Connection::operator=(Connection&& other) noexcept {
  if (this != &other) {
    Close();
    socket_ = other.socket_;
    buffer_ = std::move(other.buffer_);
    other.socket_ = kInvalidSocket;
  }
  return *this;
}

bool Connection::valid() const { return socket_ != kInvalidSocket; }

SocketHandle Connection::Release() {
  const SocketHandle s = socket_;
  socket_ = kInvalidSocket;
  return s;
}

void Connection::Close() {
  if (socket_ != kInvalidSocket) CloseSocket(socket_);
  socket_ = kInvalidSocket;
}

std::string Connection::TakeBuffered() {
  std::string out;
  out.swap(buffer_);
  return out;
}

namespace {
constexpr int kPollSliceMs = 20;
}

ReadResult Connection::Fill(Deadline deadline, const std::function<bool()>& interrupted) {
  for (;;) {
    if (interrupted && interrupted()) return ReadResult::Cancelled;
    const double remaining = RemainingMs(deadline);
    if (remaining <= 0.0) return ReadResult::Timeout;
    const int slice = static_cast<int>(std::min<double>(kPollSliceMs, std::max(1.0, remaining)));
    const WaitResult waited = WaitSocket(socket_, false, slice);
    if (waited == WaitResult::Timeout) continue;
    if (waited == WaitResult::Error) return ReadResult::Closed;
    char chunk[16384];
    const int received = static_cast<int>(::recv(Native(socket_), chunk, sizeof(chunk), 0));
    if (received <= 0) return ReadResult::Closed;
    buffer_.append(chunk, static_cast<std::size_t>(received));
    return ReadResult::Ok;
  }
}

ReadResult Connection::ReadLine(std::string& line, std::size_t max, Deadline deadline,
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
    const ReadResult filled = Fill(deadline, interrupted);
    if (filled != ReadResult::Ok) return filled;
  }
}

ReadResult Connection::ReadHead(std::string& head, std::size_t max, Deadline deadline,
                                const std::function<bool()>& interrupted) {
  for (;;) {
    const auto end = buffer_.find("\r\n\r\n");
    if (end != std::string::npos) {
      if (end + 4 > max) return ReadResult::TooLong;
      head = buffer_.substr(0, end + 4);
      buffer_.erase(0, end + 4);
      return ReadResult::Ok;
    }
    if (buffer_.size() > max) return ReadResult::TooLong;
    const ReadResult filled = Fill(deadline, interrupted);
    if (filled != ReadResult::Ok) return filled;
  }
}

ReadResult Connection::ReadSome(std::string& out, std::size_t max, Deadline deadline,
                                const std::function<bool()>& interrupted) {
  if (buffer_.empty()) {
    const ReadResult filled = Fill(deadline, interrupted);
    if (filled != ReadResult::Ok) return filled;
  }
  const std::size_t take = std::min(max, buffer_.size());
  out.append(buffer_, 0, take);
  buffer_.erase(0, take);
  return ReadResult::Ok;
}

bool Connection::SendAll(const std::string& data) { return SendAll(data.data(), data.size()); }

bool Connection::SendAll(const char* data, std::size_t size) {
  if (socket_ == kInvalidSocket) return false;
  std::size_t sent = 0;
  while (sent < size) {
    const int n = static_cast<int>(::send(Native(socket_), data + sent, static_cast<int>(size - sent), kSendFlags));
    if (n <= 0) return false;
    sent += static_cast<std::size_t>(n);
  }
  return true;
}

// ---- Pump ----

namespace {
bool SendRaw(SocketHandle s, const char* data, std::size_t size) {
  std::size_t sent = 0;
  while (sent < size) {
    const int n = static_cast<int>(::send(Native(s), data + sent, static_cast<int>(size - sent), kSendFlags));
    if (n <= 0) return false;
    sent += static_cast<std::size_t>(n);
  }
  return true;
}
}  // namespace

const char* ToString(PumpEnd value) {
  switch (value) {
    case PumpEnd::BothClosed: return "closed";
    case PumpEnd::Idle: return "idle_timeout";
    case PumpEnd::Interrupted: return "interrupted";
    case PumpEnd::Error: return "error";
  }
  return "unknown";
}

PumpResult Pump(SocketHandle a, SocketHandle b, double idle_timeout_ms, const std::function<bool()>& interrupted,
                std::atomic<std::uint64_t>* live_a_to_b, std::atomic<std::uint64_t>* live_b_to_a,
                const std::function<void()>& on_first_b_byte) {
  PumpResult result;
  bool a_open = true;  // a may still send
  bool b_open = true;
  bool first_b = true;
  std::vector<char> buffer(64 * 1024);
  auto last_activity = Clock::now();
  while (a_open || b_open) {
    if (interrupted && interrupted()) {
      result.end = PumpEnd::Interrupted;
      return result;
    }
    if (idle_timeout_ms > 0.0 &&
        std::chrono::duration<double, std::milli>(Clock::now() - last_activity).count() > idle_timeout_ms) {
      result.end = PumpEnd::Idle;
      return result;
    }
    fd_set readable;
    FD_ZERO(&readable);
    if (a_open) FD_SET(Native(a), &readable);
    if (b_open) FD_SET(Native(b), &readable);
    timeval tv{};
    tv.tv_usec = kPollSliceMs * 1000;
    // nfds is ignored on Windows; elsewhere the larger descriptor suffices.
    const int highest = static_cast<int>(std::max(Native(a), Native(b)));
    const int count = select(highest + 1, &readable, nullptr, nullptr, &tv);
    if (count < 0) {
      result.end = PumpEnd::Error;
      return result;
    }
    if (count == 0) continue;
    const auto move = [&](SocketHandle from, SocketHandle to, bool& open, std::uint64_t& total,
                          std::atomic<std::uint64_t>* live, const char* name, bool from_b) -> bool {
      const int n = static_cast<int>(::recv(Native(from), buffer.data(), static_cast<int>(buffer.size()), 0));
      if (n == 0) {
        open = false;
        if (result.first_closed.empty()) result.first_closed = name;
        ShutdownWrite(to);
        return true;
      }
      if (n < 0) return false;
      if (from_b && first_b) {
        first_b = false;
        if (on_first_b_byte) on_first_b_byte();
      }
      if (!SendRaw(to, buffer.data(), static_cast<std::size_t>(n))) return false;
      total += static_cast<std::uint64_t>(n);
      if (live) live->fetch_add(static_cast<std::uint64_t>(n));
      last_activity = Clock::now();
      return true;
    };
    if (a_open && FD_ISSET(Native(a), &readable) && !move(a, b, a_open, result.a_to_b, live_a_to_b, "a", false)) {
      result.end = PumpEnd::Error;
      return result;
    }
    if (b_open && FD_ISSET(Native(b), &readable) && !move(b, a, b_open, result.b_to_a, live_b_to_a, "b", true)) {
      result.end = PumpEnd::Error;
      return result;
    }
  }
  result.end = PumpEnd::BothClosed;
  return result;
}

std::string PeerAddress(SocketHandle s, std::uint16_t* port) {
  sockaddr_storage address{};
  socklen_t length = sizeof(address);
  if (getpeername(Native(s), reinterpret_cast<sockaddr*>(&address), &length) != 0) return std::string();
  if (port) {
    if (address.ss_family == AF_INET) *port = ntohs(reinterpret_cast<const sockaddr_in*>(&address)->sin_port);
    else if (address.ss_family == AF_INET6) *port = ntohs(reinterpret_cast<const sockaddr_in6*>(&address)->sin6_port);
  }
  return NumericAddress(reinterpret_cast<const sockaddr*>(&address), length);
}

std::uint16_t LocalPort(SocketHandle s) {
  sockaddr_storage address{};
  socklen_t length = sizeof(address);
  if (getsockname(Native(s), reinterpret_cast<sockaddr*>(&address), &length) != 0) return 0;
  if (address.ss_family == AF_INET) return ntohs(reinterpret_cast<const sockaddr_in*>(&address)->sin_port);
  if (address.ss_family == AF_INET6) return ntohs(reinterpret_cast<const sockaddr_in6*>(&address)->sin6_port);
  return 0;
}

bool PeerClosed(SocketHandle s) {
  if (WaitSocket(s, false, 0) != WaitResult::Ready) return false;
  char probe;
  const int n = static_cast<int>(::recv(Native(s), &probe, 1, MSG_PEEK));
  return n <= 0;
}

std::string HostName() {
  if (!EnsureSocketRuntime()) return std::string();
  char host[256] = {0};
  if (gethostname(host, sizeof(host) - 1) != 0) return std::string();
  return host;
}

}  // namespace aafr::forwarding::net
