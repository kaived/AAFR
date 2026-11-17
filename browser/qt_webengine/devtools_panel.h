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
namespace aafr::netstate {
class NetworkStateService;
}
namespace aafr::forwarding {
class ForwardingService;
}
namespace aafr::runtime {
class AAFRRuntime;
}
class QSpinBox;
class QPlainTextEdit;

// Native Developer Tools-style monitoring panel. It displays telemetry and
// (when a provider exists) runtime state. It never switches algorithms: the
// only actions are an explicit manual algorithm run (V0.5), a request for a
// runtime evaluation and controlled loopback-topology conditions (V0.6), each
// on a button press. Refreshes on a timer while visible instead of repainting
// per event, and keeps the event table bounded.
class DevToolsPanel final : public QDockWidget
{
    Q_OBJECT

public:
    static constexpr int kRefreshIntervalMs = 250;
    static constexpr int kMaxEventRows = 500;
    // Metrics older than this are shown as stale.
    static constexpr double kStaleAfterSeconds = 10.0;
    static const QString &notIntegratedText();
    // Shown above the capability and eligibility tables while no runtime exists.
    static const QString &activeAlgorithmUnavailableText();

    DevToolsPanel(aafr::telemetry::TelemetryPipeline &pipeline, BrowserEventCollector *collector,
                  TSharkMonitor *monitor, QWidget *parent = nullptr);

    // Null means the runtime is not integrated (the V0.3 default).
    void setRuntimeProvider(const aafr::observability::IRuntimeObservabilityProvider *provider);

    // Null: the V0.4 sections say that no NetworkState pipeline is attached.
    // The service must outlive the panel.
    void setNetworkStateService(const aafr::netstate::NetworkStateService *service);

    // V0.5: controlled forwarding topology, verified implementations and
    // manual algorithm execution. Null: those sections say so. The service
    // must outlive the panel. Running an algorithm is the only action the
    // panel can take, and only on an explicit button press.
    void setForwardingService(aafr::forwarding::ForwardingService *service);
    // Shown in the Runtime tab until a V0.6 runtime exists.
    static const QString &adaptiveRuntimeText();
    // V0.6: the adaptive runtime. It also becomes the runtime provider, so
    // the V0.3 state and decision views show its records. Null detaches it.
    // The panel reads its status and may request a diagnostic evaluation or
    // apply a controlled loopback condition; it never switches anything.
    void setAdaptiveRuntime(aafr::runtime::AAFRRuntime *runtime);
    // Shown wherever runtime fields appear while no runtime is running.
    static const QString &adaptiveRuntimeNotRunningText();
    // Extra delay a "congest" button adds to one controlled edge.
    static constexpr double kCongestionDelayMs = 60.0;

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
    // Fills the interface dropdown from the monitor's `tshark -D` list.
    void populateInterfaces();
    void refreshRuntime();
    void refreshDecisions();
    // V0.6 adaptive runtime views.
    QWidget *buildAdaptivePane(QWidget *parent);
    void refreshAdaptive();
    void applyCondition(int which);
    // V0.4: NetworkState, constraints, capabilities and eligibility.
    void refreshState();
    // V0.5 controlled forwarding and algorithm execution.
    QWidget *buildForwardingPane(QWidget *parent);
    QWidget *buildExecutionPane(QWidget *parent);
    void refreshForwarding();
    void runSelectedAlgorithm();
    void appendEvents();
    void appendEventRow(const aafr::telemetry::TelemetryEvent &event);
    bool eventPassesFilter(const aafr::telemetry::TelemetryEvent &event) const;
    void toggleDockArea();
    void updateDockButton();

    aafr::telemetry::TelemetryPipeline &pipeline_;
    QPointer<BrowserEventCollector> collector_;
    QPointer<TSharkMonitor> monitor_;
    const aafr::observability::IRuntimeObservabilityProvider *runtime_ = nullptr;
    const aafr::netstate::NetworkStateService *stateService_ = nullptr;
    // Snapshot and window the tables currently show; avoids needless repaints.
    std::uint64_t shownStateId_ = 0;
    int shownWindow_ = -1;

    QTimer refreshTimer_;
    std::uint64_t lastEventId_ = 0;

    QTabWidget *tabs_ = nullptr;
    QToolButton *dockButton_ = nullptr;
    // Overview
    QHash<QString, QLabel *> overview_;
    // Network
    QLabel *captureStatus_ = nullptr;
    QComboBox *captureInterface_ = nullptr;
    QPushButton *captureButton_ = nullptr;
    QTableWidget *networkTable_ = nullptr;
    QLabel *protocolInfo_ = nullptr;
    QComboBox *stateWindow_ = nullptr;
    QLabel *stateSummary_ = nullptr;
    QTableWidget *stateTable_ = nullptr;
    // Runtime
    QLabel *runtimeBanner_ = nullptr;
    QHash<QString, QLabel *> runtimeFields_;
    QLabel *eligibilityNote_ = nullptr;
    QTableWidget *eligibilityTable_ = nullptr;
    QTableWidget *capabilityTable_ = nullptr;
    QTableWidget *constraintsTable_ = nullptr;
    // V0.5 forwarding
    aafr::forwarding::ForwardingService *forwarding_ = nullptr;
    QLabel *adaptiveRuntime_ = nullptr;
    QLabel *forwardingSummary_ = nullptr;
    QTableWidget *topologyTable_ = nullptr;
    QTableWidget *pathsTable_ = nullptr;
    QTableWidget *implementationTable_ = nullptr;
    QComboBox *algorithmChoice_ = nullptr;
    QSpinBox *seedSpin_ = nullptr;
    QSpinBox *iterationsSpin_ = nullptr;
    QCheckBox *forwardCheck_ = nullptr;
    QCheckBox *ignoreHostState_ = nullptr;
    QPushButton *runButton_ = nullptr;
    QPushButton *cancelButton_ = nullptr;
    QLabel *executionStatus_ = nullptr;
    QHash<QString, QLabel *> executionFields_;
    std::string trackedExecution_;
    // What the implementation table shows, to avoid recomputing eligibility.
    QString shownImplementationKey_;
    // Decisions
    QLabel *decisionsBanner_ = nullptr;
    QTableWidget *decisionsTable_ = nullptr;
    // V0.6 adaptive runtime
    aafr::runtime::AAFRRuntime *adaptive_ = nullptr;
    QHash<QString, QLabel *> adaptiveFields_;
    QTableWidget *gatesTable_ = nullptr;
    QPushButton *evaluateButton_ = nullptr;
    QPushButton *congestAButton_ = nullptr;
    QPushButton *congestBButton_ = nullptr;
    QPushButton *clearConditionButton_ = nullptr;
    QLabel *adaptiveAction_ = nullptr;
    QTableWidget *evidenceTable_ = nullptr;
    QPlainTextEdit *decisionDetail_ = nullptr;
    QString shownEvidenceKey_;
    // Events
    QTableWidget *eventsTable_ = nullptr;
    QComboBox *eventFilter_ = nullptr;
    QCheckBox *pauseEvents_ = nullptr;
};
