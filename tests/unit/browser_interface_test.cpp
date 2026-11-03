#include <QAction>
#include <QApplication>
#include <QFile>
#include <QLineEdit>
#include <QProgressBar>
#include <QPointer>
#include <QSignalSpy>
#include <QStatusBar>
#include <QTabBar>
#include <QTemporaryDir>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTest>
#include <QWebEnginePage>
#include <QWebEngineSettings>
#include <QWebEngineView>

#include "browser_window.h"
#include "tab_manager.h"

class BrowserInterfaceTest : public QObject
{
    Q_OBJECT
private:
    QTemporaryDir pages_;
    QUrl first_;
    QUrl second_;

private slots:
    void initTestCase()
    {
        QVERIFY(pages_.isValid());
        auto writePage = [this](const QString &name, const QByteArray &html) {
            const QString path = pages_.filePath(name);
            QFile file(path);
            if (!file.open(QIODevice::WriteOnly) || file.write(html) != html.size()) {
                qWarning() << "Cannot create test page" << path << file.errorString();
                return QUrl();
            }
            return QUrl::fromLocalFile(path);
        };
        first_ = writePage("first.html", "<title>First page</title><a href='second.html' target='_blank'>Open second</a>");
        second_ = writePage("second.html", "<title>Second page</title><p>Second</p>");
        QVERIFY(!first_.isEmpty());
        QVERIFY(!second_.isEmpty());
    }

    void lifecycleAndTabIsolation()
    {
        TabManager tabs;
        QSignalSpy created(&tabs, &TabManager::webViewCreated);
        QSignalSpy started(&tabs, &TabManager::navigationStarted);
        QSignalSpy finished(&tabs, &TabManager::navigationFinished);
        auto *first = tabs.addTab(first_);
        QTRY_COMPARE_WITH_TIMEOUT(first->title(), QString("First page"), 15000);
        QTRY_VERIFY(!tabs.currentPageState().loading);
        QCOMPARE(created.count(), 1);
        QVERIFY(started.count() >= 1);
        QVERIFY(finished.count() >= 1);
        QVERIFY(finished.last().at(2).toBool());
        QCOMPARE(tabs.currentPageState().progress, 100);
        QVERIFY(tabs.currentPageState().error.isEmpty());

        auto *second = tabs.addTab(second_, false);
        QTRY_COMPARE_WITH_TIMEOUT(second->title(), QString("Second page"), 15000);
        QCOMPARE(tabs.currentView(), first);
        QTRY_COMPARE(tabs.tabText(0), QString("First page"));
        QTRY_COMPARE(tabs.tabText(1), QString("Second page"));
        tabs.tabBar()->moveTab(1, 0);
        QCOMPARE(tabs.currentView(), first);
        QCOMPARE(tabs.tabText(0), QString("Second page"));
        tabs.closeTab(0);
        QCOMPARE(tabs.currentView(), first);
    }

    void closeTabsAndKeepBlankTab()
    {
        TabManager tabs;
        auto *first = tabs.addTab();
        auto *second = tabs.addTab();
        QSignalSpy destroyed(first, &QObject::destroyed);
        tabs.closeTab(0);
        QCOMPARE(tabs.count(), 1);
        QCOMPARE(tabs.currentView(), second);
        QTRY_COMPARE(destroyed.count(), 1);
        tabs.closeCurrentTab();
        QCOMPARE(tabs.count(), 1);
        QVERIFY(tabs.currentView() != nullptr);
        QVERIFY(!tabs.currentPageState().loading);
        tabs.closeTab(-1);
        tabs.closeTab(99);
        QCOMPARE(tabs.count(), 1);
    }

    void failedNavigationAndRecovery()
    {
        TabManager tabs;
        QSignalSpy finished(&tabs, &TabManager::navigationFinished);
        auto *view = tabs.addTab(QUrl::fromLocalFile(pages_.filePath("missing.html")));
        QTRY_VERIFY_WITH_TIMEOUT(!tabs.currentPageState().error.isEmpty(), 15000);
        QVERIFY(!tabs.currentPageState().loading);
        QVERIFY(!finished.last().at(2).toBool());
        view->load(second_);
        QTRY_COMPARE_WITH_TIMEOUT(view->title(), QString("Second page"), 15000);
        QTRY_VERIFY(!tabs.currentPageState().loading);
        QVERIFY(tabs.currentPageState().error.isEmpty());
    }

