#include "browser_window.h"
#include "tab_manager.h"

#include <QAction>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QProgressBar>
#include <QShortcut>
#include <QStatusBar>
#include <QStyle>
#include <QToolBar>
#include <QWebEnginePage>
#include <QWebEngineView>
#include <algorithm>

BrowserWindow::BrowserWindow(QWidget *parent, const QUrl &homeUrl,
                             const QString &searchUrlTemplate)
    : QMainWindow(parent), homeUrl_(homeUrl), searchUrlTemplate_(searchUrlTemplate)
{
    setWindowTitle(tr("AAFR Browser"));
    resize(1280, 800);
    tabs_ = new TabManager(this);
    setCentralWidget(tabs_);

    auto *toolbar = addToolBar(tr("Navigation"));
    toolbar->setObjectName("navigationToolbar");
    toolbar->setMovable(false);
    back_ = toolbar->addAction(style()->standardIcon(QStyle::SP_ArrowBack), tr("Back"));
    back_->setObjectName("backAction");
    back_->setShortcut(QKeySequence::Back);
    forward_ = toolbar->addAction(style()->standardIcon(QStyle::SP_ArrowForward), tr("Forward"));
    forward_->setObjectName("forwardAction");
    forward_->setShortcut(QKeySequence::Forward);
    reload_ = toolbar->addAction(style()->standardIcon(QStyle::SP_BrowserReload), tr("Refresh"));
    reload_->setObjectName("reloadAction");
    reload_->setShortcut(QKeySequence::Refresh);
    stop_ = toolbar->addAction(style()->standardIcon(QStyle::SP_BrowserStop), tr("Stop"));
    stop_->setObjectName("stopAction");
    stop_->setShortcut(QKeySequence(Qt::Key_Escape));
    auto *home = toolbar->addAction(style()->standardIcon(QStyle::SP_DirHomeIcon), tr("Home"));
    home->setObjectName("homeAction");
    home->setShortcut(QKeySequence(Qt::ALT | Qt::Key_Home));

    address_ = new QLineEdit(this);
    address_->setObjectName("addressBar");
    address_->setPlaceholderText(tr("Enter a URL or search terms"));
    address_->setAccessibleName(tr("Address"));
    address_->setClearButtonEnabled(true);
    toolbar->addWidget(address_);
    auto *add = toolbar->addAction(tr("New Tab"));
    add->setObjectName("newTabAction");
    add->setShortcut(QKeySequence::AddTab);
    auto *close = new QAction(tr("Close Tab"), this);
    close->setObjectName("closeTabAction");
    close->setShortcuts({QKeySequence(Qt::CTRL | Qt::Key_W),
                         QKeySequence(Qt::CTRL | Qt::Key_F4)});
    addAction(close);

    progress_ = new QProgressBar(this);
    progress_->setObjectName("loadProgress");
    progress_->setRange(0, 100);
    progress_->setMaximumWidth(150);
    progress_->setAccessibleName(tr("Page loading progress"));
    statusBar()->addPermanentWidget(progress_);
    integrationStatus_ = new QLabel(tr("AAFR: standalone"), this);
    integrationStatus_->setTextFormat(Qt::PlainText);
    statusBar()->addPermanentWidget(integrationStatus_);

    connect(address_, &QLineEdit::returnPressed, this, &BrowserWindow::navigate);
    connect(back_, &QAction::triggered, this, [this] {
        if (auto *view = tabs_->currentView()) view->back();
    });
    connect(forward_, &QAction::triggered, this, [this] {
        if (auto *view = tabs_->currentView()) view->forward();
    });
    connect(reload_, &QAction::triggered, this, [this] {
        if (auto *view = tabs_->currentView()) view->reload();
    });
    connect(stop_, &QAction::triggered, this, [this] {
        if (auto *view = tabs_->currentView()) view->stop();
    });
    connect(home, &QAction::triggered, this, [this] {
        if (auto *view = tabs_->currentView()) view->load(homeUrl_);
    });
    connect(add, &QAction::triggered, this, &BrowserWindow::newTab);
    connect(close, &QAction::triggered, tabs_, &TabManager::closeCurrentTab);
    connect(tabs_, &TabManager::currentStateChanged, this, [this] { refreshState(); });
    connect(tabs_, &QTabWidget::currentChanged, this, [this] { refreshState(true); });
    connect(new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_L), this),
            &QShortcut::activated, this, [this] {
        address_->setFocus();
        address_->selectAll();
    });
    connect(new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_Tab), this),
            &QShortcut::activated, this, [this] {
        if (tabs_->count() > 0)
            tabs_->setCurrentIndex((tabs_->currentIndex() + 1) % tabs_->count());
    });
    connect(new QShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Tab), this),
            &QShortcut::activated, this, [this] {
        if (tabs_->count() > 0)
            tabs_->setCurrentIndex((tabs_->currentIndex() + tabs_->count() - 1) % tabs_->count());
    });
    tabs_->addTab(homeUrl_);
    refreshState(true);
}

