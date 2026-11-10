#include "browser_window.h"
#include "browser_event_collector.h"
#include "devtools_panel.h"
#include "tab_manager.h"
#include "tab_search_popup.h"
#include "browser_theme.h"

#include "network/telemetry/telemetry_pipeline.h"
#include "network/tshark/tshark_monitor.h"

#include <QAction>
#include <QDir>
#include <QFileInfo>
#include <QSettings>
#include <QStandardPaths>
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
#include <QPainter>
#include <QPainterPath>
#include <QStyleOptionTab>
#include <QToolBar>
#include <QToolButton>
#include <QTabBar>
#include <QVBoxLayout>
#include <QWebEnginePage>
#include <QWebEngineView>
#include <QTimer>
#include <QWindow>
#include <algorithm>
#include <cmath>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <windowsx.h>
// Private Qt API: lets Qt itself fold the native caption into the client
// area, so Qt's frame and geometry bookkeeping stays consistent.
#include <QtGui/qpa/qplatformwindow_p.h>
#endif

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

    // Chrome-style tabs, painted here because style sheets cannot draw them:
    // - the active tab has rounded top corners and concave "feet" that flare
    //   into the toolbar below, in the toolbar's colour;
    // - a hovered tab shows a rounded highlight;
    // - inactive tabs are divided by short separators, hidden next to the
    //   active or hovered tab;
    // - the icon sits at the left of the tab body with the title right after
    //   it, elided before the close button.
    // Each tab keeps kFoot px at both sides for the feet (see the style sheet
    // padding), so neighbouring tabs never overlap.
    static constexpr qreal kFoot = 8;
    static constexpr qreal kRadius = 8;
    static constexpr int kTopMargin = 8;

    static QPainterPath activeShape(const QRectF &r) {
        const qreal top = r.top() + kTopMargin;
        const qreal bottom = r.bottom() + 1;
        const qreal left = r.left() + kFoot;
        const qreal right = r.right() + 1 - kFoot;
        QPainterPath path(QPointF(left - kFoot, bottom));
        path.arcTo(QRectF(left - 2 * kFoot, bottom - 2 * kFoot, 2 * kFoot, 2 * kFoot), 270, 90);
        path.lineTo(left, top + kRadius);
        path.arcTo(QRectF(left, top, 2 * kRadius, 2 * kRadius), 180, -90);
        path.lineTo(right - kRadius, top);
        path.arcTo(QRectF(right - 2 * kRadius, top, 2 * kRadius, 2 * kRadius), 90, -90);
        path.lineTo(right, bottom - kFoot);
        path.arcTo(QRectF(right, bottom - 2 * kFoot, 2 * kFoot, 2 * kFoot), 180, 90);
        path.closeSubpath();
        return path;
    }

    void paintEvent(QPaintEvent *) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const int current = currentIndex();
        int hovered = -1;
        QList<QStyleOptionTab> options(count());
        for (int i = 0; i < count(); ++i) {
            initStyleOption(&options[i], i);
            if (options[i].state.testFlag(QStyle::State_MouseOver)) hovered = i;
        }

        // Separators between inactive, unhovered neighbours.
        painter.setPen(QPen(BrowserTheme::tabColor(BrowserTheme::TabColor::Separator), 1));
        for (int i = 0; i + 1 < count(); ++i) {
            if (i == current || i + 1 == current || i == hovered || i + 1 == hovered) continue;
            const QRectF r = options[i].rect;
            const qreal x = r.right() + 0.5;
            painter.drawLine(QPointF(x, r.top() + kTopMargin + 9), QPointF(x, r.bottom() - 8));
        }

        for (int pass = 0; pass < 2; ++pass) {
            for (int i = 0; i < count(); ++i) {
                // Inactive tabs first, then the active one on top.
                if ((pass == 0) == (i == current)) continue;
                const QStyleOptionTab &option = options[i];
                if (!option.rect.intersects(rect())) continue;
                const QRectF r = option.rect;
                painter.setPen(Qt::NoPen);
                if (i == current) {
                    painter.setBrush(BrowserTheme::tabColor(BrowserTheme::TabColor::Active));
                    painter.drawPath(activeShape(r));
                    if (hasFocus()) {
                        // Keyboard focus ring for the tab strip.
                        painter.setBrush(Qt::NoBrush);
                        painter.setPen(QPen(BrowserTheme::tabColor(BrowserTheme::TabColor::Focus), 1.5));
                        painter.drawRoundedRect(QRectF(r.left() + kFoot + 1, r.top() + kTopMargin + 1,
                                                       r.width() - 2 * kFoot - 2, r.height() - kTopMargin - 4),
                                                kRadius, kRadius);
                        painter.setPen(Qt::NoPen);
                    }
                } else if (i == hovered) {
                    painter.setBrush(BrowserTheme::tabColor(BrowserTheme::TabColor::Hover));
                    painter.drawRoundedRect(QRectF(r.left() + kFoot, r.top() + kTopMargin + 3,
                                                   r.width() - 2 * kFoot, r.height() - kTopMargin - 7),
                                            kRadius, kRadius);
                }

                // Tab body: inside the feet and below the top margin.
                const QRect content = option.rect.adjusted(int(kFoot) + 10, kTopMargin, -int(kFoot), 0);
                int textLeft = content.left();
                if (!option.icon.isNull()) {
                    const QRect iconRect(content.left(), content.center().y() - 8, 16, 16);
                    option.icon.paint(&painter, iconRect);
                    textLeft = iconRect.right() + 9;
                }
                int textRight = content.right();
                if (auto *close = tabButton(i, QTabBar::RightSide); close && close->isVisible())
                    textRight = close->geometry().left() - 6;
                const QRect textRect(textLeft, content.top(), qMax(0, textRight - textLeft),
                                     content.height());
                painter.setPen(BrowserTheme::tabTextColor(i == current || i == hovered));
                painter.setFont(font());
                painter.drawText(textRect, Qt::AlignLeft | Qt::AlignVCenter,
                                 fontMetrics().elidedText(option.text, Qt::ElideRight,
                                                          textRect.width()));
            }
        }
    }
};
}

