// V0.3 observability tests: browser event capture, TShark process handling
// (missing tool, failed start, off by default), and the Developer Tools panel.
// V0.4 adds the NetworkState, capability and eligibility sections of the panel.

#include <QAction>
#include <QApplication>
#include <QScrollArea>
#include <QScrollBar>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QTabWidget>
#include <QTableWidget>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QTest>
#include <QToolButton>
#include <QWebEngineView>

#include <algorithm>
#include <memory>

#include "browser_event_collector.h"
#include "browser_window.h"
#include "devtools_panel.h"
#include "devtools_overview_status.h"
#include "tab_manager.h"
#include "network/network_state/network_state_service.h"
#include "network/telemetry/telemetry_pipeline.h"
#include "routing/controller/forwarding_service.h"
#include "network/tshark/tshark_fields.h"
#include "network/tshark/tshark_monitor.h"
#include "runtime/model/ai_recommendation_provider.h"
#include "tests/support/model_test_support.h"
#include "runtime/adaptive/aafr_runtime.h"
#include "runtime/observability/runtime_contracts.h"
#include "research/research_host.h"

// Same as main.cpp: compose windows on the GPU from the start.
static const bool aafrWidgetsRhi = [] {
    if (!qEnvironmentVariableIsSet("QT_WIDGETS_RHI")) qputenv("QT_WIDGETS_RHI", "1");
    return true;
}();

using aafr::telemetry::Availability;
using aafr::telemetry::EventType;
using aafr::telemetry::TelemetryEvent;
using aafr::telemetry::TelemetryPipeline;
namespace obs = aafr::observability;

namespace {
std::vector<TelemetryEvent> eventsOf(const TelemetryPipeline &pipeline, EventType type)
{
    std::vector<TelemetryEvent> result;
    for (const auto &event : pipeline.EventsAfter(0, 100000))
        if (event.type == type) result.push_back(event);
    return result;
}

int countOf(const TelemetryPipeline &pipeline, EventType type)
{
    return static_cast<int>(eventsOf(pipeline, type).size());
}

QString attribute(const TelemetryEvent &event, const char *key)
{
    const std::string *value = event.FindAttribute(key);
    return value ? QString::fromStdString(*value) : QString();
}

// TShark "-T fields" sample, inline so no fixture files are needed:
// 9 packets (5 TCP, 4 UDP, 2 QUIC), 5928 bytes, 2 retransmissions,
// ACK RTT 12.5/20/8 ms, handshake RTT 30 and 50 ms on two connections,
// one 5.1 ms DNS response, and 2 malformed lines.
const char *const kCaptureSample[] = {
    "1731225600.000100\t74\teth:ethertype:ip:tcp\t\t\t\t\t\t\t",
    "1731225600.010200\t66\teth:ethertype:ip:tcp\t0.012500\t\t\t\t\t3\t0.030000",
    "1731225600.020300\t1514\teth:ethertype:ip:tcp\t0.020000\t1\t\t\t\t3\t0.030000",
    "1731225600.030400\t1514\teth:ethertype:ip:tcp\t\t1\t\t\t\t\t",
    "1731225600.040500\t80\teth:ethertype:ip:udp:dns\t\t\tFalse\t\t\t\t",
    "1731225600.045600\t120\teth:ethertype:ip:udp:dns\t\t\tTrue\t0.005100\t\t\t",
    "1731225600.050700\t1250\teth:ethertype:ipv6:udp:quic\t\t\t\t\t0x00000001\t\t",
    "1731225600.060800\t1250\teth:ethertype:ipv6:udp:quic\t\t\t\t\t\t\t",
    "Capturing on 'Wi-Fi'",
    "1731225600.070900\t60\teth:ethertype:ip:tcp\t0.008000\t\t\t\t\t4\t0.050000",
    "abc\t60\teth:ethertype:ip:tcp\t\t\t\t\t\t\t",
};

// Running tshark.exe and dumpcap.exe processes on this machine (Windows only).
int captureProcessCount()
{
#ifdef Q_OS_WIN
    QProcess list;
    list.start(QStringLiteral("tasklist"), {QStringLiteral("/fo"), QStringLiteral("csv"), QStringLiteral("/nh")});
    if (!list.waitForFinished(10000)) return -1;
    const QString output = QString::fromLocal8Bit(list.readAllStandardOutput()).toLower();
    return int(output.count(QStringLiteral("\"tshark.exe\"")) + output.count(QStringLiteral("\"dumpcap.exe\"")));
#else
    return 0;
#endif
}

// Publishes the sample through the same parser and aggregator the monitor uses.
void publishCaptureSample(TelemetryPipeline &pipeline)
{
    aafr::tshark::IntervalAggregator aggregator;
    for (const char *line : kCaptureSample) aggregator.AddLine(line);
    for (auto &event : aafr::tshark::ToTelemetryEvents(aggregator.Flush(), 1000.0))
        pipeline.Publish(event);
}

class FakeRuntimeProvider final : public obs::IRuntimeObservabilityProvider
{
public:
    obs::RuntimeSnapshot snapshot;
    std::vector<obs::DecisionRecord> decisions;
    obs::RuntimeSnapshot Snapshot() const override { return snapshot; }
    std::vector<obs::DecisionRecord> RecentDecisions(std::size_t max) const override
    {
        return {decisions.end() - std::min(max, decisions.size()), decisions.end()};
    }
};

class FakeResearchProvider final : public aafr::research::IResearchObservabilityProvider
{
public:
    aafr::research::ResearchObservabilitySnapshot snapshot;
    aafr::research::ResearchObservabilitySnapshot ResearchSnapshot(std::size_t) const override { return snapshot; }
};

QStringList columnTexts(QTableWidget *table, int column)
{
    QStringList texts;
    for (int row = 0; row < table->rowCount(); ++row)
        texts << (table->item(row, column) ? table->item(row, column)->text() : QString());
    return texts;
}

// Row whose first column shows `label`, or -1.
int rowOfText(QTableWidget *table, const QString &label)
{
    for (int row = 0; row < table->rowCount(); ++row)
        if (table->item(row, 0)->text() == label) return row;
    return -1;
}

int rowOfMetric(QTableWidget *table, const QString &metric)
{
    for (int row = 0; row < table->rowCount(); ++row)
        if (table->item(row, 0)->data(Qt::UserRole).toString() == metric) return row;
    return -1;
}
}

class BrowserObservabilityTest : public QObject
{
    Q_OBJECT
private:
    QTemporaryDir pages_;
    QUrl first_;
    QUrl second_;

private slots:
    void initTestCase()
    {
        // Keep layout preferences out of the developer's real settings.
        QStandardPaths::setTestModeEnabled(true);
        QVERIFY(pages_.isValid());
        auto writePage = [this](const QString &name, const QByteArray &html) {
            QFile file(pages_.filePath(name));
            if (!file.open(QIODevice::WriteOnly) || file.write(html) != html.size()) return QUrl();
            return QUrl::fromLocalFile(file.fileName());
        };
        first_ = writePage("first.html", "<title>First page</title><p>First</p>");
        second_ = writePage("second.html", "<title>Second page</title><p>Second</p>");
        QVERIFY(!first_.isEmpty() && !second_.isEmpty());
    }

    void init()
    {
        QFile::remove(BrowserWindow::layoutSettingsPath());
        // Research records persist across windows (Phase 5B.3): every test
        // starts without records of earlier tests.
        QFile::remove(BrowserWindow::researchRecordsPath());
    }

    // ---- Browser event collection -------------------------------------

    void navigationLifecycleAndTiming()
    {
        TelemetryPipeline pipeline;
        TabManager tabs;
        BrowserEventCollector collector(&tabs, pipeline);
        auto *view = tabs.addTab(first_);
        QTRY_COMPARE_WITH_TIMEOUT(view->title(), QString("First page"), 15000);
        QTRY_COMPARE_WITH_TIMEOUT(countOf(pipeline, EventType::NavigationSucceeded), 1, 15000);

        const auto created = eventsOf(pipeline, EventType::TabCreated);
        QCOMPARE(created.size(), size_t(1));
        const auto tabId = created.front().tab_id;
        QVERIFY(tabId.has_value());
        QCOMPARE(collector.tabId(view), tabId);

        const auto started = eventsOf(pipeline, EventType::NavigationStarted);
        const auto succeeded = eventsOf(pipeline, EventType::NavigationSucceeded);
        QCOMPARE(started.size(), size_t(1));
        QVERIFY(started.front().navigation_id.has_value());
        QCOMPARE(succeeded.front().navigation_id, started.front().navigation_id);
        QCOMPARE(succeeded.front().tab_id, tabId);
        QCOMPARE(succeeded.front().metric, std::string("navigation.duration"));
        QCOMPARE(succeeded.front().unit, std::string("ms"));
        QVERIFY(succeeded.front().value.has_value());
        QVERIFY(*succeeded.front().value >= 0.0);
        // The duration fits inside the monotonic span between the two events.
        const double spanMs = (succeeded.front().monotonic_ns - started.front().monotonic_ns) / 1e6;
        QVERIFY(*succeeded.front().value <= spanMs + 1.0);
        QCOMPARE(succeeded.front().availability, Availability::Available);
        // Local paths are reduced to the file name.
        QCOMPARE(attribute(started.front(), "url"), QString("file:.../first.html"));

        // Monotonic timestamps and ids never go backwards.
        const auto all = pipeline.EventsAfter(0, 1000);
        for (size_t i = 1; i < all.size(); ++i) {
            QVERIFY(all[i].event_id > all[i - 1].event_id);
            QVERIFY(all[i].monotonic_ns >= all[i - 1].monotonic_ns);
        }
        QVERIFY(countOf(pipeline, EventType::LoadProgress) >= 1);

        const auto snapshot = collector.tab(view);
        QVERIFY(snapshot);
        QCOMPARE(snapshot->status, BrowserEventCollector::NavigationStatus::Succeeded);
        QCOMPARE(snapshot->lastNavigationId, started.front().navigation_id);
        QVERIFY(snapshot->lastNavigationMs.has_value());

        // A second navigation gets a new id.
        view->load(second_);
        QTRY_COMPARE_WITH_TIMEOUT(countOf(pipeline, EventType::NavigationSucceeded), 2, 15000);
        const auto again = eventsOf(pipeline, EventType::NavigationSucceeded);
        QVERIFY(*again.back().navigation_id > *again.front().navigation_id);
    }

    void multipleTabsActivationAndClosure()
    {
        TelemetryPipeline pipeline;
        TabManager tabs;
        BrowserEventCollector collector(&tabs, pipeline);
        auto *first = tabs.addTab(first_);
        auto *second = tabs.addTab(second_, false);
        auto *third = tabs.addTab(QUrl(), false);
        QTRY_COMPARE_WITH_TIMEOUT(countOf(pipeline, EventType::NavigationSucceeded), 2, 15000);
        QCOMPARE(collector.trackedTabCount(), 3);
        const auto firstId = *collector.tabId(first);
        const auto secondId = *collector.tabId(second);
        const auto thirdId = *collector.tabId(third);
        QVERIFY(firstId != secondId && secondId != thirdId && firstId != thirdId);

        // Background navigation is attributed to its own tab.
        bool backgroundSeen = false;
        for (const auto &event : eventsOf(pipeline, EventType::NavigationSucceeded))
            backgroundSeen = backgroundSeen || event.tab_id == secondId;
        QVERIFY(backgroundSeen);

        tabs.setCurrentWidget(second);
        const auto activated = eventsOf(pipeline, EventType::TabActivated);
        QVERIFY(!activated.empty());
        QCOMPARE(activated.back().tab_id, std::optional<std::uint64_t>(secondId));

        tabs.closeTab(tabs.indexOf(second));
        auto closed = eventsOf(pipeline, EventType::TabClosed);
        QCOMPARE(closed.size(), size_t(1));
        QCOMPARE(closed.front().tab_id, std::optional<std::uint64_t>(secondId));
        QCOMPARE(collector.trackedTabCount(), 2);

        // Signals from a closed view that is still awaiting deletion are ignored.
        const int before = static_cast<int>(pipeline.Stats().published);
        second->loadProgress(50);
        second->urlChanged(first_);
        QCOMPARE(static_cast<int>(pipeline.Stats().published), before);

        // External destruction is reported once.
        delete third;
        closed = eventsOf(pipeline, EventType::TabClosed);
        QCOMPARE(closed.size(), size_t(2));
        QCOMPARE(closed.back().tab_id, std::optional<std::uint64_t>(thirdId));
        QCOMPARE(collector.trackedTabCount(), 1);
    }

    void failedAndStoppedNavigation()
    {
        TelemetryPipeline pipeline;
        TabManager tabs;
        BrowserEventCollector collector(&tabs, pipeline);
        auto *view = tabs.addTab(QUrl::fromLocalFile(pages_.filePath("missing.html")));
        QTRY_COMPARE_WITH_TIMEOUT(countOf(pipeline, EventType::NavigationFailed), 1, 15000);
        const auto failed = eventsOf(pipeline, EventType::NavigationFailed).front();
        QVERIFY(failed.value.has_value());
        QVERIFY(!attribute(failed, "error_domain").isEmpty());
        QVERIFY(!attribute(failed, "error_code").isEmpty());
        QCOMPARE(collector.tab(view)->status, BrowserEventCollector::NavigationStatus::Failed);

        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        view->load(QUrl(QString("http://127.0.0.1:%1/wait?token=secret").arg(server.serverPort())));
        QTRY_COMPARE_WITH_TIMEOUT(countOf(pipeline, EventType::NavigationStarted), 2, 15000);
        const auto started = eventsOf(pipeline, EventType::NavigationStarted).back();
        // Query parameters are never recorded.
        QCOMPARE(attribute(started, "url"), QString("http://127.0.0.1:%1/wait").arg(server.serverPort()));
        view->stop();
        QTRY_COMPARE_WITH_TIMEOUT(countOf(pipeline, EventType::NavigationStopped), 1, 15000);
        QCOMPARE(eventsOf(pipeline, EventType::NavigationStopped).front().navigation_id, started.navigation_id);
        for (const auto &event : pipeline.EventsAfter(0, 1000))
            QVERIFY(!QString::fromStdString(aafr::telemetry::ToJson(event)).contains("secret"));
    }

