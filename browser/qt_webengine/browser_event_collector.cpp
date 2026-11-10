#include "browser_event_collector.h"
#include "tab_manager.h"

#include <QFileInfo>
#include <QWebEngineLoadingInfo>
#include <QWebEnginePage>
#include <QWebEngineView>

#include "network/telemetry/telemetry_pipeline.h"

using aafr::telemetry::AttributionQuality;
using aafr::telemetry::Availability;
using aafr::telemetry::EventType;
using aafr::telemetry::MeasurementScope;
using aafr::telemetry::Source;
using aafr::telemetry::TelemetryEvent;

namespace {
std::string utf8(const QString &text) { return text.toStdString(); }

double elapsedMs(std::chrono::steady_clock::time_point start)
{
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
        .count();
}

QString domainName(QWebEngineLoadingInfo::ErrorDomain domain)
{
    switch (domain) {
    case QWebEngineLoadingInfo::NoErrorDomain: return QStringLiteral("none");
    case QWebEngineLoadingInfo::InternalErrorDomain: return QStringLiteral("internal");
    case QWebEngineLoadingInfo::ConnectionErrorDomain: return QStringLiteral("connection");
    case QWebEngineLoadingInfo::CertificateErrorDomain: return QStringLiteral("certificate");
    case QWebEngineLoadingInfo::HttpErrorDomain: return QStringLiteral("http");
    case QWebEngineLoadingInfo::FtpErrorDomain: return QStringLiteral("ftp");
    case QWebEngineLoadingInfo::DnsErrorDomain: return QStringLiteral("dns");
    case QWebEngineLoadingInfo::HttpStatusCodeDomain: return QStringLiteral("http_status");
    }
    return QStringLiteral("unknown");
}
}

BrowserEventCollector::BrowserEventCollector(TabManager *tabs,
                                             aafr::telemetry::TelemetryPipeline &pipeline,
                                             QObject *parent)
    : QObject(parent), manager_(tabs), pipeline_(pipeline)
{
    connect(tabs, &TabManager::webViewCreated, this, &BrowserEventCollector::attach);
    connect(tabs, &TabManager::tabClosed, this, &BrowserEventCollector::onTabClosed);
    connect(tabs, &QTabWidget::currentChanged, this, &BrowserEventCollector::onCurrentChanged);
    // Views that existed before the collector was attached.
    for (int i = 0; i < tabs->count(); ++i) {
        if (auto *view = qobject_cast<QWebEngineView *>(tabs->widget(i))) attach(view);
    }
    onCurrentChanged(tabs->currentIndex());
}

// Qt drops connections to this receiver automatically; tracked view pointers
// are never dereferenced after their destroyed() signal.
BrowserEventCollector::~BrowserEventCollector() = default;

QString BrowserEventCollector::redactUrl(const QUrl &url)
{
    if (url.isEmpty()) return {};
    const QString scheme = url.scheme().toLower();
    if (scheme == QLatin1String("about")) return url.adjusted(QUrl::RemoveQuery | QUrl::RemoveFragment).toString();
    if (scheme == QLatin1String("file")) {
        // Local paths usually contain the user's account name.
        return QStringLiteral("file:.../") + QFileInfo(url.path()).fileName();
    }
    if (scheme == QLatin1String("http") || scheme == QLatin1String("https")
        || scheme == QLatin1String("ws") || scheme == QLatin1String("wss")
        || scheme == QLatin1String("ftp")) {
        return url.adjusted(QUrl::RemoveUserInfo | QUrl::RemoveQuery | QUrl::RemoveFragment)
            .toString();
    }
    return scheme + QLatin1Char(':');
}

QString BrowserEventCollector::toString(NavigationStatus status)
{
    switch (status) {
    case NavigationStatus::Idle: return tr("Idle");
    case NavigationStatus::Loading: return tr("Loading");
    case NavigationStatus::Succeeded: return tr("Loaded");
    case NavigationStatus::Failed: return tr("Failed");
    case NavigationStatus::Stopped: return tr("Stopped");
    case NavigationStatus::RendererTerminated: return tr("Renderer terminated");
    }
    return tr("Unknown");
}

