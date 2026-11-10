#include "network/tshark/tshark_monitor.h"

#include <QFileInfo>
#include <QNetworkInterface>
#include <QStandardPaths>

#include "network/telemetry/telemetry_pipeline.h"

using aafr::telemetry::AttributionQuality;
using aafr::telemetry::Availability;
using aafr::telemetry::EventType;
using aafr::telemetry::MeasurementScope;
using aafr::telemetry::Source;
using aafr::telemetry::TelemetryEvent;

namespace {
constexpr qsizetype kMaxLineBytes = 64 * 1024;
constexpr qsizetype kStderrTailBytes = 2048;

QString lastLine(const QByteArray &text)
{
    const QList<QByteArray> lines = text.trimmed().split('\n');
    const QString line = lines.isEmpty() ? QString() : QString::fromLocal8Bit(lines.last()).trimmed();
    return line.left(300);
}

QString resolveExecutable(const QString &candidate)
{
    if (candidate.isEmpty()) return {};
    const QFileInfo info(candidate);
    if (info.isAbsolute() || candidate.contains(QLatin1Char('/')) || candidate.contains(QLatin1Char('\\')))
        return info.isFile() && info.isExecutable() ? info.absoluteFilePath() : QString();
    return QStandardPaths::findExecutable(candidate);
}
}

TSharkMonitor::TSharkMonitor(aafr::telemetry::TelemetryPipeline &pipeline, Config config,
                             QObject *parent)
    : NetworkMonitor(parent), pipeline_(pipeline), config_(std::move(config))
{
    program_ = locate(config_.program);
    if (program_.isEmpty()) {
        state_ = State::Unavailable;
        statusText_ = tr("TShark not found. Install Wireshark (TShark and Npcap) or set AAFR_TSHARK_PATH.");
    } else {
        statusText_ = tr("Capture off. Start it explicitly to collect network metrics.");
    }
    flushTimer_.setInterval(config_.flushInterval);
    connect(&flushTimer_, &QTimer::timeout, this, &TSharkMonitor::flushInterval);
    killTimer_.setSingleShot(true);
    connect(&killTimer_, &QTimer::timeout, this, [this] {
        if (process_) process_->kill();
    });
}

TSharkMonitor::~TSharkMonitor()
{
    // No telemetry here: the pipeline may already be gone. Owners that want a
    // capture.stopped record call shutdown() first.
    if (listProcess_) {
        disconnect(listProcess_, nullptr, this, nullptr);
        listProcess_->kill();
        listProcess_->waitForFinished(static_cast<int>(config_.stopTimeout.count()));
    }
    if (process_) {
        disconnect(process_, nullptr, this, nullptr);
        process_->kill();
        process_->waitForFinished(static_cast<int>(config_.stopTimeout.count()));
    }
}

QString TSharkMonitor::locate(const QString &configured)
{
    if (!configured.isEmpty()) return resolveExecutable(configured);
    const QString fromEnvironment = qEnvironmentVariable("AAFR_TSHARK_PATH");
    if (!fromEnvironment.isEmpty()) return resolveExecutable(fromEnvironment);
    const QString onPath = QStandardPaths::findExecutable(QStringLiteral("tshark"));
    if (!onPath.isEmpty()) return onPath;
#ifdef Q_OS_WIN
    return resolveExecutable(QStringLiteral("C:/Program Files/Wireshark/tshark.exe"));
#else
    return {};
#endif
}

QStringList TSharkMonitor::arguments() const
{
    if (config_.argumentsOverride) return *config_.argumentsOverride;
    QStringList args;
    // The resolved device when known, else whatever was configured.
    const QString device = captureDevice_.isEmpty() ? config_.interfaceName.trimmed() : captureDevice_;
    for (const std::string &arg : aafr::tshark::BuildCaptureArguments(
             device.toStdString(), config_.captureFilter.toStdString()))
        args << QString::fromStdString(arg);
    return args;
}

