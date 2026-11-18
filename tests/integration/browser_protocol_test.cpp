// V0.7.3 verification closure: HTTP/2 and WebSocket through the AAFR
// browser forwarding gateway, with real Qt WebEngine traffic.
//
//   Qt WebEngine -> AAFR gateway (CONNECT) -> confirmed relay path -> local server
//
// HTTP/2: tests/support/h2_test_server.h, TLS (Schannel) with ALPN "h2"
// only, real HTTP/2 framing and HPACK. Evidence that HTTP/2 was used comes
// from both ends: the server decoded the requests from HTTP/2 frames on a
// connection whose ALPN is h2, and the page's Resource/Navigation Timing
// reports nextHopProtocol "h2". Traversal evidence comes from the relays'
// own records (node logs and StreamTraversal byte counters), never from
// what the gateway expected.
//
// WebSocket: tests/support/ws_test_server.h (RFC 6455), ws:// and wss://.
// Messages are driven from page JavaScript and checked on both sides.
//
// Certificates: the browser trusts the test certificate only by accepting a
// certificate error whose leaf is exactly that certificate and whose only
// problem is the unknown test CA. Validation is never disabled; the
// untrusted-certificate cases prove it still fails.

#include <QApplication>
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTest>
#include <QWebEngineCertificateError>
#include <QWebEnginePage>
#include <QWebEngineProfile>
#include <QWebEngineView>

#include <future>
#include <memory>

#include "browser_forwarding.h"
#include "routing/forwarding/loopback_forwarding.h"
#include "routing/gateway/runtime_gateway_binding.h"
#include "runtime/adaptive/aafr_runtime.h"
#include "tests/support/h2_test_server.h"
#include "tests/support/http_test_server.h"
#include "tests/support/runtime_test_support.h"
#include "tests/support/ws_test_server.h"

namespace fwd = aafr::forwarding;
namespace gw = aafr::gateway;
namespace rt = aafr::runtime;

namespace {

BrowserForwardingSetup *g_setup = nullptr;
QString tlsDir() { return QStringLiteral(AAFR_TEST_TLS_DIR); }

// Relays and a verified runtime over two disjoint paths.
struct Stack {
    aafr::test::GraphHolder graph{[] {
        aafr::test::GraphHolder h(aafr::test::TwoPathGraph());
        h.SetLatency("entry", "relay_a", 5.0);
        h.SetLatency("relay_a", "dest", 5.0);
        h.SetLatency("entry", "relay_b", 25.0);
        h.SetLatency("relay_b", "dest", 25.0);
        return *h.Get();
    }()};
    std::unique_ptr<fwd::LoopbackForwardingNetwork> network;
    std::unique_ptr<fwd::LoopbackForwardingExecutor> executor;
    std::unique_ptr<aafr::algorithms::AlgorithmExecutionEngine> engine;
    std::unique_ptr<rt::AAFRRuntime> runtime;

    bool start()
    {
        fwd::LoopbackOptions options;
        options.inject_edge_latency = true;
        network = std::make_unique<fwd::LoopbackForwardingNetwork>(*graph.Get(), options);
        network->SetEgressPolicy(fwd::EgressPolicy::LocalTesting());
        executor = std::make_unique<fwd::LoopbackForwardingExecutor>(*network);
        engine = std::make_unique<aafr::algorithms::AlgorithmExecutionEngine>(nullptr, aafr::test::VerifiedRegistry(),
                                                                              executor.get());
        rt::RuntimeConfig config;
        config.session_id = "brpr";
        config.scheduler.min_interval_ms = 100;
        config.stability.min_dwell_ms = 200;
        config.stability.cooldown_ms = 300;
        config.stability.sustain_evaluations = 2;
        config.probe.rounds = 12;
        config.probe.probe_timeout_ms = 2000;
        config.health_check_interval_ms = 0;
        config.review_interval_ms = 0;
        rt::RuntimeDependencies d;
        d.graph_source = [this] { return graph.Get(); };
        d.problem_for = [](const fwd::ForwardingGraph &g) { return aafr::test::ProblemFor(g); };
        d.executor = executor.get();
        d.eligibility = engine.get();
        d.executor_injects_delay = true;
        runtime = std::make_unique<rt::AAFRRuntime>(std::move(d), config);
        std::string error;
        return network->Start(&error) && runtime->Initialize();
    }
    gw::GatewayBindings bindings()
    {
        gw::RuntimeBindingSources s;
        s.runtime = runtime.get();
        s.graph_source = [this] { return graph.Get(); };
        s.problem_for = [](const fwd::ForwardingGraph &g) { return aafr::test::ProblemFor(g); };
        s.executor = executor.get();
        s.network = network.get();
        return gw::MakeRuntimeBindings(s);
    }
    void mutate(const char *from, const char *to, double ms)
    {
        graph.SetLatency(from, to, ms);
        std::string error;
        network->SyncWith(*graph.Get(), &error);
    }
};

// A page that accepts exactly one certificate (by SHA-256 of the leaf) when
// its only error is the unknown test CA; every other error is refused.
class TrustingPage : public QWebEnginePage
{
public:
    TrustingPage(QWebEngineProfile *profile, QByteArray fingerprint, QObject *parent)
        : QWebEnginePage(profile, parent), fingerprint_(std::move(fingerprint))
    {
        connect(this, &QWebEnginePage::certificateError, this, [this](QWebEngineCertificateError e) {
            ++errors;
            const auto chain = e.certificateChain();
            const bool leaf = !chain.isEmpty() && chain.first().digest(QCryptographicHash::Sha256) == fingerprint_;
            const bool authorityOnly = e.type() == QWebEngineCertificateError::CertificateAuthorityInvalid;
            if (leaf && authorityOnly) {
                ++accepted;
                e.acceptCertificate();
            } else {
                ++rejected;
                e.rejectCertificate();
            }
        });
    }
    int errors = 0;
    int accepted = 0;
    int rejected = 0;

private:
    QByteArray fingerprint_;
};

// Page-side WebSocket harness (state is polled from C++).
const char *kWsHarness = R"JS(
window.aafr = window.aafr || { socks: {} };
window.aafrOpen = function (name, url) {
  const s = { state: 'connecting', msgs: [], closeCode: null, clean: null, error: false, t0: performance.now(), sent: {} };
  const ws = new WebSocket(url);
  ws.binaryType = 'arraybuffer';
  ws.onopen = () => { s.state = 'open'; s.openMs = performance.now() - s.t0; };
  ws.onmessage = (e) => {
    const now = performance.now();
    if (typeof e.data === 'string') {
      const m = { t: 'text', d: e.data, at: now };
      if (s.sent[e.data] !== undefined) m.rtt = now - s.sent[e.data];
      s.msgs.push(m);
    } else {
      const b = new Uint8Array(e.data); let sum = 0;
      for (let i = 0; i < b.length; ++i) sum = (sum + b[i]) % 65521;
      s.msgs.push({ t: 'bin', n: b.length, sum: sum, at: now });
    }
  };
  ws.onclose = (e) => { s.state = 'closed'; s.closeCode = e.code; s.clean = e.wasClean; };
  ws.onerror = () => { s.error = true; };
  s.ws = ws; window.aafr.socks[name] = s; return true;
};
window.aafrSend = function (name, text) { const s = window.aafr.socks[name]; s.sent[text] = performance.now(); s.ws.send(text); return true; };
window.aafrSendBin = function (name, n) {
  const b = new Uint8Array(n); let sum = 0;
  for (let i = 0; i < n; ++i) { b[i] = (i * 7) % 256; sum = (sum + b[i]) % 65521; }
  window.aafr.socks[name].ws.send(b.buffer); return sum;
};
window.aafrClose = function (name, code) { window.aafr.socks[name].ws.close(code); return true; };
window.aafrState = function (name) {
  const s = window.aafr.socks[name]; if (!s) return 'null';
  return JSON.stringify({ state: s.state, msgs: s.msgs, closeCode: s.closeCode, clean: s.clean, error: s.error, openMs: s.openMs });
};
true;
)JS";

