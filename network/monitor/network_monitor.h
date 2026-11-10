#pragma once

#include <QObject>
#include <QString>

// Interface for a network observation source. Monitors run independently of
// browser navigation and publish NetworkMetric events to a TelemetryPipeline.
// Implementations must be safe to construct when their tool is missing and
// must never start capturing until start() is called explicitly.
class NetworkMonitor : public QObject
{
    Q_OBJECT

public:
    enum class State {
        Disabled,     // Not capturing (the default).
        Unavailable,  // The required tool or driver is missing.
        Starting,
        Running,
        Stopping,
        Error,        // The last capture failed; start() may be retried.
    };
    Q_ENUM(State)

    using QObject::QObject;
    ~NetworkMonitor() override = default;

    virtual QString name() const = 0;
    virtual State state() const = 0;
    // Human-readable detail for the current state, e.g. the last error.
    virtual QString statusText() const = 0;
    // True if the tool was found; capture may still fail (permissions, driver).
    virtual bool isAvailable() const = 0;

    static QString toString(State state);

public slots:
    virtual bool start() = 0;
    virtual void stop() = 0;

signals:
    void stateChanged(NetworkMonitor::State state);
};
