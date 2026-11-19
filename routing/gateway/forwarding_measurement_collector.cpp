#include "routing/gateway/forwarding_measurement_collector.h"

#include <algorithm>
#include <chrono>
#include <vector>

#include "runtime/adaptive/aafr_runtime.h"

namespace aafr::gateway {

namespace ns = aafr::netstate;
namespace fwd = aafr::forwarding;

namespace {
double MicrosSince(std::chrono::steady_clock::time_point start) {
  return std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count();
}

std::string PathText(const fwd::Path& path) {
  std::string out;
  for (const std::string& node : path) out += (out.empty() ? "" : ">") + node;
  return out;
}

std::string FailureOutcome(const GatewayConnectionRecord& r) {
  if (r.relay_failure) return "relay_failure";
  if (r.forwarding_status == "TIMEOUT") return "timeout";
  if (!r.forwarding_status.empty() && r.forwarding_status != "OK") return "destination_failure";
  return "rejected";
}
}  // namespace

ForwardingMeasurementCollector::ForwardingMeasurementCollector(std::shared_ptr<ns::ForwardingObservationStore> store,
                                                               CollectorSources sources)
    : store_(std::move(store)), sources_(std::move(sources)), gateway_(sources_.gateway) {}

CollectorStats ForwardingMeasurementCollector::Stats() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return stats_;
}

void ForwardingMeasurementCollector::Push(const ns::ForwardingObservation& o, std::int64_t now_ns) {
  const ns::ObservationValidity v = store_->Ingest(o, now_ns);
  std::lock_guard<std::mutex> lock(mutex_);
  ++stats_.observations;
  if (v != ns::ObservationValidity::Accepted) ++stats_.rejected;
}

ns::ObservationProvenance ForwardingMeasurementCollector::Base(const GatewayConnectionRecord& r,
                                                               ns::ObservationSource source,
                                                               const std::string& component,
                                                               const std::string& suffix) const {
  ns::ObservationProvenance p;
  p.measurement_id = r.connection_id + ":" + suffix;
  p.source_component = component;
  p.source_type = source;
  p.wall_time = telemetry::WallClock::now();
  p.connection_id = r.connection_id;
  p.config_generation = r.generation;
  p.path = PathText(r.expected_path);
  p.impairment = sources_.executor_injects_delay ? ns::Impairment::EmulatedEdgeDelay : ns::Impairment::None;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto g = generations_.find(r.generation);
    if (g != generations_.end()) {
      p.graph_id = g->second.graph_id;
      p.graph_version = g->second.graph_version;
    }
  }
  return p;
}