int bigSum(int n)
{
    int sum = 0;
    for (int k = 0; k < n; ++k) sum = (sum + (k % 251)) % 65521;
    return sum;
}

}  // namespace

class BrowserProtocolTest : public QObject
{
    Q_OBJECT

private:
    gw::AAFRBrowserForwardingGateway *gateway() const { return g_setup->gateway(); }

    QVariant js(QWebEnginePage *page, const QString &script, int timeoutMs = 10000)
    {
        QVariant out;
        bool done = false;
        page->runJavaScript(script, [&](const QVariant &v) {
            out = v;
            done = true;
        });
        QElapsedTimer t;
        t.start();
        while (!done && t.elapsed() < timeoutMs) QTest::qWait(5);
        return out;
    }
    QJsonObject wsState(QWebEnginePage *page, const QString &name)
    {
        return QJsonDocument::fromJson(js(page, QStringLiteral("aafrState('%1')").arg(name)).toString().toUtf8()).object();
    }
    // Waits until the socket has at least n messages (or closes).
    QJsonArray waitMessages(QWebEnginePage *page, const QString &name, int n, int timeoutMs = 15000)
    {
        QElapsedTimer t;
        t.start();
        QJsonObject s;
        while (t.elapsed() < timeoutMs) {
            s = wsState(page, name);
            if (s["msgs"].toArray().size() >= n || s["state"] == "closed") break;
            QTest::qWait(10);
        }
        return s["msgs"].toArray();
    }
    QString waitState(QWebEnginePage *page, const QString &name, const QString &state, int timeoutMs = 15000)
    {
        QElapsedTimer t;
        t.start();
        QString now;
        while (t.elapsed() < timeoutMs) {
            now = wsState(page, name)["state"].toString();
            if (now == state || now == "closed") break;
            QTest::qWait(10);
        }
        return now;
    }
    bool load(QWebEngineView *view, const QUrl &url, int timeoutMs = 30000)
    {
        QSignalSpy spy(view, &QWebEngineView::loadFinished);
        view->load(url);
        if (!spy.wait(timeoutMs)) return false;
        return spy.takeFirst().at(0).toBool();
    }
    std::unique_ptr<QWebEngineView> makeView(QWebEngineProfile *profile)
    {
        auto view = std::make_unique<QWebEngineView>();
        view->setPage(new TrustingPage(profile, trusted_, view.get()));
        view->resize(800, 600);
        view->show();
        return view;
    }
    TrustingPage *page(QWebEngineView *view) { return static_cast<TrustingPage *>(view->page()); }

