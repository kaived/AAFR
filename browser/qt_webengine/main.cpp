
#include <QApplication>
#include <QCommandLineParser>

#include "browser_window.h"
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
    parser.process(app);

    BrowserWindow window;
    QString directory = parser.value(telemetryDir);
    if (directory.isEmpty()) directory = qEnvironmentVariable("AAFR_TELEMETRY_DIR");
    if (!directory.isEmpty() && !window.telemetry().EnableJsonl(std::filesystem::path(directory.toStdWString())))
        window.setIntegrationStatus(QObject::tr("Telemetry JSONL output could not be opened in %1").arg(directory));
    window.show();

    return app.exec();
}
