#include "devtools_panel.h"

#include "browser_event_collector.h"
#include "browser_theme.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMainWindow>
#include <QPushButton>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QScrollArea>
#include <QSpinBox>
#include <QTabWidget>
#include <QTableWidget>
#include <QToolButton>
#include <QVBoxLayout>

#include "algorithms/eligibility_evaluator.h"
#include "algorithms/execution/algorithm_execution_engine.h"
#include "network/network_state/network_state_service.h"
#include "routing/controller/forwarding_service.h"
#include "network/telemetry/telemetry_pipeline.h"
#include "network/tshark/tshark_fields.h"
#include "network/tshark/tshark_monitor.h"
#include "runtime/observability/runtime_contracts.h"

using aafr::telemetry::EventType;
using aafr::telemetry::Source;
using aafr::telemetry::TelemetryEvent;
namespace obs = aafr::observability;
namespace ns = aafr::netstate;
namespace alg = aafr::algorithms;

namespace {
QString text(const std::string &value) { return QString::fromStdString(value); }

QString formatWall(std::chrono::system_clock::time_point time)
{
    const qint64 ms = std::chrono::duration_cast<std::chrono::milliseconds>(time.time_since_epoch()).count();
    return QDateTime::fromMSecsSinceEpoch(ms).toString(QStringLiteral("HH:mm:ss.zzz"));
}

QString formatWallFull(std::chrono::system_clock::time_point time)
{
    const qint64 ms = std::chrono::duration_cast<std::chrono::milliseconds>(time.time_since_epoch()).count();
    return QDateTime::fromMSecsSinceEpoch(ms).toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz"));
}

QString formatValue(double value, const std::string &unit)
{
    if (unit == "ms") return QLocale().toString(value, 'f', value < 10 ? 2 : 1);
    if (unit == "Mbps") return QLocale().toString(value, 'f', value < 1 ? 3 : 2);
    if (unit == "percent") return QString::number(qRound(value));
    return QLocale().toString(static_cast<qlonglong>(qRound64(value)));
}

QString attributesText(const TelemetryEvent &event)
{
    QStringList parts;
    for (const auto &attribute : event.attributes)
        parts << text(attribute.key) + QLatin1Char('=') + text(attribute.value);
    parts << QStringLiteral("scope=%1").arg(aafr::telemetry::ToString(event.scope));
    parts << QStringLiteral("attribution=%1").arg(aafr::telemetry::ToString(event.attribution));
    if (event.availability != aafr::telemetry::Availability::Available)
        parts << QStringLiteral("availability=%1").arg(aafr::telemetry::ToString(event.availability));
    return parts.join(QStringLiteral("; "));
}

QLabel *valueLabel(const QString &name, QWidget *parent)
{
    auto *label = new QLabel(parent);
    label->setObjectName(name);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    label->setWordWrap(true);
    return label;
}

QTableWidget *makeTable(const QString &name, const QStringList &headers, QWidget *parent)
{
    auto *table = new QTableWidget(0, headers.size(), parent);
    table->setObjectName(name);
    table->setHorizontalHeaderLabels(headers);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setWordWrap(false);
    table->verticalHeader()->hide();
    table->verticalHeader()->setDefaultSectionSize(22);
    table->horizontalHeader()->setStretchLastSection(true);
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    return table;
}

void setCell(QTableWidget *table, int row, int column, const QString &value)
{
    auto *item = table->item(row, column);
    if (!item) {
        item = new QTableWidgetItem;
        table->setItem(row, column, item);
    }
    if (item->text() != value) item->setText(value);
}

// A feature value in its own unit. A missing value is a dash, never zero.
QString featureValue(const ns::Feature &feature)
{
    if (!feature.value) return QStringLiteral("—");
    switch (feature.unit) {
    case ns::Unit::Milliseconds: return QLocale().toString(*feature.value, 'f', 2);
    case ns::Unit::Mbps: return QLocale().toString(*feature.value, 'f', 3);
    case ns::Unit::Ratio: return QLocale().toString(*feature.value, 'f', 4);
    case ns::Unit::Percent: return QLocale().toString(*feature.value, 'f', 1);
    case ns::Unit::Count:
    case ns::Unit::Bytes: break;
    }
    return QLocale().toString(static_cast<qlonglong>(qRound64(*feature.value)));
}

QString checkNames(const std::vector<alg::EligibilityCheck> &checks)
{
    QStringList names;
    for (const auto check : checks) names << QString::fromLatin1(alg::ToString(check));
    return names.isEmpty() ? QObject::tr("none") : names.join(QStringLiteral(", "));
}

template <typename Items, typename ToText>
QString joined(const Items &items, ToText toText)
{
    QStringList parts;
    for (const auto &item : items) parts << toText(item);
    return parts.isEmpty() ? QObject::tr("none") : parts.join(QStringLiteral(", "));
}

QString metricLabel(const std::string &metric)
{
    static const QHash<QString, QString> labels = {
        {"net.tcp.handshake_rtt.min", QObject::tr("Latency: TCP handshake RTT (min)")},
        {"net.tcp.handshake_rtt.mean", QObject::tr("Latency: TCP handshake RTT (mean)")},
        {"net.tcp.handshake_rtt.max", QObject::tr("Latency: TCP handshake RTT (max)")},
        {"net.tcp.ack_rtt.latest", QObject::tr("ACK delay, both directions (latest)")},
        {"net.tcp.ack_rtt.min", QObject::tr("ACK delay, both directions (min)")},
        {"net.tcp.ack_rtt.mean", QObject::tr("ACK delay, both directions (mean)")},
        {"net.tcp.ack_rtt.max", QObject::tr("ACK delay, both directions (max)")},
        {"net.tcp.ack_rtt.jitter", QObject::tr("ACK delay jitter")},
        {"net.throughput", QObject::tr("Throughput (both directions)")},
        {"net.tcp.retransmissions", QObject::tr("TCP retransmissions")},
        {"net.dns.response_time.mean", QObject::tr("DNS response time (mean)")},
        {"net.dns.response_time.max", QObject::tr("DNS response time (max)")},
        {"net.dns.responses", QObject::tr("DNS responses")},
        {"net.packets", QObject::tr("Packets")},
        {"net.bytes", QObject::tr("Bytes")},
        {"net.tcp.packets", QObject::tr("TCP packets")},
        {"net.udp.packets", QObject::tr("UDP packets")},
        {"net.quic.packets", QObject::tr("QUIC packets")},
        {"net.capture.packets_total", QObject::tr("Packets since capture start")},
        {"net.capture.bytes_total", QObject::tr("Bytes since capture start")},
    };
    return labels.value(text(metric), text(metric));
}

QString unitFor(const std::string &metric)
{
    if (metric.find("rtt") != std::string::npos || metric.find("response_time") != std::string::npos)
        return QStringLiteral("ms");
    if (metric == "net.throughput") return QStringLiteral("Mbps");
    if (metric.find("bytes") != std::string::npos) return QStringLiteral("bytes");
    if (metric == "net.tcp.retransmissions") return QStringLiteral("segments / interval");
    if (metric == "net.dns.responses") return QStringLiteral("responses / interval");
    if (metric.find("total") != std::string::npos) return QStringLiteral("packets");
    return QStringLiteral("packets / interval");
}
}

const QString &DevToolsPanel::notIntegratedText()
{
    static const QString value = tr("Not available — runtime not integrated");
    return value;
}

const QString &DevToolsPanel::adaptiveRuntimeText()
{
    static const QString value = tr("Adaptive Runtime: Not integrated");
    return value;
}

const QString &DevToolsPanel::activeAlgorithmUnavailableText()
{
    static const QString value = tr("Runtime not integrated — active algorithm unavailable.");
    return value;
}