    std::vector<gw::GatewayConnectionRecord> records(const std::string &target, std::int64_t after = 0, bool finished = false)
    {
        std::vector<gw::GatewayConnectionRecord> out;
        for (const auto &r : gateway()->Connections(512))
            if (r.target == target && r.accepted_ns > after && (!finished || r.finished_ns != 0)) out.push_back(r);
        return out;
    }
    std::int64_t mark()
    {
        const auto all = gateway()->Connections(512);
        return all.empty() ? 0 : all.back().accepted_ns;
    }
    std::size_t live(const std::string &target)
    {
        std::size_t n = 0;
        for (const auto &r : gateway()->Connections(512)) n += (r.target == target && r.finished_ns == 0) ? 1 : 0;
        return n;
    }
    // Bytes each relay node carried toward the client / destination.
    std::map<std::string, std::pair<std::uint64_t, std::uint64_t>> relayBytes(const std::string &id)
    {
        std::map<std::string, std::pair<std::uint64_t, std::uint64_t>> out;
        for (const auto &t : stack_.network->StreamTraversals(id))
            out[t.node] = {t.bytes_toward_client, t.bytes_toward_destination};
        return out;
    }
    bool relaysClosed(const std::string &id)
    {
        for (const auto &t : stack_.network->StreamTraversals(id))
            if (t.open) return false;
        return true;
    }
    // The tunnel carrying a single live connection to `target` (opened after `after`).
    std::string liveTunnel(const std::string &target, std::int64_t after = 0)
    {
        for (const auto &r : gateway()->Connections(512))
            if (r.target == target && r.finished_ns == 0 && r.path_confirmed && r.accepted_ns > after) return r.connection_id;
        return std::string();
    }
    // Brings a relay back whatever happens in the test.
    struct RelayOffline {
        fwd::LoopbackForwardingNetwork *network;
        std::string relay;
        RelayOffline(fwd::LoopbackForwardingNetwork *n, std::string r) : network(n), relay(std::move(r))
        {
            network->SetNodeOnline(relay, false);
        }
        ~RelayOffline() { network->SetNodeOnline(relay, true); }
    };
    void checkTraversal(const std::string &id, const fwd::Path &path)
    {
        for (const auto &node : path) QVERIFY2(stack_.network->NodeLogged(node, id), node.c_str());
        for (const char *other : {"relay_a", "relay_b"}) {
            if (std::find(path.begin(), path.end(), std::string(other)) == path.end())
                QVERIFY2(!stack_.network->NodeLogged(other, id), other);
        }
    }
    void stepAsync(const std::function<void()> &f)
    {
        auto future = std::async(std::launch::async, f);
        QTRY_VERIFY_WITH_TIMEOUT(future.wait_for(std::chrono::seconds(0)) == std::future_status::ready, 120000);
    }
    // Evaluations (each through every stability gate) until the runtime
    // commits a switch; at most `max`. Returns the last decision.
    rt::DecisionRecord switchUntilCommitted(int waitMs, int max = 6)
    {
        const auto switches = stack_.runtime->Status().counters.switches;
        rt::DecisionRecord last = switchAfterChange(waitMs);
        for (int i = 0; i < max && stack_.runtime->Status().counters.switches == switches; ++i) {
            qInfo("evaluation %d: %s (%s)", i, rt::ToString(last.type), last.explanation.c_str());
            stepAsync([this] {
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                stack_.runtime->RequestEvaluation(rt::EvaluationTrigger::Manual, "confirm");
                stack_.runtime->Step();
            });
            last = stack_.runtime->Decisions(1).back();
        }
        return last;
    }
    rt::DecisionRecord switchAfterChange(int waitMs)
    {
        stepAsync([this, waitMs] {
            std::this_thread::sleep_for(std::chrono::milliseconds(waitMs));
            stack_.runtime->Step();
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
            stack_.runtime->RequestEvaluation(rt::EvaluationTrigger::Manual, "confirm");
            stack_.runtime->Step();
        });
        return stack_.runtime->Decisions(1).back();
    }
    std::string h2Target() const { return "localhost:" + std::to_string(h2_.port()); }
    QUrl h2Url(const QString &path) const { return QUrl(QStringLiteral("https://localhost:%1%2").arg(h2_.port()).arg(path)); }
    std::string wsTarget() const { return "127.0.0.1:" + std::to_string(ws_.port()); }
    std::string wssTarget() const { return "localhost:" + std::to_string(wss_.port()); }
    // Opens a page on the wss origin (accepting the test certificate) with the harness.
    std::unique_ptr<QWebEngineView> wssPage(QWebEngineProfile *profile)
    {
        auto view = makeView(profile);
        if (!load(view.get(), QUrl(QStringLiteral("https://localhost:%1/").arg(wss_.port())))) return nullptr;
        js(view->page(), QString::fromUtf8(kWsHarness));
        return view;
    }
    QString wssUrl() const { return QStringLiteral("wss://localhost:%1/ws").arg(wss_.port()); }

    Stack stack_;
    aafr::test::H2TestServer h2_;
    aafr::test::H2TestServer h2Untrusted_;
    aafr::test::WsTestServer ws_;
    aafr::test::WsTestServer wss_;
    QByteArray trusted_;
    std::unique_ptr<QWebEngineProfile> profile_;
    std::unique_ptr<ForwardingRequestObserver> observer_;

private slots:
    void initTestCase()
    {
        QVERIFY(g_setup && g_setup->gateway());
        QVERIFY(h2_.start(tlsDir() + "/localhost.crt", tlsDir() + "/localhost.key"));
        QVERIFY(h2Untrusted_.start(tlsDir() + "/untrusted.crt", tlsDir() + "/untrusted.key"));
        QVERIFY(ws_.start());
        QVERIFY(wss_.start(tlsDir() + "/localhost.crt", tlsDir() + "/localhost.key"));
        trusted_ = h2_.certificate().digest(QCryptographicHash::Sha256);
        QVERIFY(stack_.start());
        std::string error;
        QVERIFY(gateway()->Start(&error));
        gateway()->SetBindings(stack_.bindings());
        profile_ = std::make_unique<QWebEngineProfile>();  // off the record
        profile_->setHttpCacheType(QWebEngineProfile::NoCache);
        observer_ = std::make_unique<ForwardingRequestObserver>(gateway(), true);
        profile_->setUrlRequestInterceptor(observer_.get());
    }

    void cleanupTestCase()
    {
        profile_.reset();
        gateway()->Detach();
        stack_.runtime->Stop();
        stack_.network->Stop();
    }

    // ------------------------------------------------------------ HTTP/2