    void webLinksOpenTabs()
    {
        TabManager tabs;
        auto *view = tabs.addTab(first_);
        QTRY_COMPARE_WITH_TIMEOUT(view->title(), QString("First page"), 15000);
        view->page()->runJavaScript("document.querySelector('a').click()");
        QTRY_COMPARE_WITH_TIMEOUT(tabs.count(), 2, 15000);
        QTRY_COMPARE_WITH_TIMEOUT(tabs.currentView()->title(), QString("Second page"), 15000);
    }

    void windowNavigationAndShortcuts()
    {
        BrowserWindow window(nullptr, QUrl("about:blank"));
        window.show();
        auto *tabs = window.tabManager();
        tabs->currentView()->stop();
        auto *address = window.findChild<QLineEdit *>("addressBar");
        QVERIFY(address);
        address->setFocus();
        address->setText(first_.toString());
        QTest::keyClick(address, Qt::Key_Return);
        QTRY_COMPARE_WITH_TIMEOUT(tabs->currentView()->title(), QString("First page"), 15000);
        QTRY_VERIFY(!tabs->currentPageState().loading);
        QVERIFY(window.windowTitle().contains("First page"));
        QCOMPARE(QUrl(address->text()), first_);

        tabs->currentView()->load(second_);
        QTRY_COMPARE_WITH_TIMEOUT(tabs->currentView()->title(), QString("Second page"), 15000);
        auto *back = window.findChild<QAction *>("backAction");
        auto *forward = window.findChild<QAction *>("forwardAction");
        QVERIFY(back);
        QVERIFY(forward);
        QTRY_VERIFY(back->isEnabled());
        back->trigger();
        QTRY_COMPARE_WITH_TIMEOUT(tabs->currentView()->title(), QString("First page"), 15000);
        QTRY_VERIFY(forward->isEnabled());
        forward->trigger();
        QTRY_COMPARE_WITH_TIMEOUT(tabs->currentView()->title(), QString("Second page"), 15000);

        // A background title/load change must preserve an address being edited.
        address->setFocus();
        address->setText("http://localhost/draft");
        auto *background = tabs->addTab(first_, false);
        QTRY_COMPARE_WITH_TIMEOUT(background->title(), QString("First page"), 15000);
        QCOMPARE(address->text(), QString("http://localhost/draft"));
        tabs->setCurrentWidget(background);
        QCOMPARE(QUrl(address->text()), first_);
        QTest::keyClick(&window, Qt::Key_T, Qt::ControlModifier);
        QTRY_COMPARE(tabs->count(), 3);
        QTest::keyClick(&window, Qt::Key_W, Qt::ControlModifier);
        QTRY_COMPARE(tabs->count(), 2);
        auto *previous = tabs->currentView();
        QTest::keyClick(&window, Qt::Key_Tab, Qt::ControlModifier);
        QTRY_VERIFY(tabs->currentView() != previous);
        QTest::keyClick(&window, Qt::Key_Tab, Qt::ControlModifier | Qt::ShiftModifier);
        QTRY_COMPARE(tabs->currentView(), previous);
    }

    void addressNormalization_data()
    {
        QTest::addColumn<QString>("input");
        QTest::addColumn<QUrl>("expected");
        QTest::newRow("https") << "https://example.com/path?q=1" << QUrl("https://example.com/path?q=1");
        QTest::newRow("http") << "http://example.com/path" << QUrl("http://example.com/path");
        QTest::newRow("hostname") << "example.com" << QUrl("https://example.com");
        QTest::newRow("hostname-path") << " example.com/path " << QUrl("https://example.com/path");
        QTest::newRow("localhost-port") << "localhost:8080/test" << QUrl("https://localhost:8080/test");
        QTest::newRow("query") << "qt browser tests" << QUrl("https://search.test/?q=qt%20browser%20tests");
        QTest::newRow("single-word") << "browser" << QUrl("https://search.test/?q=browser");
        QTest::newRow("query-encoding") << "cats & dogs + birds" << QUrl("https://search.test/?q=cats%20%26%20dogs%20%2B%20birds");
        QTest::newRow("unicode-query") << QString::fromUtf8("café browser") << QUrl("https://search.test/?q=caf%C3%A9%20browser");
        QTest::newRow("local-file") << first_.toString() << first_;
        QTest::newRow("blank") << "about:blank" << QUrl("about:blank");
        QTest::newRow("empty") << "  " << QUrl();
        QTest::newRow("invalid-host") << "https://[broken" << QUrl();
        QTest::newRow("missing-host") << "https://" << QUrl();
    }