BrowserWindow::BrowserWindow(QWidget *parent, const QUrl &homeUrl,
                             const QString &searchUrlTemplate)
    : QMainWindow(parent), telemetry_(std::make_unique<aafr::telemetry::TelemetryPipeline>()),
      homeUrl_(homeUrl), searchUrlTemplate_(searchUrlTemplate)
{
    setWindowTitle(tr("New Tab"));
    // The AAFR mark is the browser's logo, as Chrome's is for Chrome.
    setWindowIcon(BrowserTheme::icon(BrowserTheme::Icon::Brand));
    QFont chromeFont = font();
    chromeFont.setFamily(QStringLiteral("Segoe UI"));
    chromeFont.setPointSize(10);
    setFont(chromeFont);
    setStyleSheet(BrowserTheme::styleSheet());
    setMinimumSize(800, 600);
    resize(1280, 800);
    tabs_ = new TabManager(this);
    // Observe tabs from the first one onwards. Capture is created but never
    // started here: it runs only after the user enables it.
    collector_ = new BrowserEventCollector(tabs_, *telemetry_, this);
    TSharkMonitor::Config captureConfig;
    captureConfig.interfaceName = qEnvironmentVariable("AAFR_TSHARK_INTERFACE");
    monitor_ = new TSharkMonitor(*telemetry_, captureConfig, this);
    // Keep the existing QTabWidget lifecycle/history model. Its presentation bar
    // is mirrored in a layout-owned header so navigation can sit below the tabs.
    tabs_->tabBar()->hide();
    auto *shell = new QWidget(this);
    shell->setObjectName("browserShell");
    auto *layout = new QVBoxLayout(shell);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    setCentralWidget(shell);
    // The tab strip doubles as the window's title bar (see titleBarHit).
    auto *header = new QWidget(shell);
    header->setObjectName("tabHeader");
    header_ = header;
    auto *headerLayout = new QHBoxLayout(header);
    // The first tab's left foot provides Chrome's small lead-in gap.
    headerLayout->setContentsMargins(0, 0, 0, 0);
    headerLayout->setSpacing(4);
    // Tab search, at the left of the strip as in Chrome on Windows.
    tabSearchButton_ = new QToolButton(header);
    tabSearchButton_->setObjectName("tabSearchButton");
    tabSearchButton_->setIcon(BrowserTheme::icon(BrowserTheme::Icon::ChevronDown));
    tabSearchButton_->setIconSize(QSize(16, 16));
    tabSearchButton_->setFixedSize(36, 34);
    tabSearchButton_->setFocusPolicy(Qt::NoFocus);
    tabSearchButton_->setAccessibleName(tr("Search tabs"));
    tabSearchButton_->setToolTip(tr("Search tabs (Ctrl+Shift+A)"));
    headerLayout->addWidget(tabSearchButton_, 0, Qt::AlignTop);
    tabStrip_ = new ChromeTabBar(header);
    tabStrip_->setObjectName("tabStrip");
    tabStrip_->setAccessibleName(tr("Browser tabs"));
    tabStrip_->setDocumentMode(true);
    tabStrip_->setExpanding(false);
    tabStrip_->setMovable(true);
    tabStrip_->setElideMode(Qt::ElideRight);
    tabStrip_->setUsesScrollButtons(true);
    // Tab focus only, for chrome controls throughout: a mouse click must not
    // leave a focus ring behind (Chrome shows rings for keyboard focus only),
    // while Tab still reaches every control.
    tabStrip_->setFocusPolicy(Qt::TabFocus);
    tabStrip_->setIconSize(QSize(16, 16));
    // Hover state drives the painted highlight and separators.
    tabStrip_->setAttribute(Qt::WA_Hover);
    tabStrip_->setMouseTracking(true);
    headerLayout->addWidget(tabStrip_, 1);
    auto *addButton = new QToolButton(header);
    addButton->setObjectName("newTabButton");
    addButton->setFixedSize(32, 32);
    addButton->setFocusPolicy(Qt::TabFocus);
    headerLayout->addWidget(addButton);
    newTabButton_ = addButton;
    headerLayout->addStretch();
    // Window caption buttons, drawn by AAFR because the native title bar is
    // folded into the tab strip.
    auto captionButton = [this, header, headerLayout](const char *name, BrowserTheme::Icon icon,
                                                      const QString &label) {
        auto *button = new QToolButton(header);
        button->setObjectName(name);
        button->setIcon(BrowserTheme::icon(icon));
        button->setIconSize(QSize(10, 10));
        button->setFixedSize(46, 34);
        button->setFocusPolicy(Qt::NoFocus);
        button->setAccessibleName(label);
        button->setToolTip(label);
        headerLayout->addWidget(button, 0, Qt::AlignTop);
        return button;
    };
    minimizeButton_ = captionButton("captionMinimize", BrowserTheme::Icon::Minimize, tr("Minimize"));
    maximizeButton_ = captionButton("captionMaximize", BrowserTheme::Icon::Maximize, tr("Maximize"));
    closeButton_ = captionButton("captionClose", BrowserTheme::Icon::WindowClose, tr("Close"));
    connect(minimizeButton_, &QToolButton::clicked, this, &QWidget::showMinimized);
    connect(maximizeButton_, &QToolButton::clicked, this, [this] {
        isMaximized() ? showNormal() : showMaximized();
    });
    connect(closeButton_, &QToolButton::clicked, this, &QWidget::close);
#ifndef Q_OS_WIN
    // Elsewhere the native title bar is kept, with its own buttons.
    for (auto *button : {minimizeButton_, maximizeButton_, closeButton_}) button->hide();
#endif
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
    // Home has no toolbar button; it stays available as Alt+Home.
    auto *home = new QAction(BrowserTheme::icon(BrowserTheme::Icon::Home), tr("Home"), this);
    addAction(home);
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

    // Developer Tools: a dock outside the central widget, so webpage layout
    // and rendering are unaffected. Hidden by default; Ctrl+Shift+M toggles.
    devTools_ = new DevToolsPanel(*telemetry_, collector_, monitor_, this);
    addDockWidget(Qt::BottomDockWidgetArea, devTools_);
    devTools_->hide();
    auto *devToolsAction = devTools_->toggleViewAction();
    devToolsAction->setObjectName("devToolsAction");
    devToolsAction->setIcon(BrowserTheme::icon(BrowserTheme::Icon::DevTools));
    devToolsAction->setText(tr("Developer tools"));
    devToolsAction->setToolTip(tr("AAFR Developer Tools (Ctrl+Shift+M)"));
    devToolsAction->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_M));
    devToolsAction->setShortcutContext(Qt::WindowShortcut);
    addAction(devToolsAction);
    toolbar->addAction(devToolsAction);
    if (auto *button = qobject_cast<QToolButton *>(toolbar->widgetForAction(devToolsAction)))
        button->setObjectName("devToolsButton");

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
        button->setFocusPolicy(Qt::TabFocus);
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
    cardLayout->addSpacing(12);
    newTabSearch_ = new QLineEdit(card);
    newTabSearch_->setObjectName("newTabSearch");
    newTabSearch_->setAccessibleName(tr("Search or enter an address"));
    newTabSearch_->setPlaceholderText(tr("Search or enter an address"));
    auto *newTabProvider = newTabSearch_->addAction(searchIcon, QLineEdit::LeadingPosition);
    newTabProvider->setObjectName("newTabSearchProvider");
    newTabProvider->setToolTip(searchHint);
    cardLayout->addWidget(newTabSearch_);
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

    // Tab search popup and recently closed tabs.
    tabSearch_ = new TabSearchPopup(this);
    connect(tabSearchButton_, &QToolButton::clicked, this, &BrowserWindow::showTabSearch);
    auto *searchTabs = new QAction(tr("Search tabs"), this);
    searchTabs->setObjectName("searchTabsAction");
    searchTabs->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_A));
    addAction(searchTabs);
    connect(searchTabs, &QAction::triggered, this, &BrowserWindow::showTabSearch);
    auto *reopen = new QAction(tr("Reopen closed tab"), this);
    reopen->setObjectName("reopenClosedTabAction");
    reopen->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_T));
    addAction(reopen);
    connect(reopen, &QAction::triggered, this, &BrowserWindow::reopenLastClosedTab);
    connect(tabSearch_, &TabSearchPopup::activateTab, tabs_, &QTabWidget::setCurrentIndex);
    connect(tabSearch_, &TabSearchPopup::closeTab, this, [this](int index) {
        tabs_->closeTab(index);
        // The popup stays open, as in Chrome; refresh once the tab is gone.
        QTimer::singleShot(0, this, &BrowserWindow::refreshTabSearch);
    });
    connect(tabSearch_, &TabSearchPopup::reopenClosedTab, this, &BrowserWindow::reopenClosedTab);
    connect(tabs_, &QTabWidget::currentChanged, this, &BrowserWindow::recordActivation);
    connect(tabs_, &TabManager::tabClosed, this, &BrowserWindow::recordClosed);
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
    restoreLayout();
}

