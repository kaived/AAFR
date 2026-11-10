// V0.3 observability tests: browser event capture, TShark process handling
// (with a deterministic fake tshark), and the Developer Tools panel.

#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QPointer>
#include <QPushButton>
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
#include "tab_manager.h"
#include "network/telemetry/telemetry_pipeline.h"
#include "network/tshark/tshark_fields.h"
#include "network/tshark/tshark_monitor.h"
#include "runtime/observability/runtime_contracts.h"

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

QString fixture(const char *name)
{
    return QStringLiteral(AAFR_TEST_FIXTURES_DIR "/tshark/") + QLatin1String(name);
}

TSharkMonitor::Config fakeConfig(const QStringList &arguments)
{
    TSharkMonitor::Config config;
    config.program = QStringLiteral(AAFR_FAKE_TSHARK);
    config.argumentsOverride = arguments;
    config.flushInterval = std::chrono::milliseconds(100);
    return config;
}

// Skips honestly if Windows application control refuses the unsigned helper.
#define SKIP_IF_HELPER_BLOCKED(monitor)                                                        \
    do {                                                                                       \
        if ((monitor).state() == NetworkMonitor::State::Error                                  \
            && (monitor).statusText().contains(QLatin1String("could not be started")))          \
            QSKIP(qPrintable(QStringLiteral("fake-tshark could not start: ") + (monitor).statusText())); \
    } while (0)

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

