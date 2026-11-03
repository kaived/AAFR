#include "browser_window.h"
#include "tab_manager.h"
#include "browser_theme.h"

#include <QAction>
#include <QKeySequence>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QProgressBar>
#include <QPointer>
#include <QShortcut>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QStatusBar>
#include <QStyle>
#include <QToolBar>
#include <QToolButton>
#include <QTabBar>
#include <QVBoxLayout>
#include <QWebEnginePage>
#include <QWebEngineView>
#include <algorithm>

namespace {
class ChromeTabBar final : public QTabBar
{
public:
    using QTabBar::QTabBar;
    QSize minimumSizeHint() const override { return {0, 42}; }
    int preferredWidth() const {
        int width = 0;
        for (int i = 0; i < count(); ++i) width += tabSizeHint(i).width();
        return width;
    }
protected:
    QSize minimumTabSizeHint(int) const override { return {150, 42}; }
    QSize tabSizeHint(int index) const override {
        return {qBound(150, QTabBar::tabSizeHint(index).width(), 230), 42};
    }
};
}

BrowserWindow::BrowserWindow(QWidget *parent, const QUrl &homeUrl,
                             const QString &searchUrlTemplate)
    : QMainWindow(parent), homeUrl_(homeUrl), searchUrlTemplate_(searchUrlTemplate)
{
    setWindowTitle(tr("New Tab"));
    setWindowIcon(BrowserTheme::icon(BrowserTheme::Icon::Search));
    QFont chromeFont = font();
    chromeFont.setFamily(QStringLiteral("Segoe UI"));
    chromeFont.setPointSize(10);
    setFont(chromeFont);
    setStyleSheet(BrowserTheme::styleSheet());
    setMinimumSize(800, 600);
    resize(1280, 800);
    tabs_ = new TabManager(this);
    // Keep the existing QTabWidget lifecycle/history model. Its presentation bar
    // is mirrored in a layout-owned header so navigation can sit below the tabs.
    tabs_->tabBar()->hide();
    auto *shell = new QWidget(this);
    shell->setObjectName("browserShell");
    auto *layout = new QVBoxLayout(shell);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    setCentralWidget(shell);
    auto *header = new QWidget(shell);
    header->setObjectName("tabHeader");
    auto *headerLayout = new QHBoxLayout(header);
    headerLayout->setContentsMargins(12, 0, 12, 0);
    headerLayout->setSpacing(4);
    tabStrip_ = new ChromeTabBar(header);
    tabStrip_->setObjectName("tabStrip");
    tabStrip_->setAccessibleName(tr("Browser tabs"));
    tabStrip_->setDocumentMode(true);
    tabStrip_->setExpanding(false);
    tabStrip_->setMovable(true);
    tabStrip_->setElideMode(Qt::ElideRight);
    tabStrip_->setUsesScrollButtons(true);
    tabStrip_->setFocusPolicy(Qt::StrongFocus);
    headerLayout->addWidget(tabStrip_, 1);
    auto *addButton = new QToolButton(header);
    addButton->setObjectName("newTabButton");
    addButton->setFixedSize(32, 32);
    addButton->setFocusPolicy(Qt::StrongFocus);
    headerLayout->addWidget(addButton);
    headerLayout->addStretch();
    layout->addWidget(header);

    auto *toolbar = new QToolBar(tr("Navigation"), shell);
    toolbar->setObjectName("navigationToolbar");
    toolbar->setMovable(false);
    toolbar->setFloatable(false);
    toolbar->setIconSize(QSize(20, 20));
    layout->addWidget(toolbar);
    back_ = toolbar->addAction(BrowserTheme::icon(BrowserTheme::Icon::Back), tr("Back"));
    back_->setObjectName("backAction");
    back_->setShortcut(QKeySequence::Back);
    forward_ = toolbar->addAction(BrowserTheme::icon(BrowserTheme::Icon::Forward), tr("Forward"));
    forward_->setObjectName("forwardAction");
    forward_->setShortcut(QKeySequence::Forward);
    reload_ = new QAction(BrowserTheme::icon(BrowserTheme::Icon::Reload), tr("Reload"), this);
    addAction(reload_);
    reload_->setObjectName("reloadAction");
    reload_->setShortcuts({QKeySequence(Qt::Key_F5), QKeySequence(Qt::CTRL | Qt::Key_R)});
    stop_ = new QAction(BrowserTheme::icon(BrowserTheme::Icon::Stop), tr("Stop loading"), this);
    addAction(stop_);
    stop_->setObjectName("stopAction");
    stop_->setShortcut(QKeySequence(Qt::Key_Escape));
    reloadStop_ = new QToolButton(toolbar);
    reloadStop_->setObjectName("reloadStopButton");
    reloadStop_->setDefaultAction(reload_);
    toolbar->addWidget(reloadStop_);
    auto *home = toolbar->addAction(BrowserTheme::icon(BrowserTheme::Icon::Home), tr("Home"));
    home->setObjectName("homeAction");
    home->setShortcut(QKeySequence(Qt::ALT | Qt::Key_Home));

    address_ = new QLineEdit(this);
    address_->setObjectName("addressBar");
    address_->setPlaceholderText(tr("Enter a URL or search terms"));
    address_->setAccessibleName(tr("Address"));
    address_->setClearButtonEnabled(true);
    address_->setMinimumWidth(200);
    address_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    const QString searchHost = QUrl(searchUrlTemplate_.arg(QStringLiteral("query"))).host().toLower();
    const bool googleSearch = searchHost == QLatin1String("google.com")
        || searchHost == QLatin1String("www.google.com");
    const QIcon searchIcon = googleSearch ? QIcon(QStringLiteral(":/browser/assets/google-g.png"))
                                         : BrowserTheme::icon(BrowserTheme::Icon::Search);
    const QString searchHint = googleSearch ? tr("Search with Google") : tr("Search the web");
    auto *addressProvider = address_->addAction(searchIcon, QLineEdit::LeadingPosition);
    addressProvider->setObjectName("addressSearchProvider");
    addressProvider->setToolTip(searchHint);
    toolbar->addWidget(address_);
    auto *add = new QAction(BrowserTheme::icon(BrowserTheme::Icon::Plus), tr("New tab"), this);
    addAction(add);
    add->setObjectName("newTabAction");
    add->setShortcut(QKeySequence::AddTab);
    addButton->setDefaultAction(add);
    addButton->setAccessibleName(tr("New tab"));
    auto *close = new QAction(tr("Close Tab"), this);
    close->setObjectName("closeTabAction");
    close->setShortcuts({QKeySequence(Qt::CTRL | Qt::Key_W),
                         QKeySequence(Qt::CTRL | Qt::Key_F4)});
    addAction(close);

    for (auto *button : toolbar->findChildren<QToolButton *>()) {
        // Size navigation buttons only, not QLineEdit's embedded provider/clear icons.
        if (button->parentWidget() != toolbar || !button->defaultAction()) continue;
        button->setFixedSize(34, 34);
        button->setFocusPolicy(Qt::StrongFocus);
        if (button->defaultAction()) button->setAccessibleName(button->defaultAction()->text());
    }
    content_ = new QStackedWidget(shell);
    content_->setObjectName("contentStack");
    content_->addWidget(tabs_);
    welcome_ = new QWidget(content_);
    welcome_->setObjectName("newTabPage");
    auto *welcomeLayout = new QVBoxLayout(welcome_);
    welcomeLayout->setContentsMargins(32, 24, 32, 24);
    welcomeLayout->addStretch();
    auto *card = new QWidget(welcome_);
    card->setObjectName("newTabCard");
    card->setMaximumWidth(560);
    auto *cardLayout = new QVBoxLayout(card);
    cardLayout->setContentsMargins(0, 0, 0, 0);
    cardLayout->setSpacing(16);
    auto addLabel = [card, cardLayout](const QString &text, const char *name) {
        auto *label = new QLabel(text, card);
        label->setObjectName(name);
        label->setAlignment(Qt::AlignCenter);
        label->setWordWrap(true);
        cardLayout->addWidget(label);
        return label;
    };
    auto *welcomeMark = addLabel({}, "newTabMark");
    welcomeMark->setPixmap(BrowserTheme::icon(BrowserTheme::Icon::Brand).pixmap(48, 48));
    addLabel(tr("AAFR / NEW TAB"), "newTabEyebrow");
    addLabel(tr("A clear place to begin."), "newTabHeading");
    addLabel(tr("Adaptive Agentic Forwarding Runtime"), "newTabSubtitle");
    cardLayout->addSpacing(12);
    newTabSearch_ = new QLineEdit(card);
    newTabSearch_->setObjectName("newTabSearch");
    newTabSearch_->setAccessibleName(tr("Search or enter an address"));
    newTabSearch_->setPlaceholderText(tr("Search or enter an address"));
    auto *newTabProvider = newTabSearch_->addAction(searchIcon, QLineEdit::LeadingPosition);
    newTabProvider->setObjectName("newTabSearchProvider");
    newTabProvider->setToolTip(searchHint);
    cardLayout->addWidget(newTabSearch_);
    addLabel(tr("Ctrl+L  address bar     /     Ctrl+T  new tab"), "newTabHint");
    auto *cardRow = new QHBoxLayout;
    cardRow->addStretch();
    cardRow->addWidget(card, 1);
    cardRow->addStretch();
    welcomeLayout->addLayout(cardRow);
    welcomeLayout->addStretch();
    content_->addWidget(welcome_);
    layout->addWidget(content_, 1);

    progress_ = new QProgressBar(this);
    progress_->setObjectName("loadProgress");
    progress_->setRange(0, 100);
    progress_->setMaximumWidth(150);
    progress_->setTextVisible(false);
    progress_->setAccessibleName(tr("Page loading progress"));
    statusBar()->addPermanentWidget(progress_);
    statusBar()->setSizeGripEnabled(false);
    connect(newTabSearch_, &QLineEdit::returnPressed, this, [this] {
        address_->setText(newTabSearch_->text());
        navigate();
    });
    connect(tabStrip_, &QTabBar::currentChanged, tabs_, &QTabWidget::setCurrentIndex);
    connect(tabStrip_, &QTabBar::tabMoved, this, [this](int from, int to) {
        tabs_->tabBar()->moveTab(from, to);
    });
    connect(tabs_, &TabManager::tabMetadataChanged, this, &BrowserWindow::syncTabStrip);
    connect(tabs_, &QTabWidget::currentChanged, this, &BrowserWindow::syncTabStrip);

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
    // A new-tab surface needs no renderer navigation or synthetic history entry.
    tabs_->addTab(homeUrl_ == QUrl("about:blank") ? QUrl() : homeUrl_);
    refreshState(true);
    if (content_->currentWidget() == welcome_) newTabSearch_->setFocus();
}