void TSharkMonitor::setState(State state, const QString &text)
{
    statusText_ = text;
    if (state_ == state) {
        emit stateChanged(state_);
        return;
    }
    state_ = state;
    emit stateChanged(state_);
}

void TSharkMonitor::publishLifecycle(EventType type, Availability availability, const QString &detail)
{
    TelemetryEvent event;
    event.type = type;
    event.source = Source::TShark;
    event.scope = MeasurementScope::CaptureInterface;
    event.attribution = AttributionQuality::NotApplicable;
    event.availability = availability;
    if (!detail.isEmpty()) event.attributes.push_back({"detail", detail.toStdString()});
    const QString interface = captureLabel_.isEmpty() ? config_.interfaceName : captureLabel_;
    if (!interface.isEmpty())
        event.attributes.push_back({"interface", interface.toStdString()});
    pipeline_.Publish(std::move(event));
}

QString TSharkMonitor::activeInterfaceName() const
{
    // Prefer a connected Wi-Fi or Ethernet adapter with an address that
    // TShark also lists; skip loopback and virtual adapters.
    QString fallback;
    for (const QNetworkInterface &nic : QNetworkInterface::allInterfaces()) {
        const auto flags = nic.flags();
        if (!flags.testFlag(QNetworkInterface::IsUp) || !flags.testFlag(QNetworkInterface::IsRunning)
            || flags.testFlag(QNetworkInterface::IsLoopBack) || nic.addressEntries().isEmpty())
            continue;
        const auto resolved = aafr::tshark::ResolveInterface(nic.humanReadableName().toStdString(),
                                                             interfaces_);
        if (!resolved) continue;
        const QString name = QString::fromStdString(resolved->name);
        if (nic.type() == QNetworkInterface::Wifi || nic.type() == QNetworkInterface::Ethernet)
            return name;
        if (fallback.isEmpty() && nic.type() != QNetworkInterface::Virtual) fallback = name;
    }
    return fallback;
}

void TSharkMonitor::refreshInterfaces()
{
    if (listProcess_ || program_.isEmpty()) return;
    auto *process = new QProcess(this);
    listProcess_ = process;
    process->setProgram(program_);
    process->setArguments({QStringLiteral("-D")});
    process->setProcessChannelMode(QProcess::SeparateChannels);
    auto finish = [this, process] {
        if (process != listProcess_) return;
        // A failed listing leaves the list empty; capture then passes the
        // configured name to TShark unchanged.
        interfaces_ = aafr::tshark::ParseInterfaceList(
            QString::fromLocal8Bit(process->readAllStandardOutput()).toStdString());
        interfacesLoaded_ = true;
        listProcess_ = nullptr;
        process->deleteLater();
        emit interfacesChanged();
        if (startAfterListing_) {
            startAfterListing_ = false;
            startCapture();
        }
    };
    connect(process, &QProcess::finished, this, finish);
    connect(process, &QProcess::errorOccurred, this, [finish](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) finish();
    });
    process->start(QIODevice::ReadOnly);
}

bool TSharkMonitor::start()
{
    if (state_ == State::Running || state_ == State::Starting) return true;
    if (program_.isEmpty()) {
        program_ = locate(config_.program);
        if (program_.isEmpty()) {
            publishLifecycle(EventType::CaptureError, Availability::Unavailable,
                             QStringLiteral("TShark executable not found"));
            setState(State::Unavailable, statusText_);
            return false;
        }
    }
    if (process_) return false;  // Previous process still finishing.
    if (!interfacesLoaded_ && !config_.argumentsOverride) {
        // Names are resolved against `tshark -D` first.
        startAfterListing_ = true;
        setState(State::Starting, tr("Finding capture interfaces…"));
        refreshInterfaces();
        return true;
    }
    return startCapture();
}