QStringList columnTexts(QTableWidget *table, int column)
{
    QStringList texts;
    for (int row = 0; row < table->rowCount(); ++row)
        texts << (table->item(row, column) ? table->item(row, column)->text() : QString());
    return texts;
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

    void init() { QFile::remove(BrowserWindow::layoutSettingsPath()); }

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

    void tsharkFixtureCapture()
    {
        TelemetryPipeline pipeline;
        TSharkMonitor monitor(pipeline, fakeConfig({"--fixture", fixture("capture_basic.tsv")}));
        QSignalSpy states(&monitor, &NetworkMonitor::stateChanged);
        QVERIFY(monitor.start());
        QTRY_VERIFY_WITH_TIMEOUT(monitor.state() != NetworkMonitor::State::Starting, 10000);
        SKIP_IF_HELPER_BLOCKED(monitor);
        QTRY_COMPARE_WITH_TIMEOUT(monitor.state(), NetworkMonitor::State::Disabled, 10000);
        QCOMPARE(countOf(pipeline, EventType::CaptureStarted), 1);
        const auto stopped = eventsOf(pipeline, EventType::CaptureStopped);
        QCOMPARE(stopped.size(), size_t(1));
        QCOMPARE(attribute(stopped.front(), "detail"), QString("TShark exited"));

        double packets = 0;
        for (const auto &event : eventsOf(pipeline, EventType::NetworkMetric))
            if (event.metric == "net.packets") packets += *event.value;
        QCOMPARE(packets, 9.0);
        QCOMPARE(*pipeline.LatestMetric("net.capture.packets_total")->value, 9.0);
        QCOMPARE(*pipeline.LatestMetric("net.capture.bytes_total")->value, 5928.0);
        QCOMPARE(*pipeline.LatestMetric("net.tcp.ack_rtt.latest")->value, 8.0);
        const auto metric = *pipeline.LatestMetric("net.tcp.ack_rtt.latest");
        QCOMPARE(metric.source, aafr::telemetry::Source::TShark);
        QCOMPARE(metric.scope, aafr::telemetry::MeasurementScope::CaptureInterface);
        QCOMPARE(metric.attribution, aafr::telemetry::AttributionQuality::Unattributed);
        QVERIFY(!metric.tab_id.has_value());
        QVERIFY(states.count() >= 2);
    }

    void tsharkNonzeroExitReportsError()
    {
        TelemetryPipeline pipeline;
        TSharkMonitor monitor(pipeline, fakeConfig({"--stderr", "tshark: The capture session could not be initiated",
                                                    "--exit-code", "2"}));
        QVERIFY(monitor.start());
        QTRY_VERIFY_WITH_TIMEOUT(monitor.state() != NetworkMonitor::State::Starting
                                     && monitor.state() != NetworkMonitor::State::Running, 10000);
        SKIP_IF_HELPER_BLOCKED(monitor);
        QCOMPARE(monitor.state(), NetworkMonitor::State::Error);
        QVERIFY(monitor.statusText().contains("code 2"));
        QVERIFY(monitor.statusText().contains("could not be initiated"));
        const auto errors = eventsOf(pipeline, EventType::CaptureError);
        QCOMPARE(errors.size(), size_t(1));
        QCOMPARE(errors.front().availability, Availability::Error);
    }

    void tsharkStopAndSafeShutdown()
    {
        TelemetryPipeline pipeline;
        auto monitor = std::make_unique<TSharkMonitor>(
            pipeline, fakeConfig({"--fixture", fixture("capture_no_tcp.tsv"), "--hang"}));
        QVERIFY(monitor->start());
        QTRY_VERIFY_WITH_TIMEOUT(monitor->state() != NetworkMonitor::State::Starting, 10000);
        SKIP_IF_HELPER_BLOCKED(*monitor);
        QCOMPARE(monitor->state(), NetworkMonitor::State::Running);
        // Metrics flow on the interval timer while the process keeps running.
        QTRY_VERIFY_WITH_TIMEOUT(pipeline.LatestMetric("net.capture.packets_total").has_value()
                                     && *pipeline.LatestMetric("net.capture.packets_total")->value == 2.0,
                                 10000);
        double udp = 0;
        for (const auto &event : eventsOf(pipeline, EventType::NetworkMetric))
            if (event.metric == "net.udp.packets") udp += *event.value;
        QCOMPARE(udp, 1.0);  // The ARP frame is counted as a packet but not as UDP.
        QVERIFY(!pipeline.LatestMetric("net.tcp.ack_rtt.latest").has_value());

        monitor->stop();
        QTRY_COMPARE_WITH_TIMEOUT(monitor->state(), NetworkMonitor::State::Disabled, 10000);
        QCOMPARE(attribute(eventsOf(pipeline, EventType::CaptureStopped).back(), "detail"),
                 QString("stopped by user"));

        // Restart, then shut down while running: bounded and recorded.
        QVERIFY(monitor->start());
        QTRY_COMPARE_WITH_TIMEOUT(monitor->state(), NetworkMonitor::State::Running, 10000);
        QElapsedTimer timer;
        timer.start();
        monitor->shutdown();
        QVERIFY(timer.elapsed() < 3000);
        QCOMPARE(attribute(eventsOf(pipeline, EventType::CaptureStopped).back(), "detail"),
                 QString("browser shutdown"));
        QCOMPARE(countOf(pipeline, EventType::CaptureError), 0);

        // Destroying a running monitor kills the process without publishing.
        QVERIFY(monitor->start());
        QTRY_COMPARE_WITH_TIMEOUT(monitor->state(), NetworkMonitor::State::Running, 10000);
        const auto published = pipeline.Stats().published;
        timer.restart();
        monitor.reset();
        QVERIFY(timer.elapsed() < 3000);
        QCOMPARE(pipeline.Stats().published, published);
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
        // Deterministic fixture data through the same parser the monitor uses.
        aafr::tshark::IntervalAggregator aggregator;
        QFile file(fixture("capture_basic.tsv"));
        QVERIFY(file.open(QIODevice::ReadOnly));
        while (!file.atEnd()) aggregator.AddLine(file.readLine().toStdString());
        for (auto &event : aafr::tshark::ToTelemetryEvents(aggregator.Flush(), 1000.0))
            window.telemetry().Publish(event);
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
        QVERIFY(panel->findChild<QLabel *>("protocolInfo")->text().contains("0x00000001"));
        QVERIFY(panel->findChild<QLabel *>("overview_metrics")->text().contains("RTT mean"));
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

        panel->setRuntimeProvider(nullptr);
        QCOMPARE(table->rowCount(), 0);
        QCOMPARE(panel->findChild<QLabel *>("runtime_state")->text(), DevToolsPanel::notIntegratedText());
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
        aafr::tshark::IntervalAggregator aggregator;
        QFile file(fixture("capture_basic.tsv"));
        QVERIFY(file.open(QIODevice::ReadOnly));
        while (!file.atEnd()) aggregator.AddLine(file.readLine().toStdString());
        for (auto &event : aafr::tshark::ToTelemetryEvents(aggregator.Flush(), 1000.0))
            window.telemetry().Publish(event);
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