    void h2PageNegotiatesH2OverConfirmedRelays()
    {
        const auto active = stack_.runtime->ConfirmedConfiguration();
        QVERIFY(active);
        const std::int64_t m = mark();
        auto view = makeView(profile_.get());
        QVERIFY(load(view.get(), h2Url("/h2/page")));
        QCOMPARE(view->title(), QStringLiteral("AAFR h2 page"));
        QTRY_COMPARE_WITH_TIMEOUT(js(view->page(), "window.__h2assets || 0").toInt(), 8, 15000);
        // Certificate validation stayed on: one CA error, for exactly the test leaf.
        QVERIFY(page(view.get())->errors >= 1 && page(view.get())->rejected == 0);
        // Browser side: every resource and the navigation came over h2.
        QCOMPARE(js(view->page(), "performance.getEntriesByType('navigation')[0].nextHopProtocol").toString(),
                 QStringLiteral("h2"));
        const QString protos = js(view->page(),
                                  "JSON.stringify(performance.getEntriesByType('resource')"
                                  ".filter(r => r.name.includes('/h2/asset.js')).map(r => r.nextHopProtocol))")
                                   .toString();
        QCOMPARE(protos, QStringLiteral("[\"h2\",\"h2\",\"h2\",\"h2\",\"h2\",\"h2\",\"h2\",\"h2\"]"));
        // Server side: ALPN h2, all nine requests decoded from HTTP/2 frames on one
        // connection, several streams open at the same time.
        const auto conns = h2_.connections();
        QCOMPARE(int(conns.size()), 1);
        QCOMPARE(conns.front().alpn, QByteArray("h2"));
        QVERIFY(conns.front().protocol_error.empty());
        QCOMPARE(h2_.requestCount("/h2/asset.js"), 8);
        QCOMPARE(h2_.requestCount("/h2/page"), 1);
        for (const auto &r : h2_.requests()) QCOMPARE(r.connection, conns.front().id);
        QVERIFY2(conns.front().max_open_streams >= 2, "no concurrent HTTP/2 streams observed");
        // Gateway and relays: the single HTTP/2 connection rode one CONNECT
        // tunnel over the confirmed path. Chromium may open a spare tunnel
        // that never completes TLS (the server saw one connection); every
        // tunnel is still relay-confirmed on the same path.
        const auto recs = records(h2Target(), m);
        // Chromium may race a second TLS connection and abandon it after the
        // server's first flight; the server then sees one connection. The
        // carrier is the tunnel with the traffic; any other carried at most a
        // handshake flight and no HTTP/2 data.
        QVERIFY(!recs.empty());
        const gw::GatewayConnectionRecord *carrier = nullptr;
        std::uint64_t most = 0;
        for (const auto &r : recs) {
            QVERIFY(r.path_confirmed);
            QCOMPARE(r.observed_path, active->path);
            const auto bytes = relayBytes(r.connection_id);
            const std::uint64_t toBrowser = bytes.count("entry") ? bytes.at("entry").first : 0;
            qInfo("h2 tunnel %s: %llu bytes to the browser", r.connection_id.c_str(), (unsigned long long)toBrowser);
            if (toBrowser > most) {
                most = toBrowser;
                carrier = &r;
            }
        }
        QVERIFY(carrier);
        for (const auto &r : recs) {
            if (&r == carrier) continue;
            const auto bytes = relayBytes(r.connection_id);
            QVERIFY(!bytes.count("entry") || bytes.at("entry").first < 4096);
        }
        const auto &tunnel = *carrier;
        QCOMPARE(tunnel.kind, gw::ConnectionKind::ConnectTunnel);
        QVERIFY(tunnel.path_confirmed);
        QCOMPARE(tunnel.observed_path, active->path);
        QCOMPARE(tunnel.generation, active->config_version);
        checkTraversal(tunnel.connection_id, active->path);
        // Response traffic returns through the relays: a 2 MiB HTTP/2 body
        // raises every path node's client-bound byte count by at least 2 MiB.
        const auto before = relayBytes(tunnel.connection_id);
        js(view->page(), "fetch('/h2/large?bytes=2097152').then(r => r.arrayBuffer()).then(b => window.__large = b.byteLength); true");
        QTRY_COMPARE_WITH_TIMEOUT(js(view->page(), "window.__large || 0").toInt(), 2097152, 20000);
        const auto after = relayBytes(tunnel.connection_id);
        for (const auto &node : active->path) {
            QVERIFY(after.at(node).first >= before.at(node).first + 2097152);
            QVERIFY(after.at(node).second > before.at(node).second);  // requests went up the same path
        }
        QCOMPARE(h2_.requestCount("/h2/large"), 1);
        QCOMPARE(h2_.requests().back().connection, conns.front().id);  // same HTTP/2 connection
        QVERIFY(gateway()->FindConnection(tunnel.connection_id)->finished_ns == 0);  // still the same live tunnel
        qInfo("h2: %d requests on 1 connection, max %d concurrent streams, tunnel %s via %s",
              int(h2_.requests().size()), conns.front().max_open_streams, tunnel.connection_id.c_str(),
              fwd::PathText(tunnel.observed_path).c_str());
    }

    void h2ReusesConnectionAndCancelsStreams()
    {
        const int connsBefore = int(h2_.connections().size());
        auto view = makeView(profile_.get());
        QVERIFY(load(view.get(), h2Url("/h2/page")));
        QTRY_COMPARE_WITH_TIMEOUT(js(view->page(), "window.__h2assets || 0").toInt(), 8, 15000);
        // Same profile: the existing HTTP/2 connection (and tunnel) is reused.
        QCOMPARE(int(h2_.connections().size()), connsBefore);
        QCOMPARE(int(live(h2Target())), 1);
        // Browser cancellation of one stream: RST_STREAM, the connection survives.
        const int rstBefore = h2_.connections().back().rst_received;
        js(view->page(),
           "window.__ac = new AbortController(); fetch('/h2/slow?ms=5000', {signal: window.__ac.signal})"
           ".catch(e => window.__aborted = e.name); true");
        QTRY_VERIFY_WITH_TIMEOUT(h2_.requestCount("/h2/slow") >= 1, 10000);
        js(view->page(), "window.__ac.abort(); true");
        QTRY_COMPARE_WITH_TIMEOUT(js(view->page(), "window.__aborted || ''").toString(), QStringLiteral("AbortError"), 10000);
        QTRY_VERIFY_WITH_TIMEOUT(h2_.connections().back().rst_received > rstBefore, 10000);
        QVERIFY(!h2_.connections().back().closed);
        QCOMPARE(int(live(h2Target())), 1);
        // The connection keeps working after the cancelled stream.
        js(view->page(), "fetch('/h2/asset.js?i=99&ms=0').then(r => window.__after = r.status); true");
        QTRY_COMPARE_WITH_TIMEOUT(js(view->page(), "window.__after || 0").toInt(), 200, 10000);
    }