bool TSharkMonitor::startCapture()
{
    captureDevice_.clear();
    captureLabel_.clear();
    if (!config_.argumentsOverride && !interfaces_.empty()) {
        QString wanted = config_.interfaceName.trimmed();
        if (wanted.isEmpty()) wanted = activeInterfaceName();
        const auto resolved = aafr::tshark::ResolveInterface(wanted.toStdString(), interfaces_);
        if (!resolved) {
            QStringList names;
            for (const auto &entry : interfaces_) names << QString::fromStdString(entry.name);
            const QString detail =
                wanted.isEmpty()
                    ? tr("No connected Wi-Fi or Ethernet adapter found. Choose an interface: %1.")
                          .arg(names.join(QStringLiteral(", ")))
                    : tr("No capture interface named \"%1\". Available: %2.")
                          .arg(wanted, names.join(QStringLiteral(", ")));
            publishLifecycle(EventType::CaptureError, Availability::Error, detail);
            setState(State::Error, detail);
            return false;
        }
        captureDevice_ = QString::fromStdString(resolved->device);
        captureLabel_ = QString::fromStdString(resolved->name);
    } else {
        captureLabel_ = config_.interfaceName.trimmed();
    }

    aggregator_ = {};
    stdoutBuffer_.clear();
    stderrTail_.clear();
    stopRequested_ = false;

    auto *process = new QProcess(this);
    process_ = process;
    process->setProgram(program_);
    process->setArguments(arguments());
    process->setProcessChannelMode(QProcess::SeparateChannels);
    connect(process, &QProcess::started, this, [this] {
        intervalStart_ = std::chrono::steady_clock::now();
        flushTimer_.start();
        publishLifecycle(EventType::CaptureStarted, Availability::Available, QString());
        setState(State::Running, tr("Capturing on %1. Metrics cover all host traffic on the interface.")
                                     .arg(captureLabel_.isEmpty() ? tr("the default interface")
                                                                  : captureLabel_));
    });
    connect(process, &QProcess::readyReadStandardOutput, this, &TSharkMonitor::readStdout);
    connect(process, &QProcess::readyReadStandardError, this, &TSharkMonitor::readStderr);
    connect(process, &QProcess::finished, this, &TSharkMonitor::onFinished);
    connect(process, &QProcess::errorOccurred, this, &TSharkMonitor::onError);
    setState(State::Starting, tr("Starting TShark…"));
    process->start(QIODevice::ReadOnly);
    return true;
}

void TSharkMonitor::stop()
{
    if (!process_ && startAfterListing_) {
        // Stopped while interfaces were still being listed.
        startAfterListing_ = false;
        setState(State::Disabled, tr("Capture off."));
        return;
    }
    if (!process_ || stopRequested_) return;
    stopRequested_ = true;
    flushTimer_.stop();
    setState(State::Stopping, tr("Stopping TShark…"));
#ifdef Q_OS_WIN
    // Console processes ignore WM_CLOSE; output is read live, so nothing is lost.
    process_->kill();
#else
    process_->terminate();
    killTimer_.start(config_.stopTimeout);
#endif
}

void TSharkMonitor::shutdown()
{
    flushTimer_.stop();
    killTimer_.stop();
    startAfterListing_ = false;
    if (listProcess_) {
        disconnect(listProcess_, nullptr, this, nullptr);
        listProcess_->kill();
        listProcess_->waitForFinished(static_cast<int>(config_.stopTimeout.count()));
        delete listProcess_;
    }
    if (!process_) return;
    QPointer<QProcess> process = process_;
    disconnect(process, nullptr, this, nullptr);
    const bool wasRunning = process->state() != QProcess::NotRunning;
    process->kill();
    process->waitForFinished(static_cast<int>(config_.stopTimeout.count()));
    if (wasRunning) {
        flushInterval();
        publishLifecycle(EventType::CaptureStopped, Availability::Available,
                         QStringLiteral("browser shutdown"));
    }
    delete process;
    stopRequested_ = false;
    state_ = program_.isEmpty() ? State::Unavailable : State::Disabled;
}

