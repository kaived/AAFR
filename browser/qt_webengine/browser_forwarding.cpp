#include "browser_forwarding.h"

#include <QByteArray>
#include <QHostAddress>
#include <QUrl>

#include <cstring>

const char *toString(BrowserForwardingMode mode)
{
    switch (mode) {
    case BrowserForwardingMode::Direct: return "direct";
    case BrowserForwardingMode::Controlled: return "controlled";
    case BrowserForwardingMode::Diagnostic: return "diagnostic";
    }
    return "unknown";
}

std::optional<BrowserForwardingMode> parseBrowserForwardingMode(const QString &text)
{
    const QString t = text.trimmed().toLower();
    if (t.isEmpty() || t == QLatin1String("direct")) return BrowserForwardingMode::Direct;
    if (t == QLatin1String("controlled")) return BrowserForwardingMode::Controlled;
    if (t == QLatin1String("diagnostic")) return BrowserForwardingMode::Diagnostic;
    return std::nullopt;
}

namespace {
bool envFlag(const char *name)
{
    const QByteArray v = qgetenv(name).trimmed().toLower();
    return v == "1" || v == "true" || v == "yes";
}
}  // namespace

BrowserForwardingOptions BrowserForwardingOptions::fromArguments(int argc, char **argv, QString *error)
{
    BrowserForwardingOptions o;
    QString modeText = QString::fromLocal8Bit(qgetenv("AAFR_BROWSER_FORWARDING"));
    o.forwardLoopback = envFlag("AAFR_FORWARD_LOOPBACK");
    o.allowPrivate = envFlag("AAFR_GATEWAY_ALLOW_PRIVATE");
    // Both "--forwarding=<mode>" and "--forwarding <mode>" (as QCommandLineParser
    // and --help accept them); a missing value is an error, never direct mode.
    for (int i = 1; i < argc; ++i) {
        const char *a = argv[i];
        if (std::strncmp(a, "--forwarding=", 13) == 0) {
            modeText = QString::fromLocal8Bit(a + 13);
        } else if (std::strcmp(a, "--forwarding") == 0) {
            if (i + 1 >= argc) {
                if (error) *error = QStringLiteral("--forwarding needs a mode (direct | controlled | diagnostic)");
                o.mode = BrowserForwardingMode::Direct;
                return o;
            }
            modeText = QString::fromLocal8Bit(argv[++i]);
        } else if (std::strcmp(a, "--forward-loopback") == 0) {
            o.forwardLoopback = true;
        }
    }
    const auto mode = parseBrowserForwardingMode(modeText);
    if (!mode) {
        if (error) *error = QStringLiteral("unknown forwarding mode '%1' (direct | controlled | diagnostic)").arg(modeText);
        o.mode = BrowserForwardingMode::Direct;
    } else {
        o.mode = *mode;
    }
    return o;
}

std::unique_ptr<BrowserForwardingSetup> BrowserForwardingSetup::prepare(const BrowserForwardingOptions &options,
                                                                        QString *error)
{
    auto setup = std::unique_ptr<BrowserForwardingSetup>(new BrowserForwardingSetup);
    setup->options_ = options;
    if (options.mode == BrowserForwardingMode::Direct) return setup;

    aafr::gateway::GatewayConfig config;
    config.mode = options.mode == BrowserForwardingMode::Diagnostic ? aafr::gateway::GatewayMode::Diagnostic
                                                                    : aafr::gateway::GatewayMode::Controlled;
    // Only this browser process (and its own child processes) may use it.
    config.restrict_to_process = true;
    config.egress.allow_private = options.allowPrivate;
    if (options.forwardLoopback) {
        // Controlled experiments with local test servers on arbitrary ports.
        config.egress.allow_loopback = true;
        config.egress.allowed_ports.clear();
    }
    setup->gateway_ = std::make_unique<aafr::gateway::AAFRBrowserForwardingGateway>(config);
    std::string bindError;
    if (!setup->gateway_->Bind(&bindError)) {
        if (error) *error = QStringLiteral("the AAFR forwarding gateway could not bind: %1").arg(QString::fromStdString(bindError));
        return nullptr;
    }
    QString flags = QStringLiteral("--proxy-server=http://127.0.0.1:%1 --disable-quic").arg(setup->gateway_->port());
    if (options.forwardLoopback) flags += QStringLiteral(" --proxy-bypass-list=<-loopback>");
    setup->flags_ = flags;
    // Keep any switches the user set, but ours must win: they come last.
    QByteArray existing = qgetenv("QTWEBENGINE_CHROMIUM_FLAGS");
    if (!existing.isEmpty()) existing += ' ';
    qputenv("QTWEBENGINE_CHROMIUM_FLAGS", existing + flags.toUtf8());
    return setup;
}

ForwardingRequestObserver::ForwardingRequestObserver(aafr::gateway::AAFRBrowserForwardingGateway *gateway,
                                                     bool forwardLoopback, QObject *parent)
    : QWebEngineUrlRequestInterceptor(parent), gateway_(gateway), forwardLoopback_(forwardLoopback)
{
}

void ForwardingRequestObserver::interceptRequest(QWebEngineUrlRequestInfo &info)
{
    const QUrl url = info.requestUrl();
    const QString scheme = url.scheme().toLower();
    const bool network = scheme == QLatin1String("http") || scheme == QLatin1String("https") ||
                         scheme == QLatin1String("ws") || scheme == QLatin1String("wss");
    if (!network) {
        ++counts_.nonNetwork;
        return;
    }
    const QString host = url.host().toLower();
    const bool loopback = host == QLatin1String("localhost") || host.endsWith(QLatin1String(".localhost")) ||
                          QHostAddress(host).isLoopback();
    if (loopback && !forwardLoopback_) {
        ++counts_.bypassed;
        // Only the class of destination, never the URL.
        if (gateway_) gateway_->ReportBypass(scheme.toStdString() + " request to a loopback host goes direct "
                                             "(Chromium's implicit loopback bypass; start with --forward-loopback "
                                             "to forward it)");
        return;
    }
    ++counts_.proxied;
    if (scheme == QLatin1String("ws") || scheme == QLatin1String("wss")) ++counts_.webSockets;
}