void BrowserWindow::newTab()
{
    tabs_->addTab();
    newTabSearch_->clear();
    newTabSearch_->setFocus();
}

void BrowserWindow::syncTabStrip()
{
    const QSignalBlocker blocker(tabStrip_);
    while (tabStrip_->count() > tabs_->count()) {
        const int index = tabStrip_->count() - 1;
        QPointer<QWidget> close = tabStrip_->tabButton(index, QTabBar::RightSide);
        tabStrip_->removeTab(index);
        if (close) close->deleteLater();
    }
    while (tabStrip_->count() < tabs_->count()) {
        const int index = tabStrip_->addTab({});
        auto *button = new QToolButton(tabStrip_);
        button->setObjectName("tabCloseButton");
        button->setIcon(BrowserTheme::icon(BrowserTheme::Icon::Close));
        button->setFixedSize(26, 26);
        button->setAccessibleName(tr("Close tab"));
        button->setToolTip(tr("Close tab (Ctrl+W)"));
        button->setFocusPolicy(Qt::StrongFocus);
        connect(button, &QToolButton::clicked, this, [this, button] {
            for (int i = 0; i < tabStrip_->count(); ++i) {
                if (tabStrip_->tabButton(i, QTabBar::RightSide) == button) {
                    tabs_->closeTab(i);
                    break;
                }
            }
        });
        tabStrip_->setTabButton(index, QTabBar::RightSide, button);
    }
    for (int i = 0; i < tabs_->count(); ++i) {
        tabStrip_->setTabText(i, tabs_->tabText(i));
        tabStrip_->setTabToolTip(i, tabs_->tabToolTip(i));
        tabStrip_->setTabIcon(i, tabs_->tabIcon(i));
    }
    tabStrip_->setCurrentIndex(tabs_->currentIndex());
    tabStrip_->setMaximumWidth(qMax(150, static_cast<ChromeTabBar *>(tabStrip_)->preferredWidth()));
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
    reloadStop_->setDefaultAction(state.loading ? stop_ : reload_);
    reloadStop_->setAccessibleName(state.loading ? tr("Stop loading") : tr("Reload"));
    progress_->setVisible(state.loading);
    progress_->setValue(state.progress);
    if (forceAddress || !address_->hasFocus()) {
        const QUrl url = view ? view->url() : QUrl();
        address_->setText(url == QUrl("about:blank") ? QString() : url.toDisplayString());
    }
    const QString title = view ? view->title() : QString();
    const QUrl requested = view ? view->page()->requestedUrl() : QUrl();
    const bool blank = view && state.error.isEmpty()
        && (view->url().isEmpty() || view->url() == QUrl("about:blank"))
        && (!state.loading || requested.isEmpty() || requested == QUrl("about:blank"));
    content_->setCurrentWidget(blank ? welcome_ : static_cast<QWidget *>(tabs_));
    setWindowTitle(title.isEmpty() ? tr("New Tab") : title);
    if (!state.error.isEmpty()) statusBar()->showMessage(state.error);
    else if (state.loading) statusBar()->showMessage(tr("Loading… %1%").arg(state.progress));
    else statusBar()->clearMessage();
}

void BrowserWindow::setIntegrationStatus(const QString &status)
{
    statusBar()->showMessage(status);
}