DevToolsPanel::DevToolsPanel(aafr::telemetry::TelemetryPipeline &pipeline,
                             BrowserEventCollector *collector, TSharkMonitor *monitor,
                             QWidget *parent)
    : QDockWidget(tr("AAFR Developer Tools"), parent), pipeline_(pipeline),
      collector_(collector), monitor_(monitor)
{
    setObjectName("devToolsDock");
    setAllowedAreas(Qt::BottomDockWidgetArea | Qt::RightDockWidgetArea);
    setFeatures(QDockWidget::DockWidgetClosable | QDockWidget::DockWidgetMovable
                | QDockWidget::DockWidgetFloatable);
    setMinimumSize(320, 160);

    // Qt's own title-bar buttons are about 10 px and sit against the window
    // edge. A title bar of our own gives them a usable size and a right
    // margin. Presses on the bar itself still reach the dock, so dragging and
    // double-click-to-float work as before.
    auto *titleBar = new QWidget(this);
    titleBar->setObjectName("devToolsTitleBar");
    titleBar->setAttribute(Qt::WA_StyledBackground);
    auto *titleLayout = new QHBoxLayout(titleBar);
    titleLayout->setContentsMargins(8, 3, 8, 3);
    titleLayout->setSpacing(4);
    auto *title = new QLabel(windowTitle(), titleBar);
    title->setObjectName("devToolsTitle");
    titleLayout->addWidget(title);
    titleLayout->addStretch(1);
    const auto titleButton = [titleBar, titleLayout](const char *name, BrowserTheme::Icon icon,
                                                     const QString &label) {
        auto *button = new QToolButton(titleBar);
        button->setObjectName(name);
        button->setIcon(BrowserTheme::icon(icon));
        button->setIconSize(QSize(16, 16));
        button->setFixedSize(28, 28);
        button->setAccessibleName(label);
        button->setToolTip(label);
        button->setFocusPolicy(Qt::TabFocus);
        titleLayout->addWidget(button);
        return button;
    };
    auto *floatButton = titleButton("devToolsFloat", BrowserTheme::Icon::Restore, tr("Float or dock the panel"));
    connect(floatButton, &QToolButton::clicked, this, [this] { setFloating(!isFloating()); });
    auto *closeButton = titleButton("devToolsClose", BrowserTheme::Icon::Close, tr("Close Developer Tools"));
    connect(closeButton, &QToolButton::clicked, this, &QDockWidget::close);
    setTitleBarWidget(titleBar);

    tabs_ = new QTabWidget(this);
    tabs_->setObjectName("devToolsTabs");
    tabs_->setDocumentMode(true);
    dockButton_ = new QToolButton(tabs_);
    dockButton_->setObjectName("devToolsDockSide");
    dockButton_->setAutoRaise(true);
    tabs_->setCornerWidget(dockButton_, Qt::TopRightCorner);
    connect(dockButton_, &QToolButton::clicked, this, &DevToolsPanel::toggleDockArea);
    connect(this, &QDockWidget::dockLocationChanged, this, &DevToolsPanel::updateDockButton);
    connect(this, &QDockWidget::topLevelChanged, this, &DevToolsPanel::updateDockButton);
    setWidget(tabs_);

    buildOverview();
    buildNetwork();
    buildRuntime();
    buildDecisions();
    buildEvents();
    updateDockButton();

    refreshTimer_.setInterval(kRefreshIntervalMs);
    connect(&refreshTimer_, &QTimer::timeout, this, &DevToolsPanel::refreshNow);
    // Telemetry keeps flowing into the pipeline while hidden; the panel only
    // pulls and repaints while someone can see it.
    connect(this, &QDockWidget::visibilityChanged, this, [this](bool visible) {
        if (visible) {
            // List interfaces (not a capture) the first time the panel opens.
            if (monitor_ && monitor_->isAvailable() && !monitor_->interfacesLoaded())
                monitor_->refreshInterfaces();
            refreshNow();
            refreshTimer_.start();
        } else {
            refreshTimer_.stop();
        }
    });
    if (monitor_) {
        connect(monitor_, &NetworkMonitor::stateChanged, this, [this] {
            refreshCapture();
            refreshOverview();
        });
        connect(monitor_, &TSharkMonitor::interfacesChanged, this, &DevToolsPanel::populateInterfaces);
    }
    refreshNow();
}

void DevToolsPanel::setRuntimeProvider(const obs::IRuntimeObservabilityProvider *provider)
{
    runtime_ = provider;
    refreshRuntime();
    refreshDecisions();
    refreshState();
}

void DevToolsPanel::setNetworkStateService(const ns::NetworkStateService *service)
{
    stateService_ = service;
    shownStateId_ = 0;
    refreshState();
}

void DevToolsPanel::setForwardingService(aafr::forwarding::ForwardingService *service)
{
    forwarding_ = service;
    trackedExecution_.clear();
    shownImplementationKey_.clear();
    pathsTable_->setRowCount(0);
    refreshForwarding();
}

void DevToolsPanel::buildOverview()
{
    auto *scroll = new QScrollArea(tabs_);
    scroll->setObjectName("devToolsOverviewScroll");
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto *page = new QWidget(scroll);
    page->setObjectName("devToolsOverview");
    auto *form = new QFormLayout(page);
    form->setContentsMargins(12, 10, 12, 10);
    // Labels and values each start at one shared left edge.
    form->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    form->setFormAlignment(Qt::AlignLeft | Qt::AlignTop);
    form->setHorizontalSpacing(24);
    const QList<QPair<QString, QString>> rows = {
        {"activeTab", tr("Active tab")},
        {"navigationStatus", tr("Navigation status")},
        {"pageLoad", tr("Page-load duration")},
        {"collection", tr("Telemetry collection")},
        {"session", tr("Monitoring session")},
        {"capture", tr("Network capture")},
        {"metrics", tr("Network metrics")},
        {"networkState", tr("NetworkState")},
        {"stateUpdated", tr("Latest state update")},
        {"featureCompleteness", tr("Feature completeness")},
        {"dataQuality", tr("Data quality")},
        {"eligibleAlgorithms", tr("Eligible algorithms")},
        {"forwardingEngine", tr("Forwarding engine")},
        {"verifiedAlgorithms", tr("Verified algorithms")},
        {"latestExecution", tr("Latest execution")},
        {"latestForwarding", tr("Latest forwarding outcome")},
    };
    for (const auto &row : rows) {
        auto *label = valueLabel("overview_" + row.first, page);
        overview_.insert(row.first, label);
        form->addRow(row.second + QLatin1Char(':'), label);
    }
    scroll->setWidget(page);
    tabs_->addTab(scroll, tr("Overview"));
}

void DevToolsPanel::buildNetwork()
{
    auto *page = new QWidget(tabs_);
    page->setObjectName("devToolsNetwork");
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(8, 8, 8, 8);
    auto *controls = new QHBoxLayout;
    captureStatus_ = valueLabel("captureStatus", page);
    // A list rather than free text: TShark rejects names that are not exact.
    captureInterface_ = new QComboBox(page);
    captureInterface_->setObjectName("captureInterface");
    captureInterface_->setAccessibleName(tr("Capture interface"));
    captureInterface_->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    captureInterface_->setMinimumContentsLength(16);
    populateInterfaces();
    captureButton_ = new QPushButton(page);
    captureButton_->setObjectName("captureToggle");
    connect(captureButton_, &QPushButton::clicked, this, [this] {
        if (!monitor_) return;
        const auto state = monitor_->state();
        if (state == NetworkMonitor::State::Running || state == NetworkMonitor::State::Starting) {
            monitor_->stop();
        } else {
            monitor_->setInterfaceName(captureInterface_->currentData().toString());
            monitor_->start();
        }
        refreshCapture();
    });
    controls->addWidget(captureStatus_, 1);
    controls->addWidget(captureInterface_);
    controls->addWidget(captureButton_);
    layout->addLayout(controls);

    networkTable_ = makeTable("networkTable",
                              {tr("Metric"), tr("Value"), tr("Unit"), tr("Source"), tr("Scope"), tr("Freshness")},
                              page);
    const auto &names = aafr::tshark::NetworkMetricNames();
    networkTable_->setRowCount(static_cast<int>(names.size()));
    for (int row = 0; row < static_cast<int>(names.size()); ++row) {
        setCell(networkTable_, row, 0, metricLabel(names[row]));
        networkTable_->item(row, 0)->setData(Qt::UserRole, text(names[row]));
    }
    networkTable_->setColumnWidth(0, 260);
    networkTable_->setColumnWidth(1, 110);
    networkTable_->setColumnWidth(2, 140);
    networkTable_->setColumnWidth(3, 80);
    networkTable_->setColumnWidth(4, 130);
    // V0.4: normalized features of one observation window, beside the raw
    // latest-interval metrics. Inner tabs give each table the full height.
    auto *statePane = new QWidget(page);
    auto *stateLayout = new QVBoxLayout(statePane);
    stateLayout->setContentsMargins(0, 4, 0, 0);
    auto *stateHeader = new QHBoxLayout;
    stateSummary_ = valueLabel("stateSummary", statePane);
    stateWindow_ = new QComboBox(statePane);
    stateWindow_->setObjectName("stateWindow");
    stateWindow_->setAccessibleName(tr("Observation window"));
    stateWindow_->setToolTip(tr("Observation window the features below are computed over"));
    stateHeader->addWidget(stateSummary_, 1);
    stateHeader->addWidget(stateWindow_);
    stateLayout->addLayout(stateHeader);
    stateTable_ = makeTable("stateTable",
                            {tr("Feature"), tr("Value"), tr("Unit"), tr("Kind"), tr("Quality"),
                             tr("Samples"), tr("Freshness"), tr("Source / scope / attribution"),
                             tr("Reason")},
                            statePane);
    stateTable_->setColumnWidth(0, 210);
    stateTable_->setColumnWidth(1, 90);
    stateTable_->setColumnWidth(2, 55);
    stateTable_->setColumnWidth(3, 75);
    stateTable_->setColumnWidth(4, 140);
    stateTable_->setColumnWidth(5, 65);
    stateTable_->setColumnWidth(6, 85);
    stateTable_->setColumnWidth(7, 260);
    stateLayout->addWidget(stateTable_, 1);
    connect(stateWindow_, &QComboBox::currentIndexChanged, this, [this] { refreshState(); });
    auto *views = new QTabWidget(page);
    views->setObjectName("networkViews");
    views->setDocumentMode(true);
    // The raw metrics keep their own notes, so the feature table is not
    // squeezed by text that describes the other view.
    auto *latestPane = new QWidget(page);
    auto *latestLayout = new QVBoxLayout(latestPane);
    latestLayout->setContentsMargins(0, 4, 0, 0);
    latestLayout->addWidget(networkTable_, 1);
    views->addTab(latestPane, tr("Latest interval"));
    views->addTab(statePane, tr("NetworkState features"));
    views->addTab(buildForwardingPane(views), tr("Controlled forwarding"));
    layout->addWidget(views, 1);
    protocolInfo_ = valueLabel("protocolInfo", latestPane);
    latestLayout->addWidget(protocolInfo_);
    auto *note = new QLabel(tr("Capture-interface metrics cover all host traffic and are not attributed to "
                               "browser tabs. Latency is the TCP handshake round trip of each connection active in "
                               "the interval. ACK delay is TShark's ACK RTT in both directions and includes this "
                               "host's own near-instant ACKs, so it reads lower than latency. Retransmissions are "
                               "TShark's heuristic and are not an exact packet-loss rate."), latestPane);
    note->setObjectName("networkScopeNote");
    note->setWordWrap(true);
    latestLayout->addWidget(note);
    tabs_->addTab(page, tr("Network"));
}

