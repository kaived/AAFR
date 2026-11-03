#pragma once

#include <QHash>
#include <QTabWidget>
#include <QUrl>

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

    explicit TabManager(QWidget *parent = nullptr);
    ~TabManager() override;
    QWebEngineView *addTab(const QUrl &url = QUrl(), bool foreground = true);
    QWebEngineView *currentView() const;
    PageState currentPageState() const;

public slots:
    void closeTab(int index);
    void closeCurrentTab();

signals:
    void currentStateChanged();

    // Emitted before the initial load, so an adapter can attach page observers.
    void webViewCreated(QWebEngineView *view);
    // Page-level lifecycle events, including background tabs; these are not
    // individual HTTP request measurements or runtime policy decisions.
    void navigationStarted(QWebEngineView *view, const QUrl &url);
    void navigationFinished(QWebEngineView *view, const QUrl &url,
                            bool success, const QString &error);

private:
    void notifyIfCurrent(QWebEngineView *view);
    QHash<QWebEngineView *, PageState> states_;
};
