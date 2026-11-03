#include "tab_manager.h"

#include <QAction>
#include <QIcon>
#include <QTabBar>
#include <QTimer>
#include <QWebEngineLoadingInfo>
#include <QWebEnginePage>
#include <QWebEngineView>
#include <functional>
#include <utility>

namespace {
// Keep WebEngine-specific new-window handling out of the window's UI code.
class BrowserView final : public QWebEngineView
{
public:
    using NewTab = std::function<QWebEngineView *(bool)>;
    BrowserView(NewTab newTab, QWidget *parent)
        : QWebEngineView(parent), newTab_(std::move(newTab)) {}

protected:
    QWebEngineView *createWindow(QWebEnginePage::WebWindowType type) override
    {
        return newTab_(type != QWebEnginePage::WebBrowserBackgroundTab);
    }

private:
    NewTab newTab_;
};
}

TabManager::TabManager(QWidget *parent) : QTabWidget(parent)
{
    qRegisterMetaType<QWebEngineView *>();
    setObjectName("browserTabs");
    setTabsClosable(true);
    setMovable(true);
    setDocumentMode(true);
    tabBar()->setExpanding(false);
    connect(this, &QTabWidget::tabCloseRequested, this, &TabManager::closeTab);
    connect(this, &QTabWidget::currentChanged, this, [this] {
        emit currentStateChanged();
    });
}

TabManager::~TabManager()
{
    // Children are deleted by QWidget after our state hash has been destroyed.
    for (auto *view : states_.keys()) {
        disconnect(view, nullptr, this, nullptr);
        disconnect(view->page(), nullptr, this, nullptr);
        for (auto action : {QWebEnginePage::Back, QWebEnginePage::Forward})
            disconnect(view->pageAction(action), nullptr, this, nullptr);
    }
}

QWebEngineView *TabManager::addTab(const QUrl &url, bool foreground)
{
    auto *view = new BrowserView([this](bool activate) {
        return addTab(QUrl(), activate);
    }, this);
    states_.insert(view, {});
    connect(view, &QObject::destroyed, this, [this, view] {
        states_.remove(view);
        const int index = indexOf(view);
        if (index >= 0) removeTab(index);
        // Let QTabWidget finish removing the destroyed widget first.
        QTimer::singleShot(0, this, [this] {
            if (count() == 0) addTab();
            emit currentStateChanged();
        });
    });

    connect(view, &QWebEngineView::titleChanged, this, [this, view](const QString &title) {
        const int index = indexOf(view);
        if (index < 0) return;
        setTabText(index, title.isEmpty() ? tr("New Tab") : title);
        setTabToolTip(index, view->url().toDisplayString());
        notifyIfCurrent(view);
    });
    connect(view, &QWebEngineView::iconChanged, this, [this, view](const QIcon &icon) {
        const int index = indexOf(view);
        if (index >= 0) setTabIcon(index, icon);
    });
    connect(view, &QWebEngineView::urlChanged, this, [this, view](const QUrl &url) {
        const int index = indexOf(view);
        if (index >= 0) setTabToolTip(index, url.toDisplayString());
        notifyIfCurrent(view);
    });
    connect(view, &QWebEngineView::loadProgress, this, [this, view](int progress) {
        states_[view].progress = progress;
        notifyIfCurrent(view);
    });
    connect(view->page(), &QWebEnginePage::loadingChanged, this,
            [this, view](const QWebEngineLoadingInfo &info) {
        auto &state = states_[view];
        switch (info.status()) {
        case QWebEngineLoadingInfo::LoadStartedStatus:
            state = {true, 0, {}};
            emit navigationStarted(view, info.url());
            break;
        case QWebEngineLoadingInfo::LoadSucceededStatus:
            state = {false, 100, {}};
            emit navigationFinished(view, info.url(), true, {});
            break;
        case QWebEngineLoadingInfo::LoadStoppedStatus:
            state.loading = false;
            state.error.clear();
            emit navigationFinished(view, info.url(), false, tr("Navigation stopped"));
            break;
        case QWebEngineLoadingInfo::LoadFailedStatus:
            state.loading = false;
            state.error = tr("Could not load %1: %2")
                              .arg(info.url().toDisplayString(), info.errorString());
            emit navigationFinished(view, info.url(), false, state.error);
            break;
        }
        notifyIfCurrent(view);
    });
    connect(view, &QWebEngineView::renderProcessTerminated, this,
            [this, view](QWebEnginePage::RenderProcessTerminationStatus, int) {
        auto &state = states_[view];
        state.loading = false;
        state.error = tr("The page renderer stopped. Press Refresh to try again.");
        emit navigationFinished(view, view->url(), false, state.error);
        notifyIfCurrent(view);
    });
    connect(view->page(), &QWebEnginePage::windowCloseRequested, this, [this, view] {
        closeTab(indexOf(view));
    });
    for (auto action : {QWebEnginePage::Back, QWebEnginePage::Forward}) {
        connect(view->pageAction(action), &QAction::changed, this, [this, view] {
            notifyIfCurrent(view);
        });
    }

    const int index = QTabWidget::addTab(view, tr("New Tab"));
    if (foreground) setCurrentIndex(index);
    emit webViewCreated(view);
    if (!url.isEmpty()) view->load(url);
    return view;
}

QWebEngineView *TabManager::currentView() const
{
    return qobject_cast<QWebEngineView *>(currentWidget());
}

TabManager::PageState TabManager::currentPageState() const
{
    return states_.value(currentView());
}

void TabManager::closeTab(int index)
{
    if (index < 0 || index >= count()) return;
    auto *view = qobject_cast<QWebEngineView *>(widget(index));
    // Disconnect before removal: currentChanged can synchronously update the UI.
    disconnect(view, nullptr, this, nullptr);
    disconnect(view->page(), nullptr, this, nullptr);
    for (auto action : {QWebEnginePage::Back, QWebEnginePage::Forward})
        disconnect(view->pageAction(action), nullptr, this, nullptr);
    states_.remove(view);
    removeTab(index);
    view->stop();
    view->deleteLater();
    // Keep the browser usable after closing its final tab.
    if (count() == 0) addTab();
}

void TabManager::closeCurrentTab()
{
    closeTab(currentIndex());
}

void TabManager::notifyIfCurrent(QWebEngineView *view)
{
    if (view == currentView()) emit currentStateChanged();
}
