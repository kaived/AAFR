#pragma once

// V0.7.3 destination policy for byte streams that leave the controlled
// forwarding layer. Plain C++17.
//
// A browser connection tunnelled through the relays ends at the graph's
// Destination node, which opens the TCP connection to the real destination
// (the egress). The policy is checked twice: by the browser gateway on the
// requested host and port before anything is forwarded, and by the
// Destination node on every address the name resolves to, immediately before
// connecting, so a public name that resolves to a loopback or private address
// is still refused (no DNS-rebinding bypass). The defaults allow only public
// addresses on ports 80 and 443: the gateway is not an open proxy into local
// or private services.

#include <cstdint>
#include <set>
#include <string>

#include "routing/forwarding/socket_io.h"

namespace aafr::forwarding {

struct EgressPolicy {
  bool allow_public{true};
  bool allow_private{false};
  bool allow_loopback{false};
  bool allow_link_local{false};
  // Empty: any port 1..65535 except denied_ports.
  std::set<std::uint16_t> allowed_ports{80, 443};
  std::set<std::uint16_t> denied_ports;
  double connect_timeout_ms{10'000.0};

  bool PermitsPort(std::uint16_t port, std::string* reason = nullptr) const;
  // Unspecified, multicast and reserved addresses are never permitted.
  bool PermitsClass(net::AddressClass cls, std::string* reason = nullptr) const;

  // For controlled experiments with local test servers: loopback allowed on
  // any port.
  static EgressPolicy LocalTesting();
};

// Host syntax a stream target may use: a DNS name (letters, digits, '-', '.',
// '_'; labels of 1..63 characters, at most 253 overall) or an IPv4 / IPv6
// literal (IPv6 without brackets). Nothing else reaches a resolver.
bool ValidTargetHost(const std::string& host);

// "host:port", with IPv6 literals in brackets.
std::string FormatTarget(const std::string& host, std::uint16_t port);
// Inverse of FormatTarget; false when malformed or the host is invalid.
bool ParseTarget(const std::string& text, std::string* host, std::uint16_t* port);

}  // namespace aafr::forwarding