void DevToolsPanel::buildRuntime()
{
    auto *page = new QWidget(tabs_);
    page->setObjectName("devToolsRuntime");
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(12, 10, 12, 10);
    // The V0.6 adaptive runtime is kept apart from V0.5 manual execution.
    adaptiveRuntime_ = valueLabel("adaptiveRuntimeStatus", page);
    adaptiveRuntime_->setText(adaptiveRuntimeText());
    layout->addWidget(adaptiveRuntime_);
    runtimeBanner_ = valueLabel("runtimeBanner", page);
    layout->addWidget(runtimeBanner_);
    auto *statePage = new QWidget(page);
    statePage->setObjectName("runtimeStatePage");
    auto *form = new QFormLayout(statePage);
    form->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    form->setFormAlignment(Qt::AlignLeft | Qt::AlignTop);
    form->setHorizontalSpacing(24);
    const QList<QPair<QString, QString>> rows = {
        {"algorithm", tr("Active algorithm")},
        {"activatedAt", tr("Activation timestamp")},
        {"reason", tr("Activation reason")},
        {"state", tr("Runtime state")},
        {"lastEvaluation", tr("Last evaluation")},
    };
    for (const auto &row : rows) {
        auto *label = valueLabel("runtime_" + row.first, page);
        runtimeFields_.insert(row.first, label);
        form->addRow(row.second + QLatin1Char(':'), label);
    }

    // V0.4 diagnostics, kept apart from the runtime state: what each
    // algorithm could do and whether it may be considered. None of this is an
    // active algorithm, a selection or a switch.
    eligibilityNote_ = valueLabel("eligibilityNote", page);
    layout->addWidget(eligibilityNote_);
    auto *diagnostics = new QTabWidget(page);
    diagnostics->setObjectName("runtimeDiagnostics");
    diagnostics->setDocumentMode(true);
    eligibilityTable_ = makeTable("eligibilityTable",
                                  {tr("Algorithm"), tr("Eligibility"), tr("Reason codes"),
                                   tr("Failed checks"), tr("Unknown checks"), tr("Passed checks"),
                                   tr("Missing information"), tr("Explanation")},
                                  diagnostics);
    eligibilityTable_->setColumnWidth(0, 210);
    eligibilityTable_->setColumnWidth(1, 110);
    eligibilityTable_->setColumnWidth(2, 320);
    eligibilityTable_->setColumnWidth(6, 180);
    capabilityTable_ = makeTable("capabilityTable",
                                 {tr("Algorithm"), tr("Family"), tr("Implementation"), tr("Problems"),
                                  tr("Topologies"), tr("Edge weights"), tr("Optimality"),
                                  tr("Time complexity"), tr("Required configuration"),
                                  tr("Required features")},
                                 diagnostics);
    capabilityTable_->setColumnWidth(0, 210);
    capabilityTable_->setColumnWidth(2, 180);
    constraintsTable_ = makeTable("constraintsTable",
                                  {tr("Constraint"), tr("Category"), tr("Value"), tr("Unit"),
                                   tr("Source"), tr("Validity"), tr("Explanation")},
                                  diagnostics);
    constraintsTable_->setColumnWidth(0, 270);
    constraintsTable_->setColumnWidth(2, 200);
    diagnostics->addTab(statePage, tr("Runtime state"));
    diagnostics->addTab(eligibilityTable_, tr("Eligibility"));
    diagnostics->addTab(capabilityTable_, tr("Capability registry"));
    diagnostics->addTab(constraintsTable_, tr("Constraints"));
    diagnostics->addTab(buildExecutionPane(diagnostics), tr("Algorithm execution"));
    layout->addWidget(diagnostics, 1);
    tabs_->addTab(page, tr("Runtime"));
}

void DevToolsPanel::buildDecisions()
{
    auto *page = new QWidget(tabs_);
    page->setObjectName("devToolsDecisions");
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(8, 8, 8, 8);
    decisionsBanner_ = valueLabel("decisionsBanner", page);
    layout->addWidget(decisionsBanner_);
    decisionsTable_ = makeTable("decisionsTable",
                                {tr("Time"), tr("Decision"), tr("Type"), tr("Previous → candidate"),
                                 tr("Reason code"), tr("Failed checks"), tr("Switch outcome"),
                                 tr("Explanation")},
                                page);
    layout->addWidget(decisionsTable_, 1);
    tabs_->addTab(page, tr("Decisions"));
}

void DevToolsPanel::buildEvents()
{
    auto *page = new QWidget(tabs_);
    page->setObjectName("devToolsEvents");
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(8, 8, 8, 8);
    auto *controls = new QHBoxLayout;
    eventFilter_ = new QComboBox(page);
    eventFilter_->setObjectName("eventFilter");
    eventFilter_->setAccessibleName(tr("Event source filter"));
    eventFilter_->addItem(tr("All sources"));
    eventFilter_->addItem(tr("Browser"));
    eventFilter_->addItem(tr("Network"));
    // NetworkState, constraint and eligibility diagnostics (not decisions).
    eventFilter_->addItem(tr("Diagnostics"));
    // V0.5 manual algorithm executions and forwarding outcomes.
    eventFilter_->addItem(tr("Forwarding"));
    pauseEvents_ = new QCheckBox(tr("Pause"), page);
    pauseEvents_->setObjectName("pauseEvents");
    auto *clear = new QPushButton(tr("Clear view"), page);
    clear->setObjectName("clearEvents");
    controls->addWidget(eventFilter_);
    controls->addWidget(pauseEvents_);
    controls->addStretch();
    controls->addWidget(clear);
    layout->addLayout(controls);
    eventsTable_ = makeTable("eventsTable",
                             {tr("Time"), tr("Session +s"), tr("Type"), tr("Source"), tr("Tab"),
                              tr("Navigation"), tr("Value"), tr("Details")},
                             page);
    eventsTable_->setColumnWidth(0, 95);
    eventsTable_->setColumnWidth(1, 80);
    eventsTable_->setColumnWidth(2, 160);
    eventsTable_->setColumnWidth(3, 95);
    eventsTable_->setColumnWidth(4, 45);
    eventsTable_->setColumnWidth(5, 80);
    eventsTable_->setColumnWidth(6, 250);
    layout->addWidget(eventsTable_, 1);
    connect(clear, &QPushButton::clicked, this, [this] { eventsTable_->setRowCount(0); });
    connect(eventFilter_, &QComboBox::currentIndexChanged, this, [this] {
        // Rebuild from retained history with the new filter.
        eventsTable_->setRowCount(0);
        lastEventId_ = 0;
        appendEvents();
    });
    connect(pauseEvents_, &QCheckBox::toggled, this, [this](bool paused) {
        if (!paused) appendEvents();
    });
    tabs_->addTab(page, tr("Events"));
}

void DevToolsPanel::refreshNow()
{
    appendEvents();
    refreshOverview();
    refreshNetwork();
    refreshCapture();
    refreshRuntime();
    refreshDecisions();
    refreshState();
    refreshForwarding();
}