BrowserWindow::~BrowserWindow()
{
    saveLayout();
    // Tear down observers before the tab manager and the pipeline they use.
    delete devTools_;
    devTools_ = nullptr;
    delete collector_;
    collector_ = nullptr;
    if (monitor_) monitor_->shutdown();
    delete monitor_;
    monitor_ = nullptr;
    telemetry_->Shutdown();
}

QString BrowserWindow::layoutSettingsPath()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation)
           + QStringLiteral("/aafr-browser-layout.ini");
}

void BrowserWindow::restoreLayout()
{
    QSettings settings(layoutSettingsPath(), QSettings::IniFormat);
    const QByteArray state = settings.value(QStringLiteral("window/state")).toByteArray();
    if (!state.isEmpty()) restoreState(state, 1);
    // Remember where and how large the panel was, but always start hidden.
    devTools_->hide();
}

int BrowserWindow::topResizeBorder() const
{
#ifdef Q_OS_WIN
    if (isMaximized() || isFullScreen() || !testAttribute(Qt::WA_WState_Created)) return 0;
    const HWND hwnd = reinterpret_cast<HWND>(winId());
    const UINT dpi = GetDpiForWindow(hwnd);
    const int native = GetSystemMetricsForDpi(SM_CYSIZEFRAME, dpi)
                       + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
    return qMax(1, int(std::ceil(native / devicePixelRatioF())));
#else
    return 0;  // The native title bar is kept outside Windows.
#endif
}

