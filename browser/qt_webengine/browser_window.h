#pragma once

#include <QMainWindow>
#include <QUrl>

class QAction;
class QLabel;
class QLineEdit;
class QProgressBar;
class TabManager;

class BrowserWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit BrowserWindow(QWidget *parent = nullptr,
                           const QUrl &homeUrl = QUrl(QStringLiteral("https://example.com")),
                           const QString &searchUrlTemplate = QStringLiteral("https://www.google.com/search?q=%1"));
    static QUrl urlFromInput(const QString &input, const QString &searchUrlTemplate);
    // Adapters may subscribe to the manager's lifecycle signals. The browser
    // neither selects nor applies adaptive forwarding policies itself.
    TabManager *tabManager() const { return tabs_; }

public slots:
    void setIntegrationStatus(const QString &status);

private:
    void navigate();
    void refreshState(bool forceAddress = false);
    void newTab();

    const QUrl homeUrl_;
    const QString searchUrlTemplate_;
    TabManager *tabs_;
    QLineEdit *address_;
    QAction *back_;
    QAction *forward_;
    QAction *reload_;
    QAction *stop_;
    QProgressBar *progress_;
    QLabel *integrationStatus_;
};