void DevToolsPanel::refreshState()
{
    static const char *const overviewKeys[] = {"networkState", "stateUpdated", "featureCompleteness",
                                                "dataQuality", "eligibleAlgorithms"};
    eligibilityNote_->setText(runtime_ ? tr("Capability and eligibility diagnostics. Eligible means an "
                                            "algorithm may be considered; it is not the active "
                                            "algorithm and not a prediction of performance.")
                                       : activeAlgorithmUnavailableText()
                                             + tr(" The Eligibility, Capability registry and "
                                                  "Constraints tabs are diagnostics, not runtime state: "
                                                  "eligible means an algorithm may be considered, "
                                                  "nothing more."));
    const auto snapshot = stateService_ ? stateService_->Latest() : nullptr;
    if (!snapshot) {
        const QString none = tr("Not available — NetworkState pipeline not attached");
        for (const char *key : overviewKeys) overview_[key]->setText(none);
        stateSummary_->setText(none);
        stateTable_->setRowCount(0);
        eligibilityTable_->setRowCount(0);
        capabilityTable_->setRowCount(0);
        constraintsTable_->setRowCount(0);
        shownStateId_ = 0;
        return;
    }
    const ns::NetworkState &state = snapshot->state;
    const ns::QualitySummary &quality = state.quality;
    const QString dot = QStringLiteral(" · ");

    // ---- Overview ----
    overview_["networkState"]->setText(tr("%1 · state #%2 (schema %3) · %4")
                                           .arg(QString::fromLatin1(ns::ToString(state.validity)))
                                           .arg(state.state_id)
                                           .arg(state.schema_version)
                                           .arg(text(state.validity_reason)));
    overview_["stateUpdated"]->setText(
        formatWallFull(state.observed_at) + dot
        + (state.freshness_ms
               ? tr("newest measurement %1 s old").arg(*state.freshness_ms / 1000.0, 0, 'f', 1)
               : tr("no measurement accepted yet")));
    const ns::WindowSnapshot *primary = state.PrimaryWindow();
    overview_["featureCompleteness"]->setText(
        tr("%1 of %2 features usable (%3%) in the '%4' window (%5 s)")
            .arg(quality.valid)
            .arg(quality.Total())
            .arg(qRound(state.FeatureCompleteness() * 100.0))
            .arg(text(state.primary_window))
            .arg(primary ? primary->duration_ms / 1000 : 0));
    overview_["dataQuality"]->setText(
        tr("valid %1 · missing %2 · stale %3 · insufficient samples %4 · invalid %5 "
           "· rejected measurements %6")
            .arg(quality.valid)
            .arg(quality.missing)
            .arg(quality.stale)
            .arg(quality.insufficient_samples)
            .arg(quality.invalid)
            .arg(state.counters.rejected_invalid));
    overview_["eligibleAlgorithms"]->setText(
        tr("%1 of %2 eligible · %3 ineligible · %4 unknown · %5 not implemented "
           "(diagnostic only; nothing is selected)")
            .arg(snapshot->CountWithStatus(alg::EligibilityStatus::Eligible))
            .arg(snapshot->eligibility.size())
            .arg(snapshot->CountWithStatus(alg::EligibilityStatus::Ineligible))
            .arg(snapshot->CountWithStatus(alg::EligibilityStatus::Unknown))
            .arg(snapshot->CountWithStatus(alg::EligibilityStatus::NotImplemented)));

    // ---- Network: features of the chosen observation window ----
    if (stateWindow_->count() != static_cast<int>(state.windows.size())) {
        const QSignalBlocker blocker(stateWindow_);
        stateWindow_->clear();
        int primaryIndex = 0;
        for (const ns::WindowSnapshot &window : state.windows) {
            if (window.name == state.primary_window) primaryIndex = stateWindow_->count();
            stateWindow_->addItem(tr("%1 window (%2 s)").arg(text(window.name)).arg(window.duration_ms / 1000),
                                  text(window.name));
        }
        stateWindow_->setCurrentIndex(primaryIndex);
    }
    QStringList summary;
    summary << tr("NetworkState #%1").arg(state.state_id);
    summary << (state.capture_session_id.empty()
                    ? tr("no capture session")
                    : tr("capture session %1 (%2)").arg(text(state.capture_session_id),
                                                         QString::fromLatin1(ns::ToString(state.sources.capture))));
    summary << tr("network features cover %1 (all host traffic, not attributed to tabs)")
                   .arg(QString::fromLatin1(aafr::telemetry::ToString(state.network_scope)));
    if (state.browser.active_tab_id) {
        QString tab = tr("active tab %1").arg(*state.browser.active_tab_id);
        if (state.browser.active_tab_load_progress_percent)
            tab += tr(", load %1%").arg(qRound(*state.browser.active_tab_load_progress_percent));
        summary << tab;
    }
    if (state.browser.last_navigation_outcome)
        summary << tr("last navigation %1").arg(QString::fromLatin1(ns::ToString(*state.browser.last_navigation_outcome)));
    stateSummary_->setText(summary.join(dot));

    const int windowIndex = qBound(0, stateWindow_->currentIndex(), static_cast<int>(state.windows.size()) - 1);
    if (!state.windows.empty() && (state.state_id != shownStateId_ || windowIndex != shownWindow_)) {
        const ns::WindowSnapshot &window = state.windows[static_cast<std::size_t>(windowIndex)];
        stateTable_->setRowCount(static_cast<int>(window.features.size()));
        for (int row = 0; row < static_cast<int>(window.features.size()); ++row) {
            const ns::Feature &feature = window.features[static_cast<std::size_t>(row)];
            setCell(stateTable_, row, 0, text(feature.id));
            setCell(stateTable_, row, 1, featureValue(feature));
            setCell(stateTable_, row, 2, QString::fromLatin1(ns::ToString(feature.unit)));
            setCell(stateTable_, row, 3, QString::fromLatin1(ns::ToString(feature.kind)));
            setCell(stateTable_, row, 4, QString::fromLatin1(ns::ToString(feature.quality)));
            setCell(stateTable_, row, 5, QString::number(feature.sample_count));
            setCell(stateTable_, row, 6, feature.age_ms ? tr("%1 s ago").arg(*feature.age_ms / 1000.0, 0, 'f', 1)
                                                        : QStringLiteral("—"));
            setCell(stateTable_, row, 7,
                    QStringLiteral("%1 / %2 / %3")
                        .arg(QString::fromLatin1(aafr::telemetry::ToString(feature.source)),
                             QString::fromLatin1(aafr::telemetry::ToString(feature.scope)),
                             QString::fromLatin1(aafr::telemetry::ToString(feature.attribution))));
            setCell(stateTable_, row, 8, text(feature.quality_reason));
        }
        shownWindow_ = windowIndex;
    }
    if (state.state_id == shownStateId_) return;
    shownStateId_ = state.state_id;

    // ---- Runtime: eligibility, capability registry, constraints ----
    const auto displayName = [&snapshot](alg::AlgorithmId id) {
        for (const auto &capability : snapshot->capabilities)
            if (capability.id == id) return text(capability.display_name);
        return QString::fromLatin1(alg::ToString(id));
    };
    const auto plain = [](const std::string &value) { return text(value); };
    eligibilityTable_->setRowCount(static_cast<int>(snapshot->eligibility.size()));
    for (int row = 0; row < static_cast<int>(snapshot->eligibility.size()); ++row) {
        const alg::EligibilityResult &result = snapshot->eligibility[static_cast<std::size_t>(row)];
        setCell(eligibilityTable_, row, 0, displayName(result.algorithm));
        eligibilityTable_->item(row, 0)->setData(Qt::UserRole, QString::fromLatin1(alg::ToString(result.algorithm)));
        setCell(eligibilityTable_, row, 1, QString::fromLatin1(alg::ToString(result.status)));
        setCell(eligibilityTable_, row, 2, text(alg::ReasonCodesText(result.reason_codes)));
        setCell(eligibilityTable_, row, 3, checkNames(result.ChecksWith(alg::CheckOutcome::Failed)));
        setCell(eligibilityTable_, row, 4, checkNames(result.ChecksWith(alg::CheckOutcome::Unknown)));
        setCell(eligibilityTable_, row, 5, checkNames(result.ChecksWith(alg::CheckOutcome::Passed)));
        setCell(eligibilityTable_, row, 6, joined(result.missing_information, plain));
        setCell(eligibilityTable_, row, 7, text(result.explanation));
        eligibilityTable_->item(row, 7)->setToolTip(text(result.explanation));
    }
    capabilityTable_->setRowCount(static_cast<int>(snapshot->capabilities.size()));
    for (int row = 0; row < static_cast<int>(snapshot->capabilities.size()); ++row) {
        const alg::AlgorithmCapability &capability = snapshot->capabilities[static_cast<std::size_t>(row)];
        const alg::AlgorithmProperties &p = capability.properties;
        setCell(capabilityTable_, row, 0, text(capability.display_name));
        capabilityTable_->item(row, 0)->setData(Qt::UserRole, QString::fromLatin1(alg::ToString(capability.id)));
        capabilityTable_->item(row, 0)->setToolTip(joined(p.limitations, plain));
        setCell(capabilityTable_, row, 1, QString::fromLatin1(alg::ToString(p.family)));
        setCell(capabilityTable_, row, 2, QString::fromLatin1(alg::ToString(capability.availability)));
        setCell(capabilityTable_, row, 3, joined(p.problems, [](auto v) { return QString::fromLatin1(ns::ToString(v)); }));
        setCell(capabilityTable_, row, 4, joined(p.topologies, [](auto v) { return QString::fromLatin1(ns::ToString(v)); }));
        setCell(capabilityTable_, row, 5, QString::fromLatin1(alg::ToString(p.edge_weights)));
        setCell(capabilityTable_, row, 6, QString::fromLatin1(alg::ToString(p.optimality)));
        setCell(capabilityTable_, row, 7, text(p.time_complexity));
        setCell(capabilityTable_, row, 8, joined(p.required_configuration, plain));
        setCell(capabilityTable_, row, 9, joined(p.required_features, plain));
    }
    const auto &constraints = snapshot->constraints.constraints;
    constraintsTable_->setRowCount(static_cast<int>(constraints.size()));
    for (int row = 0; row < static_cast<int>(constraints.size()); ++row) {
        const ns::Constraint &constraint = constraints[static_cast<std::size_t>(row)];
        setCell(constraintsTable_, row, 0, text(constraint.id));
        setCell(constraintsTable_, row, 1, QString::fromLatin1(ns::ToString(constraint.category)));
        setCell(constraintsTable_, row, 2, text(ns::ValueText(constraint)));
        setCell(constraintsTable_, row, 3, text(constraint.unit));
        setCell(constraintsTable_, row, 4, QString::fromLatin1(ns::ToString(constraint.source)));
        setCell(constraintsTable_, row, 5, QString::fromLatin1(ns::ToString(constraint.validity)));
        setCell(constraintsTable_, row, 6, text(constraint.explanation));
    }
}