    void h2UnexpectedClosureAndNegatives()
    {
        // Unexpected server closure mid-response: the fetch fails, the tunnel
        // and its relay chain end, and the next request reconnects cleanly.
        auto view = makeView(profile_.get());
        QVERIFY(load(view.get(), h2Url("/h2/page")));
        const std::string tunnel = liveTunnel(h2Target());
        QVERIFY(!tunnel.empty());
        js(view->page(), "fetch('/h2/drop').then(r => r.arrayBuffer()).then(() => window.__drop = 'ok', e => window.__drop = 'failed'); true");
        QTRY_COMPARE_WITH_TIMEOUT(js(view->page(), "window.__drop || ''").toString(), QStringLiteral("failed"), 15000);
        QTRY_VERIFY_WITH_TIMEOUT(gateway()->FindConnection(tunnel)->finished_ns != 0, 15000);
        QTRY_VERIFY_WITH_TIMEOUT(relaysClosed(tunnel), 10000);
        js(view->page(), "fetch('/h2/asset.js?i=100&ms=0').then(r => window.__again = r.status); true");
        QTRY_COMPARE_WITH_TIMEOUT(js(view->page(), "window.__again || 0").toInt(), 200, 15000);
        QVERIFY(!liveTunnel(h2Target()).empty() && liveTunnel(h2Target()) != tunnel);

        // Invalid server certificate: refused by the browser; the server never
        // decodes an HTTP/2 request although the tunnel was established.
        const std::int64_t m = mark();
        auto bad = makeView(profile_.get());
        QVERIFY(!load(bad.get(), QUrl(QStringLiteral("https://localhost:%1/h2/page").arg(h2Untrusted_.port()))) ||
                js(bad->page(), "document.title").toString() != QStringLiteral("AAFR h2 page"));
        QVERIFY(page(bad.get())->rejected >= 1);
        QCOMPARE(int(h2Untrusted_.requests().size()), 0);
        const auto badRecs = records("localhost:" + std::to_string(h2Untrusted_.port()), m);
        QVERIFY(!badRecs.empty() && badRecs.front().path_confirmed);

        // Unreachable server and tunnel establishment failures.
        const auto closedPort = [] {
            const auto s = fwd::net::ListenLoopback(0, 1, nullptr, nullptr);
            const auto port = fwd::net::LocalPort(s);
            fwd::net::CloseSocket(s);
            return port;
        }();
        auto unreachable = makeView(profile_.get());
        QVERIFY(!load(unreachable.get(), QUrl(QStringLiteral("https://localhost:%1/").arg(closedPort))));
        bool sawUnreachable = false;
        for (const auto &r : records("localhost:" + std::to_string(closedPort), m, true))
            sawUnreachable = sawUnreachable || r.forwarding_status == "DEST_UNREACHABLE";
        QVERIFY(sawUnreachable);
        auto dns = makeView(profile_.get());
        QVERIFY(!load(dns.get(), QUrl(QStringLiteral("https://aafr-test.invalid/"))));
        bool sawDns = false;
        for (const auto &r : records("aafr-test.invalid:443", m, true)) sawDns = sawDns || r.forwarding_status == "DNS_FAILED";
        QVERIFY(sawDns);
    }

    // ------------------------------------------------------------ WebSocket

    void wsPlainBidirectional()
    {
        const auto active = stack_.runtime->ConfirmedConfiguration();
        const std::int64_t m = mark();
        auto view = makeView(profile_.get());
        QVERIFY(load(view.get(), QUrl(QStringLiteral("http://127.0.0.1:%1/").arg(ws_.port()))));
        js(view->page(), QString::fromUtf8(kWsHarness));
        js(view->page(), QStringLiteral("aafrOpen('a', 'ws://127.0.0.1:%1/ws')").arg(ws_.port()));
        QCOMPARE(waitState(view->page(), "a", "open"), QStringLiteral("open"));
        js(view->page(), "aafrSend('a', 'echo:hello')");
        auto msgs = waitMessages(view->page(), "a", 1);
        QVERIFY(!msgs.isEmpty() && msgs[0].toObject()["d"] == "echo:hello");
        const int sum = js(view->page(), "aafrSendBin('a', 4096)").toInt();
        msgs = waitMessages(view->page(), "a", 2);
        QVERIFY(msgs.size() >= 2 && msgs[1].toObject()["n"].toInt() == 4096 && msgs[1].toObject()["sum"].toInt() == sum);
        // Chromium used CONNECT for ws:// through the proxy.
        const auto recs = records(wsTarget(), m);
        bool tunnelled = false;
        for (const auto &r : recs) {
            if (r.kind != gw::ConnectionKind::ConnectTunnel || r.finished_ns != 0) continue;
            tunnelled = true;
            QVERIFY(r.path_confirmed);
            QCOMPARE(r.observed_path, active->path);
            checkTraversal(r.connection_id, active->path);
        }
        QVERIFY2(tunnelled, "ws:// did not use a CONNECT tunnel");
        js(view->page(), "aafrClose('a', 1000)");
        QCOMPARE(waitState(view->page(), "a", "closed"), QStringLiteral("closed"));
        QCOMPARE(wsState(view->page(), "a")["closeCode"].toInt(), 1000);
        QTRY_VERIFY_WITH_TIMEOUT(ws_.connections().back().close_code_received == 1000, 5000);
    }