    void rendererTermination()
    {
        TelemetryPipeline pipeline;
        TabManager tabs;
        BrowserEventCollector collector(&tabs, pipeline);
        auto *view = tabs.addTab(first_);
        QTRY_COMPARE_WITH_TIMEOUT(countOf(pipeline, EventType::NavigationSucceeded), 1, 15000);
        // Synthesized: crashing a real renderer is not deterministic in tests.
        view->renderProcessTerminated(QWebEnginePage::CrashedTerminationStatus, 3);
        const auto terminated = eventsOf(pipeline, EventType::RendererTerminated);
        QCOMPARE(terminated.size(), size_t(1));
        QCOMPARE(attribute(terminated.front(), "exit_code"), QString("3"));
        QCOMPARE(collector.tab(view)->status, BrowserEventCollector::NavigationStatus::RendererTerminated);
    }

    void urlRedaction_data()
    {
        QTest::addColumn<QUrl>("url");
        QTest::addColumn<QString>("expected");
        QTest::newRow("query-fragment-credentials")
            << QUrl("https://user:pass@example.com:8443/a/b?token=1&x=2#frag")
            << "https://example.com:8443/a/b";
        QTest::newRow("plain-http") << QUrl("http://example.com/") << "http://example.com/";
        QTest::newRow("file") << QUrl::fromLocalFile("C:/Users/someone/private/page.html") << "file:.../page.html";
        QTest::newRow("data") << QUrl("data:text/html,<p>secret</p>") << "data:";
        QTest::newRow("blob") << QUrl("blob:https://example.com/uuid") << "blob:";
        QTest::newRow("about") << QUrl("about:blank") << "about:blank";
        QTest::newRow("empty") << QUrl() << QString();
    }

    void urlRedaction()
    {
        QFETCH(QUrl, url);
        QFETCH(QString, expected);
        QCOMPARE(BrowserEventCollector::redactUrl(url), expected);
    }

    void collectorAttachesToExistingTabs()
    {
        TelemetryPipeline pipeline;
        TabManager tabs;
        auto *existing = tabs.addTab();
        BrowserEventCollector collector(&tabs, pipeline);
        QVERIFY(collector.tabId(existing).has_value());
        QCOMPARE(countOf(pipeline, EventType::TabCreated), 1);
    }

    // ---- TShark adapter -----------------------------------------------

    void tsharkMissingKeepsBrowserUsable()
    {
        TelemetryPipeline pipeline;
        TSharkMonitor::Config config;
        config.program = QStringLiteral("C:/definitely/not/installed/tshark.exe");
        TSharkMonitor monitor(pipeline, config);
        QVERIFY(!monitor.isAvailable());
        QCOMPARE(monitor.state(), NetworkMonitor::State::Unavailable);
        QVERIFY(!monitor.start());
        QCOMPARE(monitor.state(), NetworkMonitor::State::Unavailable);
        const auto errors = eventsOf(pipeline, EventType::CaptureError);
        QCOMPARE(errors.size(), size_t(1));
        QCOMPARE(errors.front().availability, Availability::Unavailable);
        monitor.stop();  // No-op without a process.
        monitor.shutdown();
        QCOMPARE(countOf(pipeline, EventType::NetworkMetric), 0);
    }

    void tsharkCaptureIsOffByDefault()
    {
        BrowserWindow window;
        auto *monitor = window.networkMonitor();
        QVERIFY(monitor);
        QVERIFY(monitor->state() == NetworkMonitor::State::Disabled
                || monitor->state() == NetworkMonitor::State::Unavailable);
        QCOMPARE(countOf(window.telemetry(), EventType::CaptureStarted), 0);
        QVERIFY(!window.telemetry().Stats().jsonl_enabled);
    }

    void tsharkFailedToStart()
    {
        // The executable is found at construction, then disappears before
        // start(), so process creation fails. Nothing is ever executed: a
        // non-program ".exe" would make Windows show a modal error dialog.
        QTemporaryDir directory;
        const QString path = directory.filePath("tshark.exe");
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.close();
        TelemetryPipeline pipeline;
        TSharkMonitor::Config config;
        config.program = path;
        TSharkMonitor monitor(pipeline, config);
        QVERIFY(monitor.isAvailable());
        QVERIFY(QFile::remove(path));
        QVERIFY(monitor.start());
        QTRY_COMPARE_WITH_TIMEOUT(monitor.state(), NetworkMonitor::State::Error, 10000);
        QVERIFY(monitor.statusText().contains("could not be started"));
        QCOMPARE(countOf(pipeline, EventType::CaptureError), 1);
        QCOMPARE(eventsOf(pipeline, EventType::CaptureError).front().availability, Availability::Error);
        // Retrying after a failure is allowed.
        QVERIFY(monitor.start());
        QTRY_COMPARE_WITH_TIMEOUT(countOf(pipeline, EventType::CaptureError), 2, 10000);
    }

    void tsharkInterfacesListedAndUnknownNameRefused()
    {
        // Uses an installed TShark to list interfaces only; never captures.
        TelemetryPipeline pipeline;
        TSharkMonitor monitor(pipeline, TSharkMonitor::Config{});
        if (!monitor.isAvailable()) QSKIP("TShark is not installed.");
        QSignalSpy listed(&monitor, &TSharkMonitor::interfacesChanged);
        monitor.refreshInterfaces();
        QTRY_VERIFY_WITH_TIMEOUT(monitor.interfacesLoaded(), 30000);
        QCOMPARE(listed.count(), 1);
        if (monitor.interfaces().empty())
            QSKIP("tshark -D listed no interfaces (capture driver missing?).");

        // An unknown name fails before TShark starts, listing the real names.
        monitor.setInterfaceName(QStringLiteral("no-such-adapter"));
        QVERIFY(!monitor.start());
        QCOMPARE(monitor.state(), NetworkMonitor::State::Error);
        QVERIFY(monitor.statusText().contains(QStringLiteral("No capture interface named \"no-such-adapter\"")));
        QVERIFY(monitor.statusText().contains(QString::fromStdString(monitor.interfaces().front().name)));
        const auto errors = eventsOf(pipeline, EventType::CaptureError);
        QCOMPARE(errors.size(), size_t(1));
        QCOMPARE(countOf(pipeline, EventType::CaptureStarted), 0);
        // No measurements are invented for a capture that never ran.
        QCOMPARE(countOf(pipeline, EventType::NetworkMetric), 0);
    }

    void tsharkLiveCaptureOptIn()
    {
        // Real capture, opt-in only: set AAFR_LIVE_CAPTURE=1 (and optionally
        // AAFR_TSHARK_INTERFACE). Records AAFR's metadata fields only, keeps
        // nothing on disk, and needs some network traffic while it runs.
        if (qEnvironmentVariableIntValue("AAFR_LIVE_CAPTURE") != 1)
            QSKIP("Set AAFR_LIVE_CAPTURE=1 to run a real 8-second TShark capture.");
        TelemetryPipeline pipeline;
        TSharkMonitor::Config config;
        config.interfaceName = qEnvironmentVariable("AAFR_TSHARK_INTERFACE", QStringLiteral("wifi"));
        TSharkMonitor monitor(pipeline, config);
        if (!monitor.isAvailable()) QSKIP("TShark is not installed.");
        QVERIFY(monitor.start());
        QTRY_VERIFY_WITH_TIMEOUT(monitor.state() == NetworkMonitor::State::Running
                                     || monitor.state() == NetworkMonitor::State::Error, 30000);
        QVERIFY2(monitor.state() == NetworkMonitor::State::Running, qPrintable(monitor.statusText()));
        qInfo() << "Capturing on" << monitor.captureInterfaceName();
        // Generate a little traffic of our own: a DNS lookup and a TCP connection.
        QTcpSocket socket;
        socket.connectToHost(QStringLiteral("example.com"), 443);
        QTRY_VERIFY_WITH_TIMEOUT(pipeline.LatestMetric("net.capture.packets_total").has_value(), 8000);
        QTest::qWait(3000);
        monitor.stop();
        QTRY_COMPARE_WITH_TIMEOUT(monitor.state(), NetworkMonitor::State::Disabled, 10000);
        QCOMPARE(countOf(pipeline, EventType::CaptureError), 0);
        for (const auto &[name, event] : pipeline.LatestMetrics())
            qInfo().noquote() << QString::fromStdString(name) << "=" << *event.value
                              << QString::fromStdString(event.unit);
        QVERIFY(*pipeline.LatestMetric("net.capture.packets_total")->value > 0);
        // Stop leaves no TShark or dumpcap (TShark's capture child) behind.
        QTRY_COMPARE_WITH_TIMEOUT(captureProcessCount(), 0, 5000);

        // Closing the browser while capturing stops the capture too.
        auto *window = new BrowserWindow;
        window->networkMonitor()->setInterfaceName(config.interfaceName);
        QVERIFY(window->networkMonitor()->start());
        QTRY_COMPARE_WITH_TIMEOUT(window->networkMonitor()->state(), NetworkMonitor::State::Running, 30000);
        QTRY_VERIFY_WITH_TIMEOUT(captureProcessCount() > 0, 5000);
        qInfo() << "Capture processes while running:" << captureProcessCount();
        delete window;
        QTRY_COMPARE_WITH_TIMEOUT(captureProcessCount(), 0, 5000);
    }

    void devToolsInterfaceDropdown()
    {
        BrowserWindow window;
        auto *panel = window.devToolsPanel();
        auto *combo = panel->findChild<QComboBox *>("captureInterface");
        QVERIFY(combo);
        QVERIFY(combo->count() >= 1);
        QVERIFY(combo->itemText(0).startsWith("Automatic"));
        QCOMPARE(combo->itemData(0).toString(), QString());  // Empty = connected adapter.
        auto *monitor = window.networkMonitor();
        if (!monitor->isAvailable()) {
            QVERIFY(!combo->isEnabled());
            return;
        }
        monitor->refreshInterfaces();
        QTRY_VERIFY_WITH_TIMEOUT(monitor->interfacesLoaded(), 30000);
        QCOMPARE(combo->count(), 1 + int(monitor->interfaces().size()));
        for (const auto &entry : monitor->interfaces())
            QVERIFY(combo->findData(QString::fromStdString(entry.name)) > 0);
        if (!monitor->activeInterfaceName().isEmpty())
            QCOMPARE(combo->itemText(0), QString("Automatic (%1)").arg(monitor->activeInterfaceName()));
    }

    // ---- Developer Tools panel ----------------------------------------