void BrowserWindow::newTab()
{
    tabs_->addTab();
    address_->setFocus();
}

QUrl BrowserWindow::urlFromInput(const QString &text, const QString &searchUrlTemplate)
{
    const QString input = text.trimmed();
    if (input.isEmpty()) return {};
    const QUrl explicitUrl(input);
    const bool explicitScheme = input.contains(QLatin1String("://"))
        || input.startsWith(QLatin1String("file:"), Qt::CaseInsensitive)
        || input.startsWith(QLatin1String("about:"), Qt::CaseInsensitive)
        || input.startsWith(QLatin1String("data:"), Qt::CaseInsensitive);
    QUrl url = QUrl::fromUserInput(input);
    if (!explicitScheme && !url.isLocalFile()) {
        const bool hasSpace = std::any_of(input.cbegin(), input.cend(),
                                         [](QChar ch) { return ch.isSpace(); });
        const QString host = url.host();
        if (hasSpace || (!host.contains('.') && !host.contains(':')
                         && host != QLatin1String("localhost"))) {
            if (!searchUrlTemplate.contains(QLatin1String("%1"))) return {};
            QString search = searchUrlTemplate;
            search.replace(QLatin1String("%1"), QString::fromLatin1(QUrl::toPercentEncoding(input)));
            url = QUrl(search, QUrl::StrictMode);
            if (url.scheme() != QLatin1String("http") && url.scheme() != QLatin1String("https"))
                return {};
        } else if (url.scheme() == QLatin1String("http")) {
            url.setScheme(QStringLiteral("https"));
        }
    } else if (explicitScheme) {
        url = explicitUrl;
    }
    if (!url.isValid() || url.scheme().isEmpty()) return {};
    if ((url.scheme() == QLatin1String("http") || url.scheme() == QLatin1String("https"))
        && url.host().isEmpty()) return {};
    return url;
}

void BrowserWindow::navigate()
{
    const QString input = address_->text().trimmed();
    if (input.isEmpty()) return;
    const QUrl url = urlFromInput(input, searchUrlTemplate_);
    if (!url.isValid() || url.scheme().isEmpty()) {
        statusBar()->showMessage(tr("Enter a valid URL."));
        return;
    }
    if (auto *view = tabs_->currentView()) {
        view->setFocus();
        view->load(url);
    }
}

void BrowserWindow::refreshState(bool forceAddress)
{
    auto *view = tabs_->currentView();
    const auto state = tabs_->currentPageState();
    back_->setEnabled(view && view->pageAction(QWebEnginePage::Back)->isEnabled());
    forward_->setEnabled(view && view->pageAction(QWebEnginePage::Forward)->isEnabled());
    reload_->setEnabled(view != nullptr);
    stop_->setEnabled(view && state.loading);
    progress_->setVisible(state.loading);
    progress_->setValue(state.progress);
    if (forceAddress || !address_->hasFocus()) {
        const QUrl url = view ? view->url() : QUrl();
        address_->setText(url == QUrl("about:blank") ? QString() : url.toDisplayString());
    }
    const QString title = view ? view->title() : QString();
    setWindowTitle(title.isEmpty() ? tr("AAFR Browser") : title + tr(" — AAFR Browser"));
    if (!state.error.isEmpty()) statusBar()->showMessage(state.error);
    else if (state.loading) statusBar()->showMessage(tr("Loading… %1%").arg(state.progress));
    else statusBar()->clearMessage();
}

void BrowserWindow::setIntegrationStatus(const QString &status)
{
    integrationStatus_->setText(tr("AAFR: %1").arg(status));
}
