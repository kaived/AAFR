#include "network/monitor/network_monitor.h"

QString NetworkMonitor::toString(State state)
{
    switch (state) {
    case State::Disabled: return tr("Off");
    case State::Unavailable: return tr("Unavailable");
    case State::Starting: return tr("Starting");
    case State::Running: return tr("Capturing");
    case State::Stopping: return tr("Stopping");
    case State::Error: return tr("Error");
    }
    return tr("Unknown");
}