BrowserWindow::TitleBarHit BrowserWindow::titleBarHit(const QPoint &pos) const
{
    if (!header_ || !rect().contains(pos)) return TitleBarHit::Client;
    // Resize band along the top edge, where the native frame used to be.
    const int border = topResizeBorder();
    if (pos.y() < border) {
        if (pos.x() < border * 2) return TitleBarHit::TopLeft;
        if (pos.x() >= width() - border * 2) return TitleBarHit::TopRight;
        return TitleBarHit::Top;
    }
    const QRect headerRect(header_->mapTo(this, QPoint()), header_->size());
    if (!headerRect.contains(pos)) return TitleBarHit::Client;
    QWidget *child = childAt(pos);
    // Empty header space, including the gap beside the + button.
    if (!child || child == header_) return TitleBarHit::Caption;
    if (child == tabStrip_) {
        // Between and after tabs, and the strip above them, drag the window.
        const QPoint local = tabStrip_->mapFrom(this, pos);
        if (tabStrip_->tabAt(local) < 0 || local.y() < 8) return TitleBarHit::Caption;
    }
    // Tabs, their close buttons, + and the caption buttons stay interactive.
    return TitleBarHit::Client;
}

bool BrowserWindow::nativeEvent(const QByteArray &eventType, void *message, qintptr *result)
{
#ifdef Q_OS_WIN
    if (eventType == "windows_generic_MSG") {
        auto *msg = static_cast<MSG *>(message);
        if (msg->message == WM_NCHITTEST && header_) {
            POINT point{GET_X_LPARAM(msg->lParam), GET_Y_LPARAM(msg->lParam)};
            RECT client;
            GetClientRect(msg->hwnd, &client);
            ScreenToClient(msg->hwnd, &point);
            // Side and bottom borders are still native frame: default handling.
            if (!PtInRect(&client, point)) return false;
            const qreal ratio = devicePixelRatioF();
            const QPoint logical(int(std::floor(point.x / ratio)), int(std::floor(point.y / ratio)));
            switch (titleBarHit(logical)) {
            case TitleBarHit::Caption: *result = HTCAPTION; return true;
            case TitleBarHit::Top: *result = HTTOP; return true;
            case TitleBarHit::TopLeft: *result = HTTOPLEFT; return true;
            case TitleBarHit::TopRight: *result = HTTOPRIGHT; return true;
            case TitleBarHit::Client: break;
            }
        }
    }
#endif
    return QMainWindow::nativeEvent(eventType, message, result);
}

