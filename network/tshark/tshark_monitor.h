#pragma once

#include <QByteArray>
#include <QPointer>
#include <QProcess>
#include <QStringList>
#include <QTimer>

#include <chrono>
#include <optional>

#include "network/monitor/network_monitor.h"
#include "network/tshark/tshark_fields.h"

namespace aafr::telemetry {
class TelemetryPipeline;
}

// Optional TShark adapter. Runs tshark asynchronously through QProcess,
// aggregates its field output and publishes one summary per interval.
// It never requests elevation or changes system settings: if the capture
// driver denies access, the error is reported and the browser carries on.
class TSharkMonitor final : public NetworkMonitor
{
    Q_OBJECT

public:
    struct Config {
        // Executable path. Empty: AAFR_TSHARK_PATH, then PATH, then the
        // default Wireshark install folder.
        QString program;
        // Capture interface name or number; empty lets TShark choose.
        QString interfaceName;
        QString captureFilter = QStringLiteral("tcp or udp");
        std::chrono::milliseconds flushInterval{1000};
        std::chrono::milliseconds stopTimeout{1500};
        // Replaces the generated arguments (used by tests with a fake tshark).
        std::optional<QStringList> argumentsOverride;
    };

    TSharkMonitor(aafr::telemetry::TelemetryPipeline &pipeline, Config config,
                  QObject *parent = nullptr);
    ~TSharkMonitor() override;

    QString name() const override { return QStringLiteral("TShark"); }
    State state() const override { return state_; }
    QString statusText() const override { return statusText_; }
    bool isAvailable() const override { return !program_.isEmpty(); }

    QString program() const { return program_; }
    QStringList arguments() const;
    const Config &config() const { return config_; }
    void setInterfaceName(const QString &name) { config_.interfaceName = name; }

    // Stops synchronously (bounded wait) for application shutdown.
    void shutdown();

    static QString locate(const QString &configured);

public slots:
    bool start() override;
    void stop() override;

private:
    void setState(State state, const QString &text);
    void readStdout();
    void readStderr();
    void flushInterval();
    void onFinished(int exitCode, QProcess::ExitStatus status);
    void onError(QProcess::ProcessError error);
    void publishLifecycle(aafr::telemetry::EventType type, aafr::telemetry::Availability availability,
                          const QString &detail);

    aafr::telemetry::TelemetryPipeline &pipeline_;
    Config config_;
    QString program_;
    QPointer<QProcess> process_;
    QTimer flushTimer_;
    QTimer killTimer_;
    aafr::tshark::IntervalAggregator aggregator_;
    std::chrono::steady_clock::time_point intervalStart_{};
    QByteArray stdoutBuffer_;
    QByteArray stderrTail_;
    State state_ = State::Disabled;
    QString statusText_;
    bool stopRequested_ = false;
};