void ForwardingMeasurementCollector::OnConnectionFinished(const GatewayConnectionRecord& r) {
  if (!store_ || r.generation == 0) return;
  const auto started = std::chrono::steady_clock::now();
  const std::int64_t now = ns::ForwardingObservationStore::SteadyNowNs();
  const std::int64_t head_end = r.accepted_ns + static_cast<std::int64_t>(r.head_ms.value_or(0.0) * 1e6);

  // Tunnel set-up (or its failure).
  if (r.path_confirmed && r.establish_ms) {
    ns::ForwardingObservation o;
    o.kind = ns::ObservationKind::TunnelEstablished;
    o.provenance = Base(r, ns::ObservationSource::Gateway, "gateway", "establish");
    o.provenance.unit = "ms";
    o.provenance.interval_start_ns = head_end;
    o.provenance.monotonic_ns = head_end + static_cast<std::int64_t>(*r.establish_ms * 1e6);
    o.provenance.method = "executor-measured tunnel request -> relay-confirmed TUNRSP OK (steady_clock)";
    o.value = *r.establish_ms;
    o.outcome = "established";
    Push(o, now);
  } else if (r.state == ConnectionState::Failed || r.state == ConnectionState::Rejected ||
             (!r.path_confirmed && r.state != ConnectionState::Cancelled)) {
    ns::ForwardingObservation o;
    o.kind = ns::ObservationKind::TunnelFailed;
    o.provenance = Base(r, ns::ObservationSource::Gateway, "gateway", "failed");
    o.provenance.unit = "event";
    o.provenance.interval_start_ns = r.accepted_ns;
    o.provenance.monotonic_ns = r.finished_ns > 0 ? r.finished_ns : now;
    o.provenance.method = "gateway connection state and executor forwarding status";
    o.outcome = FailureOutcome(r);
    Push(o, now);
  }

  // Connection lifetime and bytes.
  if (r.path_confirmed && r.duration_ms && r.finished_ns > 0) {
    ns::ForwardingObservation o;
    o.kind = ns::ObservationKind::ConnectionCompleted;
    o.provenance = Base(r, ns::ObservationSource::Gateway, "gateway", "completed");
    o.provenance.unit = "ms";
    o.provenance.interval_start_ns = r.accepted_ns;
    o.provenance.monotonic_ns = r.finished_ns;
    o.provenance.method = "gateway accept -> close (steady_clock), byte counters";
    o.value = *r.duration_ms;
    o.bytes_up = r.bytes_from_client;
    o.bytes_down = r.bytes_to_client;
    o.outcome = r.end;
    Push(o, now);
  }

  // Plain HTTP time to first byte.
  if (r.first_byte_ms && r.finished_ns > 0) {
    ns::ForwardingObservation o;
    o.kind = ns::ObservationKind::FirstByte;
    o.provenance = Base(r, ns::ObservationSource::Gateway, "gateway", "first_byte");
    o.provenance.unit = "ms";
    o.provenance.interval_start_ns = r.accepted_ns;
    o.provenance.monotonic_ns = r.finished_ns;
    o.provenance.method = "gateway request forwarded -> first response byte (steady_clock)";
    o.value = *r.first_byte_ms;
    Push(o, now);
  }

  // Relay hops: only edges this tunnel really crossed, each from both
  // relays' own records (same process, same steady clock).
  std::vector<fwd::StreamTraversal> t = r.relay_traversals;
  std::sort(t.begin(), t.end(), [](const fwd::StreamTraversal& a, const fwd::StreamTraversal& b) {
    return a.opened_ns < b.opened_ns;
  });
  for (std::size_t i = 0; i + 1 < t.size(); ++i) {
    if (t[i].next_hop != t[i + 1].node || t[i].opened_ns <= 0 || t[i + 1].opened_ns < t[i].opened_ns) continue;
    ns::ForwardingObservation o;
    o.kind = ns::ObservationKind::RelayHop;
    o.provenance = Base(r, ns::ObservationSource::Relay, "relay:" + t[i + 1].node,
                        "hop:" + t[i].node + ">" + t[i + 1].node);
    o.provenance.unit = "ms";
    o.provenance.interval_start_ns = t[i].opened_ns;
    o.provenance.monotonic_ns = t[i + 1].opened_ns;
    o.provenance.node = t[i + 1].node;
    o.provenance.edge_from = t[i].node;
    o.provenance.edge_to = t[i + 1].node;
    o.provenance.method = "relay StreamTraversal opened_ns difference (steady_clock)";
    o.value = static_cast<double>(t[i + 1].opened_ns - t[i].opened_ns) / 1e6;
    Push(o, now);
  }
  if (!t.empty()) store_->Heartbeat(ns::ObservationSource::Relay, now);

  std::lock_guard<std::mutex> lock(mutex_);
  ++stats_.connections;
  stats_.last_record_us = MicrosSince(started);
  stats_.max_record_us = std::max(stats_.max_record_us, stats_.last_record_us);
}

