#pragma once

// ForwardingMeasurementCollector (V0.7.4): turns what the V0.7.3 browser
// forwarding gateway, the loopback relays, the runtime and the forwarding
// graph already record into provenance-tagged observations for the
// NetworkState ForwardingObservationStore. Plain C++17, no Qt.
//
// It adds no instrumentation to the forwarding hot path. Two entry points:
//
//   OnConnectionFinished(record)  gateway worker thread, after a browser
//                                 connection finished (the existing
//                                 GatewayBindings::on_finished hook); a few
//                                 observations, one bounded store push each.
//   Poll()                        the NetworkState step thread, once per
//                                 step: graph availability transitions,
//                                 confirmed configuration and switches,
//                                 gateway gauge and source heartbeats.
//
// What is observed, and how:
//   tunnel set-up   GatewayConnectionRecord::establish_ms, measured by the
//                   executor from tunnel request to the relays' TUNRSP OK
//                   (steady clock), only when the path was confirmed
//   failures        relay failure, timeout, destination failure, from the
//                   record's forwarding status (no value)
//   duration/bytes  accept -> close, bytes both ways (finished connections)
//   first byte      plain HTTP only (CONNECT carries opaque TLS)
//   relay hops      consecutive StreamTraversal::opened_ns of the relays a
//                   tunnel crossed, per traversed edge only
//   link events     effective edge availability (edge and both nodes
//                   available) transitions between graph snapshots
//   reroutes        confirmed configuration generation changes
// Nothing here reads a URL path, header value, cookie or payload byte.

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

#include "network/network_state/forwarding_observations.h"
#include "routing/gateway/browser_forwarding_gateway.h"
#include "routing/topology/forwarding_graph.h"

namespace aafr::runtime {
class AAFRRuntime;
}

namespace aafr::gateway {

struct CollectorSources {
  // Optional: graph snapshots (topology transitions); same source as the runtime's.
  std::function<std::shared_ptr<const forwarding::ForwardingGraph>()> graph_source;
  // Optional: the confirmed configuration and its switches.
  const runtime::AAFRRuntime* runtime{nullptr};
  // Optional: the active-connection gauge and the gateway heartbeat.
  const AAFRBrowserForwardingGateway* gateway{nullptr};
  // The relays sleep configured edge delays: measured durations on these
  // paths are labelled Impairment::EmulatedEdgeDelay.
  bool executor_injects_delay{false};
};

struct CollectorStats {
  std::uint64_t connections{0};
  std::uint64_t observations{0};
  std::uint64_t rejected{0};
  std::uint64_t polls{0};
  std::uint64_t link_transitions{0};
  std::uint64_t switches{0};
  double last_record_us{0.0};
  double max_record_us{0.0};
  double last_poll_us{0.0};
  double max_poll_us{0.0};
};

class ForwardingMeasurementCollector {
 public:
  ForwardingMeasurementCollector(std::shared_ptr<netstate::ForwardingObservationStore> store, CollectorSources sources);

  void OnConnectionFinished(const GatewayConnectionRecord& record);
  void Poll();
  // The gateway may be attached after construction (the browser binds it
  // once forwarding starts). It must outlive the collector's polls.
  void SetGateway(const AAFRBrowserForwardingGateway* gateway) { gateway_.store(gateway); }

  CollectorStats Stats() const;
  const std::shared_ptr<netstate::ForwardingObservationStore>& store() const { return store_; }

 private:
  struct GenerationInfo {
    std::string graph_id;
    std::uint64_t graph_version{0};
    std::string path;
  };
  void Push(const netstate::ForwardingObservation& o, std::int64_t now_ns);
  netstate::ObservationProvenance Base(const GatewayConnectionRecord& record, netstate::ObservationSource source,
                                       const std::string& component, const std::string& suffix) const;

  const std::shared_ptr<netstate::ForwardingObservationStore> store_;
  const CollectorSources sources_;
  std::atomic<const AAFRBrowserForwardingGateway*> gateway_{nullptr};

  mutable std::mutex mutex_;
  CollectorStats stats_;
  std::map<std::uint64_t, GenerationInfo> generations_;
  std::uint64_t last_generation_{0};
  bool have_links_{false};
  std::map<std::string, bool> links_;  // "from->to" -> effectively available
  std::uint64_t last_graph_version_{0};
  std::uint64_t event_seq_{0};
};

}  // namespace aafr::gateway
