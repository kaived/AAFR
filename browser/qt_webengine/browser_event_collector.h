#pragma once

#include <QHash>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QUrl>

#include <chrono>
#include <cstdint>
#include <optional>

#include "network/telemetry/telemetry_event.h"

class QWebEngineLoadingInfo;
class QWebEngineView;
class TabManager;

namespace aafr::telemetry {
class TelemetryPipeline;
}

// Turns Qt WebEngine page-level signals into telemetry events. These are
// navigation lifecycle observations, not individual HTTP requests: Qt does not
// expose subresource loads, redirects or socket timing through these signals.
class BrowserEventCollector final : public QObject
{
    Q_OBJECT

public:
    enum class NavigationStatus { Idle, Loading, Succeeded, Failed, Stopped, RendererTerminated };

    struct TabSnapshot {
        std::uint64_t tabId = 0;
        std::optional<std::uint64_t> navigationId;
        NavigationStatus status = NavigationStatus::Idle;
        int progress = 0;
        QString redactedUrl;
        QString error;
        // Duration of the most recently completed navigation, monotonic clock.
        std::optional<double> lastNavigationMs;
        std::optional<std::uint64_t> lastNavigationId;
    };

    BrowserEventCollector(TabManager *tabs, aafr::telemetry::TelemetryPipeline &pipeline,
                          QObject *parent = nullptr);
    ~BrowserEventCollector() override;

    std::optional<TabSnapshot> currentTab() const;
    std::optional<TabSnapshot> tab(QWebEngineView *view) const;
    std::optional<std::uint64_t> tabId(QWebEngineView *view) const;
    int trackedTabCount() const { return tabs_.size(); }

    // Removes credentials, query and fragment. file: URLs keep only the file
    // name; data:, blob: and javascript: keep only the scheme.
    static QString redactUrl(const QUrl &url);
    static QString toString(NavigationStatus status);

private:
    struct TabRecord {
        TabSnapshot snapshot;
        std::chrono::steady_clock::time_point navigationStart{};
        int lastReportedProgress = -1;
    };

    void attach(QWebEngineView *view);
    void onLoadingChanged(QWebEngineView *view, const QWebEngineLoadingInfo &info);
    void onCurrentChanged(int index);
    void onTabClosed(QWebEngineView *view);
    aafr::telemetry::TelemetryEvent tabEvent(aafr::telemetry::EventType type,
                                             const TabRecord &record) const;
    void publish(aafr::telemetry::TelemetryEvent event);

    QPointer<TabManager> manager_;
    aafr::telemetry::TelemetryPipeline &pipeline_;
    QHash<QWebEngineView *, TabRecord> tabs_;
    QWebEngineView *activeView_ = nullptr;
    std::uint64_t nextTabId_ = 1;
    std::uint64_t nextNavigationId_ = 1;
};
