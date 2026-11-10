#pragma once

#include <QDockWidget>
#include <QHash>
#include <QPointer>
#include <QTimer>

#include <cstdint>

class BrowserEventCollector;
class NetworkMonitor;
class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QTabWidget;
class QTableWidget;
class QToolButton;
class TSharkMonitor;

namespace aafr::telemetry {
class TelemetryPipeline;
struct TelemetryEvent;
}
namespace aafr::observability {
class IRuntimeObservabilityProvider;
}

// Native Developer Tools-style monitoring panel. Read-only: it displays
// telemetry and (when a provider exists) runtime state, and never changes
// forwarding behaviour. Refreshes on a timer while visible instead of
// repainting per event, and keeps the event table bounded.
class DevToolsPanel final : public QDockWidget
{
    Q_OBJECT

public:
    static constexpr int kRefreshIntervalMs = 250;
    static constexpr int kMaxEventRows = 500;
    // Metrics older than this are shown as stale.
    static constexpr double kStaleAfterSeconds = 10.0;
    static const QString &notIntegratedText();

    DevToolsPanel(aafr::telemetry::TelemetryPipeline &pipeline, BrowserEventCollector *collector,
                  TSharkMonitor *monitor, QWidget *parent = nullptr);

    // Null means the runtime is not integrated (the V0.3 default).
    void setRuntimeProvider(const aafr::observability::IRuntimeObservabilityProvider *provider);

    QTabWidget *tabs() const { return tabs_; }
    // Pulls new telemetry and redraws every tab immediately.
    void refreshNow();

private:
    void buildOverview();
    void buildNetwork();
    void buildRuntime();
    void buildDecisions();
    void buildEvents();
    void refreshOverview();
    void refreshNetwork();
    void refreshCapture();
    void refreshRuntime();
    void refreshDecisions();
    void appendEvents();
    void appendEventRow(const aafr::telemetry::TelemetryEvent &event);
    bool eventPassesFilter(const aafr::telemetry::TelemetryEvent &event) const;
    void toggleDockArea();
    void updateDockButton();

    aafr::telemetry::TelemetryPipeline &pipeline_;
    QPointer<BrowserEventCollector> collector_;
    QPointer<TSharkMonitor> monitor_;
    const aafr::observability::IRuntimeObservabilityProvider *runtime_ = nullptr;

    QTimer refreshTimer_;
    std::uint64_t lastEventId_ = 0;

    QTabWidget *tabs_ = nullptr;
    QToolButton *dockButton_ = nullptr;
    // Overview
    QHash<QString, QLabel *> overview_;
    // Network
    QLabel *captureStatus_ = nullptr;
    QLineEdit *captureInterface_ = nullptr;
    QPushButton *captureButton_ = nullptr;
    QTableWidget *networkTable_ = nullptr;
    QLabel *protocolInfo_ = nullptr;
    // Runtime
    QLabel *runtimeBanner_ = nullptr;
    QHash<QString, QLabel *> runtimeFields_;
    // Decisions
    QLabel *decisionsBanner_ = nullptr;
    QTableWidget *decisionsTable_ = nullptr;
    // Events
    QTableWidget *eventsTable_ = nullptr;
    QComboBox *eventFilter_ = nullptr;
    QCheckBox *pauseEvents_ = nullptr;
};