void BrowserWindow::updateFrameMargins()
{
#ifdef Q_OS_WIN
    QWindow *window = windowHandle();
    if (!window) return;
    auto *native = window->nativeInterface<QNativeInterface::Private::QWindowsWindow>();
    if (!native) return;
    const HWND hwnd = reinterpret_cast<HWND>(winId());
    const UINT dpi = GetDpiForWindow(hwnd);
    const int caption = GetSystemMetricsForDpi(SM_CYCAPTION, dpi);
    const int frame = GetSystemMetricsForDpi(SM_CYSIZEFRAME, dpi)
                      + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
    // Normal: remove the caption and the top frame; the top resize band is
    // provided by titleBarHit. Maximized: the frame lies off-screen, so only
    // the caption is removed. Full screen has no caption at all.
    int top = -(caption + frame);
    if (isFullScreen()) top = 0;
    else if (isMaximized()) top = -caption;
    const QMargins margins(0, top, 0, 0);
    if (native->customMargins() != margins) native->setCustomMargins(margins);
#endif
}

void BrowserWindow::updateMaximizeButton()
{
    if (!maximizeButton_) return;
    const bool maximized = isMaximized();
    maximizeButton_->setIcon(BrowserTheme::icon(maximized ? BrowserTheme::Icon::Restore
                                                          : BrowserTheme::Icon::Maximize));
    maximizeButton_->setToolTip(maximized ? tr("Restore") : tr("Maximize"));
    maximizeButton_->setAccessibleName(maximizeButton_->toolTip());
}

void BrowserWindow::changeEvent(QEvent *event)
{
    QMainWindow::changeEvent(event);
    if (event->type() == QEvent::WindowStateChange) {
        updateFrameMargins();
        updateMaximizeButton();
    }
}