    void addressNormalization()
    {
        QFETCH(QString, input);
        QFETCH(QUrl, expected);
        QCOMPARE(BrowserWindow::urlFromInput(input, "https://search.test/?q=%1"), expected);
    }

    void configuredSearchAndInvalidInput()
    {
        QVERIFY(BrowserWindow::urlFromInput("query words", "https://search.test/").isEmpty());
        QVERIFY(BrowserWindow::urlFromInput("query words", "file:///tmp/%1").isEmpty());
        // Exercise search submission using a local HTTP endpoint.
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        const QString endpoint = QString("http://127.0.0.1:%1/search?q=%2")
                                     .arg(server.serverPort()).arg("%1");
        connect(&server, &QTcpServer::newConnection, &server, [&server] {
            auto *socket = server.nextPendingConnection();
            connect(socket, &QTcpSocket::readyRead, socket, [socket] {
                socket->readAll();
                socket->write("HTTP/1.1 200 OK\r\nContent-Length: 29\r\nConnection: close\r\n\r\n<title>Search results</title>");
                socket->disconnectFromHost();
            });
            connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
        });
        BrowserWindow window(nullptr, QUrl("about:blank"), endpoint);
        window.show();
        auto *address = window.findChild<QLineEdit *>("addressBar");
        address->setText("test & query");
        QTest::keyClick(address, Qt::Key_Return);
        QTRY_COMPARE_WITH_TIMEOUT(window.tabManager()->currentView()->title(), QString("Search results"), 15000);
        QCOMPARE(window.tabManager()->currentView()->url(),
                 BrowserWindow::urlFromInput("test & query", endpoint));
        const QUrl previous = window.tabManager()->currentView()->url();
        address->setText("https://[broken");
        QTest::keyClick(address, Qt::Key_Return);
        QCOMPARE(window.tabManager()->currentView()->url(), previous);
        QVERIFY(window.statusBar()->currentMessage().contains("valid URL"));
    }

    void windowStateAndLoadingControls()
    {
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        // Keep connections open without responding until the user stops or closes the tab.
        const QUrl waiting(QString("http://127.0.0.1:%1/wait").arg(server.serverPort()));
        BrowserWindow window(nullptr, first_);
        window.show();
        auto *tabs = window.tabManager();
        QCOMPARE(tabs->count(), 1);
        auto *first = tabs->currentView();
        QTRY_COMPARE_WITH_TIMEOUT(first->title(), QString("First page"), 15000);
        QTRY_VERIFY(!tabs->currentPageState().loading);
        auto *address = window.findChild<QLineEdit *>("addressBar");
        auto *reload = window.findChild<QAction *>("reloadAction");
        auto *stop = window.findChild<QAction *>("stopAction");
        auto *progress = window.findChild<QProgressBar *>("loadProgress");
        QVERIFY(reload && stop && progress);
        QVERIFY(reload->isEnabled());
        QVERIFY(!stop->isEnabled());
        QVERIFY(progress->isHidden());
        QSignalSpy started(tabs, &TabManager::navigationStarted);
        auto *background = tabs->addTab(waiting, false);
        QTRY_VERIFY_WITH_TIMEOUT(started.count() > 0, 15000);
        QCOMPARE(QUrl(address->text()), first_);
        QVERIFY(window.windowTitle().contains("First page"));
        QVERIFY(!stop->isEnabled());
        tabs->setCurrentWidget(background);
        QVERIFY(stop->isEnabled());
        QVERIFY(!progress->isHidden());
        QCOMPARE(progress->value(), tabs->currentPageState().progress);
        QVERIFY(!window.findChild<QAction *>("backAction")->isEnabled());
        stop->trigger();
        QTRY_VERIFY(!tabs->currentPageState().loading);
        QVERIFY(!stop->isEnabled());
        QVERIFY(tabs->currentPageState().error.isEmpty());
        background->load(waiting);
        QTRY_VERIFY(tabs->currentPageState().loading);
        QPointer<QWebEngineView> closed(background);
        tabs->closeCurrentTab();
        QTRY_VERIFY(closed.isNull());
        QCOMPARE(tabs->currentView(), first);
        QCOMPARE(QUrl(address->text()), first_);
        first->load(second_);
        QTRY_COMPARE_WITH_TIMEOUT(first->title(), QString("Second page"), 15000);
        window.findChild<QAction *>("homeAction")->trigger();
        QTRY_COMPARE_WITH_TIMEOUT(first->title(), QString("First page"), 15000);
        reload->trigger();
        QTRY_VERIFY(!tabs->currentPageState().loading);
    }