void ForwardingMeasurementCollector::Poll() {
  if (!store_) return;
  const auto started = std::chrono::steady_clock::now();
  const std::int64_t now = ns::ForwardingObservationStore::SteadyNowNs();

  // ---- Topology: effective link availability transitions ----
  if (sources_.graph_source) {
    if (const auto graph = sources_.graph_source()) {
      std::map<std::string, bool> links;
      for (const auto& e : graph->edges()) {
        const auto* a = graph->FindNode(e.from);
        const auto* b = graph->FindNode(e.to);
        links[e.from + "->" + e.to] = e.available && a && a->available && b && b->available;
      }
      std::vector<std::pair<std::string, bool>> transitions;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        if (have_links_) {
          for (const auto& l : links) {
            const auto before = links_.find(l.first);
            if (before != links_.end() && before->second != l.second) transitions.emplace_back(l.first, l.second);
          }
        }
        links_ = links;
        have_links_ = true;
        last_graph_version_ = graph->version();
      }
      for (const auto& tr : transitions) {
        ns::ForwardingObservation o;
        o.kind = ns::ObservationKind::LinkTransition;
        std::uint64_t seq;
        {
          std::lock_guard<std::mutex> lock(mutex_);
          seq = ++event_seq_;
          ++stats_.link_transitions;
        }
        o.provenance.measurement_id = "topology:" + std::to_string(graph->version()) + ":" + tr.first + ":" +
                                      std::to_string(seq);
        o.provenance.source_component = "forwarding_graph";
        o.provenance.source_type = ns::ObservationSource::Topology;
        o.provenance.unit = "event";
        o.provenance.monotonic_ns = now;
        o.provenance.interval_start_ns = now;
        o.provenance.wall_time = telemetry::WallClock::now();
        o.provenance.graph_id = graph->id();
        o.provenance.graph_version = graph->version();
        const auto arrow = tr.first.find("->");
        o.provenance.edge_from = tr.first.substr(0, arrow);
        o.provenance.edge_to = tr.first.substr(arrow + 2);
        o.provenance.method = "effective edge availability (edge and both nodes) compared between graph snapshots";
        o.outcome = tr.second ? "recovery" : "failure";
        Push(o, now);
      }
      store_->Heartbeat(ns::ObservationSource::Topology, now);
    }
  }

  // ---- Runtime: confirmed configuration and switches ----
  if (sources_.runtime) {
    if (const auto active = sources_.runtime->ConfirmedConfiguration()) {
      bool switched = false;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        generations_[active->config_version] = {active->graph_id, active->graph_version, PathText(active->path)};
        while (generations_.size() > 64) generations_.erase(generations_.begin());
        if (active->config_version > last_generation_) {
          switched = last_generation_ != 0;
          last_generation_ = active->config_version;
          if (switched) ++stats_.switches;
        }
      }
      store_->SetCurrentConfiguration(active->graph_id, active->graph_version, active->config_version,
                                      PathText(active->path),
                                      sources_.executor_injects_delay ? ns::Impairment::EmulatedEdgeDelay
                                                                      : ns::Impairment::None);
      if (switched) {
        ns::ForwardingObservation o;
        o.kind = ns::ObservationKind::ConfirmedSwitch;
        o.provenance.measurement_id = "runtime:switch:" + std::to_string(active->config_version);
        o.provenance.source_component = "runtime";
        o.provenance.source_type = ns::ObservationSource::Runtime;
        o.provenance.unit = "event";
        o.provenance.monotonic_ns = now;
        o.provenance.interval_start_ns = now;
        o.provenance.wall_time = telemetry::WallClock::now();
        o.provenance.graph_id = active->graph_id;
        o.provenance.graph_version = active->graph_version;
        o.provenance.config_generation = active->config_version;
        o.provenance.path = PathText(active->path);
        o.provenance.method = "confirmed configuration generation advanced (observed at poll)";
        o.outcome = "switch";
        Push(o, now);
      }
    }
    store_->Heartbeat(ns::ObservationSource::Runtime, now);
  }

  // ---- Gateway gauge and heartbeat ----
  if (const AAFRBrowserForwardingGateway* gateway = gateway_.load()) {
    const GatewayStatus status = gateway->Status();
    if (status.running && status.has_bindings) {
      store_->SetActiveConnections(status.active_connections, now);
      store_->Heartbeat(ns::ObservationSource::Gateway, now);
    }
  }

  std::lock_guard<std::mutex> lock(mutex_);
  ++stats_.polls;
  stats_.last_poll_us = MicrosSince(started);
  stats_.max_poll_us = std::max(stats_.max_poll_us, stats_.last_poll_us);
}

}  // namespace aafr::gateway