void TSharkMonitor::readStdout()
{
    if (!process_) return;
    stdoutBuffer_ += process_->readAllStandardOutput();
    qsizetype start = 0;
    for (;;) {
        const qsizetype newline = stdoutBuffer_.indexOf('\n', start);
        if (newline < 0) break;
        aggregator_.AddLine(stdoutBuffer_.mid(start, newline - start).toStdString());
        start = newline + 1;
    }
    stdoutBuffer_.remove(0, start);
    // A runaway line without a newline is not TShark field output.
    if (stdoutBuffer_.size() > kMaxLineBytes) {
        stdoutBuffer_.clear();
        aggregator_.AddLine("<oversized line>");
    }
}

void TSharkMonitor::readStderr()
{
    if (!process_) return;
    stderrTail_ += process_->readAllStandardError();
    if (stderrTail_.size() > kStderrTailBytes) stderrTail_ = stderrTail_.right(kStderrTailBytes);
}

void TSharkMonitor::flushInterval()
{
    if (aggregator_.Empty() && intervalStart_ == std::chrono::steady_clock::time_point{}) return;
    const auto now = std::chrono::steady_clock::now();
    const double intervalMs = std::chrono::duration<double, std::milli>(now - intervalStart_).count();
    intervalStart_ = now;
    const aafr::tshark::IntervalSummary summary = aggregator_.Flush();
    // Until TShark has delivered a packet, a zero is not a measurement: it
    // may not be capturing at all (for example a wrong interface). Leave the
    // metrics "Not observed" rather than publish zeros.
    if (summary.total_packets == 0) return;
    for (TelemetryEvent &event : aafr::tshark::ToTelemetryEvents(summary, intervalMs)) {
        if (summary.malformed_lines > 0 && event.metric == "net.packets")
            event.attributes.push_back({"malformed_lines", std::to_string(summary.malformed_lines)});
        pipeline_.Publish(std::move(event));
    }
}

void TSharkMonitor::onFinished(int exitCode, QProcess::ExitStatus status)
{
    readStdout();
    if (!stdoutBuffer_.isEmpty()) {
        aggregator_.AddLine(stdoutBuffer_.toStdString());
        stdoutBuffer_.clear();
    }
    readStderr();
    flushTimer_.stop();
    killTimer_.stop();
    flushInterval();
    intervalStart_ = {};

    if (stopRequested_) {
        publishLifecycle(EventType::CaptureStopped, Availability::Available,
                         QStringLiteral("stopped by user"));
        setState(State::Disabled, tr("Capture off."));
    } else if (status == QProcess::NormalExit && exitCode == 0) {
        publishLifecycle(EventType::CaptureStopped, Availability::Available,
                         QStringLiteral("TShark exited"));
        setState(State::Disabled, tr("TShark exited. Capture off."));
    } else {
        QString detail = status == QProcess::CrashExit
                             ? tr("TShark stopped unexpectedly.")
                             : tr("TShark exited with code %1.").arg(exitCode);
        const QString reason = lastLine(stderrTail_);
        if (!reason.isEmpty()) detail += QLatin1Char(' ') + reason;
        publishLifecycle(EventType::CaptureError, Availability::Error, detail);
        setState(State::Error, detail);
    }
    stopRequested_ = false;
    if (process_) process_->deleteLater();
    process_ = nullptr;
}

void TSharkMonitor::onError(QProcess::ProcessError error)
{
    if (error != QProcess::FailedToStart || !process_) return;  // finished() handles the rest.
    const QString detail = tr("TShark could not be started: %1").arg(process_->errorString());
    flushTimer_.stop();
    publishLifecycle(EventType::CaptureError, Availability::Error, detail);
    setState(State::Error, detail);
    stopRequested_ = false;
    process_->deleteLater();
    process_ = nullptr;
}