std::optional<BrowserEventCollector::TabSnapshot> BrowserEventCollector::currentTab() const
{
    if (!manager_) return std::nullopt;
    return tab(manager_->currentView());
}

std::optional<BrowserEventCollector::TabSnapshot> BrowserEventCollector::tab(QWebEngineView *view) const
{
    const auto found = tabs_.constFind(view);
    if (found == tabs_.cend()) return std::nullopt;
    return found->snapshot;
}

std::optional<std::uint64_t> BrowserEventCollector::tabId(QWebEngineView *view) const
{
    const auto found = tabs_.constFind(view);
    if (found == tabs_.cend()) return std::nullopt;
    return found->snapshot.tabId;
}

TelemetryEvent BrowserEventCollector::tabEvent(EventType type, const TabRecord &record) const
{
    TelemetryEvent event;
    event.type = type;
    event.source = Source::QtWebEngine;
    event.scope = MeasurementScope::Tab;
    event.attribution = AttributionQuality::Direct;
    event.tab_id = record.snapshot.tabId;
    return event;
}

void BrowserEventCollector::publish(TelemetryEvent event)
{
    pipeline_.Publish(std::move(event));
}

void BrowserEventCollector::attach(QWebEngineView *view)
{
    if (!view || tabs_.contains(view)) return;
    TabRecord record;
    record.snapshot.tabId = nextTabId_++;
    tabs_.insert(view, record);
    publish(tabEvent(EventType::TabCreated, record));

    // Covers destruction paths where TabManager reports no closure, such as
    // manager teardown; keeps tabs_ free of dangling keys.
    connect(view, &QObject::destroyed, this, [this, view] {
        tabs_.remove(view);
        if (activeView_ == view) activeView_ = nullptr;
    });
    connect(view->page(), &QWebEnginePage::loadingChanged, this,
            [this, view](const QWebEngineLoadingInfo &info) { onLoadingChanged(view, info); });
    connect(view, &QWebEngineView::loadProgress, this, [this, view](int progress) {
        auto found = tabs_.find(view);
        if (found == tabs_.end()) return;
        found->snapshot.progress = progress;
        // Record progress in coarse steps so the event log stays readable.
        const int step = progress / 25 * 25;
        if (found->snapshot.status != NavigationStatus::Loading
            || step <= found->lastReportedProgress)
            return;
        found->lastReportedProgress = step;
        TelemetryEvent event = tabEvent(EventType::LoadProgress, *found);
        event.scope = MeasurementScope::Navigation;
        event.navigation_id = found->snapshot.navigationId;
        event.metric = "navigation.progress";
        event.value = progress;
        event.unit = "percent";
        publish(std::move(event));
    });
    connect(view, &QWebEngineView::urlChanged, this, [this, view](const QUrl &url) {
        auto found = tabs_.find(view);
        if (found == tabs_.end()) return;
        const QString redacted = redactUrl(url);
        if (redacted == found->snapshot.redactedUrl) return;
        found->snapshot.redactedUrl = redacted;
        TelemetryEvent event = tabEvent(EventType::UrlChanged, *found);
        event.navigation_id = found->snapshot.navigationId;
        event.attributes.push_back({"url", utf8(redacted)});
        publish(std::move(event));
    });
    connect(view, &QWebEngineView::renderProcessTerminated, this,
            [this, view](QWebEnginePage::RenderProcessTerminationStatus status, int exitCode) {
        auto found = tabs_.find(view);
        if (found == tabs_.end()) return;
        found->snapshot.status = NavigationStatus::RendererTerminated;
        TelemetryEvent event = tabEvent(EventType::RendererTerminated, *found);
        event.navigation_id = found->snapshot.navigationId;
        event.attributes.push_back({"termination_status", std::to_string(static_cast<int>(status))});
        event.attributes.push_back({"exit_code", std::to_string(exitCode)});
        publish(std::move(event));
    });
}

