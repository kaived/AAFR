#pragma once

#include <QDateTime>
#include <QHash>
#include <QIcon>
#include <QList>
#include <QMainWindow>
#include <QUrl>

#include <memory>

class BrowserEventCollector;
class DevToolsPanel;
class TabSearchPopup;
class TSharkMonitor;
class QWebEngineProfile;
class QWebEngineView;
class QAction;
class QLabel;
class QLineEdit;
class QProgressBar;
class QStackedWidget;
class QTabBar;
class QToolButton;
class TabManager;

namespace aafr::telemetry {
class TelemetryPipeline;
}

class BrowserWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit BrowserWindow(QWidget *parent = nullptr,
                           const QUrl &homeUrl = QUrl(QStringLiteral("about:blank")),
                           const QString &searchUrlTemplate = QStringLiteral("https://www.google.com/search?q=%1"),
                           // Profile for every tab; it must outlive the window.
                           // Null uses Qt's default, which keeps nothing on disk.
                           QWebEngineProfile *profile = nullptr);
    ~BrowserWindow() override;
    static QUrl urlFromInput(const QString &input, const QString &searchUrlTemplate);
    // Adapters may subscribe to the manager's lifecycle signals. The browser
    // neither selects nor applies adaptive forwarding policies itself.
    TabManager *tabManager() const { return tabs_; }

    // V0.3 observability. Telemetry is collected in memory from startup;
    // JSONL output and network capture stay off until explicitly enabled.
    aafr::telemetry::TelemetryPipeline &telemetry() const { return *telemetry_; }
    BrowserEventCollector *eventCollector() const { return collector_; }
    TSharkMonitor *networkMonitor() const { return monitor_; }
    DevToolsPanel *devToolsPanel() const { return devTools_; }
    // INI file holding the Developer Tools layout (dock side, size, floating).
    static QString layoutSettingsPath();

    // Chrome-style tab search (Ctrl+Shift+A) and recently closed tabs
    // (Ctrl+Shift+T reopens the latest). Closed tabs are kept in memory only.
    TabSearchPopup *tabSearch() const { return tabSearch_; }
    void showTabSearch();
    bool reopenLastClosedTab();
    int recentlyClosedCount() const { return recentlyClosed_.size(); }
    static constexpr int kMaxRecentlyClosed = 10;
    // Short description for tab search, e.g. "example.com" or "New tab page".
    static QString describeUrl(const QUrl &url);

    // The tab strip is the window's title bar. For a point in window
    // coordinates, says whether Windows should treat it as a caption (drag,
    // double-click to maximize, system menu), a top resize edge, or client
    // area that our own widgets handle.
    enum class TitleBarHit { Client, Caption, Top, TopLeft, TopRight };
    TitleBarHit titleBarHit(const QPoint &windowPos) const;
    // Height of the invisible top resize band, in logical pixels; zero when
    // maximized or full screen.
    int topResizeBorder() const;

protected:
    bool nativeEvent(const QByteArray &eventType, void *message, qintptr *result) override;
    void changeEvent(QEvent *event) override;
    bool event(QEvent *event) override;
    void showEvent(QShowEvent *event) override;

public slots:
    void setIntegrationStatus(const QString &status);

private:
    void navigate();
    void refreshState(bool forceAddress = false);
    void newTab();
    void syncTabStrip();
    void restoreLayout();
    void saveLayout();
    // Folds the native title bar into the client area (Windows only).
    void updateFrameMargins();
    void updateMaximizeButton();
    void refreshTabSearch();
    void recordActivation();
    void recordClosed(QWebEngineView *view);
    bool reopenClosedTab(int id);

    struct TabRecord {
        QString title;
        QUrl url;
        QIcon icon;
        QDateTime lastActive;
    };
    struct ClosedRecord {
        int id = 0;
        QString title;
        QUrl url;
        QIcon icon;
        QDateTime closedAt;
    };
    QHash<QWebEngineView *, TabRecord> tabRecords_;
    QList<ClosedRecord> recentlyClosed_;  // Newest first.
    int nextClosedId_ = 1;
    QWebEngineView *activeView_ = nullptr;
    TabSearchPopup *tabSearch_ = nullptr;
    QToolButton *tabSearchButton_ = nullptr;

    std::unique_ptr<aafr::telemetry::TelemetryPipeline> telemetry_;
    BrowserEventCollector *collector_ = nullptr;
    TSharkMonitor *monitor_ = nullptr;
    DevToolsPanel *devTools_ = nullptr;
    const QUrl homeUrl_;
    const QString searchUrlTemplate_;
    TabManager *tabs_;
    QLineEdit *address_;
    QAction *back_;
    QAction *forward_;
    QAction *reload_;
    QAction *stop_;
    QProgressBar *progress_;
    QTabBar *tabStrip_;
    QStackedWidget *content_;
    QWidget *welcome_;
    QLineEdit *newTabSearch_;
    QToolButton *reloadStop_;
    QWidget *header_ = nullptr;
    QToolButton *newTabButton_ = nullptr;
    QToolButton *minimizeButton_ = nullptr;
    QToolButton *maximizeButton_ = nullptr;
    QToolButton *closeButton_ = nullptr;
    QIcon brandIcon_;
    QIcon globeIcon_;
};
