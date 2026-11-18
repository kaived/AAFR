#include "routing/forwarding/egress_policy.h"

#include <algorithm>
#include <cctype>

namespace aafr::forwarding {

bool EgressPolicy::PermitsPort(std::uint16_t port, std::string* reason) const {
  const auto refuse = [reason](const std::string& why) {
    if (reason) *reason = why;
    return false;
  };
  if (port == 0) return refuse("port 0 is not a destination");
  if (denied_ports.count(port) != 0) return refuse("port " + std::to_string(port) + " is denied");
  if (!allowed_ports.empty() && allowed_ports.count(port) == 0) {
    return refuse("port " + std::to_string(port) + " is not in the allowed set");
  }
  return true;
}

bool EgressPolicy::PermitsClass(net::AddressClass cls, std::string* reason) const {
  bool allowed = false;
  switch (cls) {
    case net::AddressClass::Public: allowed = allow_public; break;
    case net::AddressClass::Private: allowed = allow_private; break;
    case net::AddressClass::Loopback: allowed = allow_loopback; break;
    case net::AddressClass::LinkLocal: allowed = allow_link_local; break;
    case net::AddressClass::Unspecified:
    case net::AddressClass::Multicast:
    case net::AddressClass::Reserved: allowed = false; break;
  }
  if (!allowed && reason) *reason = std::string(net::ToString(cls)) + " destinations are not permitted";
  return allowed;
}

EgressPolicy EgressPolicy::LocalTesting() {
  EgressPolicy policy;
  policy.allow_loopback = true;
  policy.allowed_ports.clear();
  policy.connect_timeout_ms = 5'000.0;
  return policy;
}

bool ValidTargetHost(const std::string& host) {
  if (host.empty() || host.size() > 253) return false;
  net::AddressClass cls;
  if (host.find(':') != std::string::npos) return net::ClassifyLiteral(host, &cls);
  std::size_t label = 0;
  for (std::size_t i = 0; i < host.size(); ++i) {
    const unsigned char c = static_cast<unsigned char>(host[i]);
    if (c == '.') {
      if (label == 0) return false;
      label = 0;
      continue;
    }
    if (!(std::isalnum(c) || c == '-' || c == '_')) return false;
    if (++label > 63) return false;
  }
  return label > 0 || host.back() == '.';
}

std::string FormatTarget(const std::string& host, std::uint16_t port) {
  const bool v6 = host.find(':') != std::string::npos;
  return (v6 ? "[" + host + "]" : host) + ":" + std::to_string(port);
}

bool ParseTarget(const std::string& text, std::string* host, std::uint16_t* port) {
  std::string h;
  std::string p;
  if (!text.empty() && text.front() == '[') {
    const auto close = text.find(']');
    if (close == std::string::npos || close + 1 >= text.size() || text[close + 1] != ':') return false;
    h = text.substr(1, close - 1);
    p = text.substr(close + 2);
    if (h.find(':') == std::string::npos) return false;
  } else {
    const auto colon = text.rfind(':');
    if (colon == std::string::npos || colon == 0) return false;
    h = text.substr(0, colon);
    p = text.substr(colon + 1);
    if (h.find(':') != std::string::npos) return false;  // Unbracketed IPv6.
  }
  if (p.empty() || p.size() > 5 || !std::all_of(p.begin(), p.end(), [](char c) { return c >= '0' && c <= '9'; })) {
    return false;
  }
  const unsigned long value = std::stoul(p);
  if (value == 0 || value > 65535) return false;
  if (!ValidTargetHost(h)) return false;
  if (host) *host = h;
  if (port) *port = static_cast<std::uint16_t>(value);
  return true;
}

}  // namespace aafr::forwarding