    void wssBidirectionalMessagingAndTraversal()
    {
        const auto active = stack_.runtime->ConfirmedConfiguration();
        auto view = wssPage(profile_.get());
        QVERIFY(view);
        const std::int64_t m = mark();
        js(view->page(), QStringLiteral("aafrOpen('s', '%1')").arg(wssUrl()));
        QCOMPARE(waitState(view->page(), "s", "open"), QStringLiteral("open"));
        // Diagnostics: how long the open took and what the gateway saw meanwhile.
        qInfo("wss open took %.1f ms (page clock); certificate errors so far %d",
              wsState(view->page(), "s")["openMs"].toDouble(), page(view.get())->errors);
        for (const auto &rec : records(wssTarget(), m))
            qInfo("  wss tunnel %s +%.1f ms establish %.1f state %s end %s bytes %llu/%llu",
                  rec.connection_id.c_str(), (rec.accepted_ns - m) / 1e6, rec.establish_ms.value_or(-1),
                  gw::ToString(rec.state), rec.end.c_str(), (unsigned long long)rec.bytes_from_client,
                  (unsigned long long)rec.bytes_to_client);
        const std::string id = liveTunnel(wssTarget());
        QVERIFY(!id.empty());
        const auto rec = *gateway()->FindConnection(id);
        QVERIFY(rec.accepted_ns > m && rec.kind == gw::ConnectionKind::ConnectTunnel);
        QCOMPARE(rec.observed_path, active->path);
        checkTraversal(id, active->path);
        // Sequential text messages, binary, large messages both ways, ping/pong.
        for (int i = 0; i < 20; ++i) js(view->page(), QStringLiteral("aafrSend('s', 'echo:%1')").arg(i));
        auto msgs = waitMessages(view->page(), "s", 20);
        QCOMPARE(msgs.size(), 20);
        for (int i = 0; i < 20; ++i) QCOMPARE(msgs[i].toObject()["d"].toString(), QStringLiteral("echo:%1").arg(i));
        const int sum = js(view->page(), "aafrSendBin('s', 1048576)").toInt();
        msgs = waitMessages(view->page(), "s", 21, 30000);
        QVERIFY(msgs.size() >= 21);
        QCOMPARE(msgs[20].toObject()["n"].toInt(), 1048576);
        QCOMPARE(msgs[20].toObject()["sum"].toInt(), sum);
        js(view->page(), "aafrSend('s', 'big:2097152')");
        msgs = waitMessages(view->page(), "s", 22, 30000);
        QCOMPARE(msgs[21].toObject()["n"].toInt(), 2097152);
        QCOMPARE(msgs[21].toObject()["sum"].toInt(), bigSum(2097152));
        js(view->page(), "aafrSend('s', 'ping-me')");
        msgs = waitMessages(view->page(), "s", 23);
        QCOMPARE(msgs[22].toObject()["d"].toString(), QStringLiteral("pong-received"));  // the browser answered the ping
        // Bytes moved through every relay of the path, in both directions.
        for (const auto &e : relayBytes(id)) {
            QVERIFY(e.second.first >= 3u * 1024 * 1024);   // toward the browser
            QVERIFY(e.second.second >= 1u * 1024 * 1024);  // toward the server
        }
        const auto server = ws_.connections();  // (plain server untouched)
        Q_UNUSED(server);
        QVERIFY(observer_->counts().webSockets >= 1);
        // Normal close from the server side.
        js(view->page(), "aafrSend('s', 'close-me')");
        QCOMPARE(waitState(view->page(), "s", "closed"), QStringLiteral("closed"));
        QCOMPARE(wsState(view->page(), "s")["closeCode"].toInt(), 1000);
        QTRY_VERIFY_WITH_TIMEOUT(gateway()->FindConnection(id)->finished_ns != 0, 10000);
        QTRY_VERIFY_WITH_TIMEOUT(relaysClosed(id), 10000);
    }

    void wssAbnormalDisconnectAndCancellation()
    {
        auto view = wssPage(profile_.get());
        QVERIFY(view);
        // Abnormal server disconnect: 1006, nothing left open.
        js(view->page(), QStringLiteral("aafrOpen('d', '%1')").arg(wssUrl()));
        QCOMPARE(waitState(view->page(), "d", "open"), QStringLiteral("open"));
        const std::string dropped = liveTunnel(wssTarget());
        js(view->page(), "aafrSend('d', 'drop-me')");
        QCOMPARE(waitState(view->page(), "d", "closed"), QStringLiteral("closed"));
        QCOMPARE(wsState(view->page(), "d")["closeCode"].toInt(), 1006);
        QTRY_VERIFY_WITH_TIMEOUT(gateway()->FindConnection(dropped)->finished_ns != 0, 10000);
        QTRY_VERIFY_WITH_TIMEOUT(relaysClosed(dropped), 10000);
        // Browser cancellation: navigating away closes the socket.
        js(view->page(), QStringLiteral("aafrOpen('n', '%1')").arg(wssUrl()));
        QCOMPARE(waitState(view->page(), "n", "open"), QStringLiteral("open"));
        const std::string navigated = liveTunnel(wssTarget());
        const int openBefore = wss_.openWebSockets();
        QVERIFY(load(view.get(), QUrl(QStringLiteral("about:blank"))));
        QTRY_VERIFY_WITH_TIMEOUT(wss_.openWebSockets() < openBefore, 10000);
        QTRY_VERIFY_WITH_TIMEOUT(gateway()->FindConnection(navigated)->finished_ns != 0, 15000);
        QTRY_VERIFY_WITH_TIMEOUT(relaysClosed(navigated), 10000);
    }