// ---- V0.5 controlled forwarding ----

namespace fwd = aafr::forwarding;

namespace {
QString pathText(const fwd::Path &path) { return QString::fromStdString(fwd::PathText(path)); }

QString measurementText(const fwd::Measurement &measurement, int precision)
{
    if (!measurement.value) return QObject::tr("unknown");
    return QLocale().toString(*measurement.value, 'f', precision);
}

QString msText(double ms) { return QLocale().toString(ms, 'f', ms < 10 ? 3 : 1) + QStringLiteral(" ms"); }
}

QWidget *DevToolsPanel::buildForwardingPane(QWidget *parent)
{
    auto *pane = new QWidget(parent);
    pane->setObjectName("forwardingPane");
    auto *layout = new QVBoxLayout(pane);
    layout->setContentsMargins(0, 4, 0, 0);
    forwardingSummary_ = valueLabel("forwardingSummary", pane);
    layout->addWidget(forwardingSummary_);
    topologyTable_ = makeTable("topologyTable",
                               {tr("From"), tr("To"), tr("Latency (ms)"), tr("Loss ratio"), tr("Bandwidth (Mbps)"),
                                tr("Provenance"), tr("Available"), tr("Edge cost c(e)"), tr("Requests at 'to'")},
                               pane);
    pathsTable_ = makeTable("forwardingPathsTable",
                            {tr("Candidate path"), tr("Hops"), tr("Latency (ms)"), tr("J(P)"), tr("Feasible"),
                             tr("Issues")},
                            pane);
    pathsTable_->setColumnWidth(0, 280);
    auto *views = new QTabWidget(pane);
    views->setObjectName("forwardingViews");
    views->setDocumentMode(true);
    views->addTab(topologyTable_, tr("Topology and edge measurements"));
    views->addTab(pathsTable_, tr("Available paths"));
    layout->addWidget(views, 1);
    auto *note = new QLabel(tr("Controlled application-layer topology: AAFR-owned relays on 127.0.0.1 only. "
                               "Edge values are configured, not measured; nothing here describes Internet "
                               "routing. Browser page loads do not pass through these relays."),
                            pane);
    note->setObjectName("forwardingScopeNote");
    note->setWordWrap(true);
    layout->addWidget(note);
    return pane;
}

QWidget *DevToolsPanel::buildExecutionPane(QWidget *parent)
{
    // The controls and result form are wider and taller than a docked strip
    // can be; scrolling keeps the dock freely resizable in both directions.
    auto *scroll = new QScrollArea(parent);
    scroll->setObjectName("executionScroll");
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto *pane = new QWidget(scroll);
    pane->setObjectName("executionPane");
    auto *layout = new QVBoxLayout(pane);
    layout->setContentsMargins(0, 4, 0, 0);

    auto *controls = new QHBoxLayout;
    algorithmChoice_ = new QComboBox(pane);
    algorithmChoice_->setObjectName("executionAlgorithm");
    algorithmChoice_->setAccessibleName(tr("Algorithm to run"));
    for (const alg::AlgorithmId id : alg::kAllAlgorithmIds)
        algorithmChoice_->addItem(QString::fromLatin1(alg::ToString(id)), static_cast<int>(id));
    seedSpin_ = new QSpinBox(pane);
    seedSpin_->setObjectName("executionSeed");
    seedSpin_->setRange(1, 1000000);
    seedSpin_->setPrefix(tr("seed "));
    iterationsSpin_ = new QSpinBox(pane);
    iterationsSpin_->setObjectName("executionIterations");
    iterationsSpin_->setRange(1, 10000);
    iterationsSpin_->setValue(50);
    iterationsSpin_->setPrefix(tr("iterations "));
    forwardCheck_ = new QCheckBox(tr("Forward over loopback relays"), pane);
    forwardCheck_->setObjectName("executionForward");
    forwardCheck_->setChecked(true);
    ignoreHostState_ = new QCheckBox(tr("Ignore host NetworkState checks"), pane);
    ignoreHostState_->setObjectName("executionIgnoreHostState");
    ignoreHostState_->setToolTip(tr("The controlled topology does not use captured host traffic for its costs. "
                                    "When ticked, the required_features and data_quality checks may fail "
                                    "without blocking the run; every other check must still pass."));
    runButton_ = new QPushButton(tr("Run"), pane);
    runButton_->setObjectName("executionRun");
    cancelButton_ = new QPushButton(tr("Cancel"), pane);
    cancelButton_->setObjectName("executionCancel");
    controls->addWidget(algorithmChoice_);
    controls->addWidget(seedSpin_);
    controls->addWidget(iterationsSpin_);
    controls->addWidget(forwardCheck_);
    controls->addWidget(ignoreHostState_);
    controls->addStretch();
    controls->addWidget(runButton_);
    controls->addWidget(cancelButton_);
    layout->addLayout(controls);
    connect(runButton_, &QPushButton::clicked, this, &DevToolsPanel::runSelectedAlgorithm);
    connect(cancelButton_, &QPushButton::clicked, this, [this] {
        if (forwarding_ && !trackedExecution_.empty()) forwarding_->engine().Cancel(trackedExecution_);
        refreshForwarding();
    });
    connect(ignoreHostState_, &QCheckBox::toggled, this, [this] {
        shownImplementationKey_.clear();
        refreshForwarding();
    });

    executionStatus_ = valueLabel("executionStatus", pane);
    layout->addWidget(executionStatus_);

    auto *split = new QHBoxLayout;
    implementationTable_ = makeTable("implementationTable",
                                     {tr("Algorithm"), tr("Registry status"), tr("Implementation checks"),
                                      tr("Real forwarding verified"), tr("Worst runtime (ms)"),
                                      tr("Eligibility (controlled topology)"), tr("Reason codes")},
                                     pane);
    implementationTable_->setColumnWidth(0, 110);
    implementationTable_->setColumnWidth(1, 150);
    implementationTable_->setColumnWidth(5, 190);
    split->addWidget(implementationTable_, 3);
    // Scrolls rather than forcing a minimum height, so the dock stays
    // resizable to a short strip.
    auto *resultScroll = new QScrollArea(pane);
    resultScroll->setObjectName("executionResultScroll");
    resultScroll->setWidgetResizable(true);
    resultScroll->setFrameShape(QFrame::NoFrame);
    auto *result = new QWidget(resultScroll);
    auto *form = new QFormLayout(result);
    form->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    form->setFormAlignment(Qt::AlignLeft | Qt::AlignTop);
    const QList<QPair<QString, QString>> rows = {
        {"execution", tr("Execution")},
        {"stage", tr("Progress")},
        {"eligibility", tr("Eligibility")},
        {"candidatePath", tr("Candidate path")},
        {"computation", tr("Computation time")},
        {"iterations", tr("Iterations / evaluations")},
        {"objective", tr("Objective score J(P)")},
        {"forwarding", tr("Forwarding result")},
        {"confirmedPath", tr("Confirmed path")},
        {"endToEnd", tr("End-to-end duration")},
        {"evidence", tr("Evidence")},
    };
    for (const auto &row : rows) {
        auto *label = valueLabel("execution_" + row.first, result);
        executionFields_.insert(row.first, label);
        form->addRow(row.second + QLatin1Char(':'), label);
    }
    resultScroll->setWidget(result);
    split->addWidget(resultScroll, 2);
    layout->addLayout(split, 1);
    scroll->setWidget(pane);
    return scroll;
}

void DevToolsPanel::runSelectedAlgorithm()
{
    if (!forwarding_) return;
    std::optional<ns::NetworkState> state;
    if (stateService_) {
        if (const auto snapshot = stateService_->Latest()) state = snapshot->state;
    }
    const auto id = static_cast<alg::AlgorithmId>(algorithmChoice_->currentData().toInt());
    auto request = forwarding_->MakeRequest(id, static_cast<std::uint64_t>(seedSpin_->value()),
                                            static_cast<std::uint64_t>(iterationsSpin_->value()),
                                            forwardCheck_->isChecked(), std::move(state),
                                            !ignoreHostState_->isChecked());
    // Queued for the engine's worker thread; the GUI thread never waits.
    trackedExecution_ = forwarding_->engine().Submit(std::move(request));
    refreshForwarding();
}

