#include "runtime/model/feature_acquisition.h"

namespace aafr::model {

const char* ToString(FeatureCategory value) {
  switch (value) {
    case FeatureCategory::DirectlyObservable: return "A_direct";
    case FeatureCategory::DerivedFromObservations: return "B_derived";
    case FeatureCategory::Estimated: return "C_estimated";
    case FeatureCategory::Unavailable: return "D_unavailable";
    case FeatureCategory::Incompatible: return "E_incompatible";
  }
  return "unknown";
}

char Letter(FeatureCategory value) {
  switch (value) {
    case FeatureCategory::DirectlyObservable: return 'A';
    case FeatureCategory::DerivedFromObservations: return 'B';
    case FeatureCategory::Estimated: return 'C';
    case FeatureCategory::Unavailable: return 'D';
    case FeatureCategory::Incompatible: return 'E';
  }
  return '?';
}

namespace {

using C = FeatureCategory;

const char* kPre = "none in AAFR; the worker's features.py engineer_features() uses it unchanged, inf/NaN -> 0";
const char* kGraphSource = "forwarding graph snapshot + ForwardingProblem (ComputeGraphFeatures)";
const char* kGraphFresh = "valid until the graph version changes (recomputed per evaluation)";
const char* kGraphMissing = "abstain when the graph is absent or the destination unreachable";
const char* kGraphDomain =
    "same formula as simulator/topology.py, but AAFR's controlled topology (4 nodes, 5 edges) lies far "
    "outside the training support (20-650 nodes)";
const char* kNoValue = "abstain (E_FEATURES_INCOMPLETE); never 0, never a default";

FeatureAcquisitionSpec Graph(const char* name, int index, const char* type, const char* unit, const char* training,
                             const char* rule) {
  return {name, index, type, unit, training, kPre, kGraphSource,
          "breadth-first search / Jacobi eigenvalues on the available directed graph and its undirected projection",
          kGraphFresh, kGraphMissing, C::DirectlyObservable, kGraphDomain, rule, ""};
}

FeatureAcquisitionSpec Estimate(const char* name, int index, const char* unit, const char* training,
                                const char* fwd_id, const char* method, const char* domain) {
  return {name, index, "float64", unit, training, kPre, fwd_id, method,
          "estimate only: 30 s window, newest sample <= 15 s, minimum samples per statistic",
          "not accepted as the model feature even when the estimate is Valid: abstain", C::Estimated, domain,
          "never satisfies the measured-input contract", fwd_id};
}

FeatureAcquisitionSpec Unavailable(const char* name, int index, const char* type, const char* unit,
                                   const char* training, const char* why, FeatureCategory category) {
  return {name, index, type, unit, training, kPre, "none", why, "n/a", kNoValue, category, why,
          "no AAFR value exists; any offered measured value is rejected as unsupported", ""};
}

}  // namespace

const std::vector<FeatureAcquisitionSpec>& FeatureAcquisitionContract() {
  static const std::vector<FeatureAcquisitionSpec> kSpecs = {
      // ---- Topology (A) ----
      Graph("num_nodes", 0, "int64", "count", "G.number_of_nodes() of the simulated topology", "integer >= 1"),
      Graph("num_edges", 1, "int64", "count", "G.number_of_edges() of the directed simulated topology", "integer >= 0"),
      Graph("network_density", 2, "float64", "ratio", "2|E_undirected| / (n(n-1)), rounded to 6 digits", "0..1"),
      Graph("avg_node_degree", 3, "float64", "count", "2|E_undirected| / n, rounded to 3 digits", ">= 0"),
      Graph("diameter", 4, "int64", "hops",
            "exact diameter of the undirected projection (largest component) for n <= 100, 2-sweep BFS above",
            "integer >= 0"),
      Graph("avg_clustering_coefficient", 5, "float64", "ratio", "networkx.average_clustering(undirected), 4 digits",
            "0..1"),
      Graph("algebraic_connectivity", 6, "float64", "eigenvalue",
            "second-smallest Laplacian eigenvalue for 2 <= n <= 150 and connected, else 0; 4 digits", ">= 0"),
      // ---- Request (A) ----
      Graph("req_shortest_hops", 7, "int64", "hops", "BFS hop count source -> destination on the directed graph",
            "integer >= 0; abstain when unreachable"),
      Graph("source_in_degree", 8, "int64", "count", "G.in_degree(source)", "integer >= 0"),
      Graph("source_out_degree", 9, "int64", "count", "G.out_degree(source)", "integer >= 0"),
      Graph("dest_in_degree", 10, "int64", "count", "G.in_degree(destination)", "integer >= 0"),
      Graph("dest_out_degree", 11, "int64", "count", "G.out_degree(destination)", "integer >= 0"),
      // ---- Telemetry ----
      Estimate("latency", 12, "ms",
               "mean one-way delivery latency of every delivered simulated packet of every flow (network-wide, "
               "includes queueing); 500 when nothing was delivered",
               "fwd.path.establish_ms.mean",
               "gateway tunnel set-up duration through the relays (TCP connects hop by hop + relay processing + "
               "any emulated delay), active path only",
               "a per-connection application-layer set-up time on one path, not a per-packet one-way delay over "
               "all flows"),
      Estimate("jitter", 13, "ms", "mean |delta| of consecutive delivered packets' transit times (RFC 3393 style)",
               "fwd.path.establish_jitter_ms", "same formula over consecutive tunnel set-up durations",
               "formula matches, quantity does not (connections, not packets)"),
      Unavailable("loss", 14, "float64", "ratio", "dropped packets / sent packets (buffer overflow + channel loss)",
                  "TCP over loopback hides packet loss; connection failures are not packet drops and TShark cannot "
                  "see the relays' loopback sockets without a loopback capture adapter",
                  C::Unavailable),
      Estimate("throughput", 15, "mbps", "8 * delivered bytes of all flows / simulated duration",
               "fwd.path.goodput_mbps.mean", "8 * bytes / connection lifetime per finished browser connection",
               "application-limited goodput including idle keep-alive time, not delivered network throughput"),
      Unavailable("mean_link_utilization", 16, "float64", "percent",
                  "mean over edges of transmitted bytes / (bandwidth * duration) * 100",
                  "a loopback TCP relay edge has no link capacity; the graph's bandwidth is a configured nominal "
                  "value, and dividing measured bytes by it would present a configured value as a measured condition",
                  C::Incompatible),
      Unavailable("max_link_utilization", 17, "float64", "percent", "maximum over edges of the utilization above",
                  "no link capacity in application-layer relays (see mean_link_utilization)", C::Incompatible),
      Unavailable("link_utilization_std", 18, "float64", "percent", "standard deviation over edges of the utilization",
                  "no link capacity in application-layer relays (see mean_link_utilization)", C::Incompatible),
      Unavailable("avg_queue_delay_ms", 19, "float64", "ms", "mean total queueing delay of delivered packets",
                  "the relays keep no packet queue; kernel socket-buffer occupancy is not observable from AAFR",
                  C::Unavailable),
      Unavailable("max_queue_depth", 20, "int64", "packets", "largest queue observed on any edge (packets)",
                  "no packet queue exists or is observable in the relays", C::Unavailable),
      Unavailable("avg_queue_occupancy", 21, "float64", "ratio", "mean over edges of max queue / queue limit",
                  "no packet queue exists or is observable in the relays", C::Unavailable),
      Estimate("offered_load_mbps", 22, "mbps", "8 * bytes of all generated packets (delivered or not) / duration",
               "fwd.gateway.forwarded_mbps", "8 * bytes of connections that finished in the window / window",
               "delivered (TCP) bytes of browser traffic, not offered load of a traffic generator"),
      Unavailable("packet_delivery_ratio", 23, "float64", "ratio", "delivered packets / sent packets",
                  "packet-level delivery is not observable (TCP retransmits hide drops); the connection success "
                  "ratio fwd.path.success_ratio is a different quantity",
                  C::Unavailable),
      Estimate("jains_fairness_index", 24, "ratio", "Jain's index over per-flow delivered rates",
               "fwd.path.goodput_fairness", "Jain's index over per-connection goodput of finished connections",
               "browser connections are application-limited, so unequal goodput is not unfairness"),
      // ---- Latency distribution (C) ----
      Estimate("latency_p50", 25, "ms", "50th percentile of delivered packets' latency", "fwd.path.establish_ms.p50",
               "tunnel set-up durations, linear-interpolated percentile", "see latency"),
      Estimate("latency_p90", 26, "ms", "90th percentile of delivered packets' latency", "fwd.path.establish_ms.p90",
               "tunnel set-up durations, needs >= 10 samples", "see latency"),
      Estimate("latency_p95", 27, "ms", "95th percentile of delivered packets' latency", "fwd.path.establish_ms.p95",
               "tunnel set-up durations, needs >= 20 samples", "see latency"),
      Estimate("latency_p99", 28, "ms", "99th percentile of delivered packets' latency", "fwd.path.establish_ms.p99",
               "tunnel set-up durations, needs >= 100 samples", "see latency"),
      Estimate("latency_min", 29, "ms", "minimum delivered packet latency", "fwd.path.establish_ms.min",
               "tunnel set-up durations", "see latency"),
      Estimate("latency_max", 30, "ms", "maximum delivered packet latency", "fwd.path.establish_ms.max",
               "tunnel set-up durations", "see latency"),
      Estimate("latency_std", 31, "ms", "population standard deviation of delivered packet latency",
               "fwd.path.establish_ms.std", "tunnel set-up durations (numpy.std)", "see latency"),
      // ---- Dynamics (B) ----
      {"link_failure_events", 32, "int64", "count",
       "simulated link failures during the 1.5 s run (simulator/dynamic.py)", kPre, "fwd.topology.link_failure_events",
       "effective edge availability true -> false between consecutive forwarding-graph snapshots",
       "valid only when the topology source observed the whole 30 s window; zero is a real observation",
       "abstain when the topology source is missing, disabled or observed less than the window",
       C::DerivedFromObservations,
       "event semantics match (a link stops carrying traffic), but the window is 30 s instead of 1.5 s",
       "integer >= 0, topology source active for the full window", "fwd.topology.link_failure_events"},
      {"link_recovery_events", 33, "int64", "count", "simulated link recoveries during the run", kPre,
       "fwd.topology.link_recovery_events",
       "effective edge availability false -> true between consecutive forwarding-graph snapshots",
       "valid only when the topology source observed the whole 30 s window",
       "abstain when the topology source is missing, disabled or observed less than the window",
       C::DerivedFromObservations, "as link_failure_events", "integer >= 0, topology source active for the full window",
       "fwd.topology.link_recovery_events"},
      {"reroute_count", 34, "int64", "count", "flows rerouted by the simulator after link failures", kPre,
       "fwd.runtime.reroute_count", "confirmed configuration generation changes of the AAFR runtime",
       "valid only when the runtime source observed the whole 30 s window",
       "abstain when the runtime source is missing or observed less than the window", C::DerivedFromObservations,
       "AAFR reroutes the browser's configuration, not individual flows; window 30 s instead of 1.5 s",
       "integer >= 0, runtime source active for the full window", "fwd.runtime.reroute_count"},
  };
  return kSpecs;
}

const FeatureAcquisitionSpec* FindAcquisitionSpec(const std::string& name) {
  for (const FeatureAcquisitionSpec& s : FeatureAcquisitionContract()) {
    if (s.name == name) return &s;
  }
  return nullptr;
}

const std::vector<ModelColumnSpec>& EngineeredColumnContract() {
  using D = FeatureCategory;
  static const std::vector<ModelColumnSpec> kColumns = {
      {"congestion_ratio", 35, "offered_load_mbps / clip(throughput, 0.01)", {"offered_load_mbps", "throughput"}, D::DerivedFromObservations, "worker"},
      {"loss_jitter_product", 36, "loss * jitter", {"loss", "jitter"}, D::DerivedFromObservations, "worker"},
      {"topology_complexity", 37, "diameter * req_shortest_hops / clip(avg_node_degree, 1)", {"diameter", "req_shortest_hops", "avg_node_degree"}, D::DerivedFromObservations, "worker"},
      {"utilization_skew", 38, "max_link_utilization / clip(mean_link_utilization, 0.001)", {"max_link_utilization", "mean_link_utilization"}, D::DerivedFromObservations, "worker"},
      {"degree_asymmetry", 39, "|source_out_degree - dest_in_degree|", {"source_out_degree", "dest_in_degree"}, D::DerivedFromObservations, "worker"},
      {"latency_range", 40, "latency_max - latency_min", {"latency_max", "latency_min"}, D::DerivedFromObservations, "worker"},
      {"latency_tail_ratio", 41, "latency_p99 / clip(latency_p50, 0.01)", {"latency_p99", "latency_p50"}, D::DerivedFromObservations, "worker"},
      {"path_length_ratio", 42, "req_shortest_hops / clip(diameter, 1)", {"req_shortest_hops", "diameter"}, D::DerivedFromObservations, "worker"},
      {"edge_density_per_node", 43, "num_edges / clip(num_nodes, 1)", {"num_edges", "num_nodes"}, D::DerivedFromObservations, "worker"},
      {"capacity_headroom", 44, "1 - clip(mean_link_utilization, upper=1)  (percent clipped as a ratio, as trained)", {"mean_link_utilization"}, D::DerivedFromObservations, "worker"},
      {"is_dynamic", 45, "(link_failure_events + reroute_count) > 0", {"link_failure_events", "reroute_count"}, D::DerivedFromObservations, "worker"},
      {"network_scale_log", 46, "log1p(num_nodes)", {"num_nodes"}, D::DerivedFromObservations, "worker"},
      {"clustering_x_density", 47, "avg_clustering_coefficient * network_density", {"avg_clustering_coefficient", "network_density"}, D::DerivedFromObservations, "worker"},
      {"cat_A_Excellent", 48, "scenario_category == A_Excellent", {}, D::Incompatible, "training label; 0 at inference (never seen in training)"},
      {"cat_B_Congested", 49, "scenario_category == B_Congested", {}, D::Incompatible, "training label; 0 at inference"},
      {"cat_C_Unreliable", 50, "scenario_category == C_Unreliable", {}, D::Incompatible, "training label; 0 at inference (stage-1 importance 1.0)"},
      {"cat_D_Sparse", 51, "scenario_category == D_Sparse", {}, D::Incompatible, "training label; 0 at inference (stage-1 importance 0.4)"},
      {"cat_E_Dense", 52, "scenario_category == E_Dense", {}, D::Incompatible, "training label; 0 at inference"},
      {"cat_F_Dynamic", 53, "scenario_category == F_Dynamic", {}, D::Incompatible, "training label; 0 at inference"},
      {"cat_G_Large", 54, "scenario_category == G_Large", {}, D::Incompatible, "training label; 0 at inference"},
  };
  return kColumns;
}

std::vector<std::string> ValidateAcquisitionContract(const ModelManifest* manifest) {
  std::vector<std::string> issues;
  const auto& raw = RawFeatureContract();
  const auto& acq = FeatureAcquisitionContract();
  if (acq.size() != raw.size()) issues.push_back("acquisition contract has " + std::to_string(acq.size()) + " features, raw contract " + std::to_string(raw.size()));
  for (std::size_t i = 0; i < acq.size() && i < raw.size(); ++i) {
    if (acq[i].name != raw[i].name) issues.push_back("feature " + std::to_string(i) + ": " + acq[i].name + " != " + raw[i].name);
    if (acq[i].index != static_cast<int>(i)) issues.push_back(acq[i].name + ": index " + std::to_string(acq[i].index));
    if (acq[i].unit != raw[i].unit) issues.push_back(acq[i].name + ": unit " + acq[i].unit + " != " + raw[i].unit);
    if ((acq[i].data_type == "int64") != raw[i].integer) issues.push_back(acq[i].name + ": integer flag differs");
    if (acq[i].category == FeatureCategory::DerivedFromObservations && acq[i].network_state_feature.empty())
      issues.push_back(acq[i].name + ": derived feature without a NetworkState source");
  }
  const auto& eng = EngineeredColumnContract();
  if (manifest) {
    const auto& cols = manifest->model_feature_columns;
    if (cols.size() != acq.size() + eng.size()) issues.push_back("manifest has " + std::to_string(cols.size()) + " model columns");
    for (std::size_t i = 0; i < cols.size(); ++i) {
      const std::string expected = i < acq.size() ? acq[i].name : (i - acq.size() < eng.size() ? eng[i - acq.size()].name : "");
      if (cols[i] != expected) issues.push_back("model column " + std::to_string(i) + ": " + cols[i] + " != " + expected);
    }
  }
  for (std::size_t i = 0; i < eng.size(); ++i) {
    if (eng[i].index != static_cast<int>(acq.size() + i)) issues.push_back(eng[i].name + ": column index " + std::to_string(eng[i].index));
    for (const std::string& in : eng[i].inputs) {
      if (!FindRawFeature(in)) issues.push_back(eng[i].name + ": unknown input " + in);
    }
  }
  return issues;
}

CategoryCounts CountCategories() {
  CategoryCounts c;
  for (const auto& s : FeatureAcquisitionContract()) {
    switch (s.category) {
      case FeatureCategory::DirectlyObservable: ++c.direct; break;
      case FeatureCategory::DerivedFromObservations: ++c.derived; break;
      case FeatureCategory::Estimated: ++c.estimated; break;
      case FeatureCategory::Unavailable: ++c.unavailable; break;
      case FeatureCategory::Incompatible: ++c.incompatible; break;
    }
  }
  return c;
}

}  // namespace aafr::model
