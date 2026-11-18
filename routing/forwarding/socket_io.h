#pragma once

// Socket primitives shared by the loopback relays and the V0.7.3 browser
// forwarding gateway. Plain C++17 over Winsock / BSD sockets; no Qt.
//
// Everything here is bounded: every wait has a deadline, reads poll in short
// slices so cancellation and shutdown are observed promptly, and sends carry
// a socket-level timeout so a peer that stops reading cannot block a worker
// forever.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <string>

namespace aafr::forwarding::net {

using Clock = std::chrono::steady_clock;
using Deadline = Clock::time_point;

#ifdef _WIN32
using SocketHandle = std::uintptr_t;  // SOCKET
#else
using SocketHandle = int;
#endif
extern const SocketHandle kInvalidSocket;

bool EnsureSocketRuntime();
void CloseSocket(SocketHandle s);
void SetNonBlocking(SocketHandle s, bool on);
// Bounds a blocking send (and recv) on the socket.
void SetIoTimeouts(SocketHandle s, int send_timeout_ms);
void SetNoDelay(SocketHandle s);
void ShutdownWrite(SocketHandle s);
// Closes with a reset (SO_LINGER 0), so the peer sees an abort, not a
// half-close.
void AbortClose(SocketHandle s);

double RemainingMs(Deadline deadline);
Deadline After(double ms);

enum class WaitResult { Ready, Timeout, Error };
WaitResult WaitSocket(SocketHandle s, bool for_write, int timeout_ms);

// Connects to 127.0.0.1:port within timeout_ms; kInvalidSocket on failure.
SocketHandle ConnectLoopback(std::uint16_t port, double timeout_ms);

// Binds 127.0.0.1:port (0 = ephemeral) and listens. Never a public interface.
SocketHandle ListenLoopback(std::uint16_t port, int backlog, std::uint16_t* bound_port, std::string* error);
// Accepts one pending connection; kInvalidSocket when none is ready.
SocketHandle Accept(SocketHandle listener, std::string* peer_address, std::uint16_t* peer_port);

// What an IP address is, for destination policy (egress_policy.h).
enum class AddressClass {
  Loopback,     // 127.0.0.0/8, ::1
  Private,      // RFC 1918, fc00::/7, 100.64.0.0/10 (CGNAT)
  LinkLocal,    // 169.254.0.0/16, fe80::/10
  Public,
  Unspecified,  // 0.0.0.0/8, ::
  Multicast,    // 224.0.0.0/4, ff00::/8, broadcast
  Reserved,     // 240.0.0.0/4 and other non-routable ranges
};
const char* ToString(AddressClass value);
// Classifies a numeric IPv4 / IPv6 literal (IPv6 without brackets). False
// when the text is not an IP literal.
bool ClassifyLiteral(const std::string& text, AddressClass* out);

enum class ConnectError { None, ResolveFailed, NotPermitted, Refused, Timeout, Failed };
const char* ToString(ConnectError value);

// Resolves host (name or literal) and connects to the first resolved address
// that `permit` accepts, within timeout_ms overall. `permit` sees every
// resolved address, so a name that resolves to a forbidden range is refused
// after resolution (no DNS-rebinding bypass). On success returns a connected,
// blocking socket and the numeric address it reached.
SocketHandle ConnectHost(const std::string& host, std::uint16_t port, double timeout_ms,
                         const std::function<bool(AddressClass, std::string* reason)>& permit, ConnectError* error,
                         std::string* reached_address, std::string* reason);

enum class ReadResult { Ok, Timeout, Closed, TooLong, Cancelled };

// Owns one socket. Line-oriented reads never block longer than one poll
// slice. Bytes read past a line stay buffered (TakeBuffered).
class Connection {
 public:
  Connection() = default;
  explicit Connection(SocketHandle s) : socket_(s) {}
  ~Connection();
  Connection(Connection&& other) noexcept;
  Connection& operator=(Connection&& other) noexcept;
  Connection(const Connection&) = delete;
  Connection& operator=(const Connection&) = delete;

  ReadResult ReadLine(std::string& line, std::size_t max, Deadline deadline, const std::function<bool()>& interrupted);
  // Reads until "\r\n\r\n" (inclusive) into head; extra bytes stay buffered.
  ReadResult ReadHead(std::string& head, std::size_t max, Deadline deadline, const std::function<bool()>& interrupted);
  // Appends at most max bytes (buffered bytes first) once any are available.
  ReadResult ReadSome(std::string& out, std::size_t max, Deadline deadline, const std::function<bool()>& interrupted);
  bool SendAll(const std::string& data);
  bool SendAll(const char* data, std::size_t size);
  std::string TakeBuffered();
  bool valid() const;
  SocketHandle socket() const { return socket_; }
  // Gives up ownership without closing.
  SocketHandle Release();
  void Close();

 private:
  ReadResult Fill(Deadline deadline, const std::function<bool()>& interrupted);
  SocketHandle socket_{kInvalidSocket};
  std::string buffer_;
};

enum class PumpEnd {
  BothClosed,   // Each side finished sending (orderly).
  Idle,         // No byte in either direction for idle_timeout_ms.
  Interrupted,  // `interrupted` returned true.
  Error,        // A socket failed or reset.
};
const char* ToString(PumpEnd value);

struct PumpResult {
  std::uint64_t a_to_b{0};
  std::uint64_t b_to_a{0};
  PumpEnd end{PumpEnd::BothClosed};
  // Which side ended the exchange first: "a", "b" or "".
  std::string first_closed;
};

// Copies bytes in both directions between a and b until both have closed
// their sending side (a half-close is propagated), no byte moves for
// idle_timeout_ms, `interrupted` is true or a socket fails. The live
// counters, when given, are updated as bytes move. `on_first_b_byte` runs
// once, before the first byte from b is forwarded.
PumpResult Pump(SocketHandle a, SocketHandle b, double idle_timeout_ms, const std::function<bool()>& interrupted,
                std::atomic<std::uint64_t>* live_a_to_b = nullptr, std::atomic<std::uint64_t>* live_b_to_a = nullptr,
                const std::function<void()>& on_first_b_byte = {});

// The peer address of a connected socket ("" when unknown).
std::string PeerAddress(SocketHandle s, std::uint16_t* port = nullptr);
std::uint16_t LocalPort(SocketHandle s);
// True when the peer has closed its sending side or reset (checked without
// blocking or consuming data).
bool PeerClosed(SocketHandle s);
// This host's name ("" when unavailable).
std::string HostName();

}  // namespace aafr::forwarding::net
