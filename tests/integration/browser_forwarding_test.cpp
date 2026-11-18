// V0.7.3 end-to-end browser forwarding: real Qt WebEngine page loads through
// the AAFR browser forwarding gateway and the loopback relays.
//
// Qt WebEngine's proxy is process-wide and fixed at start-up, so main() sets
// controlled forwarding up (BrowserForwardingSetup: gateway bound, Chromium
// --proxy-server) before QApplication, exactly as aafr-browser does. Every
// assertion about traversal uses the relays' own records. Local servers only:
// an HTTP origin (tests/support/http_test_server.h) and an HTTPS origin
// (QSslServer with the test-only certificate in tests/fixtures/tls).
//
// HTTPS trust: the test certificate is trusted explicitly and only here, by
// accepting a certificate error whose leaf is exactly the test certificate
// and whose only problem is the unknown test CA. Certificate checking is
// never disabled; an unknown certificate is rejected and its load fails.

#include <QApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QLabel>
#include <QTableWidget>
#include <QFile>
#include <QFileInfo>
#include <QSignalSpy>
#include <QSslCertificate>
#include <QSslConfiguration>
#include <QSslKey>
#include <QSslServer>
#include <QSslSocket>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QWebEngineCertificateError>
#include <QWebEngineDownloadRequest>
#include <QWebEnginePage>
#include <QWebEngineProfile>
#include <QWebEngineView>

#include <future>
#include <memory>

#include "browser_forwarding.h"
#include "browser_window.h"
#include "devtools_panel.h"
#include "tab_manager.h"
#include "routing/controller/forwarding_service.h"
#include "routing/forwarding/loopback_forwarding.h"
#include "routing/gateway/runtime_gateway_binding.h"
#include "runtime/adaptive/aafr_runtime.h"
#include "tests/support/http_test_server.h"
#include "tests/support/runtime_test_support.h"

namespace fwd = aafr::forwarding;
namespace gw = aafr::gateway;
namespace rt = aafr::runtime;

namespace {

BrowserForwardingSetup *g_setup = nullptr;

QByteArray readFile(const QString &path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

QString tlsDir() { return QStringLiteral(AAFR_TEST_TLS_DIR); }

// A minimal HTTPS origin on 127.0.0.1 (Schannel), one response per connection.
class TlsOrigin : public QObject
{
public:
    bool start(const QString &certFile, const QString &keyFile, const QByteArray &body)
    {
        body_ = body;
        QSslConfiguration conf = QSslConfiguration::defaultConfiguration();
        const auto certs = QSslCertificate::fromData(readFile(certFile));
        if (certs.isEmpty()) return false;
        certificate_ = certs.first();
        conf.setLocalCertificate(certificate_);
        conf.setPrivateKey(QSslKey(readFile(keyFile), QSsl::Rsa));
        server_.setSslConfiguration(conf);
        connect(&server_, &QSslServer::pendingConnectionAvailable, this, [this] {
            while (auto *s = qobject_cast<QSslSocket *>(server_.nextPendingConnection())) {
                connect(s, &QSslSocket::readyRead, s, [this, s] {
                    buffer_[s] += s->readAll();
                    if (!buffer_[s].contains("\r\n\r\n")) return;
                    ++requests_;
                    const QByteArray head = buffer_[s];
                    const bool favicon = head.startsWith("GET /favicon.ico");
                    const QByteArray b = favicon ? QByteArray() : body_;
                    s->write("HTTP/1.1 " + QByteArray(favicon ? "204 No Content" : "200 OK") +
                             "\r\nContent-Type: text/html\r\nContent-Length: " + QByteArray::number(b.size()) +
                             "\r\nConnection: close\r\n\r\n" + b);
                    s->disconnectFromHost();
                    buffer_.remove(s);
                });
                connect(s, &QSslSocket::disconnected, s, &QObject::deleteLater);
            }
        });
        return server_.listen(QHostAddress::LocalHost, 0);
    }
    quint16 port() const { return server_.serverPort(); }
    int requests() const { return requests_; }
    QSslCertificate certificate() const { return certificate_; }

private:
    QSslServer server_;
    QSslCertificate certificate_;
    QByteArray body_;
    QHash<QSslSocket *, QByteArray> buffer_;
    int requests_ = 0;
};

// Everything AAFR needs for the test runtime (two disjoint relay paths).
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
        config.session_id = "brfw";
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

}  // namespace

class BrowserForwardingTest : public QObject
{
    Q_OBJECT

private:
    gw::AAFRBrowserForwardingGateway *gateway() const { return g_setup->gateway(); }