    void devToolsHiddenByDefaultAndToggles()
    {
        BrowserWindow window;
        window.show();
        window.activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(&window));
        auto *panel = window.devToolsPanel();
        QVERIFY(panel);
        QVERIFY(!panel->isVisible());
        auto *action = window.findChild<QAction *>("devToolsAction");
        auto *button = window.findChild<QToolButton *>("devToolsButton");
        QVERIFY(action && button);
        QCOMPARE(action->shortcut(), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_M));
        QTest::mouseClick(button, Qt::LeftButton);
        QTRY_VERIFY(panel->isVisible());
        QCOMPARE(window.dockWidgetArea(panel), Qt::BottomDockWidgetArea);
        QTest::keyClick(&window, Qt::Key_M, Qt::ControlModifier | Qt::ShiftModifier);
        QTRY_VERIFY(!panel->isVisible());
        QTest::keyClick(&window, Qt::Key_M, Qt::ControlModifier | Qt::ShiftModifier);
        QTRY_VERIFY(panel->isVisible());
        QVERIFY(action->isChecked());

        QStringList names;
        for (int i = 0; i < panel->tabs()->count(); ++i) names << panel->tabs()->tabText(i);
        QCOMPARE(names, QStringList({"Overview", "Network", "Runtime", "Decisions", "Events"}));
    }

    void devToolsDockingResizingAndContentLayout()
    {
        BrowserWindow window;
        window.resize(1280, 800);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        auto *panel = window.devToolsPanel();
        auto *content = window.findChild<QStackedWidget *>("contentStack");
        panel->show();
        QCoreApplication::processEvents();
        QTRY_VERIFY(panel->isVisible());
        const QRect bottomPanel(panel->mapTo(&window, QPoint()), panel->size());
        const QRect contentRect(content->mapTo(&window, QPoint()), content->size());
        QVERIFY(!contentRect.intersects(bottomPanel));
        QVERIFY(contentRect.bottom() < bottomPanel.top());
        QVERIFY(content->height() > 100);

        window.resizeDocks({panel}, {320}, Qt::Vertical);
        QCoreApplication::processEvents();
        QTRY_VERIFY(qAbs(panel->height() - 320) <= 40);

        auto *side = panel->findChild<QToolButton *>("devToolsDockSide");
        QVERIFY(side);
        QCOMPARE(side->text(), QString("Dock right"));
        QTest::mouseClick(side, Qt::LeftButton);
        QTRY_COMPARE(window.dockWidgetArea(panel), Qt::RightDockWidgetArea);
        QCOMPARE(side->text(), QString("Dock bottom"));
        QCoreApplication::processEvents();
        const QRect rightPanel(panel->mapTo(&window, QPoint()), panel->size());
        const QRect contentRight(content->mapTo(&window, QPoint()), content->size());
        QVERIFY(contentRight.right() < rightPanel.left());
        window.resizeDocks({panel}, {420}, Qt::Horizontal);
        QCoreApplication::processEvents();
        QTRY_VERIFY(qAbs(panel->width() - 420) <= 40);

        // Detachable and re-dockable.
        QVERIFY(panel->features().testFlag(QDockWidget::DockWidgetFloatable));
        panel->setFloating(true);
        QTRY_VERIFY(panel->isFloating());
        QCOMPARE(side->text(), QString("Dock right"));
        QTest::mouseClick(side, Qt::LeftButton);
        QTRY_VERIFY(!panel->isFloating());
        QCOMPARE(window.dockWidgetArea(panel), Qt::RightDockWidgetArea);
        QTest::mouseClick(side, Qt::LeftButton);
        QTRY_COMPARE(window.dockWidgetArea(panel), Qt::BottomDockWidgetArea);
        QVERIFY(!(panel->allowedAreas() & (Qt::LeftDockWidgetArea | Qt::TopDockWidgetArea)));

        // Title-bar buttons: large enough to hit, and clear of the right edge.
        auto *closeButton = panel->findChild<QToolButton *>("devToolsClose");
        auto *floatButton = panel->findChild<QToolButton *>("devToolsFloat");
        QVERIFY(closeButton && floatButton);
        QVERIFY(closeButton->width() >= 28 && closeButton->height() >= 28);
        const int closeRight = closeButton->mapTo(panel, QPoint(closeButton->width(), 0)).x();
        QVERIFY(panel->width() - closeRight >= 6);
        QVERIFY(floatButton->geometry().right() < closeButton->geometry().left());
        QTest::mouseClick(floatButton, Qt::LeftButton);
        QTRY_VERIFY(panel->isFloating());
        QTest::mouseClick(floatButton, Qt::LeftButton);
        QTRY_VERIFY(!panel->isFloating());
        QTest::mouseClick(closeButton, Qt::LeftButton);
        QTRY_VERIFY(!panel->isVisible());
    }

    void devToolsLayoutPersistsButStartsHidden()
    {
        {
            BrowserWindow window;
            window.show();
            QVERIFY(QTest::qWaitForWindowExposed(&window));
            window.devToolsPanel()->show();
            window.findChild<QToolButton *>("devToolsDockSide")->click();
            QTRY_COMPARE(window.dockWidgetArea(window.devToolsPanel()), Qt::RightDockWidgetArea);
        }
        QVERIFY(QFile::exists(BrowserWindow::layoutSettingsPath()));
        BrowserWindow restored;
        restored.show();
        QVERIFY(QTest::qWaitForWindowExposed(&restored));
        QVERIFY(!restored.devToolsPanel()->isVisible());
        QCOMPARE(restored.dockWidgetArea(restored.devToolsPanel()), Qt::RightDockWidgetArea);
        restored.devToolsPanel()->show();
        QTRY_VERIFY(restored.devToolsPanel()->isVisible());
        QCOMPARE(restored.dockWidgetArea(restored.devToolsPanel()), Qt::RightDockWidgetArea);
    }

    void devToolsShowsBrowserEvents()
    {
        BrowserWindow window(nullptr, first_);
        window.show();
        QTRY_COMPARE_WITH_TIMEOUT(window.tabManager()->currentView()->title(), QString("First page"), 15000);
        QTRY_VERIFY_WITH_TIMEOUT(countOf(window.telemetry(), EventType::NavigationSucceeded) == 1, 15000);
        auto *panel = window.devToolsPanel();
        panel->show();
        panel->refreshNow();
        auto *events = panel->findChild<QTableWidget *>("eventsTable");
        QVERIFY(events);
        const QStringList types = columnTexts(events, 2);
        QVERIFY(types.contains("tab.created"));
        QVERIFY(types.contains("navigation.started"));
        QVERIFY(types.contains("navigation.succeeded"));
        QVERIFY(columnTexts(events, 3).contains("qt_webengine"));
        QVERIFY(!columnTexts(events, 0).first().isEmpty());

        auto *pageLoad = panel->findChild<QLabel *>("overview_pageLoad");
        QVERIFY(pageLoad->text().contains(" ms"));
        QVERIFY(panel->findChild<QLabel *>("overview_navigationStatus")->text().startsWith("Loaded"));
        QVERIFY(panel->findChild<QLabel *>("overview_activeTab")->text().contains("first.html"));
        QVERIFY(panel->findChild<QLabel *>("overview_session")->text().contains(
            QString::fromStdString(window.telemetry().SessionId())));

        // Source filter rebuilds the view from retained history.
        auto *filter = panel->findChild<QComboBox *>("eventFilter");
        filter->setCurrentIndex(2);
        QCOMPARE(events->rowCount(), 0);
        filter->setCurrentIndex(1);
        QVERIFY(events->rowCount() > 0);
        QVERIFY(!columnTexts(events, 3).contains("tshark"));

        // New events appear on the next refresh tick without manual refresh.
        const int rows = events->rowCount();
        window.tabManager()->currentView()->load(second_);
        QTRY_VERIFY_WITH_TIMEOUT(events->rowCount() > rows, 15000);
    }

    void devToolsEventTableIsBounded()
    {
        BrowserWindow window;
        auto *panel = window.devToolsPanel();
        for (int i = 0; i < 1200; ++i) {
            TelemetryEvent event;
            event.type = EventType::PipelineNotice;
            event.attributes.push_back({"n", std::to_string(i)});
            window.telemetry().Publish(event);
        }
        panel->refreshNow();
        auto *events = panel->findChild<QTableWidget *>("eventsTable");
        QCOMPARE(events->rowCount(), DevToolsPanel::kMaxEventRows);
        QVERIFY(events->item(events->rowCount() - 1, 7)->text().contains("n=1199"));
    }

    void devToolsMissingMetrics()
    {
        BrowserWindow window;
        auto *panel = window.devToolsPanel();
        panel->refreshNow();
        auto *table = panel->findChild<QTableWidget *>("networkTable");
        QVERIFY(table);
        QCOMPARE(table->rowCount(), static_cast<int>(aafr::tshark::NetworkMetricNames().size()));
        const QString expected = window.networkMonitor()->isAvailable() ? "Not observed" : "Unavailable";
        for (const QString &value : columnTexts(table, 1)) QCOMPARE(value, expected);
        for (const QString &freshness : columnTexts(table, 5)) QCOMPARE(freshness, QString("—"));
        QVERIFY(panel->findChild<QLabel *>("overview_metrics")->text().startsWith("No network metrics"));
        QVERIFY(panel->findChild<QLabel *>("protocolInfo")->text().contains("none observed"));
        auto *toggle = panel->findChild<QPushButton *>("captureToggle");
        QCOMPARE(toggle->isEnabled(), window.networkMonitor()->isAvailable());
    }

    void devToolsShowsNetworkMeasurements()
    {
        BrowserWindow window;
        auto *panel = window.devToolsPanel();
        // Deterministic sample data through the same parser the monitor uses.
        publishCaptureSample(window.telemetry());
        panel->refreshNow();
        auto *table = panel->findChild<QTableWidget *>("networkTable");
        const int rtt = rowOfMetric(table, "net.tcp.ack_rtt.latest");
        QVERIFY(rtt >= 0);
        QCOMPARE(table->item(rtt, 1)->text(), QLocale().toString(8.0, 'f', 2));
        QCOMPARE(table->item(rtt, 2)->text(), QString("ms"));
        QCOMPARE(table->item(rtt, 3)->text(), QString("tshark"));
        QCOMPARE(table->item(rtt, 4)->text(), QString("capture_interface"));
        QVERIFY(table->item(rtt, 5)->text().endsWith("s ago"));
        QCOMPARE(table->item(rowOfMetric(table, "net.tcp.retransmissions"), 1)->text(), QString("2"));
        QCOMPARE(table->item(rowOfMetric(table, "net.packets"), 1)->text(), QString("9"));
        const int jitter = rowOfMetric(table, "net.tcp.ack_rtt.jitter");
        QVERIFY(jitter >= 0);
        QCOMPARE(table->item(jitter, 1)->text(), QLocale().toString(9.75, 'f', 2));
        QCOMPARE(table->item(jitter, 2)->text(), QString("ms"));
        const int throughput = rowOfMetric(table, "net.throughput");
        QVERIFY(throughput >= 0);
        QCOMPARE(table->item(throughput, 1)->text(), QLocale().toString(0.047424, 'f', 3));
        QCOMPARE(table->item(throughput, 2)->text(), QString("Mbps"));
        QVERIFY(panel->findChild<QLabel *>("protocolInfo")->text().contains("0x00000001"));
        const int latency = rowOfMetric(table, "net.tcp.handshake_rtt.mean");
        QVERIFY(latency >= 0);
        QVERIFY(table->item(latency, 0)->text().startsWith("Latency"));
        QCOMPARE(table->item(latency, 1)->text(), QLocale().toString(40.0, 'f', 1));
        QCOMPARE(table->item(latency, 2)->text(), QString("ms"));
        QVERIFY(panel->findChild<QLabel *>("overview_metrics")->text().startsWith("Latency"));
        QVERIFY(panel->findChild<QLabel *>("overview_metrics")->text().contains("ACK delay mean"));
        QVERIFY(panel->findChild<QLabel *>("overview_metrics")->text().contains("ACK delay jitter"));
        QVERIFY(panel->findChild<QLabel *>("overview_metrics")->text().contains("Throughput"));
        auto *events = panel->findChild<QTableWidget *>("eventsTable");
        QVERIFY(columnTexts(events, 3).contains("tshark"));
    }

    void runtimeAndDecisionsUnavailable()
    {
        BrowserWindow window;
        auto *panel = window.devToolsPanel();
        panel->refreshNow();
        QCOMPARE(panel->findChild<QLabel *>("runtimeBanner")->text(), DevToolsPanel::notIntegratedText());
        for (const char *name : {"runtime_algorithm", "runtime_activatedAt", "runtime_reason",
                                 "runtime_state", "runtime_lastEvaluation"})
            QCOMPARE(panel->findChild<QLabel *>(name)->text(), DevToolsPanel::notIntegratedText());
        QCOMPARE(panel->findChild<QLabel *>("decisionsBanner")->text(), DevToolsPanel::notIntegratedText());
        QCOMPARE(panel->findChild<QTableWidget *>("decisionsTable")->rowCount(), 0);
        QVERIFY(DevToolsPanel::notIntegratedText().contains("runtime not integrated"));
    }

    void runtimeContractDisplay()
    {
        // Exercises the read-only contracts with a test provider; the
        // browser itself ships no provider and shows no runtime data.
        FakeRuntimeProvider provider;
        const auto now = std::chrono::system_clock::now();
        provider.snapshot.state = obs::RuntimeState::Running;
        provider.snapshot.activation_reason = "test-reason";
        provider.snapshot.last_evaluation_at = now;
        obs::AlgorithmStatus status;
        status.algorithm_id = "test-algorithm";
        status.activated_at = now;
        status.execution_state = obs::ExecutionState::Active;
        provider.snapshot.active_algorithm = status;
        obs::DecisionRecord approved;
        approved.decision_id = "d-1";
        approved.timestamp = now;
        approved.previous_algorithm = "a";
        approved.candidate_algorithm = "b";
        approved.decision_type = obs::DecisionType::SwitchApproved;
        approved.execution = obs::ExecutionConfirmation::Pending;
        obs::DecisionRecord committed = approved;
        committed.decision_id = "d-2";
        committed.execution = obs::ExecutionConfirmation::Committed;
        obs::DecisionRecord rejected = approved;
        rejected.decision_id = "d-3";
        rejected.decision_type = obs::DecisionType::SwitchRejected;
        rejected.execution = obs::ExecutionConfirmation::NotApplicable;
        rejected.stability_gates = {{"min_dwell", false, ""}};
        rejected.reason_code = "MINIMUM_DWELL_NOT_MET";
        rejected.explanation = "internal diagnostic rt-session-act3";
        provider.decisions = {approved, committed, rejected};

        BrowserWindow window;
        auto *panel = window.devToolsPanel();
        panel->setRuntimeProvider(&provider);
        QVERIFY(panel->findChild<QLabel *>("runtime_algorithm")->text().startsWith("test-algorithm (ACTIVE)"));
        QCOMPARE(panel->findChild<QLabel *>("runtime_state")->text(), QString("RUNNING"));
        QCOMPARE(panel->findChild<QLabel *>("runtime_reason")->text(), QString("test-reason"));
        auto *table = panel->findChild<QTableWidget *>("decisionsTable");
        QCOMPARE(table->rowCount(), 3);
        // Newest first.
        QCOMPARE(columnTexts(table, 1), QStringList({"d-3", "d-2", "d-1"}));
        const QStringList outcomes = columnTexts(table, 6);
        QCOMPARE(outcomes.at(1), QString("Switch committed"));
        QCOMPARE(outcomes.at(2), QString("Switch approved, awaiting commit"));
        QCOMPARE(outcomes.at(0), QString("Switch rejected"));
        QCOMPARE(columnTexts(table, 5).at(0), QString("min_dwell"));
        panel->refreshNow();
        const auto summary = panel->findChild<QLabel *>("summary_decisions_production")->text();
        QVERIFY(summary.contains("less than the minimum dwell time"));
        QVERIFY(!summary.contains("MINIMUM_DWELL_NOT_MET"));
        QVERIFY(!summary.contains("rt-session-act3"));
        QCOMPARE(table->item(0, 7)->text(), QString("internal diagnostic rt-session-act3"));

        panel->setRuntimeProvider(nullptr);
        QCOMPARE(table->rowCount(), 0);
        QCOMPARE(panel->findChild<QLabel *>("runtime_state")->text(), DevToolsPanel::notIntegratedText());
    }

    // ---- V0.4: NetworkState, capabilities and eligibility ----------------

    void devToolsWithoutStatePipeline()
    {
        TelemetryPipeline pipeline;
        DevToolsPanel panel(pipeline, nullptr, nullptr);
        panel.refreshNow();
        for (const char *name : {"overview_networkState", "overview_stateUpdated",
                                 "overview_featureCompleteness", "overview_dataQuality",
                                 "overview_eligibleAlgorithms", "stateSummary"})
            QVERIFY(panel.findChild<QLabel *>(name)->text().contains("not attached"));
        QCOMPARE(panel.findChild<QTableWidget *>("stateTable")->rowCount(), 0);
        QCOMPARE(panel.findChild<QTableWidget *>("eligibilityTable")->rowCount(), 0);
    }

    void devToolsShowsNetworkStateWithoutCapture()
    {
        BrowserWindow window;
        auto *panel = window.devToolsPanel();
        auto *service = window.networkStateService();
        QVERIFY(service);
        QVERIFY(service->Latest());  // Available as soon as the window exists.
        QVERIFY(service->Stats().running);
        service->ProcessOnce();
        panel->refreshNow();

        // Reduced observability without packet capture is shown as such.
        const QString stateText = panel->findChild<QLabel *>("overview_networkState")->text();
        QVERIFY2(stateText.contains("never_started"), qPrintable(stateText));
        QVERIFY(panel->findChild<QLabel *>("overview_stateUpdated")->text().contains("no measurement"));
        QVERIFY(panel->findChild<QLabel *>("overview_featureCompleteness")->text().contains("features usable"));
        QVERIFY(panel->findChild<QLabel *>("overview_dataQuality")->text().startsWith("valid "));
        const QString eligible = panel->findChild<QLabel *>("overview_eligibleAlgorithms")->text();
        QVERIFY2(eligible.startsWith("0 of 10 eligible"), qPrintable(eligible));
        QVERIFY(eligible.contains("10 not implemented"));
        QVERIFY(eligible.contains("nothing is selected"));

        // Missing network features are shown as missing, never as zero.
        auto *stateTable = panel->findChild<QTableWidget *>("stateTable");
        QCOMPARE(stateTable->rowCount(), static_cast<int>(aafr::netstate::FeatureIds().size()));
        const int rtt = rowOfText(stateTable, "rtt.handshake.mean_ms");
        QVERIFY(rtt >= 0);
        QCOMPARE(stateTable->item(rtt, 1)->text(), QString("—"));
        QCOMPARE(stateTable->item(rtt, 2)->text(), QString("ms"));
        QCOMPARE(stateTable->item(rtt, 4)->text(), QString("missing"));
        QVERIFY(stateTable->item(rtt, 8)->text().contains("has not been started"));
        QVERIFY(panel->findChild<QLabel *>("stateSummary")->text().contains("not attributed to tabs"));
        QCOMPARE(panel->findChild<QComboBox *>("stateWindow")->count(), 3);

        // Runtime tab: registry and eligibility, clearly not runtime state.
        QVERIFY(panel->findChild<QLabel *>("eligibilityNote")->text().startsWith(
            DevToolsPanel::activeAlgorithmUnavailableText()));
        QCOMPARE(DevToolsPanel::activeAlgorithmUnavailableText(),
                 QString("Runtime not integrated — active algorithm unavailable."));
        auto *eligibility = panel->findChild<QTableWidget *>("eligibilityTable");
        auto *capabilities = panel->findChild<QTableWidget *>("capabilityTable");
        auto *constraints = panel->findChild<QTableWidget *>("constraintsTable");
        QCOMPARE(eligibility->rowCount(), 10);
        QCOMPARE(capabilities->rowCount(), 10);
        for (int row = 0; row < 10; ++row) {
            QCOMPARE(eligibility->item(row, 1)->text(), QString("not_implemented"));
            QVERIFY(eligibility->item(row, 2)->text().contains("IMPLEMENTATION_UNAVAILABLE"));
            QVERIFY(eligibility->item(row, 2)->text().contains("TOPOLOGY_UNAVAILABLE"));
            QVERIFY(eligibility->item(row, 7)->text().startsWith("Not implemented"));
            // The registry advertises no executable algorithm.
            QCOMPARE(capabilities->item(row, 2)->text(), QString("implementation_unavailable"));
        }
        QVERIFY(rowOfText(eligibility, "Dijkstra") >= 0);
        QVERIFY(rowOfText(capabilities, "Whale Optimization Algorithm (WOA)") >= 0);
        const int topology = rowOfText(constraints, "network.topology.available");
        QVERIFY(topology >= 0);
        QCOMPARE(constraints->item(topology, 2)->text(), QString("false"));
        QCOMPARE(constraints->item(topology, 4)->text(), QString("configured"));
        const int deadline = rowOfText(constraints, "performance.computation_deadline_ms");
        QCOMPARE(constraints->item(deadline, 2)->text(), QString("unknown"));
        QCOMPARE(constraints->item(deadline, 5)->text(), QString("unknown"));

        // No fabricated active algorithm and no fabricated decisions.
        QCOMPARE(panel->findChild<QLabel *>("runtime_algorithm")->text(), DevToolsPanel::notIntegratedText());
        QCOMPARE(panel->findChild<QLabel *>("runtimeBanner")->text(), DevToolsPanel::notIntegratedText());
        QCOMPARE(panel->findChild<QLabel *>("decisionsBanner")->text(), DevToolsPanel::notIntegratedText());
        QCOMPARE(panel->findChild<QTableWidget *>("decisionsTable")->rowCount(), 0);
    }

    void devToolsShowsNetworkStateFromTelemetry()
    {
        BrowserWindow window;
        auto *panel = window.devToolsPanel();
        auto *service = window.networkStateService();
        // Three capture intervals through the V0.3 parser, then one V0.4 step.
        for (int i = 0; i < 3; ++i) publishCaptureSample(window.telemetry());
        service->ProcessOnce();
        panel->refreshNow();

        auto *stateTable = panel->findChild<QTableWidget *>("stateTable");
        const int rtt = rowOfText(stateTable, "rtt.handshake.mean_ms");
        QVERIFY(rtt >= 0);
        QCOMPARE(stateTable->item(rtt, 1)->text(), QLocale().toString(40.0, 'f', 2));
        QCOMPARE(stateTable->item(rtt, 2)->text(), QString("ms"));
        QCOMPARE(stateTable->item(rtt, 3)->text(), QString("measured"));
        QCOMPARE(stateTable->item(rtt, 4)->text(), QString("valid"));
        QCOMPARE(stateTable->item(rtt, 5)->text(), QString("3"));
        QVERIFY(stateTable->item(rtt, 6)->text().endsWith("s ago"));
        QCOMPARE(stateTable->item(rtt, 7)->text(), QString("tshark / capture_interface / unattributed"));
        const int ratio = rowOfText(stateTable, "tcp.retransmission_ratio");
        QCOMPARE(stateTable->item(ratio, 2)->text(), QString("ratio"));
        QCOMPARE(stateTable->item(ratio, 3)->text(), QString("derived"));
        // Page-load features stay separate from network latency.
        const int navigation = rowOfText(stateTable, "nav.duration.median_ms");
        QCOMPARE(stateTable->item(navigation, 1)->text(), QString("—"));
        QVERIFY(panel->findChild<QLabel *>("overview_networkState")->text().startsWith("degraded"));
        QVERIFY(panel->findChild<QLabel *>("overview_stateUpdated")->text().contains("newest measurement"));
        QVERIFY(panel->findChild<QLabel *>("stateSummary")->text().contains("capture session"));

        // Other observation windows can be inspected.
        auto *windows = panel->findChild<QComboBox *>("stateWindow");
        QCOMPARE(windows->currentData().toString(), QString("medium"));
        windows->setCurrentIndex(0);
        QCOMPARE(stateTable->item(rtt, 4)->text(), QString("valid"));

        // Telemetry does not make an unimplemented algorithm eligible.
        auto *eligibility = panel->findChild<QTableWidget *>("eligibilityTable");
        for (int row = 0; row < eligibility->rowCount(); ++row) {
            QCOMPARE(eligibility->item(row, 1)->text(), QString("not_implemented"));
            QVERIFY(eligibility->item(row, 5)->text().contains("required_features"));
        }
        QVERIFY(panel->findChild<QLabel *>("overview_eligibleAlgorithms")->text().startsWith("0 of 10 eligible"));
        auto *constraints = panel->findChild<QTableWidget *>("constraintsTable");
        const int fresh = rowOfText(constraints, "operational.state_fresh");
        QCOMPARE(constraints->item(fresh, 2)->text(), QString("true"));
        QCOMPARE(constraints->item(fresh, 4)->text(), QString("derived"));

        // Diagnostics appear as their own event category, not as decisions.
        auto *events = panel->findChild<QTableWidget *>("eventsTable");
        auto *filter = panel->findChild<QComboBox *>("eventFilter");
        // V0.5 adds a fifth category, "Forwarding", for execution events;
        // V0.6 a sixth, "Adaptive runtime"; V0.7.2 a seventh, "Expert model";
        // V0.7.3 an eighth, "Browser gateway".
        QCOMPARE(filter->count(), 8);
        QCOMPARE(filter->itemText(7), QString("Browser gateway"));
        QCOMPARE(filter->itemText(5), QString("Adaptive runtime"));
        QCOMPARE(filter->itemText(6), QString("Expert model"));
        filter->setCurrentIndex(3);
        QVERIFY(events->rowCount() > 0);
        const QStringList types = columnTexts(events, 2);
        QVERIFY(types.contains("state.updated"));
        QVERIFY(types.contains("constraints.evaluated"));
        QVERIFY(types.contains("eligibility.evaluated"));
        QVERIFY(types.contains("eligibility.algorithm_excluded"));
        for (const QString &source : columnTexts(events, 3)) QCOMPARE(source, QString("state_pipeline"));
        QCOMPARE(panel->findChild<QTableWidget *>("decisionsTable")->rowCount(), 0);
        QCOMPARE(panel->findChild<QLabel *>("runtime_algorithm")->text(), DevToolsPanel::notIntegratedText());

        // Reading the snapshot never waits on processing: no UI blocking.
        QElapsedTimer timer;
        timer.start();
        for (int i = 0; i < 10000; ++i) QVERIFY(service->Latest());
        QVERIFY2(timer.elapsed() < 1000, qPrintable(QString::number(timer.elapsed())));
        for (int i = 0; i < 20; ++i) panel->refreshNow();
        const auto stats = service->Stats();
        QVERIFY(stats.events_missed == 0);
        qInfo("NetworkState step: last %lld us, max %lld us over %llu steps", stats.last_step_us,
              stats.max_step_us, static_cast<unsigned long long>(stats.steps));
    }

    void networkStateServiceStopsWithWindow()
    {
        auto *window = new BrowserWindow;
        QVERIFY(window->networkStateService()->Stats().running);
        for (int i = 0; i < 3; ++i) publishCaptureSample(window->telemetry());
        window->devToolsPanel()->show();
        window->devToolsPanel()->refreshNow();
        // The worker is joined before the telemetry pipeline is destroyed.
        delete window;
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }

    // ---- V0.5: controlled forwarding and manual execution ----------------

    // ---- V0.7.2: AI expert model in Developer Tools ----------------------

    void devToolsShowsExpertModel()
    {
        namespace mdl = aafr::model;
        namespace rt = aafr::runtime;
        namespace fwd = aafr::forwarding;
        TelemetryPipeline pipeline;
        DevToolsPanel panel(pipeline, nullptr, nullptr);
        panel.refreshNow();
        const auto label = [&](const char *name) { return panel.findChild<QLabel *>(name)->text(); };
        // Default: disabled, deterministic only, mode cannot be changed.
        QVERIFY(label("expertModel_worker").startsWith(DevToolsPanel::expertModelDisabledText()));
        auto *mode = panel.findChild<QComboBox *>("expertModelMode");
        QVERIFY(!mode->isEnabled());
        QCOMPARE(panel.findChild<QComboBox *>("eventFilter")->count(), 8);
        QCOMPARE(panel.findChild<QComboBox *>("eventFilter")->itemText(6), QString("Expert model"));

        // Attach a scripted model in shadow mode and run one recommendation.
        aafr::test::FakeModel model;
        model.Always(aafr::test::FakeModel::Label("PSO"));
        mdl::ConfiguredTelemetrySource telemetry;
        telemetry.SetProfile(aafr::test::ConfiguredTelemetryProfile(), "test");
        mdl::ModelEventLog events;
        events.SetPipeline(&pipeline);
        mdl::AIProviderConfig config;
        config.mode = rt::RecommendationMode::Shadow;
        mdl::AIRecommendationProvider provider(&model, &telemetry, config, &events);
        auto graph = std::make_shared<fwd::ForwardingGraph>("tiny");
        graph->AddNode({"entry", fwd::NodeRole::Entry, true});
        graph->AddNode({"dest", fwd::NodeRole::Destination, true});
        fwd::ForwardingEdge e;
        e.from = "entry";
        e.to = "dest";
        e.latency_ms = fwd::Measurement::Of(1.0, fwd::Provenance::Configured);
        e.loss_ratio = fwd::Measurement::Of(0.0, fwd::Provenance::Configured);
        e.bandwidth_mbps = fwd::Measurement::Of(10.0, fwd::Provenance::Configured);
        graph->AddEdge(e);
        rt::RecommendationInput in;
        in.graph = graph;
        in.problem.source = "entry";
        in.problem.destination = "dest";
        for (auto id : {aafr::algorithms::AlgorithmId::Dijkstra, aafr::algorithms::AlgorithmId::ParticleSwarmOptimization}) {
            rt::CandidateEvaluation c;
            c.algorithm = id;
            c.considered = c.computed = c.path_valid = c.path_feasible = true;
            c.path = {"entry", "dest"};
            c.objective = 1.5;
            in.candidates.push_back(c);
        }
        in.context = {"e1", rt::EvaluationTrigger::Manual, 3, graph->version()};
        provider.Recommend(in);
        panel.setExpertModel(&model, &provider, &events, QString());
        panel.refreshNow();
        QVERIFY(mode->isEnabled());
        QCOMPARE(mode->currentIndex(), 1);  // shadow
        QVERIFY2(label("expertModel_prediction").contains("pso"), qPrintable(label("expertModel_prediction")));
        QVERIFY(label("expertModel_deterministic").contains("dijkstra"));
        QVERIFY(label("expertModel_source").contains("deterministic_shadow"));
        QVERIFY(label("expertModel_inference").contains("uncalibrated"));
        QVERIFY(label("expertModel_inference").contains("configured=23"));
        // Prediction, runtime-selected and confirmed are separate fields.
        QCOMPARE(label("expertModel_confirmed"), DevToolsPanel::adaptiveRuntimeNotRunningText());
        QVERIFY(label("expertModel_comparison").contains("agreed with deterministic 0 of 1"));
        // The mode selector changes the provider's mode (no switching here).
        emit mode->activated(2);
        QCOMPARE(provider.Mode(), rt::RecommendationMode::AiPreferred);
        panel.setExpertModel(nullptr, nullptr, nullptr, QString());
    }

    void devToolsWithoutForwardingService()
    {
        TelemetryPipeline pipeline;
        DevToolsPanel panel(pipeline, nullptr, nullptr);
        panel.refreshNow();
        for (const char *name : {"overview_forwardingEngine", "overview_verifiedAlgorithms",
                                 "overview_latestExecution", "overview_latestForwarding", "forwardingSummary",
                                 "executionStatus"})
            QVERIFY2(panel.findChild<QLabel *>(name)->text().contains("not attached"), name);
        QCOMPARE(panel.findChild<QLabel *>("adaptiveRuntimeStatus")->text(), DevToolsPanel::adaptiveRuntimeText());
        QCOMPARE(DevToolsPanel::adaptiveRuntimeText(), QString("Adaptive Runtime: Not integrated"));
        QVERIFY(!panel.findChild<QPushButton *>("executionRun")->isEnabled());
        QCOMPARE(panel.findChild<QTableWidget *>("implementationTable")->rowCount(), 0);
    }

    void devToolsRunsAlgorithmOverLoopbackRelays()
    {
        BrowserWindow window;
        auto *panel = window.devToolsPanel();
        auto *forwarding = window.forwardingService();
        QVERIFY(forwarding);
        using aafr::forwarding::ForwardingService;
        // Nothing starts until Developer Tools is opened.
        QCOMPARE(forwarding->GetStatus().state, ForwardingService::State::NotStarted);
        QVERIFY(!forwarding->GetStatus().relays_running);
        window.startForwarding();
        QTRY_VERIFY_WITH_TIMEOUT(forwarding->GetStatus().state == ForwardingService::State::Ready, 60000);
        QCOMPARE(forwarding->GetStatus().verified_executable, std::size_t(10));
        panel->refreshNow();
        QVERIFY(panel->findChild<QLabel *>("overview_verifiedAlgorithms")->text().startsWith("10 of 10 verified executable"));
        QVERIFY(panel->findChild<QLabel *>("overview_forwardingEngine")->text().contains("not routed"));

        // The V0.4 capability registry now reports measured implementations.
        window.networkStateService()->ProcessOnce();
        panel->refreshNow();
        const QStringList availability = columnTexts(panel->findChild<QTableWidget *>("capabilityTable"), 2);
        QCOMPARE(availability.size(), 10);
        QCOMPARE(availability.count("verified_executable"), 10);

        // Topology, measurements with provenance, and the available paths.
        auto *topology = panel->findChild<QTableWidget *>("topologyTable");
        QCOMPARE(topology->rowCount(), 5);
        QCOMPARE(columnTexts(topology, 5).count("configured"), 5);
        QCOMPARE(panel->findChild<QTableWidget *>("forwardingPathsTable")->rowCount(), 3);
        QCOMPARE(panel->findChild<QTableWidget *>("implementationTable")->rowCount(), 10);

        // No capture is running, so the host-state checks fail; the explicit
        // override lets the controlled-topology run go ahead and says so.
        panel->findChild<QCheckBox *>("executionIgnoreHostState")->setChecked(true);
        auto *choice = panel->findChild<QComboBox *>("executionAlgorithm");
        choice->setCurrentIndex(choice->findText("dijkstra"));
        auto *run = panel->findChild<QPushButton *>("executionRun");
        QVERIFY(run->isEnabled());
        run->click();
        auto *stage = panel->findChild<QLabel *>("execution_stage");
        QTRY_VERIFY_WITH_TIMEOUT((panel->refreshNow(), stage->text().startsWith("completed")), 20000);
        QVERIFY(panel->findChild<QLabel *>("execution_confirmedPath")->text().contains("entry -> relay_a -> dest"));
        QVERIFY(panel->findChild<QLabel *>("execution_confirmedPath")->text().contains("confirmed"));
        QVERIFY(panel->findChild<QLabel *>("execution_eligibility")->text().contains("overridden"));
        QVERIFY(panel->findChild<QLabel *>("execution_objective")->text().startsWith("5 "));
        QVERIFY(panel->findChild<QLabel *>("overview_latestForwarding")->text().contains("path confirmed"));
        QVERIFY(forwarding->network().HandledCount("relay_a") >= 1);

        // Execution events form their own category. A manual execution
        // records no decision: since V0.6 every decision row comes from the
        // adaptive runtime (which starts after verification).
        auto *filter = panel->findChild<QComboBox *>("eventFilter");
        filter->setCurrentIndex(4);
        const QStringList types = columnTexts(panel->findChild<QTableWidget *>("eventsTable"), 2);
        QVERIFY(types.contains("algorithm.execution_started"));
        QVERIFY(types.contains("algorithm.execution_completed"));
        QVERIFY(types.contains("forwarding.execution_completed"));
        const QString session = QString::fromStdString(window.adaptiveRuntime()->Status().session_id);
        for (const QString &id : columnTexts(panel->findChild<QTableWidget *>("decisionsTable"), 1))
            QVERIFY2(id.startsWith(session + "-d"), qPrintable(id));
        QTRY_VERIFY_WITH_TIMEOUT((panel->refreshNow(), panel->findChild<QLabel *>("adaptiveRuntimeStatus")->text().contains(
                                     "deterministic, no ML")),
                                 30000);
    }

    // ---- V0.6: adaptive runtime ------------------------------------------

    void adaptiveRuntimeSwitchesInBrowser()
    {
        using aafr::runtime::RuntimeLifecycle;
        BrowserWindow window;
        auto *panel = window.devToolsPanel();
        auto *runtime = window.adaptiveRuntime();
        QVERIFY(runtime);
        // Nothing runs before controlled forwarding starts; nothing is invented.
        panel->refreshNow();
        QCOMPARE(panel->findChild<QLabel *>("adaptive_lifecycle")->text(), DevToolsPanel::adaptiveRuntimeNotRunningText());
        QCOMPARE(panel->findChild<QLabel *>("overview_runtimeLifecycle")->text(),
                 DevToolsPanel::adaptiveRuntimeNotRunningText());
        QVERIFY(!panel->findChild<QPushButton *>("adaptiveEvaluate")->isEnabled());

        window.startForwarding();
        QTRY_VERIFY_WITH_TIMEOUT(runtime->Status().lifecycle == RuntimeLifecycle::Ready, 90000);
        QTRY_VERIFY_WITH_TIMEOUT((panel->refreshNow(), panel->findChild<QLabel *>("adaptive_lifecycle")->text().startsWith("ready")),
                                 10000);
        const auto describe = [runtime] {
            const auto st = runtime->Status();
            QString text = QString("lifecycle=%1 active=%2 last=%3 change=%4")
                               .arg(aafr::runtime::ToString(st.lifecycle))
                               .arg(st.active ? QString::fromStdString(aafr::forwarding::PathText(st.active->path)) : "none")
                               .arg(QString::fromStdString(st.last_evaluation))
                               .arg(QString::fromStdString(st.last_change));
            const auto decisions = runtime->Decisions(3);
            for (const auto &d : decisions)
                text += QLatin1Char('\n') + QString::fromStdString(d.decision_id + " " + aafr::runtime::ToString(d.type) + " " +
                                                       aafr::runtime::ReasonsText(d.reasons) + " " + d.explanation);
            return text.toLocal8Bit();
        };
        const auto usesRelay = [runtime](const char *relay) {
            const auto status = runtime->Status();
            return status.active && status.active_verified &&
                   std::find(status.active->path.begin(), status.active->path.end(), relay) != status.active->path.end();
        };
        // Initial activation: the configured best path entry -> relay_a -> dest.
        QVERIFY(usesRelay("relay_a"));
        QVERIFY(panel->findChild<QLabel *>("overview_confirmedConfiguration")->text().contains("confirmed"));
        auto *evidence = panel->findChild<QTableWidget *>("evidenceTable");
        QVERIFY(evidence->rowCount() >= 1);
        QVERIFY(columnTexts(evidence, 3).contains("initial_activation"));
        QVERIFY(panel->findChild<QTableWidget *>("decisionsTable")->rowCount() >= 1);
        QVERIFY(panel->findChild<QPushButton *>("adaptiveEvaluate")->isEnabled());
        // V0.7.1: the measurement-review row reflects the runtime's own schedule.
        {
            const QString review = panel->findChild<QLabel *>("adaptive_review")->text();
            const double interval = runtime->config().review_interval_ms;
            QVERIFY2(interval > 0.0 ? review.startsWith(QString("every %1 s").arg(interval / 1000.0, 0, 'f', 0))
                                    : review.startsWith("disabled"),
                     review.toLocal8Bit().constData());
            QVERIFY(panel->findChild<QLabel *>("adaptive_counters")->text().contains("measurement reviews"));
        }

        // A -> B: congest relay_a through the panel; the runtime measures,
        // passes its gates, activates and confirms a path without relay_a.
        panel->findChild<QPushButton *>("adaptiveCongestA")->click();
        QTRY_VERIFY2_WITH_TIMEOUT(!usesRelay("relay_a"), describe().constData(), 90000);
        panel->refreshNow();
        QVERIFY(columnTexts(evidence, 3).contains("switch"));
        QVERIFY(panel->findChild<QLabel *>("adaptive_lastSwitch")->text().startsWith("switched to"));
        QVERIFY(panel->findChild<QTableWidget *>("gatesTable")->rowCount() == 6);

        // B -> A: restore, then congest relay_b; the runtime returns to relay_a.
        panel->findChild<QPushButton *>("adaptiveClearCondition")->click();
        panel->findChild<QPushButton *>("adaptiveCongestB")->click();
        QTRY_VERIFY2_WITH_TIMEOUT(usesRelay("relay_a") && !usesRelay("relay_b"), describe().constData(), 120000);
        QVERIFY(runtime->Status().counters.switches >= 2);

        // Decision detail and runtime events.
        panel->refreshNow();
        evidence->selectRow(0);
        QVERIFY(panel->findChild<QPlainTextEdit *>("decisionDetail")->toPlainText().contains("\"decision_id\""));
        auto *filter = panel->findChild<QComboBox *>("eventFilter");
        filter->setCurrentIndex(5);
        const QStringList types = columnTexts(panel->findChild<QTableWidget *>("eventsTable"), 2);
        QVERIFY(types.contains("runtime.switch_committed"));
        QVERIFY(types.contains("runtime.evaluation_started"));
        for (const QString &source : columnTexts(panel->findChild<QTableWidget *>("eventsTable"), 3))
            QCOMPARE(source, QString("adaptive_runtime"));
        panel->findChild<QPushButton *>("adaptiveClearCondition")->click();
    }

    void browserUsableWithPanelOpenAndClosed()
    {
        BrowserWindow window;
        window.show();
        auto *tabs = window.tabManager();
        auto *address = window.findChild<QLineEdit *>("addressBar");
        window.devToolsPanel()->show();
        address->setText(first_.toString());
        QTest::keyClick(address, Qt::Key_Return);
        QTRY_COMPARE_WITH_TIMEOUT(tabs->currentView()->title(), QString("First page"), 15000);
        QVERIFY(tabs->currentView()->isVisible());
        QVERIFY(tabs->currentView()->height() > 100);
        window.devToolsPanel()->hide();
        tabs->addTab(second_);
        QTRY_COMPARE_WITH_TIMEOUT(tabs->currentView()->title(), QString("Second page"), 15000);
        window.devToolsPanel()->show();
        tabs->closeCurrentTab();
        QTRY_COMPARE(tabs->count(), 1);
        window.devToolsPanel()->refreshNow();
        QVERIFY(countOf(window.telemetry(), EventType::TabClosed) >= 1);
    }

    void windowShutdownWithPanelAndCapture()
    {
        auto *window = new BrowserWindow(nullptr, first_);
        QPointer<DevToolsPanel> panel = window->devToolsPanel();
        QPointer<BrowserEventCollector> collector = window->eventCollector();
        window->show();
        panel->show();
        QTRY_COMPARE_WITH_TIMEOUT(window->tabManager()->currentView()->title(), QString("First page"), 15000);
        delete window;
        QVERIFY(panel.isNull() && collector.isNull());
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }

    // ---- V0.8 Phase 5B.2: research pipeline (decision only) ------------

    void overviewMapsObservedStates()
    {
        using namespace aafr::ui;
        OverviewInputs in;
        auto status = overviewStatus(in);
        QCOMPARE(status.browser, QString("Unavailable"));
        QCOMPARE(status.capture, QString("Unavailable"));
        QCOMPARE(status.runtime, QString("Unavailable"));
        QCOMPARE(status.algorithm, QString("Unavailable"));
        QCOMPARE(status.research, QString("Unavailable"));
        QCOMPARE(status.path, QString("Unavailable"));
        QCOMPARE(status.mode, QString("Direct"));
        using Nav = BrowserEventCollector::NavigationStatus;
        const QList<QPair<Nav, QString>> browserStates = {
            {Nav::Idle, "Idle"}, {Nav::Loading, "Loading"}, {Nav::Succeeded, "Page loaded"},
            {Nav::Failed, "Error"}, {Nav::Stopped, "Idle"}, {Nav::RendererTerminated, "Error"}};
        for (const auto &entry : browserStates) {
            in.browser = entry.first;
            QCOMPARE(overviewStatus(in).browser, entry.second);
        }
        using Capture = NetworkMonitor::State;
        const QList<QPair<Capture, QString>> captureStates = {
            {Capture::Disabled, "Inactive"}, {Capture::Starting, "Inactive"}, {Capture::Running, "Active"},
            {Capture::Stopping, "Inactive"}, {Capture::Unavailable, "Unavailable"}, {Capture::Error, "Error"}};
        for (const auto &entry : captureStates) {
            in.capture = entry.first;
            QCOMPARE(overviewStatus(in).capture, entry.second);
        }
        aafr::runtime::RuntimeStatus production;
        aafr::runtime::ActiveConfiguration active;
        active.algorithm = aafr::algorithms::AlgorithmId::Dijkstra;
        active.path = {"entry", "relay_a", "dest"};
        production.active = active;
        production.active_verified = true;
        using Life = aafr::runtime::RuntimeLifecycle;
        const QList<QPair<Life, QString>> runtimeStates = {
            {Life::Uninitialized, "Unavailable"}, {Life::Initializing, "Unavailable"}, {Life::Ready, "Ready"},
            {Life::Evaluating, "Evaluating"}, {Life::Switching, "Switching"}, {Life::Degraded, "Error"},
            {Life::Failed, "Error"}, {Life::Stopping, "Unavailable"}, {Life::Stopped, "Unavailable"}};
        for (const auto &entry : runtimeStates) {
            production.lifecycle = entry.first;
            in.runtime = production;
            QCOMPARE(overviewStatus(in).runtime, entry.second);
        }
        production.lifecycle = Life::Ready;
        in.runtime = production;
        status = overviewStatus(in);
        QCOMPARE(status.algorithm, QString("Dijkstra"));
        QCOMPARE(status.path, QString::fromUtf8("Entry → Relay A → Destination"));
        in.controlled = true;
        QCOMPARE(overviewStatus(in).mode, QString("Controlled"));
        QCOMPARE(overviewStatus(in).path, status.path); // Mode cannot invent or change a path.
        in.controlled = false;
        QCOMPARE(overviewStatus(in).mode, QString("Direct"));
        production.active_verified = false;
        in.runtime = production;
        QCOMPARE(overviewStatus(in).algorithm, QString("Unavailable"));
        QCOMPARE(overviewStatus(in).path, QString("Unavailable"));
        QCOMPARE(overviewStatus(in).runtime, QString("Unavailable"));

        in.runtime.reset();
        obs::RuntimeSnapshot generic;
        generic.state = obs::RuntimeState::Running;
        obs::AlgorithmStatus reported;
        reported.algorithm_id = "astar";
        reported.execution_state = obs::ExecutionState::Initializing;
        generic.active_algorithm = reported;
        in.production = generic;
        QCOMPARE(overviewStatus(in).algorithm, QString("Unavailable"));
        generic.active_algorithm->execution_state = obs::ExecutionState::Active;
        in.production = generic;
        QCOMPARE(overviewStatus(in).algorithm, QString("A* (A-star)"));
        QCOMPARE(overviewStatus(in).runtime, QString("Unavailable"));
        QCOMPARE(overviewStatus(in).path, QString("Unavailable")); // Generic contract has no path.
        in.production.reset();
        aafr::gateway::GatewayStatus gateway;
        gateway.running = gateway.has_bindings = true;
        aafr::gateway::PinnedConfiguration pinned;
        pinned.algorithm = "dijkstra";
        pinned.path = active.path;
        gateway.current = pinned;
        in.gateway = gateway;
        in.controlled = false;
        QCOMPARE(overviewStatus(in).path, QString("Unavailable"));
        in.controlled = true;
        QCOMPARE(overviewStatus(in).path, status.path);
        QCOMPARE(overviewStatus(in).runtime, QString("Unavailable")); // Gateway presence is not runtime readiness.
        in.runtime = production; // Unverified runtime must take precedence over a stale gateway configuration.
        QCOMPARE(overviewStatus(in).path, QString("Unavailable"));

        aafr::research::ResearchObservabilitySnapshot research;
        research.host_state = aafr::research::ResearchHostState::Running;
        research.adapter_present = true;
        using Research = aafr::research::ResearchRuntimeState;
        const QList<QPair<Research, QString>> researchStates = {
            {Research::Unavailable, "Unavailable"}, {Research::Idle, "Ready"},
            {Research::Evaluating, "Evaluating"}, {Research::Decided, "Evaluation complete"}, {Research::Error, "Error"}};
        for (const auto &entry : researchStates) {
            research.adapter.state = entry.first;
            in.research = research;
            QCOMPARE(overviewStatus(in).research, entry.second);
        }
        research.adapter.state = Research::Idle;
        research.evaluation_pending = true;
        in.research = research;
        QCOMPARE(overviewStatus(in).research, QString("Evaluating"));
        for (const auto state : {aafr::research::ResearchHostState::NotStarted, aafr::research::ResearchHostState::Loading,
                                 aafr::research::ResearchHostState::Stopped}) {
            research.host_state = state;
            in.research = research;
            QCOMPARE(overviewStatus(in).research, QString("Unavailable"));
        }
        research.host_state = aafr::research::ResearchHostState::Running;
        research.evaluation_pending = false;
        research.adapter.state = Research::Decided;
        aafr::research::ResearchRecommendation recommendation;
        recommendation.verdict = aafr::research::ArbitrationVerdict::Switch;
        recommendation.recommended_path = {"entry", "relay_b", "dest"};
        research.adapter.last = recommendation;
        in.research = research;
        production.active_verified = true;
        in.runtime = production;
        QCOMPARE(overviewStatus(in).research, QString("Evaluation complete"));
        QCOMPARE(overviewStatus(in).algorithm, status.algorithm);
        QCOMPARE(overviewStatus(in).path, status.path); // Research never supplies production fields.
    }

    void overviewWiresIndependentProviders()
    {
        TelemetryPipeline pipeline;
        FakeRuntimeProvider production;
        production.snapshot.state = obs::RuntimeState::Running;
        obs::AlgorithmStatus active;
        active.algorithm_id = "dijkstra";
        active.execution_state = obs::ExecutionState::Active;
        production.snapshot.active_algorithm = active;
        FakeResearchProvider research;
        research.snapshot.host_state = aafr::research::ResearchHostState::Running;
        research.snapshot.adapter_present = true;
        research.snapshot.adapter.state = aafr::research::ResearchRuntimeState::Idle;
        DevToolsPanel panel(pipeline, nullptr, nullptr);
        panel.setRuntimeProvider(&production);
        panel.setResearchObservability(&research, {});
        const auto field = [&panel](const char *key) { return panel.findChild<QLabel *>(QString("summary_overview_") + key)->text(); };
        panel.refreshNow();
        QCOMPARE(field("runtime"), QString("Unavailable"));
        QCOMPARE(field("research"), QString("Ready"));
        QCOMPARE(field("algorithm"), QString("Dijkstra"));
        QCOMPARE(field("path"), QString("Unavailable"));
        QCOMPARE(field("capture"), QString("Unavailable"));
        QCOMPARE(field("mode"), QString("Direct"));
        research.snapshot.evaluation_pending = true;
        panel.refreshNow();
        QCOMPARE(field("research"), QString("Evaluating"));
        QCOMPARE(field("runtime"), QString("Unavailable"));
        QCOMPARE(field("algorithm"), QString("Dijkstra"));
        aafr::gateway::AAFRBrowserForwardingGateway gateway({});
        panel.setBrowserForwarding(&gateway, "controlled", {});
        panel.refreshNow();
        QCOMPARE(field("mode"), QString("Controlled"));
        QCOMPARE(field("path"), QString("Unavailable"));
        research.snapshot.evaluation_pending = false;
        research.snapshot.adapter.state = aafr::research::ResearchRuntimeState::Decided;
        panel.refreshNow();
        QCOMPARE(field("research"), QString("Evaluation complete"));
        QCOMPARE(field("runtime"), QString("Unavailable"));
        QCOMPARE(field("algorithm"), QString("Dijkstra"));
        QVERIFY(panel.findChild<QWidget *>("overviewAdvancedDetails")->isHidden());
    }

    void overviewRetainsDiagnosticsInTheirTabs()
    {
        BrowserWindow window(nullptr, first_);
        auto *panel = window.devToolsPanel();
        window.show();
        panel->show();
        QTRY_COMPARE_WITH_TIMEOUT(window.tabManager()->currentView()->title(), QString("First page"), 15000);
        panel->refreshNow();
        QTRY_COMPARE_WITH_TIMEOUT((panel->refreshNow(), panel->findChild<QLabel *>("summary_overview_browser")->text()),
                                  QString("Page loaded"), 15000);
        QCOMPARE(panel->findChild<QLabel *>("summary_overview_mode")->text(), QString("Direct"));
        auto *overview = panel->tabs()->widget(0);
        const QStringList fields = {"browser", "capture", "mode", "runtime", "algorithm", "research", "path"};
        for (const auto &field : fields) QVERIFY(overview->findChild<QLabel *>("summary_overview_" + field));
        QCOMPARE(overview->findChildren<QLabel *>(QRegularExpression("^summary_overview_")).size(), 7);
        const QList<QPair<QString, QString>> relocated = {
            {"metrics", "network"}, {"dataQuality", "network"}, {"eligibleAlgorithms", "runtime"},
            {"confirmedConfiguration", "runtime"}, {"latestSwitch", "decisions"},
            {"session", "events"}, {"navigationStatus", "events"}, {"collection", "events"}};
        for (const auto &entry : relocated) {
            auto *label = panel->findChild<QLabel *>("overview_" + entry.first);
            QVERIFY(label);
            QVERIFY(!overview->isAncestorOf(label));
            auto *advanced = panel->findChild<QWidget *>(entry.second + "AdvancedDetails");
            QVERIFY(advanced->isAncestorOf(label));
            QVERIFY(advanced->isHidden());
        }
        QVERIFY(panel->findChild<QLabel *>("summary_overview_path")->toolTip().contains("Direct browser connections bypass"));
        QVERIFY(panel->findChild<QLabel *>("summary_overview_research")->toolTip().contains("not applied"));
        window.startForwarding();
        QTRY_VERIFY_WITH_TIMEOUT((panel->refreshNow(), panel->findChild<QLabel *>("summary_overview_runtime")->text() == "Ready"), 90000);
        QVERIFY(window.adaptiveRuntime()->Status().active_verified);
        QVERIFY(panel->findChild<QLabel *>("summary_overview_algorithm")->text() != "Unavailable");
        QVERIFY(panel->findChild<QLabel *>("summary_overview_path")->text() != "Unavailable");
        QCOMPARE(panel->findChild<QLabel *>("summary_overview_mode")->text(), QString("Direct"));
        QVERIFY(panel->findChild<QWidget *>("overviewAdvancedDetails")->isHidden());
        const QString directory = qEnvironmentVariable("AAFR_UI_CAPTURE_DIR");
        if (!directory.isEmpty()) {
            QVERIFY(QDir().mkpath(directory));
            window.resize(1280, 800);
            window.resizeDocks({panel}, {360}, Qt::Vertical);
            QTest::qWait(150);
            QVERIFY(window.grab().save(directory + "/overview-ready.png"));
        }
        panel->findChild<QToolButton *>("overviewAdvancedToggle")->click();
        QVERIFY(panel->findChild<QLabel *>("overviewCurrentPage")->isVisible());
        QCOMPARE(panel->findChild<QLabel *>("overviewResearchEffect")->text(), QString("Decision only; not applied"));
        for (const auto &name : {"captureStatus", "stateSummary", "forwardingSummary", "browserGatewayBanner",
                                 "browserConnectionsNote", "forwardingMeasurementsSummary", "forwardingMeasurementsNote"})
            QVERIFY(panel->findChild<QLabel *>(name)->isHidden());
    }

    void researchDiagnosticsShowUnsavedRecords()
    {
        class StoreStatusProvider final : public aafr::research::IResearchObservabilityProvider {
        public:
            aafr::research::ResearchObservabilitySnapshot ResearchSnapshot(std::size_t) const override
            {
                aafr::research::ResearchObservabilitySnapshot snapshot;
                aafr::research::RecordStoreStatus store;
                store.configured = true;
                store.stored = store.unsaved = 2;
                store.capacity = 200;
                store.path = "integration-status-fixture.jsonl";
                store.last_error = "record store busy (locked by another instance)";
                snapshot.store = store;
                return snapshot;
            }
        } provider;
        TelemetryPipeline pipeline;
        DevToolsPanel panel(pipeline, nullptr, nullptr);
        panel.setResearchObservability(&provider, {});
        panel.refreshNow();
        auto *store = panel.findChild<QLabel *>("research_store");
        QVERIFY(store->text().contains("2 of this session not yet written (kept in memory)"));
        QVERIFY(store->text().contains("record store busy"));
        auto *details = panel.findChild<QWidget *>("runtimeAdvancedDetails");
        QVERIFY(details->isAncestorOf(store));
        QVERIFY(details->isHidden());
        panel.tabs()->setCurrentIndex(2);
        panel.show();
        panel.findChild<QToolButton *>("runtimeAdvancedToggle")->click();
        auto *diagnostics = panel.findChild<QTabWidget *>("runtimeDiagnostics");
        diagnostics->setCurrentIndex(diagnostics->count() - 1);
        panel.findChild<QToolButton *>("capabilityResearchToggle")->setChecked(true);
        QVERIFY(store->isVisible());
    }

    void researchRecordsFromTwoBrowserWindowsStayDistinct()
    {
        auto a = std::make_unique<BrowserWindow>(nullptr, first_);
        auto b = std::make_unique<BrowserWindow>(nullptr, second_);
        a->show();
        b->show();
        a->startForwarding();
        b->startForwarding();
        const auto ready = [](BrowserWindow *window) {
            window->devToolsPanel()->refreshNow();
            return window->devToolsPanel()->findChild<QLabel *>("adaptive_lifecycle")->text().startsWith("ready");
        };
        QTRY_VERIFY_WITH_TIMEOUT(ready(a.get()) && ready(b.get()), 90000);
        QStringList originals;
        for (auto *window : {a.get(), b.get()}) {
            auto *panel = window->devToolsPanel();
            auto *evaluate = panel->findChild<QPushButton *>("adaptiveEvaluate");
            QVERIFY(evaluate->isEnabled());
            evaluate->click();
            QTRY_VERIFY_WITH_TIMEOUT((panel->refreshNow(), panel->findChild<QLabel *>("research_latest")->text().startsWith("#1")), 30000);
            QTRY_VERIFY_WITH_TIMEOUT((panel->refreshNow(), !panel->findChild<QLabel *>("research_host")->text().contains("pending")), 30000);
            QVERIFY(panel->findChild<QLabel *>("research_requests")->text().startsWith("1 received"));
            originals << panel->findChild<QPlainTextEdit *>("researchRecordJson")->toPlainText();
        }
        a.reset();
        b.reset();  // Normal BrowserWindow shutdown joins each host and flushes.
        aafr::research::ResearchRecordStore reader(BrowserWindow::researchRecordsPath().toStdString(), 200, "reader");
        reader.Load();
        const auto records = reader.Records();
        QCOMPARE(records.size(), std::size_t(2));
        QVERIFY(records[0].session_id != records[1].session_id);
        QCOMPARE(records[0].sequence, std::uint64_t(1));
        QCOMPARE(records[1].sequence, std::uint64_t(1));
        QCOMPARE(QString::fromStdString(records[0].record_json), originals[0]);
        QCOMPARE(QString::fromStdString(records[1].record_json), originals[1]);
    }

    void presentationSummariesAndAdvancedAccess()
    {
        BrowserWindow window(nullptr, first_);
        window.show();
        window.resize(1280, 800);
        auto *panel = window.devToolsPanel();
        panel->show();
        window.resizeDocks({panel}, {540}, Qt::Vertical);
        QTRY_COMPARE_WITH_TIMEOUT(window.tabManager()->currentView()->title(), QString("First page"), 15000);
        panel->refreshNow();
        const QStringList names = {"overview", "network", "runtime", "decisions", "events"};
        QCOMPARE(panel->tabs()->count(), names.size());
        const QString directory = qEnvironmentVariable("AAFR_UI_CAPTURE_DIR");
        if (!directory.isEmpty()) QVERIFY(QDir().mkpath(directory));
        for (int i = 0; i < names.size(); ++i) {
            panel->tabs()->setCurrentIndex(i);
            auto *toggle = panel->findChild<QToolButton *>(names[i] + "AdvancedToggle");
            auto *details = panel->findChild<QWidget *>(names[i] + "AdvancedDetails");
            QVERIFY(toggle && details);
            QCOMPARE(toggle->text(), QString("Advanced diagnostics"));
            QVERIFY(!toggle->isChecked());
            QVERIFY(details->isHidden());
            if (!directory.isEmpty()) {
                QTest::qWait(150);
                QVERIFY(window.grab().save(directory + "/simplified-" + names[i] + ".png"));
            }
            toggle->click();
            QVERIFY(details->isVisible());
            // Every original diagnostic view remains in the expandable area.
            const QStringList diagnostics = {"devToolsOverviewScroll", "networkViews", "runtimeDiagnostics", "decisionsSplit", "eventsTable"};
            QVERIFY(details->findChild<QWidget *>(diagnostics[i]));
            if (!directory.isEmpty()) {
                QTest::qWait(100);
                QVERIFY(window.grab().save(directory + "/expanded-" + names[i] + ".png"));
            }
            if (i == 1 || i == 2) {
                auto *views = details->findChild<QTabWidget *>(i == 1 ? "networkViews" : "runtimeDiagnostics");
                QCOMPARE(views->count(), i == 1 ? 5 : 4);
                for (int view = 0; view < views->count(); ++view) {
                    views->setCurrentIndex(view);
                    QVERIFY(views->currentWidget()->isVisible());
                    if (!directory.isEmpty()) {
                        QTest::qWait(100);
                        QVERIFY(window.grab().save(directory + QString("/diagnostics-%1-%2.png").arg(names[i]).arg(view)));
                    }
                }
                if (i == 1) {
                    views->setCurrentIndex(2);
                    auto *nested = details->findChild<QTabWidget *>("forwardingViews");
                    QCOMPARE(nested->count(), 2);
                    for (int view = 0; view < nested->count(); ++view) {
                        nested->setCurrentIndex(view);
                        QVERIFY(nested->currentWidget()->isVisible());
                        if (!directory.isEmpty()) {
                            QTest::qWait(100);
                            QVERIFY(window.grab().save(directory + QString("/diagnostics-network-paths-%1.png").arg(view)));
                        }
                    }
                }
            }
            toggle->click();
            QVERIFY(details->isHidden());
        }
        QVERIFY(panel->findChild<QLabel *>("summary_runtime_algorithms")->text().contains("Dijkstra"));
        QVERIFY(panel->findChild<QLabel *>("summary_runtime_algorithms")->text().contains("ACO"));
        QVERIFY(panel->findChild<QLabel *>("summary_runtime_model")->text().contains("Decision only"));
        QVERIFY(panel->findChild<QWidget *>("runtimeAdvancedDetails")->isAncestorOf(
            panel->findChild<QLabel *>("summary_runtime_algorithms")));
        for (const auto *field : {"status", "algorithm", "path", "model", "recommendation", "decision", "switching", "health"})
            QVERIFY(panel->findChild<QLabel *>(QString("summary_runtime_") + field));
        QVERIFY(!panel->findChild<QPushButton *>("presentationEvaluate"));
        QVERIFY(panel->findChild<QWidget *>("runtimeAdvancedDetails")->isAncestorOf(
            panel->findChild<QPushButton *>("adaptiveEvaluate")));
        QVERIFY(panel->findChild<QLabel *>("summary_network_measured")->text().contains("not per-link"));
        QCOMPARE(panel->findChild<QLabel *>("summary_network_capture")->text(),
                 panel->findChild<QLabel *>("overview_capture")->text());
        QVERIFY(panel->findChild<QLabel *>("summary_network_paths")->text().contains("Relay A"));
        QVERIFY(panel->findChild<QLabel *>("summary_network_paths")->text().contains('\n'));
        QVERIFY(panel->findChild<QLabel *>("summary_runtime_recommendation")->text().startsWith("Production:"));
        QVERIFY(!panel->findChild<QLabel *>("summary_runtime_recommendation")->text().contains("Research:"));
        QVERIFY(panel->findChild<QWidget *>("runtimeAdvancedDetails")->isAncestorOf(
            panel->findChild<QLabel *>("summary_runtime_production")));
        for (int i = 0; i < names.size(); ++i) {
            panel->tabs()->setCurrentIndex(i);
            panel->refreshNow();
            QVERIFY(panel->findChild<QWidget *>(names[i] + "AdvancedDetails")->isHidden());
        }
        // Expanding and collapsing is presentation only: it cannot queue an evaluation.
        QCOMPARE(panel->findChild<QTableWidget *>("researchRecommendations")->rowCount(), 0);
        panel->tabs()->setCurrentIndex(2);
        panel->findChild<QToolButton *>("runtimeAdvancedToggle")->click();
        auto *diagnostics = panel->findChild<QTabWidget *>("runtimeDiagnostics");
        diagnostics->setCurrentIndex(diagnostics->count() - 1);
        panel->findChild<QToolButton *>("capabilityResearchToggle")->setChecked(true);
        QVERIFY(panel->findChild<QPlainTextEdit *>("researchRecordJson")->isVisible());
        if (!directory.isEmpty()) {
            QTest::qWait(150);
            QVERIFY(window.grab().save(directory + "/simplified-runtime-advanced.png"));
        }
    }

    void researchPipelineIsDecisionOnlyInBrowser()
    {
        using aafr::runtime::RuntimeLifecycle;
        BrowserWindow window(nullptr, first_);
        window.show();
        auto *panel = window.devToolsPanel();
        auto *runtime = window.adaptiveRuntime();
        QVERIFY(panel && runtime);
        // Detailed diagnostics remain within Capability registry.
        auto *diagnostics = panel->findChild<QTabWidget *>("runtimeDiagnostics");
        QStringList inner;
        for (int i = 0; i < diagnostics->count(); ++i) inner << diagnostics->tabText(i);
        QCOMPARE(inner, QStringList({"Adaptive runtime", "Browser forwarding", "Runtime state",
                                     "Algorithms"}));
        auto *constraintsToggle = panel->findChild<QToolButton *>("capabilityConstraintsToggle");
        QVERIFY(constraintsToggle);
        QVERIFY(!constraintsToggle->isChecked());
        QVERIFY(panel->findChild<QTableWidget *>("constraintsTable")->isHidden());
        constraintsToggle->setChecked(true);
        QVERIFY(!panel->findChild<QTableWidget *>("constraintsTable")->isHidden());
        constraintsToggle->setChecked(false);
        auto *researchToggle = panel->findChild<QToolButton *>("capabilityResearchToggle");
        QVERIFY(researchToggle);
        QVERIFY(!researchToggle->isChecked());
        QVERIFY(panel->findChild<QScrollArea *>("researchScroll")->isHidden());
        researchToggle->setChecked(true);
        QVERIFY(!panel->findChild<QScrollArea *>("researchScroll")->isHidden());
        researchToggle->setChecked(false);
        auto *executionToggle = panel->findChild<QToolButton *>("capabilityExecutionToggle");
        QVERIFY(executionToggle);
        QVERIFY(!executionToggle->isChecked());
        QVERIFY(panel->findChild<QWidget *>("executionPane")->isHidden());
        executionToggle->setChecked(true);
        QVERIFY(!panel->findChild<QWidget *>("executionPane")->isHidden());
        executionToggle->setChecked(false);
        auto *eligibilityToggle = panel->findChild<QToolButton *>("capabilityEligibilityToggle");
        QVERIFY(eligibilityToggle);
        QVERIFY(!eligibilityToggle->isChecked());
        QVERIFY(panel->findChild<QTableWidget *>("eligibilityTable")->isHidden());
        eligibilityToggle->setChecked(true);
        QVERIFY(!panel->findChild<QTableWidget *>("eligibilityTable")->isHidden());
        eligibilityToggle->setChecked(false);
        QCOMPARE(panel->findChild<QLabel *>("researchBanner")->text(), DevToolsPanel::researchDecisionOnlyText());
        const auto label = [panel](const char *name) { return panel->findChild<QLabel *>(QString("research_") + name)->text(); };
        // The model loads on the research host's worker; the GUI keeps running.
        QTRY_VERIFY_WITH_TIMEOUT((panel->refreshNow(), label("state") == "idle"), 30000);
        QVERIFY(label("model").contains("xgboost-link-weight-v3") && label("model").contains("6b6c008"));
        QVERIFY(label("statistics").contains("incl. test rows"));
        QVERIFY(label("actuation").startsWith("disabled: decision only"));
        // Page loads do not evaluate anything.
        QTRY_COMPARE_WITH_TIMEOUT(window.tabManager()->currentView()->title(), QString("First page"), 15000);
        panel->refreshNow();
        QCOMPARE(panel->findChild<QTableWidget *>("researchRecommendations")->rowCount(), 0);
        QVERIFY(label("requests").startsWith("0 received"));

        window.startForwarding();
        QTRY_VERIFY_WITH_TIMEOUT(runtime->Status().lifecycle == RuntimeLifecycle::Ready, 90000);
        // Developer Tools attach the runtime through a queued call: wait until
        // the panel shows it ready (as adaptiveRuntimeSwitchesInBrowser does).
        QTRY_VERIFY_WITH_TIMEOUT((panel->refreshNow(), panel->findChild<QLabel *>("adaptive_lifecycle")->text().startsWith("ready")),
                                 10000);
        const auto before = runtime->Status();
        QVERIFY(before.active);
        // The existing diagnostic action queues one research evaluation.
        auto *evaluate = panel->findChild<QPushButton *>("adaptiveEvaluate");
        QVERIFY(evaluate->isEnabled());
        evaluate->click();
        QVERIFY(panel->findChild<QLabel *>("adaptiveAction")->text().contains("decision only"));
        QTRY_VERIFY_WITH_TIMEOUT((panel->refreshNow(), label("latest").startsWith("#1")), 30000);
        QVERIFY(label("latest").contains("not applied"));
        QVERIFY(label("telemetry").startsWith("controlled:"));
        QVERIFY(!label("telemetry").startsWith("controlled: controlled:"));
        QVERIFY(label("telemetry").contains("not per-link SDN telemetry"));
        QVERIFY(label("telemetry").contains("utilization not measured"));
        QVERIFY(label("probes").startsWith("none supplied"));
        auto *recommendations = panel->findChild<QTableWidget *>("researchRecommendations");
        QCOMPARE(recommendations->rowCount(), 1);
        QCOMPARE(recommendations->item(0, 2)->text(), QString("no (decision only)"));
        QCOMPARE(recommendations->item(0, 5)->text(), QString("controlled"));
        // Without utilization only Dijkstra can run; A*, ACO and GA say why.
        auto *candidates = panel->findChild<QTableWidget *>("researchCandidates");
        QCOMPARE(candidates->rowCount(), 4);
        QCOMPARE(candidates->item(0, 0)->text(), QString("dijkstra"));
        QCOMPARE(candidates->item(0, 1)->text(), QString("path_found"));
        for (int r = 1; r < 4; ++r) QCOMPARE(candidates->item(r, 1)->text(), QString("missing_cost"));
        auto *links = panel->findChild<QTableWidget *>("researchLinks");
        QVERIFY(links->rowCount() > 0);
        QVERIFY(columnTexts(links, 1).contains("missing_input"));
        QVERIFY(columnTexts(panel->findChild<QTableWidget *>("researchRequests"), 2).join("|").contains("evaluated: recommendation #1"));
        // The research recommendation changed nothing in forwarding.
        const auto after = runtime->Status();
        QVERIFY(after.active && after.active->config_version == before.active->config_version);
        QCOMPARE(after.active->path, before.active->path);
        // The existing evaluation control retains the production + research action.
        const auto productionEvaluation = runtime->Status().last_evaluation_at;
        panel->findChild<QPushButton *>("adaptiveEvaluate")->click();
        QTRY_VERIFY_WITH_TIMEOUT((panel->refreshNow(), label("latest").startsWith("#2")), 30000);
        QTRY_VERIFY_WITH_TIMEOUT(runtime->Status().last_evaluation_at != productionEvaluation, 30000);
        QVERIFY(label("requests").startsWith("2 received"));
        QCOMPARE(panel->findChild<QLabel *>("summary_runtime_recommendation")->toolTip(),
                 panel->findChild<QLabel *>("summary_decisions_research")->text());
        QVERIFY(panel->findChild<QLabel *>("summary_decisions_research")->text().contains("Not applied"));
        for (const auto &name : {"overview", "network", "runtime", "decisions", "events"}) {
            QVERIFY(panel->findChild<QWidget *>(QString(name) + "AdvancedDetails")->isHidden());
        }
        const QString directory = qEnvironmentVariable("AAFR_UI_CAPTURE_DIR");
        if (!directory.isEmpty()) {
            QVERIFY(QDir().mkpath(directory));
            panel->show();
            window.resize(1400, 900);
            window.resizeDocks({panel}, {560}, Qt::Vertical);
            panel->tabs()->setCurrentIndex(2);
            panel->refreshNow();
            QTest::qWait(150);
            QVERIFY(window.grab().save(directory + "/evaluated-runtime.png"));
            panel->tabs()->setCurrentIndex(3);
            QTest::qWait(150);
            QVERIFY(window.grab().save(directory + "/evaluated-decisions.png"));
            panel->tabs()->setCurrentIndex(2);
            panel->findChild<QToolButton *>("runtimeAdvancedToggle")->setChecked(true);
            diagnostics->setCurrentIndex(diagnostics->count() - 1);
            panel->findChild<QToolButton *>("capabilityResearchToggle")->setChecked(true);
            panel->refreshNow();
            QTest::qWait(500);
            QVERIFY(window.grab().save(directory + "/devtools-runtime-research.png"));
            auto *scroll = panel->findChild<QScrollArea *>("researchScroll");
            scroll->verticalScrollBar()->setValue(scroll->verticalScrollBar()->maximum() * 55 / 100);
            QTest::qWait(300);
            QVERIFY(window.grab().save(directory + "/devtools-runtime-research-tables.png"));
        }
    }

    void researchPipelineUnavailableWithoutXGBoost()
    {
        const QByteArray previous = qgetenv("AAFR_XGBOOST_LIBRARY");
        qputenv("AAFR_XGBOOST_LIBRARY", "no_such_xgboost_library.dll");
        {
            BrowserWindow window(nullptr, first_);
            window.show();
            auto *panel = window.devToolsPanel();
            const auto label = [panel](const char *name) { return panel->findChild<QLabel *>(QString("research_") + name)->text(); };
            QTRY_VERIFY_WITH_TIMEOUT((panel->refreshNow(), label("state") == "unavailable"), 30000);
            QVERIFY(label("reason").contains("unavailable"));
            QVERIFY(label("host").startsWith("running"));
            // The browser itself is unaffected.
            QTRY_COMPARE_WITH_TIMEOUT(window.tabManager()->currentView()->title(), QString("First page"), 15000);
            panel->show();
            QTRY_VERIFY(panel->isVisible());
            panel->hide();
        }  // Shutdown with an unavailable research host.
        if (previous.isEmpty()) qunsetenv("AAFR_XGBOOST_LIBRARY");
        else qputenv("AAFR_XGBOOST_LIBRARY", previous);
    }

    // ---- V0.8 Phase 5B.3: research records and demonstration readiness --

    void researchRecordsSurviveRestart()
    {
        using aafr::runtime::RuntimeLifecycle;
        QString storedJson;
        {
            BrowserWindow window(nullptr, first_);
            window.show();
            auto *panel = window.devToolsPanel();
            auto *runtime = window.adaptiveRuntime();
            const auto label = [panel](const char *name) { return panel->findChild<QLabel *>(QString("research_") + name)->text(); };
            QTRY_VERIFY_WITH_TIMEOUT((panel->refreshNow(), label("state") == "idle"), 30000);
            QVERIFY(label("store").startsWith("0 records kept (0 from earlier sessions, at most 200)"));
            QVERIFY(label("store").contains(BrowserWindow::researchRecordsPath()));
            QVERIFY(label("measured").startsWith("none: no research input in this build is measured live telemetry"));
            QCOMPARE(panel->findChild<QPlainTextEdit *>("researchRecordJson")->toPlainText(), QString("No research record yet."));
            window.startForwarding();
            QTRY_VERIFY_WITH_TIMEOUT(runtime->Status().lifecycle == RuntimeLifecycle::Ready, 90000);
            QTRY_VERIFY_WITH_TIMEOUT((panel->refreshNow(), panel->findChild<QLabel *>("adaptive_lifecycle")->text().startsWith("ready")),
                                     10000);
            const auto before = runtime->Status();
            panel->findChild<QPushButton *>("adaptiveEvaluate")->click();
            QTRY_VERIFY_WITH_TIMEOUT((panel->refreshNow(), label("latest").startsWith("#1")), 30000);
            QVERIFY(label("store").startsWith("1 records kept (0 from earlier sessions, at most 200)"));
            auto *recommendations = panel->findChild<QTableWidget *>("researchRecommendations");
            QCOMPARE(recommendations->rowCount(), 1);
            QCOMPARE(recommendations->item(0, 8)->text(), QString("this session"));
            storedJson = panel->findChild<QPlainTextEdit *>("researchRecordJson")->toPlainText();
            QVERIFY(storedJson.startsWith("{\"schema\":\"aafr.research.decision_record.v1\""));
            QVERIFY(storedJson.contains("\"origin\":\"controlled\""));
            // The stored line embeds that exact record.
            QFile file(BrowserWindow::researchRecordsPath());
            QVERIFY(file.open(QIODevice::ReadOnly));
            const QByteArray content = file.readAll();
            QCOMPARE(content.count('\n'), 1);
            QVERIFY(content.contains(storedJson.toUtf8()));
            QVERIFY(content.startsWith("{\"schema\":\"aafr.research.record_store.v1\""));
            // Storing changed nothing in forwarding.
            const auto after = runtime->Status();
            QVERIFY(after.active && before.active && after.active->config_version == before.active->config_version);
            QCOMPARE(after.active->path, before.active->path);
        }  // Browser shutdown.

        // A restarted browser shows the record of the earlier session as stored.
        BrowserWindow window(nullptr, first_);
        window.show();
        auto *panel = window.devToolsPanel();
        const auto label = [panel](const char *name) { return panel->findChild<QLabel *>(QString("research_") + name)->text(); };
        QTRY_VERIFY_WITH_TIMEOUT((panel->refreshNow(), label("state") == "idle"), 30000);
        QVERIFY(label("store").startsWith("1 records kept (1 from earlier sessions, at most 200), 1 read at start, 0 unreadable"));
        QVERIFY(label("latest").startsWith("none yet"));  // Nothing evaluated in this session.
        auto *recommendations = panel->findChild<QTableWidget *>("researchRecommendations");
        QCOMPARE(recommendations->rowCount(), 1);
        QVERIFY(recommendations->item(0, 8)->text().startsWith("earlier: "));
        QCOMPARE(recommendations->item(0, 1)->text(), QString("keep"));
        QCOMPARE(recommendations->item(0, 2)->text(), QString("no (decision only)"));
        QCOMPARE(recommendations->item(0, 5)->text(), QString("controlled"));
        QCOMPARE(recommendations->item(0, 6)->text(), QString("none"));
        QCOMPARE(panel->findChild<QPlainTextEdit *>("researchRecordJson")->toPlainText(), storedJson);
        const QString directory = qEnvironmentVariable("AAFR_UI_CAPTURE_DIR");
        if (!directory.isEmpty()) {
            QVERIFY(QDir().mkpath(directory));
            panel->show();
            window.resize(1400, 900);
            window.resizeDocks({panel}, {560}, Qt::Vertical);
            panel->tabs()->setCurrentIndex(2);
            auto *diagnostics = panel->findChild<QTabWidget *>("runtimeDiagnostics");
            diagnostics->setCurrentIndex(diagnostics->count() - 1);
            panel->findChild<QToolButton *>("capabilityResearchToggle")->setChecked(true);
            panel->refreshNow();
            auto *scroll = panel->findChild<QScrollArea *>("researchScroll");
            scroll->ensureWidgetVisible(recommendations);
            QTest::qWait(500);
            QVERIFY(window.grab().save(directory + "/devtools-research-records-after-restart.png"));
        }
    }

    void researchDevToolsOpenCloseRepeatedly()
    {
        auto *window = new BrowserWindow(nullptr, first_);
        QPointer<DevToolsPanel> panel = window->devToolsPanel();
        window->show();
        const auto label = [&panel](const char *name) { return panel->findChild<QLabel *>(QString("research_") + name)->text(); };
        QTRY_COMPARE_WITH_TIMEOUT(window->tabManager()->currentView()->title(), QString("First page"), 15000);
        auto *diagnostics = panel->findChild<QTabWidget *>("runtimeDiagnostics");
        for (int i = 0; i < 12; ++i) {
            panel->show();
            QTRY_VERIFY(panel->isVisible());
            panel->tabs()->setCurrentIndex(2);
            panel->findChild<QToolButton *>("runtimeAdvancedToggle")->setChecked(true);
            diagnostics->setCurrentIndex(diagnostics->count() - 1);
            panel->findChild<QToolButton *>("capabilityResearchToggle")->setChecked(true);
            panel->refreshNow();
            QCOMPARE(panel->findChild<QLabel *>("researchBanner")->text(), DevToolsPanel::researchDecisionOnlyText());
            QVERIFY(label("actuation").startsWith("disabled: decision only"));
            panel->tabs()->setCurrentIndex(i % panel->tabs()->count());
            panel->hide();
            QTRY_VERIFY(!panel->isVisible());
            // The page keeps working with the panel open and closed.
            window->tabManager()->currentView()->setUrl(i % 2 ? first_ : second_);
            QTRY_COMPARE_WITH_TIMEOUT(window->tabManager()->currentView()->title(),
                                      QString(i % 2 ? "First page" : "Second page"), 15000);
        }
        QTRY_VERIFY_WITH_TIMEOUT((panel->refreshNow(), label("state") == "idle"), 30000);
        QCOMPARE(panel->findChild<QTableWidget *>("researchRecommendations")->rowCount(), 0);
        panel->show();
        delete window;  // Shutdown with the panel open.
        QVERIFY(panel.isNull());
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }

    void captureDevToolsScreenshots()
    {
        // Opt-in rendering artifacts, as in the V0.2 suite.
        const QString directory = qEnvironmentVariable("AAFR_UI_CAPTURE_DIR");
        if (directory.isEmpty()) QSKIP("Set AAFR_UI_CAPTURE_DIR to save Developer Tools captures.");
        QVERIFY(QDir().mkpath(directory));
        BrowserWindow window(nullptr, first_);
        window.setAttribute(Qt::WA_DontShowOnScreen);
        window.resize(1280, 800);
        window.show();
        QTRY_COMPARE_WITH_TIMEOUT(window.tabManager()->currentView()->title(), QString("First page"), 15000);
        publishCaptureSample(window.telemetry());
        auto *panel = window.devToolsPanel();
        panel->show();
        window.resizeDocks({panel}, {300}, Qt::Vertical);
        const QStringList names = {"overview", "network", "runtime", "decisions", "events"};
        for (int i = 0; i < names.size(); ++i) {
            panel->tabs()->setCurrentIndex(i);
            panel->refreshNow();
            QTest::qWait(400);
            QVERIFY(window.grab().save(directory + QString("/devtools-bottom-%1.png").arg(names[i])));
        }
        // V0.4 inner tabs.
        panel->findChild<QToolButton *>("networkAdvancedToggle")->setChecked(true);
        panel->findChild<QToolButton *>("runtimeAdvancedToggle")->setChecked(true);
        window.networkStateService()->ProcessOnce();
        panel->tabs()->setCurrentIndex(1);
        panel->findChild<QTabWidget *>("networkViews")->setCurrentIndex(1);
        panel->refreshNow();
        QTest::qWait(400);
        QVERIFY(window.grab().save(directory + "/devtools-bottom-network-state.png"));
        panel->findChild<QTabWidget *>("networkViews")->setCurrentIndex(0);
        panel->tabs()->setCurrentIndex(2);
        auto *diagnostics = panel->findChild<QTabWidget *>("runtimeDiagnostics");
        const QStringList inner = {"eligibility", "capabilities", "constraints"};
        for (int i = 0; i < inner.size(); ++i) {
            diagnostics->setCurrentIndex(3);
            panel->findChild<QToolButton *>("capabilityConstraintsToggle")->setChecked(i == 2);
            panel->findChild<QToolButton *>("capabilityEligibilityToggle")->setChecked(i == 0);
            QTest::qWait(300);
            QVERIFY(window.grab().save(directory + QString("/devtools-bottom-runtime-%1.png").arg(inner[i])));
        }
        diagnostics->setCurrentIndex(0);
        panel->findChild<QToolButton *>("devToolsDockSide")->click();
        window.resizeDocks({panel}, {460}, Qt::Horizontal);
        panel->tabs()->setCurrentIndex(1);
        QTest::qWait(400);
        QVERIFY(window.grab().save(directory + "/devtools-right-network.png"));
        panel->hide();
        QTest::qWait(200);
        QVERIFY(window.grab().save(directory + "/devtools-hidden.png"));
    }
};

QTEST_MAIN(BrowserObservabilityTest)
#include "browser_observability_test.moc"
