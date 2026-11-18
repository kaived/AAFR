// V0.7.3 browser forwarding gateway over real loopback relays.
//
// Every test runs a controlled origin server on 127.0.0.1, the V0.5 relays
// (entry, relay_a, relay_b, dest) on 127.0.0.1 and the gateway on 127.0.0.1.
// Clients speak to the gateway exactly as a browser does (absolute-form
// requests and CONNECT). Traversal is checked against the relays' own
// StreamTraversal records, never against what the gateway expected.
// No public website is contacted (the DNS-failure case uses the reserved
// .invalid domain, which never resolves).

#include <chrono>
#include <cstdlib>
#include <future>
#include <memory>
#include <thread>

#include "routing/forwarding/loopback_forwarding.h"
#include "routing/gateway/browser_forwarding_gateway.h"
#include "routing/gateway/runtime_gateway_binding.h"
#include "runtime/adaptive/aafr_runtime.h"
#include "tests/support/http_test_server.h"
#include "tests/support/runtime_test_support.h"
#include "tests/support/test_support.h"

using namespace aafr;
using namespace aafr::gateway;
using aafr::test::kPathA;
using aafr::test::kPathB;
namespace net = aafr::forwarding::net;

namespace {

void Sleep(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

template <typename F>
bool WaitFor(F condition, int timeout_ms = 3000) {
  const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (std::chrono::steady_clock::now() < until) {
    if (condition()) return true;
    Sleep(10);
  }
  return condition();
}

GatewayConfig TestGatewayConfig() {
  GatewayConfig c;
  c.session_id = "gwtest";
  c.egress = forwarding::EgressPolicy::LocalTesting();
  c.configuration_wait_ms = 200;
  c.establish_timeout_ms = 3000;
  c.idle_timeout_ms = 60'000;  // Longer than a probing campaign.
  c.restrict_to_process = true;  // The test client is this process.
  return c;
}

// Relays + origin + gateway with a configuration the test sets directly.
struct Bed {
  aafr::test::GraphHolder graph{aafr::test::TwoPathGraph()};
  std::unique_ptr<forwarding::LoopbackForwardingNetwork> network;
  std::unique_ptr<forwarding::LoopbackForwardingExecutor> executor;
  aafr::test::HttpTestServer origin;
  std::unique_ptr<AAFRBrowserForwardingGateway> gateway;
  std::mutex mutex;
  std::optional<PinnedConfiguration> config;
  std::function<bool(const PinnedConfiguration&, std::uint64_t*, std::string*)> usable;
  bool ok{false};

  explicit Bed(GatewayConfig gc = TestGatewayConfig(), forwarding::LoopbackOptions options = {}) {
    network = std::make_unique<forwarding::LoopbackForwardingNetwork>(*graph.Get(), options);
    network->SetEgressPolicy(gc.egress);
    executor = std::make_unique<forwarding::LoopbackForwardingExecutor>(*network);
    gateway = std::make_unique<AAFRBrowserForwardingGateway>(gc);
    std::string error;
    ok = network->Start(&error) && origin.Start() && gateway->Start(&error);
    if (!ok) std::cerr << "test bed did not start: " << error << "\n";
    Use(1, kPathA);
    gateway->SetBindings(Bindings());
  }
  ~Bed() {
    gateway->Stop();
    origin.Stop();
    network->Stop();
  }
  void Use(std::uint64_t generation, const forwarding::Path& path, const std::string& algorithm = "Dijkstra") {
    std::lock_guard<std::mutex> lock(mutex);
    PinnedConfiguration p;
    p.generation = generation;
    p.algorithm = algorithm;
    p.path = path;
    p.decision_id = "d" + std::to_string(generation);
    config = p;
  }
  void Clear() {
    std::lock_guard<std::mutex> lock(mutex);
    config.reset();
  }
  GatewayBindings Bindings() {
    GatewayBindings b;
    b.executor = executor.get();
    b.configuration = [this] {
      std::lock_guard<std::mutex> lock(mutex);
      return config;
    };
    b.usable = [this](const PinnedConfiguration& p, std::uint64_t* v, std::string* why) {
      return usable ? usable(p, v, why) : true;
    };
    auto* n = network.get();
    b.relay_evidence = [n](const std::string& id) { return n->StreamTraversals(id); };
    return b;
  }
  std::uint16_t port() const { return gateway->port(); }
  std::string Url(const std::string& path) const { return origin.Base() + path; }
  std::string Authority() const { return "127.0.0.1:" + std::to_string(origin.port()); }
  // The most recently accepted connection, once it has finished.
  GatewayConnectionRecord Last() {
    WaitFor([&] {
      const auto all = gateway->Connections(1);
      return !all.empty() && all.back().finished_ns != 0;
    });
    const auto all = gateway->Connections(1);
    return all.empty() ? GatewayConnectionRecord{} : all.back();
  }
  // Nodes whose relay records show they carried a connection.
  forwarding::Path Traversed(const std::string& id) const {
    forwarding::Path nodes;
    for (const auto& t : network->StreamTraversals(id)) {
      if (t.status == "OK") nodes.push_back(t.node);
    }
    return nodes;
  }
};

bool SameNodes(forwarding::Path a, forwarding::Path b) {
  std::sort(a.begin(), a.end());
  std::sort(b.begin(), b.end());
  return a == b;
}

// ---------------------------------------------------------------- HTTP

void HttpRequestTraversesPathA() {
  Bed bed;
  if (!bed.ok) {
    CHECK(false);
    return;
  }
  const auto r = aafr::test::ProxyGet(bed.port(), bed.Url("/page"), "Cookie: session=secret\r\n");
  CHECK(r.status == 200);
  CHECK(aafr::test::Contains(r.body, "forwarded-page-body"));
  CHECK(aafr::test::Contains(r.head, "Connection: close"));
  const GatewayConnectionRecord rec = bed.Last();
  CHECK(rec.kind == ConnectionKind::HttpForward);
  CHECK(rec.state == ConnectionState::Completed);
  CHECK(rec.path_confirmed);
  CHECK(rec.expected_path == kPathA);
  CHECK(rec.observed_path == kPathA);
  CHECK(rec.generation == 1);
  CHECK(rec.http_status == 200);
  CHECK(rec.establish_ms && rec.first_byte_ms && rec.duration_ms);
  CHECK(rec.bytes_to_client > 0 && rec.bytes_from_client == 0);
  // Relay-level evidence: entry, relay_a and dest carried it; relay_b did not.
  CHECK(SameNodes(bed.Traversed(rec.connection_id), kPathA));
  CHECK(!bed.network->NodeLogged("relay_b", rec.connection_id));
  for (const auto& t : rec.relay_traversals) {
    CHECK(t.bytes_toward_destination > 0 && t.bytes_toward_client > 0);
    CHECK(!t.open);
  }
  CHECK(rec.relay_traversals.size() == 3);
  // The origin saw a rebuilt origin-form request.
  const std::string head = bed.origin.LastHead();
  CHECK(head.rfind("GET /page HTTP/1.1\r\n", 0) == 0);
  CHECK(aafr::test::Contains(head, "Host: " + bed.Authority()));
  CHECK(aafr::test::Contains(head, "Connection: close"));
  CHECK(!aafr::test::Contains(head, "Proxy-Connection"));
  CHECK(aafr::test::Contains(head, "Cookie: session=secret"));  // passed through, never logged
  // No header value appears in the gateway's records or events.
  for (const auto& e : bed.gateway->Events(100)) CHECK(!aafr::test::Contains(e.detail, "secret"));
  CHECK(!aafr::test::Contains(rec.target, "/page"));  // host:port only
  std::cout << "  HTTP via A: establish " << *rec.establish_ms << " ms, first byte " << *rec.first_byte_ms
            << " ms, total " << *rec.duration_ms << " ms\n";
}

void ConnectTunnelTraversesPathB() {
  Bed bed;
  if (!bed.ok) {
    CHECK(false);
    return;
  }
  bed.Use(2, kPathB, "AntColonyOptimization");
  net::Connection tunnel;
  std::string reply;
  CHECK(aafr::test::ProxyConnect(bed.port(), bed.Authority(), &tunnel, &reply) == 200);
  CHECK(reply == "HTTP/1.1 200 Connection Established\r\n\r\n");
  const auto r = aafr::test::RequestOverTunnel(tunnel, bed.Authority(), "/page2", true);
  CHECK(r.status == 200 && aafr::test::Contains(r.body, "second-page-body"));
  tunnel.Close();
  const GatewayConnectionRecord rec = bed.Last();
  CHECK(rec.kind == ConnectionKind::ConnectTunnel);
  CHECK(rec.state == ConnectionState::Completed);
  CHECK(rec.observed_path == kPathB && rec.path_confirmed);
  CHECK(rec.generation == 2 && rec.algorithm == "AntColonyOptimization");
  CHECK(SameNodes(bed.Traversed(rec.connection_id), kPathB));
  CHECK(!bed.network->NodeLogged("relay_a", rec.connection_id));
  CHECK(rec.egress_address == "127.0.0.1");
}

void RedirectPassesThrough() {
  Bed bed;
  if (!bed.ok) {
    CHECK(false);
    return;
  }
  const auto r = aafr::test::ProxyGet(bed.port(), bed.Url("/redirect"));
  CHECK(r.status == 302);
  CHECK(aafr::test::Contains(r.head, "Location: /target"));  // The proxy does not follow it.
  const auto t = aafr::test::ProxyGet(bed.port(), bed.Url("/target"));
  CHECK(t.status == 200 && aafr::test::Contains(t.body, "redirect-target-body"));
}

void PostBodyForwardedExactly() {
  Bed bed;
  if (!bed.ok) {
    CHECK(false);
    return;
  }
  const std::string body(70'000, 'p');
  const auto r = aafr::test::RawExchange(
      bed.port(), "POST " + bed.Url("/echo") + " HTTP/1.1\r\nHost: " + bed.Authority() +
                      "\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body);
  CHECK(r.status == 200 && aafr::test::Contains(r.body, "received=70000"));
  CHECK(bed.Last().bytes_from_client == body.size());
}

void LargeResponse() {
  Bed bed;
  if (!bed.ok) {
    CHECK(false);
    return;
  }
  const std::size_t size = 8u << 20;
  const auto start = std::chrono::steady_clock::now();
  const auto r = aafr::test::ProxyGet(bed.port(), bed.Url("/large?bytes=" + std::to_string(size)), "", 30'000);
  const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
  CHECK(r.status == 200);
  CHECK(r.body.size() == size);
  CHECK(r.body.size() > 4096 && r.body[4096] == 'B');
  const auto rec = bed.Last();
  CHECK(rec.state == ConnectionState::Completed && rec.end == "content_length: complete");
  for (const auto& t : rec.relay_traversals) CHECK(t.bytes_toward_client >= size);
  std::cout << "  8 MiB through 3 relays: " << ms << " ms (" << (size / 1048576.0) / (ms / 1000.0) << " MiB/s)\n";
}

void StreamingChunkedResponse() {
  Bed bed;
  if (!bed.ok) {
    CHECK(false);
    return;
  }
  const net::SocketHandle s = net::ConnectLoopback(bed.port(), 1000);
  net::Connection c(s);
  c.SendAll("GET " + bed.Url("/chunked") + " HTTP/1.1\r\nHost: " + bed.Authority() + "\r\n\r\n");
  const auto start = std::chrono::steady_clock::now();
  std::string data;
  double first_chunk_ms = -1;
  for (;;) {
    std::string chunk;
    if (c.ReadSome(chunk, 65536, net::After(5000), {}) != net::ReadResult::Ok) break;
    data += chunk;
    if (first_chunk_ms < 0 && aafr::test::Contains(data, "chunk-0;")) {
      first_chunk_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    }
  }
  const double total_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
  CHECK(aafr::test::Contains(data, "Transfer-Encoding: chunked"));
  CHECK(aafr::test::Contains(data, "chunk-4;"));
  CHECK(aafr::test::Contains(data, "0\r\n\r\n"));
  CHECK(first_chunk_ms >= 0 && first_chunk_ms < total_ms - 150);  // delivered incrementally
  CHECK(bed.Last().end == "chunked: complete");
}

void KeepAliveServerStillEnds() {
  Bed bed;
  if (!bed.ok) {
    CHECK(false);
    return;
  }
  const auto start = std::chrono::steady_clock::now();
  const auto r = aafr::test::ProxyGet(bed.port(), bed.Url("/keepalive"));
  const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
  CHECK(r.status == 200 && r.body == "kept-alive");
  CHECK(ms < 3000);  // Framing ended it; no idle timeout.
  CHECK(bed.Last().state == ConnectionState::Completed);
}

void ParallelRequests() {
  Bed bed;
  if (!bed.ok) {
    CHECK(false);
    return;
  }
  std::vector<std::future<aafr::test::RawResponse>> results;
  for (int i = 0; i < 16; ++i) {
    results.push_back(std::async(std::launch::async, [&bed, i] {
      return i % 2 == 0 ? aafr::test::ProxyGet(bed.port(), bed.Url("/slow?ms=150"))
                        : aafr::test::ProxyGet(bed.port(), bed.Url("/large?bytes=200000"));
    }));
  }
  int ok = 0;
  for (auto& f : results) ok += f.get().status == 200 ? 1 : 0;
  CHECK(ok == 16);
  WaitFor([&] { return bed.gateway->Status().active_connections == 0; });
  const auto records = bed.gateway->Connections(16);
  CHECK(records.size() == 16);
  for (const auto& r : records) CHECK(r.path_confirmed && r.observed_path == kPathA);
  CHECK(WaitFor([&] { return bed.network->ActiveStreams() == 0; }));
  CHECK(bed.gateway->Status().counters.completed == 16);
}

// ---------------------------------------------------------------- refusals

void MalformedRequestsNeverForwarded() {
  Bed bed;
  if (!bed.ok) {
    CHECK(false);
    return;
  }
  const std::uint64_t before = bed.origin.Requests();
  const std::string a = bed.Authority();
  struct Case {
    std::string request;
    int status;
  };
  const std::vector<Case> cases = {
      {"GET /page HTTP/1.1\r\nHost: " + a + "\r\n\r\n", 400},
      {"GET " + bed.Url("/page") + " HTTP/1.1\r\nX: a\r\n folded\r\n\r\n", 400},
      {"POST " + bed.Url("/echo") + " HTTP/1.1\r\nContent-Length: 3\r\nTransfer-Encoding: chunked\r\n\r\nabc", 400},
      {"POST " + bed.Url("/echo") + " HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n3\r\nabc\r\n0\r\n\r\n", 501},
      {"GET https://" + a + "/page HTTP/1.1\r\n\r\n", 400},
      {"TRACE " + bed.Url("/page") + " HTTP/1.1\r\n\r\n", 405},
      {"GET " + bed.Url("/page") + " HTTP/1.1\r\nHost: a\r\nHost: b\r\n\r\n", 400},
      {"CONNECT nonsense HTTP/1.1\r\n\r\n", 400},
      {"GET " + bed.Url("/page") + " HTTP/1.1\nHost: x\n\n", 0},  // bare LF never completes a head -> timeout path
      {"GET " + bed.Url("/page") + " HTTP/1.1\r\n" + std::string(40'000, 'X') + "\r\n\r\n", 431},
  };
  for (const Case& c : cases) {
    const auto r = aafr::test::RawExchange(bed.port(), c.request, 20'000);
    if (c.status != 0) {
      CHECK(r.status == c.status);
      if (r.status != c.status) std::cerr << "  expected " << c.status << " got " << r.status << " for "
                                          << c.request.substr(0, 40) << "\n";
      CHECK(aafr::test::Contains(r.head, "X-AAFR-Reason:"));
    }
  }
  CHECK(bed.origin.Requests() == before);
  CHECK(bed.network->StreamsHandled("entry") == 0);  // No tunnel was ever opened.
  CHECK(bed.gateway->Status().counters.forwarded == 0);
}

void HeadTimeout() {
  GatewayConfig gc = TestGatewayConfig();
  gc.head_timeout_ms = 300;
  Bed bed(gc);
  if (!bed.ok) {
    CHECK(false);
    return;
  }
  const auto r = aafr::test::RawExchange(bed.port(), "GET " + bed.Url("/page") + " HTTP/1.1\r\nHost: x\r\n", 5000);
  CHECK(r.status == 408);
  CHECK(bed.Last().failure_reason == "request_head_timeout");
}

void DestinationPolicyEnforced() {
  GatewayConfig gc = TestGatewayConfig();
  gc.egress = forwarding::EgressPolicy{};  // Production default: public 80/443 only.
  Bed bed(gc);
  if (!bed.ok) {
    CHECK(false);
    return;
  }
  const std::uint64_t before = bed.origin.Requests();
  // A loopback literal: refused by the gateway before anything is forwarded.
  CHECK(aafr::test::ProxyGet(bed.port(), bed.Url("/page")).status == 403);
  net::Connection tunnel;
  CHECK(aafr::test::ProxyConnect(bed.port(), bed.Authority(), &tunnel) == 403);
  CHECK(bed.network->StreamsHandled("entry") == 0);
  // A name that resolves to loopback on an allowed port: the gateway cannot
  // tell from the name, so the egress refuses it after resolution.
  CHECK(aafr::test::ProxyConnect(bed.port(), "localhost:443", &tunnel) == 403);
  const auto rec = bed.Last();
  CHECK(rec.forwarding_status == "DEST_FORBIDDEN");
  CHECK(bed.network->StreamsHandled("dest") >= 1);  // It reached the egress, which refused.
  CHECK(bed.origin.Requests() == before);
  // Ports outside the allowed set.
  CHECK(aafr::test::ProxyConnect(bed.port(), "example.invalid:22", &tunnel) == 403);
}

void DnsFailureAndUnreachable() {
  Bed bed;
  if (!bed.ok) {
    CHECK(false);
    return;
  }
  net::Connection tunnel;
  CHECK(aafr::test::ProxyConnect(bed.port(), "aafr-test.invalid:80", &tunnel) == 502);
  auto rec = bed.Last();
  CHECK(rec.forwarding_status == "DNS_FAILED" && rec.failure_reason == "dns_failed" && !rec.relay_failure);
  // A closed local port.
  const net::SocketHandle probe = net::ListenLoopback(0, 1, nullptr, nullptr);
  const std::uint16_t closed = net::LocalPort(probe);
  net::CloseSocket(probe);
  CHECK(aafr::test::ProxyConnect(bed.port(), "127.0.0.1:" + std::to_string(closed), &tunnel) == 502);
  rec = bed.Last();
  CHECK(rec.forwarding_status == "DEST_UNREACHABLE" && !rec.relay_failure);
  // The server drops the connection without answering.
  CHECK(aafr::test::ProxyGet(bed.port(), bed.Url("/close")).status == 502);
  CHECK(bed.Last().failure_reason == "upstream_closed_before_response");
}

void NoConfigurationFailsClosed() {
  Bed bed;
  if (!bed.ok) {
    CHECK(false);
    return;
  }
  bed.Clear();
  const std::uint64_t before = bed.origin.Requests();
  const auto start = std::chrono::steady_clock::now();
  const auto r = aafr::test::ProxyGet(bed.port(), bed.Url("/page"));
  const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
  CHECK(r.status == 503 && aafr::test::Contains(r.head, "no_confirmed_configuration"));
  CHECK(ms >= 150 && ms < 3000);  // waited configuration_wait_ms, then refused
  CHECK(bed.origin.Requests() == before);  // never direct
  CHECK(bed.network->StreamsHandled("entry") == 0);
}

void UnusableOrStaleConfigurationRejected() {
  Bed bed;
  if (!bed.ok) {
    CHECK(false);
    return;
  }
  bed.usable = [](const PinnedConfiguration&, std::uint64_t*, std::string* why) {
    *why = "path is not valid on the current graph";
    return false;
  };
  auto r = aafr::test::ProxyGet(bed.port(), bed.Url("/page"));
  CHECK(r.status == 503 && aafr::test::Contains(r.head, "configuration_unusable"));
  // A configuration confirmed on an older graph version than the relays run.
  bed.usable = [](const PinnedConfiguration&, std::uint64_t* v, std::string*) {
    *v = 999;
    return true;
  };
  r = aafr::test::ProxyGet(bed.port(), bed.Url("/page"));
  CHECK(r.status == 503 && aafr::test::Contains(r.head, "stale_configuration"));
  CHECK(bed.network->StreamsHandled("entry") == 0);
}

void InvalidRelayPath() {
  Bed bed;
  if (!bed.ok) {
    CHECK(false);
    return;
  }
  bed.Use(5, {"entry", "dest"});  // No such edge.
  auto r = aafr::test::ProxyGet(bed.port(), bed.Url("/page"));
  CHECK(r.status == 502);
  CHECK(bed.Last().forwarding_status == "NOT_A_NEIGHBOUR");
  bed.Use(6, {"entry", "relay_a", "entry", "dest"});  // Loop: refused before sending.
  r = aafr::test::ProxyGet(bed.port(), bed.Url("/page"));
  CHECK(r.status == 502 && bed.Last().forwarding_status == "REJECTED");
}

void RelayFailure() {
  Bed bed;
  if (!bed.ok) {
    CHECK(false);
    return;
  }
  CHECK(bed.network->SetNodeOnline("relay_a", false));
  const auto r = aafr::test::ProxyGet(bed.port(), bed.Url("/page"));
  CHECK(r.status == 502 && aafr::test::Contains(r.head, "relay_unavailable"));
  const auto rec = bed.Last();
  CHECK(rec.relay_failure && rec.forwarding_status == "NEXT_HOP_UNAVAILABLE");
  CHECK(rec.observed_path == forwarding::Path({"entry"}));
  CHECK(bed.gateway->Status().counters.relay_failures == 1);
  // Path B still works.
  bed.Use(2, kPathB);
  CHECK(aafr::test::ProxyGet(bed.port(), bed.Url("/page")).status == 200);
  CHECK(bed.network->SetNodeOnline("relay_a", true));
}

void RelayFailureMidStream() {
  Bed bed;
  if (!bed.ok) {
    CHECK(false);
    return;
  }
  net::Connection tunnel;
  CHECK(aafr::test::ProxyConnect(bed.port(), bed.Authority(), &tunnel) == 200);
  tunnel.SendAll("GET /hold?ms=3000 HTTP/1.1\r\nHost: x\r\n\r\n");
  std::string head;
  CHECK(tunnel.ReadHead(head, 65536, net::After(5000), {}) == net::ReadResult::Ok);
  bed.network->SetNodeOnline("relay_a", false);  // The relay dies mid-response.
  std::string rest;
  net::ReadResult rr = net::ReadResult::Ok;
  while (rr == net::ReadResult::Ok) rr = tunnel.ReadSome(rest, 65536, net::After(5000), {});
  CHECK(rr == net::ReadResult::Closed);  // The browser sees the connection end; nothing hangs.
  CHECK(!aafr::test::Contains(rest, "held-response-complete"));
  tunnel.Close();  // As a browser does once its connection ends.
  CHECK(WaitFor([&] { return bed.gateway->Status().active_connections == 0; }));
  CHECK(WaitFor([&] { return bed.network->ActiveStreams() == 0; }));
  bed.network->SetNodeOnline("relay_a", true);
}

void RelayTimeout() {
  GatewayConfig gc = TestGatewayConfig();
  gc.establish_timeout_ms = 250;
  forwarding::LoopbackOptions slow;
  slow.inject_edge_latency = true;
  Bed bed(gc, slow);
  if (!bed.ok) {
    CHECK(false);
    return;
  }
  bed.graph.SetLatency("entry", "relay_a", 400.0);
  std::string error;
  CHECK(bed.network->SyncWith(*bed.graph.Get(), &error));
  const auto r = aafr::test::ProxyGet(bed.port(), bed.Url("/page"));
  CHECK(r.status == 504);
  const auto rec = bed.Last();
  CHECK(rec.forwarding_status == "TIMEOUT" || rec.forwarding_status == "NEXT_HOP_FAILED");
  CHECK(rec.state == ConnectionState::Failed);
}

void ResourceLimits() {
  GatewayConfig gc = TestGatewayConfig();
  gc.max_clients = 3;
  gc.head_timeout_ms = 3000;
  Bed bed(gc);
  if (!bed.ok) {
    CHECK(false);
    return;
  }
  std::vector<net::Connection> idle;
  for (int i = 0; i < 3; ++i) idle.emplace_back(net::ConnectLoopback(bed.port(), 1000));
  CHECK(WaitFor([&] { return bed.gateway->Status().active_connections == 3; }));
  const auto r = aafr::test::RawExchange(bed.port(), "GET " + bed.Url("/page") + " HTTP/1.1\r\n\r\n", 3000);
  CHECK(r.status == 503 && aafr::test::Contains(r.head, "gateway_at_capacity"));
  CHECK(bed.gateway->Status().counters.refused_clients >= 1);
  idle.clear();
}

void CancellationAndShutdown() {
  Bed bed;
  if (!bed.ok) {
    CHECK(false);
    return;
  }
  // Detach while a tunnel is busy: it is cancelled promptly.
  net::Connection tunnel;
  CHECK(aafr::test::ProxyConnect(bed.port(), bed.Authority(), &tunnel) == 200);
  tunnel.SendAll("GET /hold?ms=5000 HTTP/1.1\r\nHost: x\r\n\r\n");
  Sleep(100);
  const auto start = std::chrono::steady_clock::now();
  bed.gateway->Detach();
  const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
  CHECK(ms < 2000);
  const auto rec = bed.Last();
  CHECK(rec.state == ConnectionState::Cancelled && rec.failure_reason == "gateway_detached");
  CHECK(aafr::test::ProxyGet(bed.port(), bed.Url("/page")).status == 503);  // Unbound: fail closed.
  // Rebind, then a client that disconnects mid-response.
  bed.gateway->SetBindings(bed.Bindings());
  {
    const net::SocketHandle s = net::ConnectLoopback(bed.port(), 1000);
    net::Connection c(s);
    c.SendAll("GET " + bed.Url("/large?bytes=67108864") + " HTTP/1.1\r\nHost: x\r\n\r\n");
    std::string head;
    c.ReadHead(head, 65536, net::After(3000), {});
  }  // closed by the client
  CHECK(WaitFor([&] { return bed.gateway->Status().active_connections == 0; }, 5000));
  CHECK(bed.Last().state == ConnectionState::Cancelled);
  // Stop with a live tunnel: joins promptly, port closed.
  net::Connection live;
  CHECK(aafr::test::ProxyConnect(bed.port(), bed.Authority(), &live) == 200);
  const auto stop_start = std::chrono::steady_clock::now();
  bed.gateway->Stop();
  const double stop_ms =
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - stop_start).count();
  CHECK(stop_ms < 2000);
  const net::SocketHandle after = net::ConnectLoopback(bed.port(), 300);
  CHECK(after == net::kInvalidSocket);
  if (after != net::kInvalidSocket) net::CloseSocket(after);
  CHECK(WaitFor([&] { return bed.network->ActiveStreams() == 0; }));
}

void ForeignProcessRefused() {
#ifdef _WIN32
  Bed bed;
  if (!bed.ok) {
    CHECK(false);
    return;
  }
  const std::uint64_t before = bed.origin.Requests();
  // Another process on this machine tries to use the gateway as a proxy.
  const std::string command =
      "powershell -NoProfile -NonInteractive -Command \"try { $c = New-Object Net.Sockets.TcpClient('127.0.0.1', " +
      std::to_string(bed.port()) + "); $s = $c.GetStream(); $b = [Text.Encoding]::ASCII.GetBytes('GET " +
      bed.Url("/page") + " HTTP/1.1`r`nHost: x`r`n`r`n'); $s.Write($b, 0, $b.Length); Start-Sleep -Milliseconds 500; "
      "$c.Close() } catch { }\" >NUL 2>&1";
  std::system(command.c_str());
  CHECK(WaitFor([&] { return bed.gateway->Status().counters.refused_clients >= 1; }, 5000));
  CHECK(bed.origin.Requests() == before);
  bool saw = false;
  for (const auto& e : bed.gateway->Events(50)) saw = saw || aafr::test::Contains(e.detail, "foreign_process");
  CHECK(saw);
#endif
}

// Rapid, repeated and abandoned connections: nothing leaks and nothing hangs.
void RapidConnectionStress() {
  Bed bed;
  if (!bed.ok) {
    CHECK(false);
    return;
  }
  // 1) 100 sequential short requests.
  for (int i = 0; i < 100; ++i) CHECK(aafr::test::ProxyGet(bed.port(), bed.Url("/page")).status == 200);
  // 2) 32 parallel tunnels opened and abandoned at random points: before the
  //    CONNECT answer, mid-request and mid-response.
  std::vector<std::thread> clients;
  for (int i = 0; i < 32; ++i) {
    clients.emplace_back([&bed, i] {
      const net::SocketHandle s = net::ConnectLoopback(bed.port(), 1000);
      net::Connection c(s);
      c.SendAll("CONNECT " + bed.Authority() + " HTTP/1.1\r\nHost: x\r\n\r\n");
      if (i % 3 == 0) return;  // gone before the answer
      std::string head;
      c.ReadHead(head, 65536, net::After(5000), {});
      c.SendAll("GET /large?bytes=4000000 HTTP/1.1\r\nHost: x\r\n\r\n");
      if (i % 3 == 1) return;  // gone mid-request
      std::string part;
      c.ReadSome(part, 65536, net::After(5000), {});
      net::AbortClose(c.Release());  // reset mid-response
    });
  }
  for (auto& t : clients) t.join();
  CHECK(WaitFor([&] { return bed.gateway->Status().active_connections == 0; }, 15000));
  CHECK(WaitFor([&] { return bed.network->ActiveStreams() == 0; }, 15000));
  CHECK(bed.network->ActiveConnections() == 0 || WaitFor([&] { return bed.network->ActiveConnections() == 0; }, 5000));
  // 3) Still fully functional, and shutdown is prompt.
  CHECK(aafr::test::ProxyGet(bed.port(), bed.Url("/page")).status == 200);
  const auto start = std::chrono::steady_clock::now();
  bed.gateway->Stop();
  const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
  CHECK(ms < 2000);
  const auto c = bed.gateway->Status().counters;
  std::cout << "  stress: " << c.accepted << " accepted, " << c.completed << " completed, " << c.cancelled
            << " cancelled, " << c.failed << " failed; stop " << ms << " ms\n";
}

// ---------------------------------------------------------------- pinning

void ConnectionPinningAndDraining() {
  Bed bed;
  if (!bed.ok) {
    CHECK(false);
    return;
  }
  // A long-lived connection on generation 1 (path A).
  net::Connection old_tunnel;
  CHECK(aafr::test::ProxyConnect(bed.port(), bed.Authority(), &old_tunnel) == 200);
  auto first = aafr::test::RequestOverTunnel(old_tunnel, bed.Authority(), "/keepalive");
  CHECK(first.status == 200);
  const std::string old_id = bed.gateway->Connections(1).back().connection_id;
  // The confirmed configuration changes to B.
  bed.Use(2, kPathB, "GreyWolfOptimizer");
  CHECK(WaitFor([&] {
    const auto s = bed.gateway->Status();
    return s.current && s.current->generation == 2;
  }));
  // New connections use B.
  const auto fresh = aafr::test::ProxyGet(bed.port(), bed.Url("/page"));
  CHECK(fresh.status == 200);
  const auto fresh_rec = bed.Last();
  CHECK(fresh_rec.observed_path == kPathB && fresh_rec.generation == 2);
  CHECK(SameNodes(bed.Traversed(fresh_rec.connection_id), kPathB));
  // The old connection is still on A, still works, and is reported draining.
  const auto status = bed.gateway->Status();
  CHECK(status.draining_connections == 1);
  CHECK(status.active_by_generation.count(1) == 1);
  const auto before = bed.network->StreamTraversals(old_id);
  std::uint64_t relay_a_before = 0;
  for (const auto& t : before) relay_a_before += t.node == "relay_a" ? t.bytes_toward_client : 0;
  auto second = aafr::test::RequestOverTunnel(old_tunnel, bed.Authority(), "/keepalive");
  CHECK(second.status == 200 && second.body == "kept-alive");
  std::uint64_t relay_a_after = 0;
  for (const auto& t : bed.network->StreamTraversals(old_id)) relay_a_after += t.node == "relay_a" ? t.bytes_toward_client : 0;
  CHECK(relay_a_after > relay_a_before);  // relay_a carried the post-switch bytes
  CHECK(!bed.network->NodeLogged("relay_b", old_id));
  bool draining_event = false;
  bool changed_event = false;
  for (const auto& e : bed.gateway->Events(200)) {
    draining_event = draining_event || e.type == GatewayEventType::ConfigurationDraining;
    changed_event = changed_event || e.type == GatewayEventType::RelayPathChanged;
  }
  CHECK(changed_event);
  // Close it: generation 1 retires.
  old_tunnel.Close();
  CHECK(WaitFor([&] {
    for (const auto& e : bed.gateway->Events(200)) {
      if (e.type == GatewayEventType::ConfigurationRetired && e.generation == 1) return true;
    }
    return false;
  }));
  CHECK(bed.gateway->Status().draining_connections == 0);
  (void)draining_event;
}

void BoundedDrainCancelsOldConnections() {
  GatewayConfig gc = TestGatewayConfig();
  gc.drain_timeout_ms = 300;
  Bed bed(gc);
  if (!bed.ok) {
    CHECK(false);
    return;
  }
  net::Connection old_tunnel;
  CHECK(aafr::test::ProxyConnect(bed.port(), bed.Authority(), &old_tunnel) == 200);
  CHECK(aafr::test::RequestOverTunnel(old_tunnel, bed.Authority(), "/keepalive").status == 200);
  const std::string old_id = bed.gateway->Connections(1).back().connection_id;
  bed.Use(2, kPathB);
  CHECK(WaitFor([&] {
    const auto r = bed.gateway->FindConnection(old_id);
    return r && r->finished_ns != 0;
  }, 3000));
  const auto r = bed.gateway->FindConnection(old_id);
  CHECK(r && r->state == ConnectionState::Cancelled && r->failure_reason == "drain_timeout");
  // Without a bound (the default), nothing is cancelled: covered above.
}

// ---------------------------------------------------------------- runtime

forwarding::ForwardingGraph SlowTwoPathGraph() {
  aafr::test::GraphHolder holder(aafr::test::TwoPathGraph());
  holder.SetLatency("entry", "relay_a", 5.0);
  holder.SetLatency("relay_a", "dest", 5.0);
  holder.SetLatency("entry", "relay_b", 25.0);
  holder.SetLatency("relay_b", "dest", 25.0);
  return *holder.Get();
}

struct RuntimeBed {
  aafr::test::GraphHolder graph{SlowTwoPathGraph()};
  std::unique_ptr<forwarding::LoopbackForwardingNetwork> network;
  std::unique_ptr<forwarding::LoopbackForwardingExecutor> executor;
  std::unique_ptr<algorithms::AlgorithmExecutionEngine> engine;
  std::unique_ptr<runtime::AAFRRuntime> runtime;
  aafr::test::HttpTestServer origin;
  std::unique_ptr<AAFRBrowserForwardingGateway> gateway;
  bool ok{false};

  explicit RuntimeBed(const runtime::IRecommendationProvider* provider = nullptr) {
    forwarding::LoopbackOptions options;
    options.inject_edge_latency = true;
    network = std::make_unique<forwarding::LoopbackForwardingNetwork>(*graph.Get(), options);
    network->SetEgressPolicy(forwarding::EgressPolicy::LocalTesting());
    executor = std::make_unique<forwarding::LoopbackForwardingExecutor>(*network);
    engine = std::make_unique<algorithms::AlgorithmExecutionEngine>(nullptr, aafr::test::VerifiedRegistry(), executor.get());
    runtime::RuntimeConfig config;
    config.session_id = "gwrt";
    config.scheduler.min_interval_ms = 100;
    config.stability.min_dwell_ms = 200;
    config.stability.cooldown_ms = 300;
    config.stability.sustain_evaluations = 2;
    config.probe.rounds = 16;
    config.probe.probe_timeout_ms = 2000;
    config.health_check_interval_ms = 0;
    config.review_interval_ms = 0;
    config.failures_for_safety_override = 2;
    runtime::RuntimeDependencies d;
    d.graph_source = [this] { return graph.Get(); };
    d.problem_for = [](const forwarding::ForwardingGraph& g) { return aafr::test::ProblemFor(g); };
    d.executor = executor.get();
    d.eligibility = engine.get();
    d.executor_injects_delay = true;
    d.provider = provider;
    runtime = std::make_unique<runtime::AAFRRuntime>(std::move(d), config);
    gateway = std::make_unique<AAFRBrowserForwardingGateway>(TestGatewayConfig());
    std::string error;
    ok = network->Start(&error) && origin.Start() && gateway->Start(&error);
    if (!ok) std::cerr << "runtime bed did not start: " << error << "\n";
    RuntimeBindingSources sources;
    sources.runtime = runtime.get();
    sources.graph_source = [this] { return graph.Get(); };
    sources.problem_for = [](const forwarding::ForwardingGraph& g) { return aafr::test::ProblemFor(g); };
    sources.executor = executor.get();
    sources.network = network.get();
    gateway->SetBindings(MakeRuntimeBindings(sources));
  }
  ~RuntimeBed() {
    gateway->Detach();
    gateway->Stop();
    runtime->Stop();
    origin.Stop();
    network->Stop();
  }
  void Mutate(const char* from, const char* to, double ms) {
    graph.SetLatency(from, to, ms);
    std::string error;
    CHECK(network->SyncWith(*graph.Get(), &error));
  }
  // Evaluations after a change until the runtime commits one switch (each
  // through every gate; under CPU load the probe evidence can need more than
  // the minimum two). True when exactly one more switch was committed.
  bool SwitchCommitted(int wait_ms, int extra = 4) {
    const auto before = runtime->Status().counters.switches;
    runtime::DecisionRecord r = SwitchAfterChange(wait_ms);
    for (int i = 0; i < extra && runtime->Status().counters.switches == before; ++i) {
      std::cout << "  evaluation needed more evidence: " << runtime::ToString(r.type) << " (" << r.explanation << ")\n";
      Sleep(200);
      runtime->RequestEvaluation(runtime::EvaluationTrigger::Manual, "confirm");
      runtime->Step();
      r = runtime->Decisions(1).back();
    }
    return runtime->Status().counters.switches == before + 1;
  }
  // Two evaluations (sustain_evaluations = 2) after a change.
  runtime::DecisionRecord SwitchAfterChange(int wait_ms) {
    Sleep(wait_ms);
    runtime->Step();
    Sleep(150);
    runtime->RequestEvaluation(runtime::EvaluationTrigger::Manual, "confirm");
    runtime->Step();
    return runtime->Decisions(1).back();
  }
  GatewayConnectionRecord Get(const std::string& path, int* status = nullptr) {
    const auto r = aafr::test::ProxyGet(gateway->port(), origin.Base() + path);
    if (status) *status = r.status;
    WaitFor([&] {
      const auto all = gateway->Connections(1);
      return !all.empty() && all.back().finished_ns != 0;
    });
    return gateway->Connections(1).back();
  }
  std::string Authority() const { return "127.0.0.1:" + std::to_string(origin.port()); }
};

void RuntimeConfirmedConfigurationDrivesForwarding() {
  RuntimeBed bed;
  if (!bed.ok) {
    CHECK(false);
    return;
  }
  // Before Initialize there is no confirmed configuration: fail closed.
  int status = 0;
  bed.Get("/page", &status);
  CHECK(status == 503);
  CHECK(bed.runtime->Initialize());
  const auto active = bed.runtime->ConfirmedConfiguration();
  CHECK(active && active->path == kPathA);
  auto rec = bed.Get("/page", &status);
  CHECK(status == 200);
  CHECK(rec.generation == active->config_version);
  CHECK(rec.algorithm == algorithms::ToString(active->algorithm));
  CHECK(rec.decision_id == active->decision_id);
  CHECK(rec.observed_path == active->path && rec.path_confirmed);

  // A long-lived connection pinned to A.
  net::Connection pinned;
  CHECK(aafr::test::ProxyConnect(bed.gateway->port(), bed.Authority(), &pinned) == 200);
  CHECK(aafr::test::RequestOverTunnel(pinned, bed.Authority(), "/keepalive").status == 200);
  const std::string pinned_id = [&] {
    for (const auto& c : bed.gateway->Connections(10)) {
      if (c.kind == ConnectionKind::ConnectTunnel && c.finished_ns == 0) return c.connection_id;
    }
    return std::string();
  }();
  CHECK(!pinned_id.empty());

  // A -> B: path A congests; the runtime switches through its gates.
  bed.Mutate("entry", "relay_a", 150.0);
  CHECK(bed.SwitchCommitted(150));
  const runtime::DecisionRecord r = bed.runtime->Decisions(1).back();
  CHECK(r.activation == runtime::ActivationStatus::Confirmed || r.type == runtime::DecisionType::Retain);
  const auto b = bed.runtime->ConfirmedConfiguration();
  CHECK(b && b->path == kPathB);
  const auto switched_at = std::chrono::steady_clock::now();
  rec = bed.Get("/page2", &status);
  const double switch_to_new_path_ms =
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - switched_at).count();
  CHECK(status == 200);
  CHECK(rec.observed_path == kPathB && rec.generation == b->config_version);
  CHECK(bed.network->NodeLogged("relay_b", rec.connection_id));
  CHECK(!bed.network->NodeLogged("relay_a", rec.connection_id));
  // The pinned connection still works over A.
  CHECK(aafr::test::RequestOverTunnel(pinned, bed.Authority(), "/keepalive").status == 200);
  CHECK(bed.network->NodeLogged("relay_a", pinned_id));
  CHECK(!bed.network->NodeLogged("relay_b", pinned_id));
  CHECK(bed.gateway->Status().draining_connections == 1);
  pinned.Close();

  // B -> A recovery once the congestion clears.
  bed.Mutate("entry", "relay_a", 5.0);
  CHECK(bed.SwitchCommitted(350));
  const auto back = bed.runtime->ConfirmedConfiguration();
  CHECK(back && back->path == kPathA);
  rec = bed.Get("/page", &status);
  CHECK(status == 200 && rec.observed_path == kPathA && rec.generation == back->config_version);
  std::cout << "  first browser connection after the A->B commit used B (" << switch_to_new_path_ms << " ms request)\n";

  // Browser streams are outcome observations of their decision.
  const auto totals = bed.runtime->OutcomeTotals();
  CHECK(totals.browser_streams >= 4);
  const auto outcome = bed.runtime->Outcome(back->decision_id);
  CHECK(outcome && outcome->browser_streams >= 1);
}

void FailedActivationPreservesPath() {
  RuntimeBed bed;
  if (!bed.ok) {
    CHECK(false);
    return;
  }
  CHECK(bed.runtime->Initialize());
  const auto before = bed.runtime->ConfirmedConfiguration();
  CHECK(before && before->path == kPathA);
  // relay_b's link to dest goes down in reality but not in the graph, then
  // A congests: whatever the runtime tries for B cannot be confirmed.
  bed.Mutate("entry", "relay_a", 150.0);
  // After the graph sync (which would re-enable it).
  CHECK(bed.network->SetEdgeEnabled("relay_b", "dest", false));
  bed.SwitchAfterChange(150);
  bed.SwitchAfterChange(400);
  const auto after = bed.runtime->ConfirmedConfiguration();
  CHECK(after && after->path == kPathA);  // never advertised B
  if (!after || after->path != kPathA) {
    for (const auto& d : bed.runtime->Decisions(4)) std::cerr << runtime::ToJson(d) << "\n";
  }
  int status = 0;
  const auto rec = bed.Get("/page", &status);
  CHECK(status == 200 && rec.observed_path == kPathA);
  CHECK(bed.network->SetEdgeEnabled("relay_b", "dest", true));
}

void RelayFailureTriggersRecovery() {
  RuntimeBed bed;
  if (!bed.ok) {
    CHECK(false);
    return;
  }
  CHECK(bed.runtime->Initialize());
  CHECK(bed.runtime->ConfirmedConfiguration()->path == kPathA);
  // relay_a dies. Browser connections over A fail; the gateway reports the
  // relay failures, which count towards the runtime's safety override.
  bed.network->SetNodeOnline("relay_a", false);
  int status = 0;
  for (int i = 0; i < 2; ++i) {
    const auto rec = bed.Get("/page", &status);
    CHECK(status == 502 && rec.relay_failure);
  }
  // The failures requested a FailureRecovery evaluation; the next step runs it.
  bed.runtime->Step();
  const auto now = bed.runtime->ConfirmedConfiguration();
  CHECK(now && now->path == kPathB);
  const auto last = bed.runtime->Decisions(1).back();
  CHECK(last.trigger == runtime::EvaluationTrigger::FailureRecovery);
  const auto rec = bed.Get("/page", &status);
  CHECK(status == 200 && rec.observed_path == kPathB);
  // A destination failure is not a relay failure and does not count.
  const auto failures_before = bed.runtime->Status().consecutive_failures;
  net::Connection t;
  aafr::test::ProxyConnect(bed.gateway->port(), "aafr-test.invalid:80", &t);
  aafr::test::ProxyConnect(bed.gateway->port(), "aafr-test.invalid:80", &t);
  bed.runtime->Step();
  CHECK(bed.runtime->Status().consecutive_failures <= failures_before);
  CHECK(bed.runtime->Decisions(1).back().trigger != runtime::EvaluationTrigger::FailureRecovery ||
        bed.runtime->Decisions(1).back().decision_id == last.decision_id);
  bed.network->SetNodeOnline("relay_a", true);
}

}  // namespace

int main() {
  return aafr::test::RunTests({
      {"HttpRequestTraversesPathA", HttpRequestTraversesPathA},
      {"ConnectTunnelTraversesPathB", ConnectTunnelTraversesPathB},
      {"RedirectPassesThrough", RedirectPassesThrough},
      {"PostBodyForwardedExactly", PostBodyForwardedExactly},
      {"LargeResponse", LargeResponse},
      {"StreamingChunkedResponse", StreamingChunkedResponse},
      {"KeepAliveServerStillEnds", KeepAliveServerStillEnds},
      {"ParallelRequests", ParallelRequests},
      {"MalformedRequestsNeverForwarded", MalformedRequestsNeverForwarded},
      {"HeadTimeout", HeadTimeout},
      {"DestinationPolicyEnforced", DestinationPolicyEnforced},
      {"DnsFailureAndUnreachable", DnsFailureAndUnreachable},
      {"NoConfigurationFailsClosed", NoConfigurationFailsClosed},
      {"UnusableOrStaleConfigurationRejected", UnusableOrStaleConfigurationRejected},
      {"InvalidRelayPath", InvalidRelayPath},
      {"RelayFailure", RelayFailure},
      {"RelayFailureMidStream", RelayFailureMidStream},
      {"RelayTimeout", RelayTimeout},
      {"ResourceLimits", ResourceLimits},
      {"CancellationAndShutdown", CancellationAndShutdown},
      {"ForeignProcessRefused", ForeignProcessRefused},
      {"RapidConnectionStress", RapidConnectionStress},
      {"ConnectionPinningAndDraining", ConnectionPinningAndDraining},
      {"BoundedDrainCancelsOldConnections", BoundedDrainCancelsOldConnections},
      {"RuntimeConfirmedConfigurationDrivesForwarding", RuntimeConfirmedConfigurationDrivesForwarding},
      {"FailedActivationPreservesPath", FailedActivationPreservesPath},
      {"RelayFailureTriggersRecovery", RelayFailureTriggersRecovery},
  });
}