    void navigationWithoutActiveTab()
    {
        BrowserWindow window(nullptr, QUrl("about:blank"));
        auto *tabs = window.tabManager();
        // QTabWidget's inherited removal API can temporarily leave no active view.
        tabs->removeTab(0);
        QVERIFY(!tabs->currentView());
        for (const auto *name : {"backAction", "forwardAction", "reloadAction", "stopAction"})
            QVERIFY(!window.findChild<QAction *>(name)->isEnabled());
        window.findChild<QAction *>("homeAction")->trigger();
        window.findChild<QAction *>("closeTabAction")->trigger();
        auto *address = window.findChild<QLineEdit *>("addressBar");
        address->setText(first_.toString());
        QTest::keyClick(address, Qt::Key_Return);
        QCOMPARE(tabs->count(), 0);
        window.findChild<QAction *>("newTabAction")->trigger();
        QCOMPARE(tabs->count(), 1);
        QVERIFY(tabs->currentView());
    }

    void repeatedClosureAndExternalDestruction()
    {
        TabManager tabs;
        QVERIFY(!tabs.currentView());
        QVERIFY(!tabs.currentPageState().loading);
        tabs.closeCurrentTab();
        tabs.addTab();
        for (int i = 0; i < 20; ++i) {
            QPointer<QWebEngineView> old(tabs.currentView());
            tabs.closeCurrentTab();
            QCOMPARE(tabs.count(), 1);
            QTRY_VERIFY(old.isNull());
        }
        QPointer<QWebEngineView> external(tabs.currentView());
        external->deleteLater();
        QTRY_VERIFY(external.isNull());
        QTRY_COMPARE(tabs.count(), 1);
        QVERIFY(tabs.currentView());
        QVERIFY(tabs.currentPageState().error.isEmpty());
    }

    // Diagnostic control: intentionally uses no AAFR window, manager, or adapters.
    void plainWebEngineLifecycle_data()
    {
        QTest::addColumn<bool>("missing");
        QTest::addColumn<bool>("errorPageEnabled");
        QTest::newRow("loaded") << false << true;
        QTest::newRow("missing") << true << true;
        QTest::newRow("missing-without-error-page") << true << false;
    }

