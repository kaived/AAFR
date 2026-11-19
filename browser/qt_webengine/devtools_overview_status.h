#pragma once

// UI-only translation of observed provider states. No evaluation or actuation.
#include <QStringList>
#include "browser_event_collector.h"
#include "network/monitor/network_monitor.h"
#include "research/research_host.h"
#include "routing/gateway/browser_forwarding_gateway.h"
#include "runtime/adaptive/aafr_runtime.h"

namespace aafr::ui {

struct OverviewInputs {
    std::optional<BrowserEventCollector::NavigationStatus> browser;
    std::optional<NetworkMonitor::State> capture;
    bool controlled = false;
    std::optional<runtime::RuntimeStatus> runtime;
    std::optional<observability::RuntimeSnapshot> production;
    std::optional<gateway::GatewayStatus> gateway;
    std::optional<research::ResearchObservabilitySnapshot> research;
};

struct OverviewStatus {
    QString browser = QObject::tr("Unavailable");
    QString capture = QObject::tr("Unavailable");
    QString mode;
    QString runtime = QObject::tr("Unavailable");
    QString algorithm = QObject::tr("Unavailable");
    QString research = QObject::tr("Unavailable");
    QString path = QObject::tr("Unavailable");
};

inline QString overviewAlgorithm(const std::string &id)
{
    const auto algorithm = algorithms::ParseAlgorithmId(id);
    if (!algorithm) return QObject::tr("Unavailable");
    switch (*algorithm) {
    case algorithms::AlgorithmId::Dijkstra: return QStringLiteral("Dijkstra");
    case algorithms::AlgorithmId::AStar: return QStringLiteral("A* (A-star)");
    case algorithms::AlgorithmId::BellmanFord: return QStringLiteral("Bellman-Ford");
    case algorithms::AlgorithmId::AntColonyOptimization: return QStringLiteral("ACO");
    case algorithms::AlgorithmId::ParticleSwarmOptimization: return QStringLiteral("PSO");
    case algorithms::AlgorithmId::GeneticAlgorithm: return QStringLiteral("GA");
    case algorithms::AlgorithmId::ArtificialBeeColony: return QStringLiteral("ABC");
    case algorithms::AlgorithmId::GreyWolfOptimizer: return QStringLiteral("GWO");
    case algorithms::AlgorithmId::FireflyAlgorithm: return QStringLiteral("Firefly");
    case algorithms::AlgorithmId::WhaleOptimizationAlgorithm: return QStringLiteral("WOA");
    }
    return QObject::tr("Unavailable");
}

inline QString overviewPath(const forwarding::Path &path)
{
    if (path.empty()) return QObject::tr("Unavailable");
    QStringList nodes;
    for (const auto &id : path) {
        if (id == "entry") nodes << QObject::tr("Entry");
        else if (id == "dest") nodes << QObject::tr("Destination");
        else if (id == "relay_a") nodes << QObject::tr("Relay A");
        else if (id == "relay_b") nodes << QObject::tr("Relay B");
        else nodes << QString::fromStdString(id);
    }
    return nodes.join(QStringLiteral(" → "));
}

inline OverviewStatus overviewStatus(const OverviewInputs &in)
{
    OverviewStatus out;
    using Nav = BrowserEventCollector::NavigationStatus;
    if (in.browser) switch (*in.browser) {
    case Nav::Succeeded: out.browser = QObject::tr("Page loaded"); break;
    case Nav::Loading: out.browser = QObject::tr("Loading"); break;
    case Nav::Idle: case Nav::Stopped: out.browser = QObject::tr("Idle"); break;
    case Nav::Failed: case Nav::RendererTerminated: out.browser = QObject::tr("Error"); break;
    }
    if (in.capture) switch (*in.capture) {
    case NetworkMonitor::State::Running: out.capture = QObject::tr("Active"); break;
    case NetworkMonitor::State::Error: out.capture = QObject::tr("Error"); break;
    case NetworkMonitor::State::Unavailable: break;
    case NetworkMonitor::State::Disabled: case NetworkMonitor::State::Starting:
    case NetworkMonitor::State::Stopping: out.capture = QObject::tr("Inactive"); break;
    }
    out.mode = in.controlled ? QObject::tr("Controlled") : QObject::tr("Direct");
    if (in.runtime) {
        const auto &r = *in.runtime;
        using Life = runtime::RuntimeLifecycle;
        switch (r.lifecycle) {
        case Life::Ready:
            if (r.active && r.active_verified) out.runtime = QObject::tr("Ready");
            break;
        case Life::Evaluating: out.runtime = QObject::tr("Evaluating"); break;
        case Life::Switching: out.runtime = QObject::tr("Switching"); break;
        case Life::Degraded: case Life::Failed: out.runtime = QObject::tr("Error"); break;
        case Life::Uninitialized: case Life::Initializing: case Life::Stopping: case Life::Stopped: break;
        }
        const bool installed = r.lifecycle == Life::Ready || r.lifecycle == Life::Evaluating ||
            r.lifecycle == Life::Switching || r.lifecycle == Life::Degraded;
        if (r.active && r.active_verified && installed) {
            out.algorithm = overviewAlgorithm(algorithms::ToString(r.active->algorithm));
            out.path = overviewPath(r.active->path);
        }
    } else if (in.production) {
        const auto &r = *in.production;
        const bool confirmed = r.active_algorithm &&
            r.active_algorithm->execution_state == observability::ExecutionState::Active;
        if (r.state == observability::RuntimeState::Degraded) out.runtime = QObject::tr("Error");
        if (confirmed && (r.state == observability::RuntimeState::Running || r.state == observability::RuntimeState::Degraded))
            out.algorithm = overviewAlgorithm(r.active_algorithm->algorithm_id);
        // Running does not distinguish Ready/Evaluating/Switching. Keep the
        // precise phase and path unavailable instead of inferring readiness.
    } else if (in.controlled && in.gateway && in.gateway->running && in.gateway->has_bindings && in.gateway->current) {
        out.algorithm = overviewAlgorithm(in.gateway->current->algorithm);
        out.path = overviewPath(in.gateway->current->path);
    }
    if (in.research && in.research->host_state == research::ResearchHostState::Running && in.research->adapter_present) {
        const auto &r = *in.research;
        using State = research::ResearchRuntimeState;
        if (r.adapter.state == State::Error) out.research = QObject::tr("Error");
        else if (r.adapter.state != State::Unavailable) {
            if (r.evaluation_pending || r.adapter.state == State::Evaluating) out.research = QObject::tr("Evaluating");
            else if (r.adapter.state == State::Idle) out.research = QObject::tr("Ready");
            else if (r.adapter.state == State::Decided) out.research = QObject::tr("Evaluation complete");
        }
    }
    return out;
}

}  // namespace aafr::ui