void DevToolsPanel::refreshForwarding()
{
    static const char *const overviewKeys[] = {"forwardingEngine", "verifiedAlgorithms", "latestExecution",
                                                "latestForwarding"};
    adaptiveRuntime_->setText(runtime_ ? tr("Adaptive Runtime: provider attached (read-only)") : adaptiveRuntimeText());
    if (!forwarding_) {
        const QString none = tr("Not available — forwarding engine not attached");
        for (const char *key : overviewKeys) overview_[key]->setText(none);
        forwardingSummary_->setText(none);
        executionStatus_->setText(none);
        topologyTable_->setRowCount(0);
        implementationTable_->setRowCount(0);
        runButton_->setEnabled(false);
        cancelButton_->setEnabled(false);
        for (auto *label : std::as_const(executionFields_)) label->setText(QStringLiteral("—"));
        return;
    }
    const auto status = forwarding_->GetStatus();
    const auto report = forwarding_->Verification();
    const fwd::ForwardingGraph &graph = forwarding_->graph();
    const fwd::ForwardingProblem problem = forwarding_->Problem();
    const fwd::LoopbackForwardingNetwork &network = forwarding_->network();
    auto &engine = forwarding_->engine();

    // ---- Overview ----
    overview_["forwardingEngine"]->setText(
        tr("%1 · %2 · relays %3 on 127.0.0.1 · browser traffic is not routed through it")
            .arg(QString::fromLatin1(fwd::ToString(status.state)), text(status.message),
                 status.relays_running ? tr("running") : tr("stopped")));
    overview_["verifiedAlgorithms"]->setText(
        status.state == fwd::ForwardingService::State::Ready
            ? tr("%1 of 10 verified executable (implementation checks and real loopback forwarding); "
                 "%2 of 10 passed implementation checks; verification took %3")
                  .arg(status.verified_executable)
                  .arg(status.implementations_passed)
                  .arg(msText(status.verification_ms))
            : tr("Verification %1").arg(QString::fromLatin1(fwd::ToString(status.state))));
    const auto tracked = trackedExecution_.empty() ? engine.Latest() : engine.Find(trackedExecution_);
    const auto latest = engine.Latest();
    if (latest) {
        QString line = tr("%1 %2: %3 (%4)").arg(text(latest->execution_id), QString::fromLatin1(alg::ToString(latest->algorithm)),
                                                QString::fromLatin1(alg::ToString(latest->stage)), text(latest->outcome));
        if (latest->computation && latest->computation->objective)
            line += tr(" · J=%1 · %2").arg(*latest->computation->objective, 0, 'g', 6).arg(msText(latest->computation->duration_ms));
        overview_["latestExecution"]->setText(line);
        overview_["latestForwarding"]->setText(
            latest->forwarding
                ? tr("%1 · %2 · confirmed path %3 · %4 (%5)")
                      .arg(QString::fromLatin1(fwd::ToString(latest->forwarding->status)),
                           latest->forwarding->path_confirmed ? tr("path confirmed") : tr("path NOT confirmed"),
                           pathText(latest->forwarding->confirmed_path), msText(latest->forwarding->end_to_end_ms),
                           QString::fromLatin1(fwd::ToString(latest->forwarding->provenance)))
                : tr("No forwarding for the latest execution"));
    } else {
        overview_["latestExecution"]->setText(tr("No manual execution yet"));
        overview_["latestForwarding"]->setText(tr("No forwarding executed yet"));
    }

    // ---- Network: topology, measurements, paths ----
    QStringList constraintParts;
    if (problem.constraints.max_hops) constraintParts << tr("max hops %1").arg(*problem.constraints.max_hops);
    if (problem.constraints.latency_budget_ms) constraintParts << tr("latency budget %1 ms").arg(*problem.constraints.latency_budget_ms);
    if (problem.constraints.computation_deadline_ms)
        constraintParts << tr("computation deadline %1 ms").arg(*problem.constraints.computation_deadline_ms);
    constraintParts << tr("protocol %1").arg(QString::fromLatin1(aafr::netstate::ToString(problem.constraints.protocol)));
    QStringList ports;
    for (const auto &node : graph.nodes()) {
        const auto port = network.PortOf(node.id);
        ports << tr("%1 (%2)%3").arg(text(node.id), QString::fromLatin1(fwd::ToString(node.role)),
                                     port ? QStringLiteral(":%1").arg(port) : tr(" offline"));
    }
    forwardingSummary_->setText(tr("Topology '%1' version %2 · %3 nodes, %4 edges · %5 → %6 · constraints: %7 · nodes: %8")
                                    .arg(text(graph.id()))
                                    .arg(graph.version())
                                    .arg(graph.nodes().size())
                                    .arg(graph.edges().size())
                                    .arg(text(problem.source), text(problem.destination), constraintParts.join(QStringLiteral(", ")),
                                         ports.join(QStringLiteral(", "))));
    topologyTable_->setRowCount(static_cast<int>(graph.edges().size()));
    for (int row = 0; row < static_cast<int>(graph.edges().size()); ++row) {
        const fwd::ForwardingEdge &edge = graph.edges()[static_cast<std::size_t>(row)];
        const auto cost = fwd::EdgeCost(edge, problem.objective);
        setCell(topologyTable_, row, 0, text(edge.from));
        setCell(topologyTable_, row, 1, text(edge.to));
        setCell(topologyTable_, row, 2, measurementText(edge.latency_ms, 2));
        setCell(topologyTable_, row, 3, measurementText(edge.loss_ratio, 4));
        setCell(topologyTable_, row, 4, measurementText(edge.bandwidth_mbps, 0));
        setCell(topologyTable_, row, 5, QString::fromLatin1(fwd::ToString(edge.latency_ms.provenance)));
        setCell(topologyTable_, row, 6, edge.available ? tr("yes") : tr("no"));
        setCell(topologyTable_, row, 7, cost ? QString::number(*cost, 'g', 6) : tr("undefined"));
        setCell(topologyTable_, row, 8, QString::number(network.HandledCount(edge.to)));
    }
    if (pathsTable_->rowCount() == 0) {
        const auto paths = forwarding_->CandidatePaths();
        pathsTable_->setRowCount(static_cast<int>(paths.size()));
        for (int row = 0; row < static_cast<int>(paths.size()); ++row) {
            const auto &candidate = paths[static_cast<std::size_t>(row)];
            QStringList issues;
            for (const auto issue : candidate.validation.issues) issues << QString::fromLatin1(fwd::ToString(issue));
            setCell(pathsTable_, row, 0, pathText(candidate.path));
            setCell(pathsTable_, row, 1, QString::number(candidate.validation.hops));
            setCell(pathsTable_, row, 2, QLocale().toString(candidate.validation.latency_ms, 'f', 2));
            setCell(pathsTable_, row, 3, candidate.validation.objective ? QString::number(*candidate.validation.objective, 'g', 6)
                                                                        : tr("undefined"));
            setCell(pathsTable_, row, 4, candidate.validation.feasible ? tr("yes") : tr("no"));
            setCell(pathsTable_, row, 5, issues.isEmpty() ? tr("none") : issues.join(QStringLiteral(", ")));
        }
    }

    // ---- Runtime: implementations and eligibility ----
    const auto snapshot = stateService_ ? stateService_->Latest() : nullptr;
    const QString key = QStringLiteral("%1|%2|%3|%4")
                            .arg(QString::fromLatin1(fwd::ToString(status.state)))
                            .arg(snapshot ? snapshot->state.state_id : 0)
                            .arg(ignoreHostState_->isChecked() ? 1 : 0)
                            .arg(engine.ForwardingAvailable() ? 1 : 0);
    // Before verification there is nothing executable to evaluate; skip the
    // work so a window that never opens Developer Tools pays nothing.
    if (status.state != fwd::ForwardingService::State::Ready) {
        if (implementationTable_->rowCount() != 0) implementationTable_->setRowCount(0);
        shownImplementationKey_.clear();
    } else if (key != shownImplementationKey_) {
        shownImplementationKey_ = key;
        const alg::CapabilityRegistry registry = engine.Registry();
        std::optional<ns::NetworkState> state;
        if (snapshot) state = snapshot->state;
        implementationTable_->setRowCount(static_cast<int>(alg::kAllAlgorithmIds.size()));
        int row = 0;
        for (const alg::AlgorithmId id : alg::kAllAlgorithmIds) {
            const alg::AlgorithmCapability *capability = registry.Find(id);
            const alg::AlgorithmVerification *verification = report ? report->Find(id) : nullptr;
            const auto request = forwarding_->MakeRequest(id, 1, 50, true, state, !ignoreHostState_->isChecked());
            const alg::EligibilityResult eligibility = engine.Eligibility(id, request);
            QString eligible = QString::fromLatin1(alg::ToString(eligibility.status));
            if (eligibility.status != alg::EligibilityStatus::Eligible && ignoreHostState_->isChecked()) {
                bool others = true;
                for (const auto &check : eligibility.checks) {
                    if (check.outcome == alg::CheckOutcome::Passed) continue;
                    others = others && (check.check == alg::EligibilityCheck::RequiredFeatures
                                        || check.check == alg::EligibilityCheck::DataQuality);
                }
                if (others && eligibility.status != alg::EligibilityStatus::NotImplemented)
                    eligible += tr(" (runs with host-state override)");
            }
            setCell(implementationTable_, row, 0, QString::fromLatin1(alg::ToString(id)));
            setCell(implementationTable_, row, 1,
                    capability ? QString::fromLatin1(alg::ToString(capability->availability)) : tr("not registered"));
            setCell(implementationTable_, row, 2,
                    verification ? (verification->implementation_passed
                                        ? tr("passed (%1 checks)").arg(verification->passed_checks.size())
                                        : tr("FAILED: %1").arg(text(verification->failures.empty() ? std::string()
                                                                                                    : verification->failures.front())))
                                 : tr("not verified yet"));
            setCell(implementationTable_, row, 3,
                    verification ? (verification->forwarding_passed ? tr("yes") : tr("no")) : tr("not verified yet"));
            setCell(implementationTable_, row, 4,
                    capability && capability->implementation.worst_case_runtime_ms
                        ? QLocale().toString(*capability->implementation.worst_case_runtime_ms, 'f', 3)
                        : QStringLiteral("—"));
            setCell(implementationTable_, row, 5, eligible);
            setCell(implementationTable_, row, 6, text(alg::ReasonCodesText(eligibility.reason_codes)));
            implementationTable_->item(row, 6)->setToolTip(text(eligibility.explanation));
            ++row;
        }
    }

    // ---- Runtime: tracked execution ----
    const bool busy = tracked && !tracked->Finished();
    runButton_->setEnabled(status.state == fwd::ForwardingService::State::Ready && !busy);
    cancelButton_->setEnabled(busy);
    executionStatus_->setText(
        status.state != fwd::ForwardingService::State::Ready
            ? tr("Forwarding engine %1: %2").arg(QString::fromLatin1(fwd::ToString(status.state)), text(status.message))
            : tr("Manual execution on the controlled topology. Nothing runs unless you press Run; no algorithm is "
                 "chosen or switched automatically. %1 pending.").arg(engine.Pending()));
    const auto set = [this](const char *field, const QString &value) { executionFields_[field]->setText(value); };
    if (!tracked) {
        for (auto *label : std::as_const(executionFields_)) label->setText(QStringLiteral("—"));
        return;
    }
    const alg::ExecutionRecord &record = *tracked;
    set("execution", tr("%1 · %2 · scenario %3 · NetworkState #%4 · graph %5 v%6")
                         .arg(text(record.execution_id), QString::fromLatin1(alg::ToString(record.algorithm)),
                              text(record.scenario_id))
                         .arg(record.network_state_id)
                         .arg(text(record.graph_id))
                         .arg(record.graph_version));
    set("stage", tr("%1 · %2%3").arg(QString::fromLatin1(alg::ToString(record.stage)), text(record.outcome),
                                     record.message.empty() ? QString() : QStringLiteral(" — ") + text(record.message)));
    set("eligibility", record.eligibility
                           ? tr("%1 · %2%3")
                                 .arg(QString::fromLatin1(alg::ToString(record.eligibility->status)),
                                      text(alg::ReasonCodesText(record.eligibility->reason_codes)),
                                      record.overridden_checks.empty()
                                          ? QString()
                                          : tr(" · overridden: %1").arg(joined(record.overridden_checks, [](const std::string &v) {
                                                return QString::fromStdString(v);
                                            })))
                           : tr("not evaluated yet"));
    if (record.computation) {
        const alg::AlgorithmResult &c = *record.computation;
        set("candidatePath", tr("%1 (%2)").arg(pathText(c.path), QString::fromLatin1(alg::ToString(c.status))));
        set("computation", tr("%1 · termination %2").arg(msText(c.duration_ms), QString::fromLatin1(alg::ToString(c.termination))));
        set("iterations", tr("%1 / %2").arg(c.iterations).arg(c.evaluations));
        set("objective", c.objective ? tr("%1 · %2 · %3")
                                           .arg(*c.objective, 0, 'g', 8)
                                           .arg(c.feasible ? tr("feasible") : tr("infeasible"), text(c.optimality_note))
                                     : tr("none"));
    } else {
        for (const char *field : {"candidatePath", "computation", "iterations", "objective"}) set(field, tr("not computed"));
    }
    if (record.forwarding) {
        const fwd::ForwardingExecutionResult &f = *record.forwarding;
        set("forwarding", tr("%1 · response %2 · request %3")
                              .arg(QString::fromLatin1(fwd::ToString(f.status)), text(f.response_status),
                                   text(f.request_id)));
        set("confirmedPath", tr("%1 · %2").arg(pathText(f.confirmed_path),
                                              f.path_confirmed ? tr("confirmed") : tr("not confirmed")));
        set("endToEnd", tr("%1 (%2; network time, not computation time)")
                            .arg(msText(f.end_to_end_ms), QString::fromLatin1(fwd::ToString(f.provenance))));
        set("evidence", f.evidence.empty() ? text(f.message) : text(f.evidence));
    } else {
        for (const char *field : {"forwarding", "confirmedPath", "endToEnd", "evidence"}) set(field, tr("not forwarded"));
    }
}

