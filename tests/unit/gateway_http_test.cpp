// V0.7.3 gateway unit tests: proxy request parsing and validation (including
// request-smuggling ambiguities), origin-form rebuilding, response framing,
// the chunked-body tracker, destination policy and address classification.
// No sockets are opened except by the loopback-binding and lifecycle tests.

#include <string>

#include "routing/forwarding/egress_policy.h"
#include "routing/forwarding/socket_io.h"
#include "routing/gateway/browser_forwarding_gateway.h"
#include "routing/gateway/proxy_http.h"
#include "tests/support/test_support.h"

using namespace aafr;
using namespace aafr::gateway;
namespace net = aafr::forwarding::net;

namespace {

ParseOutcome Parse(const std::string& head, ProxyRequest* out = nullptr) {
  ProxyRequest r;
  return ParseProxyRequestHead(head, out ? out : &r);
}

void ValidAbsoluteFormGet() {
  ProxyRequest r;
  const auto p = Parse("GET http://Example.COM:8080/a/b?x=1 HTTP/1.1\r\nHost: example.com:8080\r\n"
                       "Proxy-Connection: keep-alive\r\nCookie: secret=1\r\n\r\n",
                       &r);
  CHECK(p.ok);
  CHECK(r.kind == ProxyRequestKind::Forward);
  CHECK(r.target_host == "Example.COM" || r.target_host == "example.com");
  CHECK(r.target_port == 8080);
  CHECK(r.origin_form == "/a/b?x=1");
  const std::string rebuilt = BuildOriginRequestHead(r);
  CHECK(rebuilt.rfind("GET /a/b?x=1 HTTP/1.1\r\n", 0) == 0);
  CHECK(aafr::test::Contains(rebuilt, "\r\nConnection: close\r\n\r\n"));
  CHECK(!aafr::test::Contains(rebuilt, "Proxy-Connection"));
  CHECK(aafr::test::Contains(rebuilt, "Cookie: secret=1"));  // End-to-end headers pass unchanged.
  // Default port and a bare authority.
  CHECK(Parse("GET http://example.com HTTP/1.1\r\nHost: example.com\r\n\r\n", &r).ok);
  CHECK(r.target_port == 80 && r.origin_form == "/");
  CHECK(Parse("GET http://example.com?q HTTP/1.1\r\n\r\n", &r).ok && r.origin_form == "/?q");
  CHECK(Parse("GET http://[::1]:8081/x HTTP/1.1\r\n\r\n", &r).ok && r.target_host == "::1" && r.target_port == 8081);
}

void ValidConnect() {
  ProxyRequest r;
  CHECK(Parse("CONNECT example.com:443 HTTP/1.1\r\nHost: example.com:443\r\n\r\n", &r).ok);
  CHECK(r.kind == ProxyRequestKind::Connect && r.target_host == "example.com" && r.target_port == 443);
  CHECK(Parse("CONNECT [2001:db8::1]:443 HTTP/1.1\r\n\r\n", &r).ok && r.target_host == "2001:db8::1");
  CHECK(Parse("CONNECT 127.0.0.1:8443 HTTP/1.1\r\n\r\n", &r).ok && r.target_port == 8443);
}

void InvalidConnectTargets() {
  CHECK(!Parse("CONNECT example.com HTTP/1.1\r\n\r\n").ok);          // no port
  CHECK(!Parse("CONNECT example.com:0 HTTP/1.1\r\n\r\n").ok);
  CHECK(!Parse("CONNECT example.com:99999 HTTP/1.1\r\n\r\n").ok);
  CHECK(!Parse("CONNECT exa mple.com:443 HTTP/1.1\r\n\r\n").ok);
  CHECK(!Parse("CONNECT user@example.com:443 HTTP/1.1\r\n\r\n").ok);
  CHECK(!Parse("CONNECT ::1:443 HTTP/1.1\r\n\r\n").ok);              // unbracketed IPv6
  CHECK(!Parse("CONNECT example.com:443 HTTP/1.1\r\nContent-Length: 5\r\n\r\n").ok);
}

void MalformedRequests() {
  CHECK(Parse("GET /page HTTP/1.1\r\nHost: x\r\n\r\n").reason == "origin_form_to_proxy");
  CHECK(Parse("GET https://example.com/ HTTP/1.1\r\n\r\n").reason == "https_requires_connect");
  CHECK(Parse("GET ftp://example.com/ HTTP/1.1\r\n\r\n").reason == "unsupported_scheme");
  CHECK(Parse("GET http://user:pw@example.com/ HTTP/1.1\r\n\r\n").reason == "invalid_authority");
  CHECK(Parse("GET http://example.com/#frag HTTP/1.1\r\n\r\n").reason == "fragment_in_target");
  CHECK(Parse("GET  http://example.com/ HTTP/1.1\r\n\r\n").reason == "malformed_request_line");
  CHECK(Parse("GET http://example.com/ HTTP/2.0\r\n\r\n").status == 505);
  CHECK(Parse("TRACE http://example.com/ HTTP/1.1\r\n\r\n").status == 405);
  CHECK(Parse("BREW http://example.com/ HTTP/1.1\r\n\r\n").status == 405);
  CHECK(Parse("GET http://example.com/ HTTP/1.1\nHost: x\r\n\r\n").reason == "bad_line_endings");  // bare LF
  CHECK(Parse("GET http://example.com/ HTTP/1.1\r\nHost: x\rY: z\r\n\r\n").reason == "bad_line_endings");
  CHECK(Parse("GET http://example.com/ HTTP/1.1\r\nHost: x\r\n").reason == "bad_line_endings");  // incomplete
  CHECK(Parse("GET http://example.com/ HTTP/1.1\r\nX: a\r\n b\r\n\r\n").reason == "obs_fold");
  CHECK(Parse("GET http://example.com/ HTTP/1.1\r\nHost : x\r\n\r\n").reason == "invalid_header_name");
  CHECK(Parse("GET http://example.com/ HTTP/1.1\r\nX: a\x01" "b\r\n\r\n").reason == "control_character");
  CHECK(Parse("GET http://example.com/ HTTP/1.1\r\nHost: a\r\nHost: b\r\n\r\n").reason == "multiple_host_headers");
  std::string many = "GET http://example.com/ HTTP/1.1\r\n";
  for (int i = 0; i < 101; ++i) many += "X-" + std::to_string(i) + ": v\r\n";
  CHECK(Parse(many + "\r\n").reason == "too_many_headers");
}

void SmugglingAmbiguitiesRejected() {
  CHECK(Parse("POST http://example.com/ HTTP/1.1\r\nContent-Length: 4\r\nTransfer-Encoding: chunked\r\n\r\n").reason ==
        "smuggling_ambiguity");
  CHECK(Parse("POST http://example.com/ HTTP/1.1\r\nContent-Length: 4\r\nContent-Length: 5\r\n\r\n").reason ==
        "conflicting_content_length");
  CHECK(Parse("POST http://example.com/ HTTP/1.1\r\nContent-Length: 4, 4\r\n\r\n").reason == "invalid_content_length");
  CHECK(Parse("POST http://example.com/ HTTP/1.1\r\nContent-Length: -1\r\n\r\n").reason == "invalid_content_length");
  CHECK(Parse("POST http://example.com/ HTTP/1.1\r\nContent-Length: +4\r\n\r\n").reason == "invalid_content_length");
  const auto te = Parse("POST http://example.com/ HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n");
  CHECK(!te.ok && te.status == 501);
  // Identical repeated lengths are the same message.
  ProxyRequest r;
  CHECK(Parse("POST http://example.com/ HTTP/1.1\r\nContent-Length: 4\r\nContent-Length: 4\r\n\r\n", &r).ok);
  CHECK(r.content_length && *r.content_length == 4);
}

void HopByHopRemoved() {
  ProxyRequest r;
  CHECK(Parse("GET http://example.com/ HTTP/1.1\r\nHost: evil.example\r\nConnection: X-Secret, keep-alive\r\n"
              "X-Secret: 1\r\nKeep-Alive: timeout=5\r\nUpgrade: websocket\r\nTE: trailers\r\n"
              "Proxy-Authorization: Basic abc\r\nAccept: */*\r\n\r\n",
              &r).ok);
  const std::string out = BuildOriginRequestHead(r);
  CHECK(aafr::test::Contains(out, "\r\nHost: example.com\r\n"));  // Host follows the target.
  CHECK(!aafr::test::Contains(out, "evil.example"));
  CHECK(!aafr::test::Contains(out, "X-Secret"));
  CHECK(!aafr::test::Contains(out, "Keep-Alive"));
  CHECK(!aafr::test::Contains(out, "Upgrade"));
  CHECK(!aafr::test::Contains(out, "TE:"));
  CHECK(!aafr::test::Contains(out, "Proxy-Authorization"));
  CHECK(aafr::test::Contains(out, "Accept: */*"));
}

void ResponseFramingRules() {
  ResponseHead h;
  std::string why;
  CHECK(ParseResponseHead("HTTP/1.1 200 OK\r\nContent-Length: 10\r\nConnection: keep-alive\r\n\r\n", &h, &why));
  std::uint64_t length = 0;
  CHECK(ResponseFraming(h, "GET", &length) == BodyFraming::ContentLength && length == 10);
  CHECK(ResponseFraming(h, "HEAD", &length) == BodyFraming::None);
  const std::string client = BuildClientResponseHead(h);
  CHECK(aafr::test::Contains(client, "Connection: close"));
  CHECK(!aafr::test::Contains(client, "keep-alive"));
  CHECK(ParseResponseHead("HTTP/1.1 204 No Content\r\n\r\n", &h, &why));
  CHECK(ResponseFraming(h, "GET", &length) == BodyFraming::None);
  CHECK(ParseResponseHead("HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip, chunked\r\n\r\n", &h, &why));
  CHECK(ResponseFraming(h, "GET", &length) == BodyFraming::Chunked);
  CHECK(ParseResponseHead("HTTP/1.0 200 OK\r\n\r\n", &h, &why));
  CHECK(ResponseFraming(h, "GET", &length) == BodyFraming::UntilClose);
  CHECK(ParseResponseHead("HTTP/1.1 200 OK\r\nContent-Length: 1\r\nContent-Length: 2\r\n\r\n", &h, &why));
  CHECK(ResponseFraming(h, "GET", &length) == BodyFraming::Invalid);
  CHECK(!ParseResponseHead("HTTP/1.1 2000 OK\r\n\r\n", &h, &why));
  CHECK(!ParseResponseHead("ICY 200 OK\r\n\r\n", &h, &why));
  CHECK(ParseResponseHead("HTTP/1.1 200\r\n\r\n", &h, &why) && h.status == 200);
}

void ChunkedTrackerFindsEnd() {
  const std::string body = "4\r\nWiki\r\n5;ext=1\r\npedia\r\n0\r\nTrailer: x\r\n\r\nEXTRA";
  {
    ChunkedTracker t;
    const std::size_t used = t.Feed(body.data(), body.size());
    CHECK(t.done() && !t.failed());
    CHECK(used == body.size() - 5);  // Stops before the extra bytes.
  }
  {
    ChunkedTracker t;  // Byte by byte.
    std::size_t used = 0;
    for (std::size_t i = 0; i < body.size() && !t.done(); ++i) used += t.Feed(body.data() + i, 1);
    CHECK(t.done() && used == body.size() - 5);
  }
  {
    ChunkedTracker t;
    t.Feed("zz\r\n", 4);
    CHECK(t.failed());
  }
  {
    ChunkedTracker t;
    t.Feed("3\r\nabcX", 7);  // Missing CRLF after data.
    CHECK(t.failed());
  }
  {
    ChunkedTracker t;
    t.Feed("ffffffffffffffff\r\n", 18);  // Size overflow guard.
    CHECK(t.failed());
  }
}

void GatewayErrorsEchoNothing() {
  const std::string r = GatewayErrorResponse(403, "destination_forbidden");
  CHECK(r.rfind("HTTP/1.1 403 Forbidden\r\n", 0) == 0);
  CHECK(aafr::test::Contains(r, "X-AAFR-Reason: destination_forbidden"));
  CHECK(aafr::test::Contains(r, "Connection: close"));
}

void TargetSyntax() {
  using forwarding::ValidTargetHost;
  CHECK(ValidTargetHost("example.com"));
  CHECK(ValidTargetHost("a-b_c.example"));
  CHECK(ValidTargetHost("127.0.0.1"));
  CHECK(ValidTargetHost("::1"));
  CHECK(!ValidTargetHost(""));
  CHECK(!ValidTargetHost("bad host"));
  CHECK(!ValidTargetHost("a..b"));
  CHECK(!ValidTargetHost("x\ny"));
  CHECK(!ValidTargetHost(std::string(64, 'a') + ".com"));
  CHECK(!ValidTargetHost("not:an:ip"));
  std::string host;
  std::uint16_t port = 0;
  CHECK(forwarding::ParseTarget("[::1]:443", &host, &port) && host == "::1" && port == 443);
  CHECK(forwarding::FormatTarget("::1", 443) == "[::1]:443");
  CHECK(!forwarding::ParseTarget("host:", &host, &port));
  CHECK(!forwarding::ParseTarget("host:12a", &host, &port));
}

void AddressClassification() {
  net::AddressClass c;
  CHECK(net::ClassifyLiteral("127.0.0.1", &c) && c == net::AddressClass::Loopback);
  CHECK(net::ClassifyLiteral("127.8.9.10", &c) && c == net::AddressClass::Loopback);
  CHECK(net::ClassifyLiteral("::1", &c) && c == net::AddressClass::Loopback);
  CHECK(net::ClassifyLiteral("::ffff:127.0.0.1", &c) && c == net::AddressClass::Loopback);  // v4-mapped
  CHECK(net::ClassifyLiteral("10.1.2.3", &c) && c == net::AddressClass::Private);
  CHECK(net::ClassifyLiteral("172.16.0.1", &c) && c == net::AddressClass::Private);
  CHECK(net::ClassifyLiteral("172.32.0.1", &c) && c == net::AddressClass::Public);
  CHECK(net::ClassifyLiteral("192.168.1.1", &c) && c == net::AddressClass::Private);
  CHECK(net::ClassifyLiteral("100.64.0.1", &c) && c == net::AddressClass::Private);
  CHECK(net::ClassifyLiteral("fd00::1", &c) && c == net::AddressClass::Private);
  CHECK(net::ClassifyLiteral("169.254.1.1", &c) && c == net::AddressClass::LinkLocal);
  CHECK(net::ClassifyLiteral("fe80::1", &c) && c == net::AddressClass::LinkLocal);
  CHECK(net::ClassifyLiteral("0.0.0.0", &c) && c == net::AddressClass::Unspecified);
  CHECK(net::ClassifyLiteral("224.0.0.1", &c) && c == net::AddressClass::Multicast);
  CHECK(net::ClassifyLiteral("255.255.255.255", &c) && c == net::AddressClass::Multicast);
  CHECK(net::ClassifyLiteral("93.184.216.34", &c) && c == net::AddressClass::Public);
  CHECK(!net::ClassifyLiteral("example.com", &c));
}

void EgressPolicyDefaults() {
  forwarding::EgressPolicy p;  // Default: public only, ports 80 and 443.
  CHECK(p.PermitsPort(443) && p.PermitsPort(80));
  CHECK(!p.PermitsPort(22) && !p.PermitsPort(0));
  CHECK(p.PermitsClass(net::AddressClass::Public));
  CHECK(!p.PermitsClass(net::AddressClass::Loopback));
  CHECK(!p.PermitsClass(net::AddressClass::Private));
  CHECK(!p.PermitsClass(net::AddressClass::LinkLocal));
  CHECK(!p.PermitsClass(net::AddressClass::Unspecified));
  CHECK(!p.PermitsClass(net::AddressClass::Multicast));
  forwarding::EgressPolicy local = forwarding::EgressPolicy::LocalTesting();
  CHECK(local.PermitsClass(net::AddressClass::Loopback) && local.PermitsPort(8443));
  CHECK(!local.PermitsClass(net::AddressClass::Multicast));  // Never.
  local.denied_ports.insert(25);
  CHECK(!local.PermitsPort(25));
}

void GatewayConfigValidation() {
  GatewayConfig c;
  CHECK(c.Validate().empty());
  c.max_clients = 0;
  CHECK(!c.Validate().empty());
  c = GatewayConfig{};
  c.session_id = "bad id";
  CHECK(!c.Validate().empty());
  c = GatewayConfig{};
  c.head_timeout_ms = 0;
  CHECK(!c.Validate().empty());
}

void GatewayBindsLoopbackOnly() {
  GatewayConfig config;
  config.session_id = "unit";
  AAFRBrowserForwardingGateway gateway(config);
  std::string error;
  CHECK(gateway.Bind(&error));
  CHECK(gateway.port() != 0);
  const GatewayStatus s = gateway.Status();
  CHECK(s.bound && !s.running && s.listen_address == "127.0.0.1:" + std::to_string(gateway.port()));
  CHECK(gateway.Start(&error));
  CHECK(gateway.Status().running);
  CHECK(gateway.Start(&error));  // Idempotent.
  // Reachable on loopback.
  const net::SocketHandle ok = net::ConnectLoopback(gateway.port(), 1000);
  CHECK(ok != net::kInvalidSocket);
  net::CloseSocket(ok);
  // Not reachable through any other local address (when the host has one).
  std::string reason;
  net::ConnectError ce = net::ConnectError::None;
  std::string reached;
  int other_addresses = 0;
  const std::string host = net::HostName();
  if (!host.empty()) {
    const net::SocketHandle s = net::ConnectHost(
        host, gateway.port(), 1000,
        [&other_addresses](net::AddressClass cls, std::string*) {
          if (cls == net::AddressClass::Loopback) return false;
          ++other_addresses;
          return true;
        },
        &ce, &reached, &reason);
    CHECK(s == net::kInvalidSocket);  // Bound to 127.0.0.1 only.
    if (s != net::kInvalidSocket) net::CloseSocket(s);
  }
  std::cout << "  non-loopback local addresses tried: " << other_addresses << "\n";
  gateway.Stop();
  CHECK(!gateway.Status().running);
  const net::SocketHandle after = net::ConnectLoopback(gateway.port(), 300);
  CHECK(after == net::kInvalidSocket);
  if (after != net::kInvalidSocket) net::CloseSocket(after);
  gateway.Stop();  // Idempotent.
}

}  // namespace

int main() {
  return aafr::test::RunTests({
      {"ValidAbsoluteFormGet", ValidAbsoluteFormGet},
      {"ValidConnect", ValidConnect},
      {"InvalidConnectTargets", InvalidConnectTargets},
      {"MalformedRequests", MalformedRequests},
      {"SmugglingAmbiguitiesRejected", SmugglingAmbiguitiesRejected},
      {"HopByHopRemoved", HopByHopRemoved},
      {"ResponseFramingRules", ResponseFramingRules},
      {"ChunkedTrackerFindsEnd", ChunkedTrackerFindsEnd},
      {"GatewayErrorsEchoNothing", GatewayErrorsEchoNothing},
      {"TargetSyntax", TargetSyntax},
      {"AddressClassification", AddressClassification},
      {"EgressPolicyDefaults", EgressPolicyDefaults},
      {"GatewayConfigValidation", GatewayConfigValidation},
      {"GatewayBindsLoopbackOnly", GatewayBindsLoopbackOnly},
  });
}
