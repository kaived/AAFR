#pragma once

// V0.7.3 browser forwarding modes and Qt WebEngine proxy integration.
//
// Qt WebEngine has no per-profile or per-request proxy routing; its proxy is
// process-wide and read when the WebEngine context initialises. AAFR
// therefore uses Chromium's supported command-line switches, passed through
// QTWEBENGINE_CHROMIUM_FLAGS before QApplication (and before any
// QWebEngineProfile) exists:
//
//   --proxy-server=http://127.0.0.1:<gateway port>   (HTTP, HTTPS, ws, wss)
//   --disable-quic                                    (no HTTP/3 around the proxy)
//   --proxy-bypass-list=<-loopback>                   (only with forward_loopback)
//
// The gateway port must be known first, so the gateway binds its loopback
// listener here, before Qt starts. Changing the mode needs a restart: the
// flags cannot change for a running process. Direct mode sets nothing and
// creates no gateway; the browser behaves exactly as V0.7.2.
//
// Without forward_loopback, Chromium's implicit rule sends localhost and
// 127.0.0.0/8 directly; such requests are reported as ProxyBypassDetected
// (never as forwarded).

#include <QString>
#include <QWebEngineUrlRequestInterceptor>

#include <memory>
#include <optional>

#include "routing/gateway/browser_forwarding_gateway.h"

enum class BrowserForwardingMode { Direct, Controlled, Diagnostic };
const char *toString(BrowserForwardingMode mode);
std::optional<BrowserForwardingMode> parseBrowserForwardingMode(const QString &text);

struct BrowserForwardingOptions {
    BrowserForwardingMode mode = BrowserForwardingMode::Direct;
    // Send localhost / 127.0.0.0/8 through the gateway too (controlled
    // experiments with local servers); also lets the egress reach loopback.
    bool forwardLoopback = false;
    // Let the egress reach private (RFC 1918) addresses.
    bool allowPrivate = false;

    // From argv (--forwarding=<mode>, --forward-loopback) and the environment
    // (AAFR_BROWSER_FORWARDING, AAFR_FORWARD_LOOPBACK=1,
    // AAFR_GATEWAY_ALLOW_PRIVATE=1). Callable before QApplication exists.
    static BrowserForwardingOptions fromArguments(int argc, char **argv, QString *error);
};

class BrowserForwardingSetup
{
public:
    // Must run before QApplication. Direct mode returns a setup without a
    // gateway. Null (with an error) when the gateway cannot bind: the caller
    // must not start the browser then, since it would silently go direct.
    static std::unique_ptr<BrowserForwardingSetup> prepare(const BrowserForwardingOptions &options, QString *error);

    const BrowserForwardingOptions &options() const { return options_; }
    BrowserForwardingMode mode() const { return options_.mode; }
    // Null in direct mode.
    aafr::gateway::AAFRBrowserForwardingGateway *gateway() const { return gateway_.get(); }
    // The switches handed to Chromium (empty in direct mode).
    const QString &chromiumFlags() const { return flags_; }

private:
    BrowserForwardingOptions options_;
    std::unique_ptr<aafr::gateway::AAFRBrowserForwardingGateway> gateway_;
    QString flags_;
};

// Observes (never blocks or rewrites) every request the profile issues and
// reports the ones that will bypass the gateway. Keeps counts only: no URL is
// stored.
class ForwardingRequestObserver final : public QWebEngineUrlRequestInterceptor
{
public:
    ForwardingRequestObserver(aafr::gateway::AAFRBrowserForwardingGateway *gateway, bool forwardLoopback,
                              QObject *parent = nullptr);
    void interceptRequest(QWebEngineUrlRequestInfo &info) override;

    struct Counts {
        quint64 proxied = 0;      // http/https/ws/wss expected through the gateway
        quint64 bypassed = 0;     // network requests the proxy rules send direct
        quint64 nonNetwork = 0;   // data:, blob:, qrc:, file:, chrome:, ...
        // Of the proxied ones: WebSocket handshakes (ws/wss), which Chromium
        // tunnels through CONNECT; their frames are opaque to AAFR.
        quint64 webSockets = 0;
    };
    Counts counts() const { return counts_; }

private:
    aafr::gateway::AAFRBrowserForwardingGateway *gateway_;
    bool forwardLoopback_;
    Counts counts_;
};