    void wssRapidRepeatedAndMultipleTabs()
    {
        auto view = wssPage(profile_.get());
        QVERIFY(view);
        const std::int64_t m = mark();
        // 20 open/close cycles in quick succession.
        for (int i = 0; i < 20; ++i) {
            const QString name = QStringLiteral("r%1").arg(i);
            js(view->page(), QStringLiteral("aafrOpen('%1', '%2')").arg(name, wssUrl()));
            QCOMPARE(waitState(view->page(), name, "open"), QStringLiteral("open"));
            js(view->page(), QStringLiteral("aafrSend('%1', 'echo:r')").arg(name));
            QCOMPARE(waitMessages(view->page(), name, 1).size(), 1);
            js(view->page(), QStringLiteral("aafrClose('%1', 1000)").arg(name));
        }
        QTRY_VERIFY_WITH_TIMEOUT(live(wssTarget()) == 0, 15000);
        const auto cycles = records(wssTarget(), m, true);
        QVERIFY(cycles.size() >= 20);
        for (const auto &r : cycles) {
            QVERIFY(r.path_confirmed);
            QTRY_VERIFY_WITH_TIMEOUT(relaysClosed(r.connection_id), 5000);
        }
        // Three tabs, one socket each: three tunnels on the confirmed path.
        const auto active = stack_.runtime->ConfirmedConfiguration();
        std::vector<std::unique_ptr<QWebEngineView>> tabs;
        for (int i = 0; i < 3; ++i) {
            tabs.push_back(wssPage(profile_.get()));
            QVERIFY(tabs.back());
            js(tabs.back()->page(), QStringLiteral("aafrOpen('t', '%1')").arg(wssUrl()));
        }
        for (auto &t : tabs) QCOMPARE(waitState(t->page(), "t", "open"), QStringLiteral("open"));
        QTRY_COMPARE_WITH_TIMEOUT(int(live(wssTarget())), 3, 10000);
        for (const auto &r : gateway()->Connections(64)) {
            if (r.target == wssTarget() && r.finished_ns == 0) QCOMPARE(r.observed_path, active->path);
        }
        tabs.clear();  // browser tabs closed while their sockets are open
        QTRY_VERIFY_WITH_TIMEOUT(live(wssTarget()) == 0, 15000);
        QTRY_VERIFY_WITH_TIMEOUT(wss_.openWebSockets() == 0, 15000);
    }

    void wssPinnedAcrossSwitchAndDrains()
    {
        const auto a = stack_.runtime->ConfirmedConfiguration();
        QVERIFY(a && a->path == aafr::test::kPathA);
        auto view = wssPage(profile_.get());
        QVERIFY(view);
        const std::int64_t oldMark = mark();
        js(view->page(), QStringLiteral("aafrOpen('old', '%1')").arg(wssUrl()));
        QCOMPARE(waitState(view->page(), "old", "open"), QStringLiteral("open"));
        const std::string oldId = liveTunnel(wssTarget(), oldMark);
        QVERIFY(!oldId.empty());
        QCOMPARE(gateway()->FindConnection(oldId)->observed_path, aafr::test::kPathA);

        // Authorised runtime switch A -> B (through every gate).
        stack_.mutate("entry", "relay_a", 150.0);
        const auto switchesBefore = stack_.runtime->Status().counters.switches;
        switchUntilCommitted(150);
        QCOMPARE(stack_.runtime->Status().counters.switches, switchesBefore + 1);
        const auto b = stack_.runtime->ConfirmedConfiguration();
        QVERIFY(b && b->path == aafr::test::kPathB);
        QTRY_VERIFY_WITH_TIMEOUT(gateway()->Status().current && gateway()->Status().current->generation == b->config_version, 5000);

        // The existing socket stays pinned to A and keeps working over A.
        const auto before = relayBytes(oldId);
        QCOMPARE(wsState(view->page(), "old")["state"].toString(), QStringLiteral("open"));
        const int n0 = wsState(view->page(), "old")["msgs"].toArray().size();
        for (int i = 0; i < 5; ++i) js(view->page(), QStringLiteral("aafrSend('old', 'echo:after-switch-%1')").arg(i));
        QCOMPARE(waitMessages(view->page(), "old", n0 + 5).size(), n0 + 5);
        const auto after = relayBytes(oldId);
        QVERIFY(after.at("relay_a").first > before.at("relay_a").first);
        QVERIFY(after.count("relay_b") == 0);
        QCOMPARE(gateway()->FindConnection(oldId)->observed_path, aafr::test::kPathA);
        QVERIFY(gateway()->FindConnection(oldId)->superseded);
        QVERIFY(gateway()->Status().draining_connections >= 1);

        // A new socket uses B.
        const std::int64_t newMark = mark();
        js(view->page(), QStringLiteral("aafrOpen('new', '%1')").arg(wssUrl()));
        QCOMPARE(waitState(view->page(), "new", "open"), QStringLiteral("open"));
        const std::string newId = liveTunnel(wssTarget(), newMark);
        QVERIFY(!newId.empty());
        QCOMPARE(gateway()->FindConnection(newId)->observed_path, aafr::test::kPathB);
        checkTraversal(newId, aafr::test::kPathB);
        js(view->page(), "aafrSend('new', 'echo:on-b')");
        QVERIFY(!waitMessages(view->page(), "new", 1).isEmpty());

        // Closing the old socket releases its pin. Generation A retires only
        // once nothing references it: Chromium may still hold other pooled
        // connections pinned to A (e.g. the HTTP/2 connection from earlier
        // cases), and while they live A must stay draining, not retired.
        js(view->page(), "aafrClose('old', 1000)");
        QCOMPARE(waitState(view->page(), "old", "closed"), QStringLiteral("closed"));
        QTRY_VERIFY_WITH_TIMEOUT(relaysClosed(oldId), 10000);
        QTRY_VERIFY_WITH_TIMEOUT(gateway()->FindConnection(oldId)->finished_ns != 0, 10000);
        const auto retired = [&] {
            for (const auto &e : gateway()->Events(2000))
                if (e.type == gw::GatewayEventType::ConfigurationRetired && e.generation == a->config_version) return true;
            return false;
        };
        const auto pinnedToA = [&] {
            std::size_t n = 0;
            for (const auto &c : gateway()->Connections(512))
                n += (c.finished_ns == 0 && c.generation == a->config_version) ? 1 : 0;
            return n;
        };
        QTest::qWait(300);  // a watcher tick
        if (pinnedToA() > 0) {
            QVERIFY(!retired());  // still referenced: not retired
            QVERIFY(gateway()->Status().active_by_generation.count(a->config_version) == 1);
            qInfo("generation %llu still referenced by %d pooled connection(s); closing them",
                  (unsigned long long)a->config_version, int(pinnedToA()));
            h2_.closeAll();  // GOAWAY: Chromium closes its pooled HTTP/2 connection
        }
        QTRY_VERIFY_WITH_TIMEOUT(pinnedToA() == 0, 15000);
        QTRY_VERIFY_WITH_TIMEOUT(retired(), 10000);
        js(view->page(), "aafrClose('new', 1000)");
        QCOMPARE(waitState(view->page(), "new", "closed"), QStringLiteral("closed"));
        QTRY_VERIFY_WITH_TIMEOUT(relaysClosed(newId), 10000);

        // Recovery to A for the remaining tests.
        stack_.mutate("entry", "relay_a", 5.0);
        switchUntilCommitted(400);
        QCOMPARE(stack_.runtime->Status().counters.switches, switchesBefore + 2);
        QVERIFY(stack_.runtime->ConfirmedConfiguration()->path == aafr::test::kPathA);
    }

