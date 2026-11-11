
#include <QApplication>
#include <QCommandLineParser>
#include <QWebEngineProfile>
#include <QWebEngineView>

#include <filesystem>
#include <memory>

#include "browser_window.h"
#include "tab_manager.h"
#include "network/telemetry/telemetry_pipeline.h"

int main(int argc, char *argv[])
{
    // Compose the window on the GPU from the start. Otherwise Qt destroys and
    // recreates the native window the first time WebEngine renders (for
    // example on Refresh in a new tab), which flashes the whole window.
    if (!qEnvironmentVariableIsSet("QT_WIDGETS_RHI")) qputenv("QT_WIDGETS_RHI", "1");
    QApplication app(argc, argv);

    QApplication::setApplicationName("AAFR Browser");
    QApplication::setOrganizationName("AAFR");

    QCommandLineParser parser;
    parser.addHelpOption();
    // Off by default. Records are privacy-redacted and size-bounded.
    const QCommandLineOption telemetryDir(
        "telemetry-dir",
        "Write diagnostic telemetry JSONL to <dir> (also AAFR_TELEMETRY_DIR).", "dir");
    parser.addOption(telemetryDir);
    const QCommandLineOption privateMode(
        "private", "Keep no cache, cookies or site data on disk for this run.");
    parser.addOption(privateMode);
    parser.addPositionalArgument("url", "Page or search terms to open in the first tab.", "[url]");
    parser.process(app);

    // A named profile keeps the HTTP cache, cookies and site data on disk, so
    // pages are not downloaded from scratch after every start. Qt's default
    // profile keeps them in memory only. Declared before the window so that
    // it outlives every page.
    std::unique_ptr<QWebEngineProfile> profile;
    if (!parser.isSet(privateMode))
        profile = std::make_unique<QWebEngineProfile>(QStringLiteral("aafr"));

    const QString searchTemplate = QStringLiteral("https://www.google.com/search?q=%1");
    BrowserWindow window(nullptr, QUrl(QStringLiteral("about:blank")), searchTemplate, profile.get());
    if (!parser.positionalArguments().isEmpty()) {
        const QUrl url = BrowserWindow::urlFromInput(parser.positionalArguments().constFirst(),
                                                     searchTemplate);
        if (auto *view = window.tabManager()->currentView(); view && url.isValid()) view->load(url);
    }
    QString directory = parser.value(telemetryDir);
    if (directory.isEmpty()) directory = qEnvironmentVariable("AAFR_TELEMETRY_DIR");
    if (!directory.isEmpty() && !window.telemetry().EnableJsonl(std::filesystem::path(directory.toStdWString())))
        window.setIntegrationStatus(QObject::tr("Telemetry JSONL output could not be opened in %1").arg(directory));
    window.show();

    return app.exec();
}