void BrowserEventCollector::onLoadingChanged(QWebEngineView *view, const QWebEngineLoadingInfo &info)
{
    auto found = tabs_.find(view);
    if (found == tabs_.end()) return;
    TabRecord &record = *found;
    const QString url = redactUrl(info.url());

    if (info.status() == QWebEngineLoadingInfo::LoadStartedStatus) {
        record.snapshot.navigationId = nextNavigationId_++;
        record.snapshot.status = NavigationStatus::Loading;
        record.snapshot.progress = 0;
        record.snapshot.error.clear();
        record.navigationStart = std::chrono::steady_clock::now();
        record.lastReportedProgress = -1;
        TelemetryEvent event = tabEvent(EventType::NavigationStarted, record);
        event.scope = MeasurementScope::Navigation;
        event.navigation_id = record.snapshot.navigationId;
        event.attributes.push_back({"url", utf8(url)});
        publish(std::move(event));
        return;
    }

    // A completion without an observed start (collector attached mid-load)
    // cannot be timed or tied to a navigation id.
    const bool started = record.snapshot.status == NavigationStatus::Loading
                         && record.snapshot.navigationId.has_value();
    EventType type = EventType::NavigationSucceeded;
    switch (info.status()) {
    case QWebEngineLoadingInfo::LoadSucceededStatus:
        record.snapshot.status = NavigationStatus::Succeeded;
        record.snapshot.progress = 100;
        break;
    case QWebEngineLoadingInfo::LoadStoppedStatus:
        type = EventType::NavigationStopped;
        record.snapshot.status = NavigationStatus::Stopped;
        break;
    case QWebEngineLoadingInfo::LoadFailedStatus:
    default:
        type = EventType::NavigationFailed;
        record.snapshot.status = NavigationStatus::Failed;
        record.snapshot.error = info.errorString();
        break;
    }

    TelemetryEvent event = tabEvent(type, record);
    event.scope = MeasurementScope::Navigation;
    event.metric = "navigation.duration";
    event.unit = "ms";
    event.attributes.push_back({"url", utf8(url)});
    if (started) {
        const double duration = elapsedMs(record.navigationStart);
        event.navigation_id = record.snapshot.navigationId;
        event.value = duration;
        record.snapshot.lastNavigationMs = duration;
        record.snapshot.lastNavigationId = record.snapshot.navigationId;
    } else {
        event.availability = Availability::Unavailable;
        event.attribution = AttributionQuality::Unattributed;
        event.attributes.push_back({"note", "navigation start not observed"});
    }
    if (type == EventType::NavigationFailed) {
        event.attributes.push_back({"error_domain", utf8(domainName(info.errorDomain()))});
        event.attributes.push_back({"error_code", std::to_string(info.errorCode())});
        event.attributes.push_back({"error", utf8(info.errorString())});
    }
    publish(std::move(event));
}

void BrowserEventCollector::onCurrentChanged(int index)
{
    if (!manager_) return;
    auto *view = qobject_cast<QWebEngineView *>(manager_->widget(index));
    if (view == activeView_) return;
    activeView_ = view;
    const auto found = tabs_.constFind(view);
    if (found == tabs_.cend()) return;
    publish(tabEvent(EventType::TabActivated, *found));
}

void BrowserEventCollector::onTabClosed(QWebEngineView *view)
{
    const auto found = tabs_.find(view);
    if (found == tabs_.end()) return;
    TelemetryEvent event = tabEvent(EventType::TabClosed, *found);
    if (found->snapshot.status == NavigationStatus::Loading) {
        event.navigation_id = found->snapshot.navigationId;
        event.attributes.push_back({"closed_while_loading", "true"});
    }
    tabs_.erase(found);
    // Closed views may stay alive until DeferredDelete; stop listening now.
    // The view may be mid-destruction here, so only disconnect by sender
    // pointer: Qt tolerates this and never dereferences a dead sender.
    disconnect(view, nullptr, this, nullptr);
    if (view == activeView_) activeView_ = nullptr;
    publish(std::move(event));
}