bool BrowserWindow::event(QEvent *event)
{
    const bool handled = QMainWindow::event(event);
    // If Qt ever recreates the native window, the new one starts with a
    // normal title bar: fold it into the tab strip again.
    if (event->type() == QEvent::WinIdChange && isVisible()) {
        QTimer::singleShot(0, this, [this] {
            updateFrameMargins();
            if (QWindow *window = windowHandle())
                connect(window, &QWindow::screenChanged, this, &BrowserWindow::updateFrameMargins,
                        Qt::UniqueConnection);
        });
    }
    return handled;
}

void BrowserWindow::showEvent(QShowEvent *event)
{
    QMainWindow::showEvent(event);
    updateFrameMargins();
    if (QWindow *window = windowHandle()) {
        // Frame metrics depend on the monitor's DPI.
        connect(window, &QWindow::screenChanged, this, &BrowserWindow::updateFrameMargins,
                Qt::UniqueConnection);
    }
}

void BrowserWindow::saveLayout()
{
    const QString path = layoutSettingsPath();
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSettings settings(path, QSettings::IniFormat);
    settings.setValue(QStringLiteral("window/state"), saveState(1));
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
        // 20 px circle plus the style sheet's 8 px top and 16 px right margins
        // (see theme): QTabBar puts the button flush with the tab's edge,
        // where the tab's curved foot is.
        button->setFixedSize(36, 28);
        button->setIconSize(QSize(14, 14));
        button->setAccessibleName(tr("Close tab"));
        button->setToolTip(tr("Close tab (Ctrl+W)"));
        button->setFocusPolicy(Qt::TabFocus);
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
    // Chrome-style icons: the AAFR mark on new tabs, the page favicon when it
    // has one, and a neutral globe otherwise.
    if (brandIcon_.isNull()) {
        brandIcon_ = BrowserTheme::icon(BrowserTheme::Icon::Brand);
        globeIcon_ = BrowserTheme::icon(BrowserTheme::Icon::Globe);
    }
    const QIcon &brand = brandIcon_;
    const QIcon &globe = globeIcon_;
    for (int i = 0; i < tabs_->count(); ++i) {
        tabStrip_->setTabText(i, tabs_->tabText(i));
        tabStrip_->setTabToolTip(i, tabs_->tabToolTip(i));
        const auto *view = qobject_cast<QWebEngineView *>(tabs_->widget(i));
        const QUrl url = view ? view->url() : QUrl();
        const QIcon favicon = tabs_->tabIcon(i);
        if (url.isEmpty() || url == QUrl("about:blank")) tabStrip_->setTabIcon(i, brand);
        else tabStrip_->setTabIcon(i, favicon.isNull() ? globe : favicon);
        // Keep what tab search and "recently closed" need, so it is still
        // known when the view is already being destroyed.
        if (view) {
            TabRecord &record = tabRecords_[const_cast<QWebEngineView *>(view)];
            record.title = view->title().isEmpty() ? tabs_->tabText(i) : view->title();
            record.url = url;
            record.icon = tabStrip_->tabIcon(i);
            if (!record.lastActive.isValid()) record.lastActive = QDateTime::currentDateTimeUtc();
        }
    }
    tabStrip_->setCurrentIndex(tabs_->currentIndex());
    tabStrip_->setMaximumWidth(qMax(150, static_cast<ChromeTabBar *>(tabStrip_)->preferredWidth()));
}

QString BrowserWindow::describeUrl(const QUrl &url)
{
    if (url.isEmpty() || url == QUrl("about:blank")) return tr("New tab page");
    const QString scheme = url.scheme().toLower();
    if (scheme == QLatin1String("http") || scheme == QLatin1String("https")) {
        QString host = url.host();
        if (host.startsWith(QLatin1String("www."))) host.remove(0, 4);
        return host;
    }
    if (url.isLocalFile()) return QFileInfo(url.toLocalFile()).fileName();
    return scheme + QLatin1Char(':');
}