void DevToolsPanel::refreshOverview()
{
    const auto snapshot = collector_ ? collector_->currentTab() : std::nullopt;
    if (snapshot) {
        overview_["activeTab"]->setText(
            tr("Tab %1 · %2").arg(snapshot->tabId)
                .arg(snapshot->redactedUrl.isEmpty() ? tr("new tab") : snapshot->redactedUrl));
        QString status = BrowserEventCollector::toString(snapshot->status);
        if (snapshot->status == BrowserEventCollector::NavigationStatus::Loading)
            status += tr(" (%1%)").arg(snapshot->progress);
        if (snapshot->navigationId) status += tr(" · navigation #%1").arg(*snapshot->navigationId);
        if (!snapshot->error.isEmpty()) status += tr(" · %1").arg(snapshot->error);
        overview_["navigationStatus"]->setText(status);
        overview_["pageLoad"]->setText(
            snapshot->lastNavigationMs
                ? tr("%1 ms (navigation #%2, monotonic clock)")
                      .arg(QLocale().toString(*snapshot->lastNavigationMs, 'f', 1))
                      .arg(*snapshot->lastNavigationId)
                : tr("Not measured yet for this tab"));
    } else {
        overview_["activeTab"]->setText(tr("No active tab"));
        overview_["navigationStatus"]->setText(tr("Not available"));
        overview_["pageLoad"]->setText(tr("Not available"));
    }

    const auto stats = pipeline_.Stats();
    QString collection = collector_ ? tr("Browser events: collecting") : tr("Browser events: off");
    collection += stats.published == 1 ? tr(" · 1 event published")
                                       : tr(" · %1 events published").arg(stats.published);
    if (stats.evicted_from_history > 0)
        collection += tr(" (%1 older events no longer in memory)").arg(stats.evicted_from_history);
    if (stats.jsonl_enabled) {
        collection += tr(" · JSONL: %1, %2 written").arg(text(pipeline_.JsonlPath())).arg(stats.jsonl_written);
        if (stats.jsonl_dropped > 0) collection += tr(", %1 dropped").arg(stats.jsonl_dropped);
        if (!stats.jsonl_healthy) collection += tr(" (write error)");
    } else {
        collection += tr(" · JSONL output off");
    }
    overview_["collection"]->setText(collection);
    const auto started = std::chrono::system_clock::now()
                         - std::chrono::duration_cast<std::chrono::system_clock::duration>(
                             std::chrono::nanoseconds(pipeline_.MonotonicNowNs()));
    overview_["session"]->setText(tr("%1 · started %2").arg(text(pipeline_.SessionId()), formatWallFull(started)));

    overview_["capture"]->setText(monitor_ ? tr("%1: %2").arg(monitor_->name(), NetworkMonitor::toString(monitor_->state()))
                                           : tr("No network monitor configured"));

    QStringList metrics;
    const auto describe = [this, &metrics](const char *metric, const QString &label) {
        if (const auto event = pipeline_.LatestMetric(metric)) {
            const double age = (pipeline_.MonotonicNowNs() - event->monotonic_ns) / 1e9;
            metrics << tr("%1 %2 %3 (%4 s ago)").arg(label, formatValue(*event->value, event->unit),
                                                     text(event->unit), QString::number(age, 'f', 1));
        }
    };
    describe("net.tcp.handshake_rtt.mean", tr("Latency"));
    describe("net.tcp.ack_rtt.mean", tr("ACK delay mean"));
    describe("net.tcp.ack_rtt.jitter", tr("ACK delay jitter"));
    describe("net.throughput", tr("Throughput"));
    describe("net.tcp.retransmissions", tr("Retransmissions"));
    describe("net.dns.response_time.mean", tr("DNS mean"));
    describe("net.packets", tr("Packets"));
    overview_["metrics"]->setText(metrics.isEmpty() ? tr("No network metrics observed (capture off or no samples)")
                                                    : metrics.join(QStringLiteral(" · ")));
}

void DevToolsPanel::refreshNetwork()
{
    const auto latest = pipeline_.LatestMetrics();
    const std::int64_t now = pipeline_.MonotonicNowNs();
    const bool unavailable = !monitor_ || !monitor_->isAvailable();
    for (int row = 0; row < networkTable_->rowCount(); ++row) {
        const std::string metric = networkTable_->item(row, 0)->data(Qt::UserRole).toString().toStdString();
        const auto found = latest.find(metric);
        if (found == latest.end()) {
            setCell(networkTable_, row, 1, unavailable ? tr("Unavailable") : tr("Not observed"));
            setCell(networkTable_, row, 2, unitFor(metric));
            setCell(networkTable_, row, 3, aafr::telemetry::ToString(aafr::telemetry::Source::TShark));
            setCell(networkTable_, row, 4, aafr::telemetry::ToString(aafr::telemetry::MeasurementScope::CaptureInterface));
            setCell(networkTable_, row, 5, QStringLiteral("—"));
            continue;
        }
        const TelemetryEvent &event = found->second;
        const double age = (now - event.monotonic_ns) / 1e9;
        setCell(networkTable_, row, 1, formatValue(*event.value, event.unit));
        setCell(networkTable_, row, 2, unitFor(metric));
        setCell(networkTable_, row, 3, aafr::telemetry::ToString(event.source));
        setCell(networkTable_, row, 4, aafr::telemetry::ToString(event.scope));
        setCell(networkTable_, row, 5, age > kStaleAfterSeconds
                                           ? tr("stale (%1 s)").arg(age, 0, 'f', 0)
                                           : tr("%1 s ago").arg(age, 0, 'f', 1));
    }
    const auto quic = latest.find("net.quic.packets");
    const std::string *versions = quic != latest.end() ? quic->second.FindAttribute("quic_versions") : nullptr;
    protocolInfo_->setText(versions ? tr("QUIC versions seen in the latest interval: %1").arg(text(*versions))
                                    : tr("QUIC versions: none observed in the latest interval"));
}