    // ------------------------------------------------------------ failures and shutdown

    void relayFailureEndsH2AndWebSocket()
    {
        const auto active = stack_.runtime->ConfirmedConfiguration();
        const std::string relay = active->path[1];
        // A fresh profile: no pooled connections pinned to an older path.
        QWebEngineProfile fresh;
        const std::int64_t before = mark();
        auto view = wssPage(&fresh);
        QVERIFY(view);
        js(view->page(), QStringLiteral("aafrOpen('f', '%1')").arg(wssUrl()));
        QCOMPARE(waitState(view->page(), "f", "open"), QStringLiteral("open"));
        const std::string wsId = liveTunnel(wssTarget(), before);
        auto h2view = makeView(&fresh);
        QVERIFY(load(h2view.get(), h2Url("/h2/page")));
        const std::string h2Id = liveTunnel(h2Target(), before);
        QVERIFY(!wsId.empty() && !h2Id.empty());
        QCOMPARE(gateway()->FindConnection(wsId)->observed_path, active->path);
        QCOMPARE(gateway()->FindConnection(h2Id)->observed_path, active->path);
        const RelayOffline down(stack_.network.get(), relay);  // the active relay dies
        QCOMPARE(waitState(view->page(), "f", "closed"), QStringLiteral("closed"));
        QCOMPARE(wsState(view->page(), "f")["closeCode"].toInt(), 1006);
        QTRY_VERIFY_WITH_TIMEOUT(gateway()->FindConnection(wsId)->finished_ns != 0, 10000);
        QTRY_VERIFY_WITH_TIMEOUT(gateway()->FindConnection(h2Id)->finished_ns != 0, 10000);
        QTRY_VERIFY_WITH_TIMEOUT(relaysClosed(wsId) && relaysClosed(h2Id), 10000);
        // A new connection on the dead path fails as a relay failure (never direct).
        const std::int64_t m = mark();
        auto fresh2 = makeView(&fresh);
        QVERIFY(!load(fresh2.get(), h2Url("/h2/page?relay-down")));
        bool relayFailure = false;
        for (const auto &r : records(h2Target(), m, true)) relayFailure = relayFailure || r.relay_failure;
        QVERIFY(relayFailure);
        h2view.reset();
        view.reset();
        fresh2.reset();
    }

    void gatewayShutdownWithOpenWebSocket()
    {
        // Reset the runtime's failure count from the previous case.
        stepAsync([this] { stack_.runtime->Step(); });
        auto view = wssPage(profile_.get());
        QVERIFY(view);
        js(view->page(), QStringLiteral("aafrOpen('g', '%1')").arg(wssUrl()));
        QCOMPARE(waitState(view->page(), "g", "open"), QStringLiteral("open"));
        const std::string id = liveTunnel(wssTarget());
        QVERIFY(!id.empty());
        QElapsedTimer t;
        t.start();
        gateway()->Stop();  // the gateway goes away with the socket open
        QVERIFY(t.elapsed() < 5000);
        QCOMPARE(waitState(view->page(), "g", "closed"), QStringLiteral("closed"));
        QCOMPARE(wsState(view->page(), "g")["closeCode"].toInt(), 1006);
        QTRY_VERIFY_WITH_TIMEOUT(relaysClosed(id), 10000);
        QTRY_VERIFY_WITH_TIMEOUT(wss_.openWebSockets() == 0, 10000);
        QTRY_VERIFY_WITH_TIMEOUT(stack_.network->ActiveStreams() == 0, 10000);  // nothing leaked
        // And nothing goes direct afterwards.
        const int handshakes = wss_.handshakes();
        js(view->page(), QStringLiteral("aafrOpen('h', '%1')").arg(wssUrl()));
        QCOMPARE(waitState(view->page(), "h", "open"), QStringLiteral("closed"));
        QCOMPARE(wss_.handshakes(), handshakes);
    }
};

int main(int argc, char **argv)
{
    BrowserForwardingOptions options;
    options.mode = BrowserForwardingMode::Controlled;
    options.forwardLoopback = true;
    QString error;
    const auto setup = BrowserForwardingSetup::prepare(options, &error);
    if (!setup) {
        std::fprintf(stderr, "%s\n", qPrintable(error));
        return 1;
    }
    g_setup = setup.get();
    QApplication app(argc, argv);
    QSslSocket::setActiveBackend(QStringLiteral("schannel"));
    BrowserProtocolTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "browser_protocol_test.moc"