    // Loads and waits; true when Qt reports success.
    bool load(QWebEngineView *view, const QUrl &url, int timeoutMs = 30000)
    {
        QSignalSpy spy(view, &QWebEngineView::loadFinished);
        view->load(url);
        if (!spy.wait(timeoutMs)) return false;
        return spy.takeFirst().at(0).toBool();
    }
    QString pageText(QWebEngineView *view)
    {
        QString out;
        bool done = false;
        view->page()->toPlainText([&](const QString &t) {
            out = t;
            done = true;
        });
        QElapsedTimer timer;
        timer.start();
        while (!done && timer.elapsed() < 10000) QTest::qWait(20);
        return out;
    }
    // Gateway records for one target, finished ones only.
    std::vector<gw::GatewayConnectionRecord> recordsFor(const std::string &target, std::uint64_t afterAccepted = 0)
    {
        std::vector<gw::GatewayConnectionRecord> out;
        for (const auto &r : gateway()->Connections(512)) {
            if (r.target == target && r.finished_ns != 0 && r.accepted_ns > afterAccepted) out.push_back(r);
        }
        return out;
    }
    // Connections still forwarding for the origin (Chromium's idle
    // pre-connected proxy sockets carry no request and are not counted).
    std::size_t forwardingToOrigin()
    {
        std::size_t n = 0;
        for (const auto &r : gateway()->Connections(512))
            n += (r.finished_ns == 0 && r.generation != 0 && r.target == originTarget()) ? 1 : 0;
        return n;
    }
    std::int64_t lastAccepted()
    {
        const auto all = gateway()->Connections(512);
        return all.empty() ? 0 : all.back().accepted_ns;
    }
    std::string originTarget() const { return "127.0.0.1:" + std::to_string(origin_.port()); }
    QUrl originUrl(const QString &path) const
    {
        return QUrl(QStringLiteral("http://127.0.0.1:%1%2").arg(origin_.port()).arg(path));
    }
    // Runs a blocking runtime step off the GUI thread while Qt keeps running.
    void stepAsync(const std::function<void()> &f)
    {
        auto future = std::async(std::launch::async, f);
        QTRY_VERIFY_WITH_TIMEOUT(future.wait_for(std::chrono::seconds(0)) == std::future_status::ready, 120000);
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

    Stack stack_;
    aafr::test::HttpTestServer origin_;
    TlsOrigin tls_;
    TlsOrigin untrusted_;
    QByteArray trustedFingerprint_;

private slots:
    void initTestCase()
    {
        QVERIFY(g_setup && g_setup->gateway());
        QVERIFY(g_setup->chromiumFlags().contains(QStringLiteral("--proxy-server=http://127.0.0.1:")));
        QVERIFY(origin_.Start());
        QVERIFY(tls_.start(tlsDir() + "/localhost.crt", tlsDir() + "/localhost.key",
                           "<!doctype html><title>AAFR TLS page</title><p>tls-ok</p>"));
        QVERIFY(untrusted_.start(tlsDir() + "/untrusted.crt", tlsDir() + "/untrusted.key",
                                 "<!doctype html><title>untrusted</title><p>must-not-load</p>"));
        trustedFingerprint_ = tls_.certificate().digest(QCryptographicHash::Sha256);
        QVERIFY(stack_.start());
        std::string error;
        QVERIFY(gateway()->Start(&error));
        gateway()->SetBindings(stack_.bindings());
        QVERIFY(stack_.runtime->ConfirmedConfiguration().has_value());
    }

    void cleanupTestCase()
    {
        gateway()->Detach();
        stack_.runtime->Stop();
        origin_.Stop();
        stack_.network->Stop();
    }

    void pageLoadTraversesConfirmedPath()
    {
        const auto active = stack_.runtime->ConfirmedConfiguration();
        QVERIFY(active);
        const std::uint64_t before = origin_.Requests();
        QWebEngineView view;
        view.resize(800, 600);
        view.show();
        QVERIFY(load(&view, originUrl("/page")));
        QCOMPARE(view.title(), QStringLiteral("AAFR forwarded page"));
        QVERIFY(pageText(&view).contains(QStringLiteral("forwarded-page-body")));
        QTRY_VERIFY_WITH_TIMEOUT(!recordsFor(originTarget()).empty(), 5000);
        const auto records = recordsFor(originTarget());
        bool sawPage = false;
        for (const auto &r : records) {
            QVERIFY(r.path_confirmed);
            QCOMPARE(r.observed_path, active->path);
            QCOMPARE(r.generation, active->config_version);
            // The relays themselves carried it.
            for (const auto &node : active->path) QVERIFY(stack_.network->NodeLogged(node, r.connection_id));
            QVERIFY(!r.relay_traversals.empty());
            sawPage = sawPage || r.http_status == 200;
        }
        QVERIFY(sawPage);
        QVERIFY(origin_.Requests() > before);
        // The browser's socket belongs to this process (or its child).
        QVERIFY(records.front().client_pid != 0);
        qInfo("browser page load: %d gateway connection(s), first establish %.2f ms, duration %.2f ms",
              int(records.size()), records.front().establish_ms.value_or(-1), records.front().duration_ms.value_or(-1));
    }

    void multipleTabsAndNavigation()
    {
        const std::int64_t mark = lastAccepted();
        QWebEngineView a, b, c;
        for (auto *v : {&a, &b, &c}) v->resize(400, 300);
        QSignalSpy sa(&a, &QWebEngineView::loadFinished), sb(&b, &QWebEngineView::loadFinished),
            sc(&c, &QWebEngineView::loadFinished);
        a.load(originUrl("/page"));
        b.load(originUrl("/page2"));
        c.load(originUrl("/slow?ms=300"));
        QTRY_VERIFY_WITH_TIMEOUT(sa.size() == 1 && sb.size() == 1 && sc.size() == 1, 30000);
        QVERIFY(sa.first().at(0).toBool() && sb.first().at(0).toBool() && sc.first().at(0).toBool());
        QCOMPARE(b.title(), QStringLiteral("AAFR second page"));
        QCOMPARE(c.title(), QStringLiteral("AAFR slow page"));
        QVERIFY(load(&a, originUrl("/page2")));  // navigation in the same tab
        QCOMPARE(a.title(), QStringLiteral("AAFR second page"));
        QTRY_VERIFY_WITH_TIMEOUT(recordsFor(originTarget(), mark).size() >= 4, 5000);
        for (const auto &r : recordsFor(originTarget(), mark)) QVERIFY(r.path_confirmed);
    }

    void redirect()
    {
        QWebEngineView view;
        QVERIFY(load(&view, originUrl("/redirect")));
        QCOMPARE(view.url().path(), QStringLiteral("/target"));
        QCOMPARE(view.title(), QStringLiteral("AAFR redirect target"));
    }

    void httpsNavigationKeepsTlsValidation()
    {
        QWebEngineView view;
        int errors = 0;
        bool authorityOnly = true;
        bool leafMatched = false;
        connect(view.page(), &QWebEnginePage::certificateError, this, [&](QWebEngineCertificateError e) {
            ++errors;
            const auto chain = e.certificateChain();
            leafMatched = !chain.isEmpty() && chain.first().digest(QCryptographicHash::Sha256) == trustedFingerprint_;
            authorityOnly = e.type() == QWebEngineCertificateError::CertificateAuthorityInvalid;
            // Explicit trust of exactly the test certificate; anything else is refused.
            if (leafMatched && authorityOnly) e.acceptCertificate();
            else e.rejectCertificate();
        });
        const QUrl url(QStringLiteral("https://localhost:%1/").arg(tls_.port()));
        const std::int64_t mark = lastAccepted();
        QVERIFY(load(&view, url));
        QCOMPARE(view.title(), QStringLiteral("AAFR TLS page"));
        // Chromium validated the real server's certificate itself: it saw the
        // origin's leaf (no gateway certificate) and only the private CA was unknown.
        QVERIFY(errors >= 1);
        QVERIFY(leafMatched);
        QVERIFY(authorityOnly);
        const std::string target = "localhost:" + std::to_string(tls_.port());
        QTRY_VERIFY_WITH_TIMEOUT(!recordsFor(target, mark).empty(), 5000);
        const auto r = recordsFor(target, mark).front();
        QCOMPARE(r.kind, gw::ConnectionKind::ConnectTunnel);
        QVERIFY(r.path_confirmed);
        QCOMPARE(r.observed_path, stack_.runtime->ConfirmedConfiguration()->path);
        QVERIFY(r.bytes_to_client > 1000);  // certificate chain + response, encrypted, relayed opaquely
        QVERIFY(tls_.requests() >= 1);

        // An unknown certificate is still refused: the load fails, even though
        // the tunnel itself was established through the relays.
        QWebEngineView bad;
        int badErrors = 0;
        connect(bad.page(), &QWebEnginePage::certificateError, this, [&](QWebEngineCertificateError e) {
            ++badErrors;
            const auto chain = e.certificateChain();
            if (!chain.isEmpty() && chain.first().digest(QCryptographicHash::Sha256) == trustedFingerprint_)
                e.acceptCertificate();
            else
                e.rejectCertificate();
        });
        const int before = untrusted_.requests();
        load(&bad, QUrl(QStringLiteral("https://localhost:%1/").arg(untrusted_.port())));
        QVERIFY(badErrors >= 1);
        QVERIFY(!pageText(&bad).contains(QStringLiteral("must-not-load")));
        QCOMPARE(untrusted_.requests(), before);  // no HTTP request was ever sent to it
    }

    void httpsClientVerifiesThroughTunnel()
    {
        // A Qt TLS client with full verification against the test CA, through
        // the gateway's CONNECT tunnel: proves the bytes are relayed opaquely.
        QSslSocket socket;
        QSslConfiguration conf = QSslConfiguration::defaultConfiguration();
        conf.setCaCertificates(QSslCertificate::fromData(readFile(tlsDir() + "/test-ca.crt")));
        socket.setSslConfiguration(conf);
        socket.setPeerVerifyName(QStringLiteral("localhost"));
        QList<QSslError> sslErrors;
        connect(&socket, &QSslSocket::sslErrors, this, [&](const QList<QSslError> &e) { sslErrors = e; });
        socket.connectToHost(QHostAddress::LocalHost, gateway()->port());
        QVERIFY(socket.waitForConnected(5000));
        socket.write("CONNECT localhost:" + QByteArray::number(tls_.port()) + " HTTP/1.1\r\nHost: localhost\r\n\r\n");
        QByteArray reply;
        QTRY_VERIFY_WITH_TIMEOUT((reply += socket.readAll()).contains("\r\n\r\n"), 10000);
        QVERIFY(reply.startsWith("HTTP/1.1 200 Connection Established"));
        socket.startClientEncryption();
        QTRY_VERIFY_WITH_TIMEOUT(socket.isEncrypted() || !sslErrors.isEmpty(), 10000);
        QVERIFY(sslErrors.isEmpty());
        QVERIFY(socket.isEncrypted());
        QCOMPARE(socket.peerCertificate(), tls_.certificate());  // end-to-end, no interception
        socket.write("GET / HTTP/1.1\r\nHost: localhost\r\n\r\n");
        QByteArray body;
        QTRY_VERIFY_WITH_TIMEOUT((body += socket.readAll()).contains("tls-ok"), 10000);
    }

    void streamingAndDownload()
    {
        QWebEngineView view;
        QVERIFY(load(&view, originUrl("/chunked")));
        QVERIFY(pageText(&view).contains(QStringLiteral("chunk-4;")));

        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QPointer<QWebEngineDownloadRequest> download;
        auto *profile = view.page()->profile();
        const auto c = connect(profile, &QWebEngineProfile::downloadRequested, this, [&](QWebEngineDownloadRequest *d) {
            d->setDownloadDirectory(dir.path());
            d->setDownloadFileName(QStringLiteral("aafr-download.bin"));
            d->accept();
            download = d;
        });
        view.load(originUrl("/download"));
        QTRY_VERIFY_WITH_TIMEOUT(download && download->isFinished(), 30000);
        QCOMPARE(download->state(), QWebEngineDownloadRequest::DownloadCompleted);
        QCOMPARE(QFileInfo(dir.filePath(QStringLiteral("aafr-download.bin"))).size(), qint64(1 << 20));
        disconnect(c);
    }

    void switchingPinsConnectionsAndRecovers()
    {
        const auto a = stack_.runtime->ConfirmedConfiguration();
        QVERIFY(a && a->path == aafr::test::kPathA);
        // A long response that will span the switch.
        QWebEngineView held;
        QSignalSpy heldDone(&held, &QWebEngineView::loadFinished);
        const std::int64_t mark = lastAccepted();
        held.load(originUrl("/hold?ms=20000"));
        // Wait until the held response is being forwarded over a confirmed path.
        std::string heldId;
        const auto findHeld = [&] {
            for (const auto &r : gateway()->Connections(50)) {
                if (r.finished_ns == 0 && r.accepted_ns > mark && r.path_confirmed && r.target == originTarget() &&
                    r.bytes_to_client > 0)
                    heldId = r.connection_id;
            }
            return !heldId.empty();
        };
        QTRY_VERIFY_WITH_TIMEOUT(findHeld(), 15000);

        // A congests; the runtime switches to B through its gates.
        stack_.mutate("entry", "relay_a", 150.0);
        rt::DecisionRecord r = switchAfterChange(150);
        QCOMPARE(r.type, rt::DecisionType::Switch);
        const auto b = stack_.runtime->ConfirmedConfiguration();
        QVERIFY(b && b->path == aafr::test::kPathB);

        // New browser connections traverse B ...
        const std::int64_t afterSwitch = lastAccepted();
        QWebEngineView fresh;
        QVERIFY(load(&fresh, originUrl("/page2?after-switch")));
        QTRY_VERIFY_WITH_TIMEOUT(!recordsFor(originTarget(), afterSwitch).empty(), 5000);
        for (const auto &rec : recordsFor(originTarget(), afterSwitch)) {
            QCOMPARE(rec.observed_path, aafr::test::kPathB);
            QVERIFY(stack_.network->NodeLogged("relay_b", rec.connection_id));
            QVERIFY(!stack_.network->NodeLogged("relay_a", rec.connection_id));
        }
        // ... while the pinned one is still open on A and draining.
        const auto heldNow = gateway()->FindConnection(heldId);
        QVERIFY(heldNow && heldNow->finished_ns == 0);
        QCOMPARE(heldNow->observed_path, aafr::test::kPathA);
        QVERIFY(gateway()->Status().draining_connections >= 1);
        // It completes normally over A.
        QTRY_VERIFY_WITH_TIMEOUT(heldDone.size() == 1, 40000);
        QVERIFY(heldDone.first().at(0).toBool());
        QVERIFY(pageText(&held).contains(QStringLiteral("held-response-complete")));
        QTRY_VERIFY_WITH_TIMEOUT(gateway()->FindConnection(heldId)->finished_ns != 0, 10000);
        QVERIFY(!stack_.network->NodeLogged("relay_b", heldId));

        // Recovery: congestion clears and the runtime returns to A.
        stack_.mutate("entry", "relay_a", 5.0);
        r = switchAfterChange(400);
        QCOMPARE(r.type, rt::DecisionType::Switch);
        QVERIFY(stack_.runtime->ConfirmedConfiguration()->path == aafr::test::kPathA);
        const std::int64_t afterRecovery = lastAccepted();
        QWebEngineView back;
        QVERIFY(load(&back, originUrl("/page?after-recovery")));
        QTRY_VERIFY_WITH_TIMEOUT(!recordsFor(originTarget(), afterRecovery).empty(), 5000);
        for (const auto &rec : recordsFor(originTarget(), afterRecovery)) QCOMPARE(rec.observed_path, aafr::test::kPathA);
        bool retired = false;
        for (const auto &e : gateway()->Events(1000)) retired = retired || e.type == gw::GatewayEventType::ConfigurationRetired;
        QVERIFY(retired);
    }

    void cancellationAndBrowserClose()
    {
        const std::int64_t cancelMark = lastAccepted();
        {
            QWebEngineView view;
            QSignalSpy done(&view, &QWebEngineView::loadFinished);
            const std::int64_t mark = lastAccepted();
            const std::uint64_t slowBefore = origin_.Count("/slow");
            view.load(originUrl("/slow?ms=3000"));
            // The request has crossed the relays and reached the origin.
            QTRY_VERIFY_WITH_TIMEOUT(origin_.Count("/slow") > slowBefore, 10000);
            view.stop();  // the user cancels the navigation
            // Chromium drops the request; the gateway sees its connection end.
            QTRY_VERIFY_WITH_TIMEOUT(forwardingToOrigin() == 0, 10000);
            const auto records = recordsFor(originTarget(), mark);
            QVERIFY(!records.empty());
            for (const auto &r : records)
                qInfo("cancel record %s %s %s end=%s reason=%s dur=%.0f status=%d", r.connection_id.c_str(),
                      gw::ToString(r.kind), gw::ToString(r.state), r.end.c_str(), r.failure_reason.c_str(),
                      r.duration_ms.value_or(-1), r.http_status);
            // The page's own connection was cancelled; it never completed
            // (a favicon request may have completed alongside it).
            bool cancelled = false;
            for (const auto &r : records) {
                cancelled = cancelled || r.state == gw::ConnectionState::Cancelled;
                QVERIFY(r.http_status != 200);
            }
            QVERIFY(cancelled);
            QVERIFY(done.isEmpty() || !done.first().at(0).toBool());
        }
        {
            auto *view = new QWebEngineView;
            view->load(originUrl("/hold?ms=8000"));
            QTRY_VERIFY_WITH_TIMEOUT(forwardingToOrigin() >= 1, 10000);
            delete view;  // the browser closes the page mid-response
        }
        QTRY_VERIFY_WITH_TIMEOUT(forwardingToOrigin() == 0, 7000);  // before the 8 s response ends
        // The relays tore the cancelled connections' tunnels down too.
        const auto relaysClosed = [&] {
            for (const auto &r : recordsFor(originTarget(), cancelMark)) {
                for (const auto &t : stack_.network->StreamTraversals(r.connection_id))
                    if (t.open) return false;
            }
            return true;
        };
        QTRY_VERIFY_WITH_TIMEOUT(relaysClosed(), 10000);
        qInfo("relay tunnels still open (other kept-alive browser connections): %d",
              int(stack_.network->ActiveStreams()));
    }

    void browserWindowUsesItsRuntime()
    {
        // The real application wiring: BrowserWindow in controlled mode with
        // its own ForwardingService relays and verified runtime.
        gateway()->Detach();
        QWebEngineProfile profile;  // off-the-record, own interceptor
        auto window = std::make_unique<BrowserWindow>(nullptr, QUrl(QStringLiteral("about:blank")),
                                                      QStringLiteral("https://www.google.com/search?q=%1"), &profile);
        window->attachBrowserForwarding(g_setup, &profile);
        window->show();
        QTRY_VERIFY_WITH_TIMEOUT(window->adaptiveRuntime()->ConfirmedConfiguration().has_value(), 60000);
        QTRY_VERIFY_WITH_TIMEOUT(gateway()->Status().has_bindings, 10000);
        const auto active = window->adaptiveRuntime()->ConfirmedConfiguration();
        const std::int64_t mark = lastAccepted();
        auto *view = window->tabManager()->currentView();
        QVERIFY(view);
        QVERIFY(load(view, originUrl("/page?window")));
        QCOMPARE(view->title(), QStringLiteral("AAFR forwarded page"));
        QTRY_VERIFY_WITH_TIMEOUT(!recordsFor(originTarget(), mark).empty(), 5000);
        const auto rec = recordsFor(originTarget(), mark).front();
        QCOMPARE(rec.observed_path, active->path);
        QCOMPARE(rec.generation, active->config_version);
        for (const auto &node : active->path)
            QVERIFY(window->forwardingService()->network().NodeLogged(node, rec.connection_id));
        // Developer Tools shows the real forwarding state.
        auto *panel = window->devToolsPanel();
        panel->refreshNow();
        auto *listen = panel->findChild<QLabel *>(QStringLiteral("gateway_listen"));
        QVERIFY(listen && listen->text() == QStringLiteral("127.0.0.1:%1").arg(gateway()->port()));
        auto *table = panel->findChild<QTableWidget *>(QStringLiteral("browserConnectionsTable"));
        QVERIFY(table && table->rowCount() >= 1);
        auto *path = panel->findChild<QLabel *>(QStringLiteral("gateway_path"));
        QVERIFY(path && path->text() == QString::fromStdString(fwd::PathText(active->path)));
        QVERIFY(window->forwardingObserver() && window->forwardingObserver()->counts().proxied >= 1);
        window.reset();  // detaches the gateway before its runtime goes away
        QVERIFY(!gateway()->Status().has_bindings);
        gateway()->SetBindings(stack_.bindings());
    }

    void gatewayFailureNeverGoesDirect()
    {
        // Unbound: the browser gets the gateway's 503, the origin nothing.
        gateway()->Detach();
        const std::uint64_t before = origin_.Requests();
        QWebEngineView view;
        load(&view, originUrl("/page?unbound"), 40000);
        QVERIFY(pageText(&view).contains(QStringLiteral("no_confirmed_configuration")));
        QCOMPARE(origin_.Requests(), before);
        // Gateway down mid-session: the load fails instead of going direct.
        gateway()->Stop();
        QWebEngineView down;
        QVERIFY(!load(&down, originUrl("/page?gateway-down"), 40000));
        QCOMPARE(origin_.Requests(), before);
    }
};

int main(int argc, char **argv)
{
    // Exactly as aafr-browser --forwarding=controlled --forward-loopback: the
    // gateway binds and Chromium learns its address before Qt starts.
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
    BrowserForwardingTest test;
    const int result = QTest::qExec(&test, argc, argv);
    return result;
}

#include "browser_forwarding_test.moc"
