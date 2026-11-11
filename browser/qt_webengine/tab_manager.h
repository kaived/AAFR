#pragma once

#include <QHash>
#include <QTabWidget>
#include <QUrl>

class QWebEngineProfile;
class QWebEngineView;

// Owns WebEngine views and their per-tab state. No adaptive runtime dependency.
class TabManager : public QTabWidget
{
    Q_OBJECT

public:
    struct PageState {
        bool loading = false;
        int progress = 0;
        QString error;
    };

    // With no profile, tabs use Qt's default one, which keeps nothing on disk.
    // A given profile must outlive the manager.
    explicit TabManager(QWidget *parent = nullptr, QWebEngineProfile *profile = nullptr);
    ~TabManager() override;
    QWebEngineView *addTab(const QUrl &url = QUrl(), bool foreground = true);
    QWebEngineView *currentView() const;
    PageState currentPageState() const;

public slots:
    void closeTab(int index);
    void closeCurrentTab();

signals:
    void currentStateChanged();
    void tabMetadataChanged();

    // Emitted before the initial load, so an adapter can attach page observers.
    void webViewCreated(QWebEngineView *view);
    // Page-level lifecycle events, including background tabs; these are not
    // individual HTTP request measurements or runtime policy decisions.
    void navigationStarted(QWebEngineView *view, const QUrl &url);
    void navigationFinished(QWebEngineView *view, const QUrl &url,
                            bool success, const QString &error);
    // Emitted once per tab when it is closed or its view is destroyed
    // externally. The view may already be partly destroyed: use the pointer
    // only as a lookup key, never dereference it.
    void tabClosed(QWebEngineView *view);

private:
    void notifyIfCurrent(QWebEngineView *view);
    QHash<QWebEngineView *, PageState> states_;
    QWebEngineProfile *profile_ = nullptr;
};