void DevToolsPanel::populateInterfaces()
{
    // Keep the person's choice across refreshes; first time, use the
    // configured interface (AAFR_TSHARK_INTERFACE), else Automatic.
    QString selected = captureInterface_->count() > 0
                           ? captureInterface_->currentData().toString()
                           : (monitor_ ? monitor_->config().interfaceName.trimmed() : QString());
    const QSignalBlocker blocker(captureInterface_);
    captureInterface_->clear();
    const QString active = monitor_ ? monitor_->activeInterfaceName() : QString();
    captureInterface_->addItem(active.isEmpty() ? tr("Automatic") : tr("Automatic (%1)").arg(active),
                               QString());
    captureInterface_->setItemData(0, tr("Capture on the connected Wi-Fi or Ethernet adapter"),
                                   Qt::ToolTipRole);
    if (monitor_) {
        for (const auto &entry : monitor_->interfaces()) {
            const QString name = QString::fromStdString(entry.name);
            captureInterface_->addItem(name, name);
            captureInterface_->setItemData(captureInterface_->count() - 1,
                                           QString::fromStdString(entry.number + ". " + entry.device),
                                           Qt::ToolTipRole);
        }
        if (!selected.isEmpty()) {
            // Match loosely ("wifi" selects "Wi-Fi"); keep an unknown name
            // visible so starting explains it rather than silently dropping it.
            const auto resolved = aafr::tshark::ResolveInterface(selected.toStdString(), monitor_->interfaces());
            if (resolved) selected = QString::fromStdString(resolved->name);
            int index = captureInterface_->findData(selected);
            if (index < 0) {
                captureInterface_->addItem(selected, selected);
                index = captureInterface_->count() - 1;
            }
            captureInterface_->setCurrentIndex(index);
        }
    }
}

void DevToolsPanel::refreshCapture()
{
    if (!monitor_) {
        captureStatus_->setText(tr("No network monitor configured."));
        captureButton_->setText(tr("Start capture"));
        captureButton_->setEnabled(false);
        captureInterface_->setEnabled(false);
        return;
    }
    const auto state = monitor_->state();
    captureStatus_->setText(tr("%1 — %2").arg(NetworkMonitor::toString(state), monitor_->statusText()));
    const bool active = state == NetworkMonitor::State::Running || state == NetworkMonitor::State::Starting;
    captureButton_->setText(active ? tr("Stop capture") : tr("Start capture"));
    captureButton_->setEnabled(monitor_->isAvailable() && state != NetworkMonitor::State::Stopping);
    captureInterface_->setEnabled(!active && monitor_->isAvailable());
}

void DevToolsPanel::refreshRuntime()
{
    if (!runtime_) {
        runtimeBanner_->setText(notIntegratedText());
        for (auto *label : std::as_const(runtimeFields_)) label->setText(notIntegratedText());
        return;
    }
    const obs::RuntimeSnapshot snapshot = runtime_->Snapshot();
    runtimeBanner_->setText(tr("Read-only view of the runtime state provider."));
    const auto unknown = tr("Not reported");
    if (snapshot.active_algorithm) {
        const auto &algorithm = *snapshot.active_algorithm;
        QString value = text(algorithm.algorithm_id) + tr(" (%1)").arg(obs::ToString(algorithm.execution_state));
        if (algorithm.last_confirmed_at)
            value += tr(" · confirmed %1").arg(formatWallFull(*algorithm.last_confirmed_at));
        runtimeFields_["algorithm"]->setText(value);
        runtimeFields_["activatedAt"]->setText(
            algorithm.activated_at ? formatWallFull(*algorithm.activated_at) : unknown);
    } else {
        runtimeFields_["algorithm"]->setText(tr("No active algorithm reported"));
        runtimeFields_["activatedAt"]->setText(unknown);
    }
    runtimeFields_["reason"]->setText(snapshot.activation_reason.empty() ? unknown : text(snapshot.activation_reason));
    runtimeFields_["state"]->setText(obs::ToString(snapshot.state));
    runtimeFields_["lastEvaluation"]->setText(
        snapshot.last_evaluation_at ? formatWallFull(*snapshot.last_evaluation_at) : unknown);
}

void DevToolsPanel::refreshDecisions()
{
    if (!runtime_) {
        decisionsBanner_->setText(notIntegratedText());
        decisionsTable_->setRowCount(0);
        return;
    }
    const auto records = runtime_->RecentDecisions(200);
    decisionsBanner_->setText(records.empty() ? tr("No decisions recorded yet.")
                                              : tr("%1 most recent decisions, newest first. "
                                                   "An approved switch is not committed until the "
                                                   "routing controller confirms it.").arg(records.size()));
    decisionsTable_->setRowCount(static_cast<int>(records.size()));
    for (int row = 0; row < static_cast<int>(records.size()); ++row) {
        const obs::DecisionRecord &record = records[records.size() - 1 - row];
        setCell(decisionsTable_, row, 0, formatWall(record.timestamp));
        setCell(decisionsTable_, row, 1, text(record.decision_id));
        setCell(decisionsTable_, row, 2, obs::ToString(record.decision_type));
        setCell(decisionsTable_, row, 3, text(record.previous_algorithm) + QStringLiteral(" → ")
                                             + text(record.candidate_algorithm));
        setCell(decisionsTable_, row, 4, text(record.reason_code));
        const std::string failed = obs::FailedChecks(record);
        setCell(decisionsTable_, row, 5, failed.empty() ? tr("none") : text(failed));
        setCell(decisionsTable_, row, 6, text(obs::SwitchOutcome(record)));
        setCell(decisionsTable_, row, 7, text(record.explanation));
    }
}

bool DevToolsPanel::eventPassesFilter(const TelemetryEvent &event) const
{
    switch (eventFilter_->currentIndex()) {
    case 1: return event.source == Source::QtWebEngine;
    case 2: return event.source == Source::TShark;
    case 3: return event.source == Source::StatePipeline;
    case 4: return event.source == Source::ForwardingEngine;
    default: return true;
    }
}

void DevToolsPanel::appendEvents()
{
    if (pauseEvents_->isChecked()) return;
    const auto events = pipeline_.EventsAfter(lastEventId_, 2000);
    if (events.empty()) return;
    lastEventId_ = events.back().event_id;
    auto *scroll = eventsTable_->verticalScrollBar();
    const bool atBottom = scroll->value() >= scroll->maximum() - 2;
    eventsTable_->setUpdatesEnabled(false);
    for (const TelemetryEvent &event : events) {
        if (eventPassesFilter(event)) appendEventRow(event);
    }
    const int excess = eventsTable_->rowCount() - kMaxEventRows;
    for (int i = 0; i < excess; ++i) eventsTable_->removeRow(0);
    eventsTable_->setUpdatesEnabled(true);
    if (atBottom) eventsTable_->scrollToBottom();
}

void DevToolsPanel::appendEventRow(const TelemetryEvent &event)
{
    const int row = eventsTable_->rowCount();
    eventsTable_->insertRow(row);
    setCell(eventsTable_, row, 0, formatWall(event.wall_time));
    setCell(eventsTable_, row, 1, QString::number(event.monotonic_ns / 1e9, 'f', 3));
    setCell(eventsTable_, row, 2, aafr::telemetry::ToString(event.type));
    setCell(eventsTable_, row, 3, aafr::telemetry::ToString(event.source));
    setCell(eventsTable_, row, 4, event.tab_id ? QString::number(*event.tab_id) : QString());
    setCell(eventsTable_, row, 5, event.navigation_id ? QString::number(*event.navigation_id) : QString());
    QString value;
    if (event.value) value = formatValue(*event.value, event.unit) + QLatin1Char(' ') + text(event.unit);
    if (!event.metric.empty() && event.type == EventType::NetworkMetric)
        value = text(event.metric) + QStringLiteral(" = ") + value;
    setCell(eventsTable_, row, 6, value);
    setCell(eventsTable_, row, 7, attributesText(event));
    eventsTable_->item(row, 0)->setData(Qt::UserRole, QVariant::fromValue<qulonglong>(event.event_id));
}

void DevToolsPanel::toggleDockArea()
{
    auto *window = qobject_cast<QMainWindow *>(parentWidget());
    if (!window) return;
    const bool right = !isFloating() && window->dockWidgetArea(this) == Qt::RightDockWidgetArea;
    setFloating(false);
    window->addDockWidget(right ? Qt::BottomDockWidgetArea : Qt::RightDockWidgetArea, this);
    updateDockButton();
}

void DevToolsPanel::updateDockButton()
{
    auto *window = qobject_cast<QMainWindow *>(parentWidget());
    const bool right = window && !isFloating() && window->dockWidgetArea(this) == Qt::RightDockWidgetArea;
    dockButton_->setText(right ? tr("Dock bottom") : tr("Dock right"));
    dockButton_->setToolTip(right ? tr("Move the panel to the bottom of the window")
                                  : tr("Move the panel to the right side of the window"));
}