void BrowserWindow::recordActivation()
{
    const QDateTime now = QDateTime::currentDateTimeUtc();
    // The tab being left was last active now; so is the one being shown.
    if (activeView_ && tabRecords_.contains(activeView_)) tabRecords_[activeView_].lastActive = now;
    activeView_ = tabs_->currentView();
    if (activeView_) tabRecords_[activeView_].lastActive = now;
}

void BrowserWindow::recordClosed(QWebEngineView *view)
{
    // The view may already be mid-destruction: use it only as a key.
    const TabRecord record = tabRecords_.take(view);
    if (view == activeView_) activeView_ = nullptr;
    // Empty new tabs are not worth reopening, as in Chrome.
    if (record.url.isEmpty() || record.url == QUrl("about:blank")) return;
    ClosedRecord closed;
    closed.id = nextClosedId_++;
    closed.title = record.title;
    closed.url = record.url;
    closed.icon = record.icon;
    closed.closedAt = QDateTime::currentDateTimeUtc();
    recentlyClosed_.prepend(closed);
    while (recentlyClosed_.size() > kMaxRecentlyClosed) recentlyClosed_.removeLast();
}

bool BrowserWindow::reopenClosedTab(int id)
{
    for (int i = 0; i < recentlyClosed_.size(); ++i) {
        if (recentlyClosed_[i].id != id) continue;
        const QUrl url = recentlyClosed_.takeAt(i).url;
        tabs_->addTab(url);
        return true;
    }
    return false;
}

bool BrowserWindow::reopenLastClosedTab()
{
    return !recentlyClosed_.isEmpty() && reopenClosedTab(recentlyClosed_.first().id);
}

void BrowserWindow::refreshTabSearch()
{
    const QDateTime now = QDateTime::currentDateTimeUtc();
    QList<TabSearchPopup::OpenTab> open;
    for (int i = 0; i < tabs_->count(); ++i) {
        auto *view = qobject_cast<QWebEngineView *>(tabs_->widget(i));
        const TabRecord record = tabRecords_.value(view);
        TabSearchPopup::OpenTab tab;
        tab.index = i;
        tab.icon = tabStrip_->tabIcon(i);
        // WebEngine updates title() just before it signals the change, so
        // read it directly; the tab text may lag for a moment.
        const QString pageTitle = view ? view->title() : QString();
        const QUrl url = view ? view->url() : QUrl();
        tab.title = pageTitle.isEmpty() || url.isEmpty() || url == QUrl("about:blank")
                        ? tabs_->tabText(i) : pageTitle;
        tab.subtitle = describeUrl(url);
        tab.current = i == tabs_->currentIndex();
        tab.when = tab.current ? tr("Current tab")
                               : TabSearchPopup::relativeTime(record.lastActive.isValid()
                                                                  ? record.lastActive.secsTo(now) : 0);
        open << tab;
    }
    QList<TabSearchPopup::ClosedTab> closed;
    for (const ClosedRecord &record : std::as_const(recentlyClosed_)) {
        TabSearchPopup::ClosedTab tab;
        tab.id = record.id;
        tab.icon = record.icon;
        tab.title = record.title;
        tab.subtitle = describeUrl(record.url);
        tab.when = TabSearchPopup::relativeTime(record.closedAt.secsTo(now));
        closed << tab;
    }
    tabSearch_->setEntries(open, closed);
}

void BrowserWindow::showTabSearch()
{
    refreshTabSearch();
    tabSearch_->popupAt(tabSearchButton_->mapToGlobal(QPoint(0, tabSearchButton_->height() + 6)));
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
            // Local development servers rarely speak TLS; upgrade everything else.
            const bool loopback = host == QLatin1String("localhost")
                || host.endsWith(QLatin1String(".localhost"))
                || host == QLatin1String("::1")
                || host.startsWith(QLatin1String("127."));
            if (!loopback) url.setScheme(QStringLiteral("https"));
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
    // setDefaultAction() adds each action to the button. Keeping both makes
    // QToolButton treat the extras as a menu and draw a menu arrow, so the
    // inactive action is removed first.
    QAction *wanted = state.loading ? stop_ : reload_;
    if (reloadStop_->defaultAction() != wanted) {
        reloadStop_->removeAction(state.loading ? reload_ : stop_);
        reloadStop_->setDefaultAction(wanted);
    }
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
