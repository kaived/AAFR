#include "devtools_panel.h"

#include "browser_event_collector.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMainWindow>
#include <QPushButton>
#include <QScrollBar>
#include <QTabWidget>
#include <QTableWidget>
#include <QToolButton>
#include <QVBoxLayout>

#include "network/telemetry/telemetry_pipeline.h"
#include "network/tshark/tshark_fields.h"
#include "network/tshark/tshark_monitor.h"
#include "runtime/observability/runtime_contracts.h"

using aafr::telemetry::EventType;
using aafr::telemetry::Source;
using aafr::telemetry::TelemetryEvent;
namespace obs = aafr::observability;

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

QString metricLabel(const std::string &metric)
{
    static const QHash<QString, QString> labels = {
        {"net.tcp.ack_rtt.latest", QObject::tr("TCP ACK RTT (latest)")},
        {"net.tcp.ack_rtt.min", QObject::tr("TCP ACK RTT (min)")},
        {"net.tcp.ack_rtt.mean", QObject::tr("TCP ACK RTT (mean)")},
        {"net.tcp.ack_rtt.max", QObject::tr("TCP ACK RTT (max)")},
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
    }
    refreshNow();
}

void DevToolsPanel::setRuntimeProvider(const obs::IRuntimeObservabilityProvider *provider)
{
    runtime_ = provider;
    refreshRuntime();
    refreshDecisions();
}

void DevToolsPanel::buildOverview()
{
    auto *page = new QWidget(tabs_);
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
    };
    for (const auto &row : rows) {
        auto *label = valueLabel("overview_" + row.first, page);
        overview_.insert(row.first, label);
        form->addRow(row.second + QLatin1Char(':'), label);
    }
    tabs_->addTab(page, tr("Overview"));
}

void DevToolsPanel::buildNetwork()
{
    auto *page = new QWidget(tabs_);
    page->setObjectName("devToolsNetwork");
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(8, 8, 8, 8);
    auto *controls = new QHBoxLayout;
    captureStatus_ = valueLabel("captureStatus", page);
    captureInterface_ = new QLineEdit(page);
    captureInterface_->setObjectName("captureInterface");
    captureInterface_->setPlaceholderText(tr("Interface (default)"));
    captureInterface_->setAccessibleName(tr("Capture interface"));
    captureInterface_->setMaximumWidth(200);
    if (monitor_) captureInterface_->setText(monitor_->config().interfaceName);
    captureButton_ = new QPushButton(page);
    captureButton_->setObjectName("captureToggle");
    connect(captureButton_, &QPushButton::clicked, this, [this] {
        if (!monitor_) return;
        const auto state = monitor_->state();
        if (state == NetworkMonitor::State::Running || state == NetworkMonitor::State::Starting) {
            monitor_->stop();
        } else {
            monitor_->setInterfaceName(captureInterface_->text().trimmed());
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
    networkTable_->setColumnWidth(0, 210);
    networkTable_->setColumnWidth(1, 110);
    networkTable_->setColumnWidth(2, 140);
    networkTable_->setColumnWidth(3, 80);
    networkTable_->setColumnWidth(4, 130);
    layout->addWidget(networkTable_, 1);
    protocolInfo_ = valueLabel("protocolInfo", page);
    layout->addWidget(protocolInfo_);
    auto *note = new QLabel(tr("Capture-interface metrics cover all host traffic and are not attributed to "
                               "browser tabs. RTT is TCP ACK RTT as seen by this host. Retransmissions are "
                               "TShark's heuristic and are not an exact packet-loss rate."), page);
    note->setObjectName("networkScopeNote");
    note->setWordWrap(true);
    layout->addWidget(note);
    tabs_->addTab(page, tr("Network"));
}

void DevToolsPanel::buildRuntime()
{
    auto *page = new QWidget(tabs_);
    page->setObjectName("devToolsRuntime");
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(12, 10, 12, 10);
    runtimeBanner_ = valueLabel("runtimeBanner", page);
    layout->addWidget(runtimeBanner_);
    auto *form = new QFormLayout;
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
    layout->addLayout(form);
    layout->addStretch();
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
    describe("net.tcp.ack_rtt.mean", tr("RTT mean"));
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