    void plainWebEngineLifecycle()
    {
        QFETCH(bool, missing);
        QFETCH(bool, errorPageEnabled);
        QPointer<QWebEngineView> view = new QWebEngineView;
        // Control only: isolate diagnostics from Qt's built-in error document.
        // The application and all other tests retain the default error page.
        view->settings()->setAttribute(QWebEngineSettings::ErrorPageEnabled, errorPageEnabled);
        QPointer<QWebEnginePage> page = view->page();
        QPointer<QAction> action = view->pageAction(QWebEnginePage::Back);
        QCOMPARE(page->parent(), view.data());
        QSignalSpy finished(view, &QWebEngineView::loadFinished);
        view->load(missing ? QUrl::fromLocalFile(pages_.filePath("missing-control.html")) : first_);
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 15000);
        QCOMPARE(finished.first().first().toBool(), !missing);
        view->deleteLater();
        QTRY_VERIFY(view.isNull());
        QVERIFY(page.isNull());
        QVERIFY(action.isNull());
    }

    void managerOwnsViewsPagesAndPendingDeletes()
    {
        auto *tabs = new TabManager;
        QPointer<QWebEngineView> closed = tabs->addTab(first_);
        QPointer<QWebEngineView> removed = tabs->addTab(second_);
        QPointer<QWebEngineView> active = tabs->addTab(first_);
        QTRY_COMPARE_WITH_TIMEOUT(closed->title(), QString("First page"), 15000);
        QTRY_COMPARE_WITH_TIMEOUT(removed->title(), QString("Second page"), 15000);
        QTRY_COMPARE_WITH_TIMEOUT(active->title(), QString("First page"), 15000);
        QPointer<QWebEnginePage> closedPage = closed->page();
        QPointer<QWebEnginePage> removedPage = removed->page();
        QPointer<QWebEnginePage> activePage = active->page();
        QPointer<QAction> action = active->pageAction(QWebEnginePage::Back);
        QVERIFY(tabs->isAncestorOf(closed));
        QVERIFY(tabs->isAncestorOf(removed));
        QCOMPARE(activePage->parent(), active.data());
        tabs->removeTab(tabs->indexOf(removed));
        QVERIFY(tabs->isAncestorOf(removed)); // removeTab does not transfer ownership.
        tabs->closeTab(tabs->indexOf(closed));
        QVERIFY(closed); // deleteLater has not run yet.
        QSignalSpy closedDestroyed(closed, &QObject::destroyed);
        QSignalSpy removedDestroyed(removed, &QObject::destroyed);
        QSignalSpy activeDestroyed(active, &QObject::destroyed);
        delete tabs;
        QVERIFY(closed.isNull() && removed.isNull() && active.isNull());
        QVERIFY(closedPage.isNull() && removedPage.isNull() && activePage.isNull());
        QVERIFY(action.isNull());
        QCOMPARE(closedDestroyed.count(), 1);
        QCOMPARE(removedDestroyed.count(), 1);
        QCOMPARE(activeDestroyed.count(), 1);
        // A queued DeferredDelete must not cause a second destruction.
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QCOMPARE(closedDestroyed.count(), 1);
    }

    void managerDestructionDuringLoading()
    {
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        auto *tabs = new TabManager;
        QSignalSpy started(tabs, &TabManager::navigationStarted);
        const QUrl pending(QString("http://127.0.0.1:%1/pending").arg(server.serverPort()));
        QPointer<QWebEngineView> first = tabs->addTab(pending);
        QPointer<QWebEngineView> second = tabs->addTab(pending);
        QPointer<QWebEnginePage> firstPage = first->page();
        QPointer<QWebEnginePage> secondPage = second->page();
        QTRY_COMPARE_WITH_TIMEOUT(started.count(), 2, 15000);
        QVERIFY(tabs->currentPageState().loading);
        delete tabs;
        QVERIFY(first.isNull() && second.isNull());
        QVERIFY(firstPage.isNull() && secondPage.isNull());
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }

    void closedViewSignalsAreDisconnected()
    {
        TabManager tabs;
        auto *closed = tabs.addTab(first_);
        QTRY_COMPARE_WITH_TIMEOUT(closed->title(), QString("First page"), 15000);
        tabs.addTab();
        tabs.closeTab(0);
        QPointer<QWebEngineView> guard(closed);
        QSignalSpy state(&tabs, &TabManager::currentStateChanged);
        QSignalSpy finished(&tabs, &TabManager::navigationFinished);
        // The closed view remains alive until DeferredDelete, but cannot affect the manager.
        closed->loadProgress(47);
        closed->titleChanged("Stale title");
        closed->urlChanged(second_);
        closed->renderProcessTerminated(QWebEnginePage::CrashedTerminationStatus, 1);
        closed->pageAction(QWebEnginePage::Back)->setEnabled(true);
        QCOMPARE(state.count(), 0);
        QCOMPARE(finished.count(), 0);
        QCOMPARE(tabs.currentPageState().progress, 0);
        QVERIFY(tabs.currentPageState().error.isEmpty());
        QTRY_VERIFY(guard.isNull());
    }

    void externalDestructionBeforeManagerShutdown()
    {
        auto *tabs = new TabManager;
        QPointer<QWebEngineView> foreground = tabs->addTab();
        QPointer<QWebEngineView> background = tabs->addTab(QUrl(), false);
        delete background.data();
        QCOMPARE(tabs->currentView(), foreground.data());
        QCOMPARE(tabs->count(), 1);
        delete foreground.data();
        QCOMPARE(tabs->count(), 0);
        // Replacement callbacks are pending: destroying the receiver must cancel them.
        QSignalSpy created(tabs, &TabManager::webViewCreated);
        delete tabs;
        QCoreApplication::processEvents();
        QCOMPARE(created.count(), 0);
    }
};

QTEST_MAIN(BrowserInterfaceTest)
#include "browser_interface_test.moc"
