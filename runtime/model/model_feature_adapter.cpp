#include "runtime/model/model_feature_adapter.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <deque>
#include <set>
#include <sstream>

#include "network/network_state/forwarding_observations.h"
#include "runtime/model/feature_acquisition.h"

namespace aafr::model {

namespace fwd = aafr::forwarding;

const char* ToString(FeatureProvenance value) {
  switch (value) {
    case FeatureProvenance::GraphDerived: return "graph";
    case FeatureProvenance::Measured: return "measured";
    case FeatureProvenance::Configured: return "configured";
    case FeatureProvenance::Substitute: return "substitute";
    case FeatureProvenance::Missing: return "missing";
    case FeatureProvenance::Derived: return "derived";
    case FeatureProvenance::Estimated: return "estimated";
  }
  return "unknown";
}

const char* ToString(FeatureStatus value) {
  switch (value) {
    case FeatureStatus::Valid: return "valid";
    case FeatureStatus::Missing: return "missing";
    case FeatureStatus::Stale: return "stale";
    case FeatureStatus::InsufficientSamples: return "insufficient_samples";
    case FeatureStatus::InvalidUnit: return "invalid_unit";
    case FeatureStatus::OutOfRange: return "out_of_range";
    case FeatureStatus::Invalid: return "invalid";
    case FeatureStatus::Unsupported: return "unsupported";
    case FeatureStatus::Incompatible: return "incompatible";
  }
  return "unknown";
}

int ModelFeatureVector::CountStatus(FeatureStatus status) const {
  int n = 0;
  for (const auto& f : features) n += f.status == status ? 1 : 0;
  return n;
}

std::string ModelFeatureVector::Summary() const {
  std::map<FeatureProvenance, int> counts;
  for (const auto& f : features) {
    if (f.value) ++counts[f.provenance];
  }
  std::ostringstream o;
  o << "graph=" << counts[FeatureProvenance::GraphDerived] << " measured=" << counts[FeatureProvenance::Measured]
    << " derived=" << counts[FeatureProvenance::Derived]
    << " configured=" << counts[FeatureProvenance::Configured] << " substitute=" << counts[FeatureProvenance::Substitute]
    << " missing=" << missing.size() << " invalid=" << invalid.size() << " stale=" << stale.size()
    << " insufficient=" << insufficient.size();
  return o.str();
}

std::map<std::string, double> ModelFeatureVector::Values() const {
  std::map<std::string, double> out;
  for (const auto& f : features) {
    if (f.value) out[f.name] = *f.value;
  }
  return out;
}

std::vector<TelemetryFeatureInput> ConfiguredTelemetrySource::Features() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return inputs_;
}

void ConfiguredTelemetrySource::SetProfile(const std::map<std::string, double>& values,
                                           const std::string& profile_name) {
  std::vector<TelemetryFeatureInput> inputs;
  for (const auto& e : values) {
    TelemetryFeatureInput in;
    in.name = e.first;
    in.value = e.second;
    const RawFeatureSpec* spec = FindRawFeature(e.first);
    in.unit = spec ? spec->unit : "";
    in.provenance = FeatureProvenance::Configured;
    in.source = name_ + ":" + profile_name;
    inputs.push_back(in);
  }
  SetInputs(std::move(inputs));
}

void ConfiguredTelemetrySource::SetInputs(std::vector<TelemetryFeatureInput> inputs) {
  std::lock_guard<std::mutex> lock(mutex_);
  inputs_ = std::move(inputs);
}

void ConfiguredTelemetrySource::Clear() {
  std::lock_guard<std::mutex> lock(mutex_);
  inputs_.clear();
}

// ---------------------------------------------------------------- graph

std::vector<double> SymmetricEigenvalues(std::vector<std::vector<double>> a) {
  const std::size_t n = a.size();
  for (int sweep = 0; sweep < 100; ++sweep) {
    double off = 0.0;
    for (std::size_t i = 0; i < n; ++i)
      for (std::size_t j = i + 1; j < n; ++j) off += a[i][j] * a[i][j];
    if (off < 1e-22) break;
    for (std::size_t p = 0; p < n; ++p) {
      for (std::size_t q = p + 1; q < n; ++q) {
        if (std::fabs(a[p][q]) < 1e-300) continue;
        const double theta = (a[q][q] - a[p][p]) / (2.0 * a[p][q]);
        const double t = (theta >= 0 ? 1.0 : -1.0) / (std::fabs(theta) + std::sqrt(theta * theta + 1.0));
        const double c = 1.0 / std::sqrt(t * t + 1.0);
        const double s = t * c;
        for (std::size_t k = 0; k < n; ++k) {
          const double akp = a[k][p], akq = a[k][q];
          a[k][p] = c * akp - s * akq;
          a[k][q] = s * akp + c * akq;
        }
        for (std::size_t k = 0; k < n; ++k) {
          const double apk = a[p][k], aqk = a[q][k];
          a[p][k] = c * apk - s * aqk;
          a[q][k] = s * apk + c * aqk;
        }
      }
    }
  }
  std::vector<double> values(n);
  for (std::size_t i = 0; i < n; ++i) values[i] = a[i][i];
  std::sort(values.begin(), values.end());
  return values;
}

namespace {
double RoundTo(double v, int digits) {
  const double scale = std::pow(10.0, digits);
  return std::round(v * scale) / scale;
}

std::vector<int> Bfs(const std::vector<std::vector<int>>& adj, int start) {
  std::vector<int> dist(adj.size(), -1);
  std::deque<int> queue{start};
  dist[static_cast<std::size_t>(start)] = 0;
  while (!queue.empty()) {
    const int u = queue.front();
    queue.pop_front();
    for (int v : adj[static_cast<std::size_t>(u)]) {
      if (dist[static_cast<std::size_t>(v)] < 0) {
        dist[static_cast<std::size_t>(v)] = dist[static_cast<std::size_t>(u)] + 1;
        queue.push_back(v);
      }
    }
  }
  return dist;
}
}  // namespace

GraphFeatureResult ComputeGraphFeatures(const fwd::ForwardingGraph& graph, const fwd::ForwardingProblem& problem) {
  GraphFeatureResult r;
  // The active topology: available nodes and available edges between them
  // (the simulator's "active subgraph"), directed.
  std::vector<std::string> ids;
  std::map<std::string, int> index;
  for (const auto& node : graph.nodes()) {
    if (!node.available) continue;
    index[node.id] = static_cast<int>(ids.size());
    ids.push_back(node.id);
  }
  const int n = static_cast<int>(ids.size());
  std::vector<std::vector<int>> out(static_cast<std::size_t>(n));
  std::vector<int> in_degree(static_cast<std::size_t>(n), 0);
  std::set<std::pair<int, int>> directed, undirected;
  for (const auto& e : graph.edges()) {
    if (!e.available) continue;
    const auto a = index.find(e.from), b = index.find(e.to);
    if (a == index.end() || b == index.end() || a->second == b->second) continue;
    if (!directed.insert({a->second, b->second}).second) continue;
    out[static_cast<std::size_t>(a->second)].push_back(b->second);
    ++in_degree[static_cast<std::size_t>(b->second)];
    undirected.insert({std::min(a->second, b->second), std::max(a->second, b->second)});
  }
  std::vector<std::vector<int>> und(static_cast<std::size_t>(n));
  std::vector<std::set<int>> neighbours(static_cast<std::size_t>(n));
  for (const auto& e : undirected) {
    und[static_cast<std::size_t>(e.first)].push_back(e.second);
    und[static_cast<std::size_t>(e.second)].push_back(e.first);
    neighbours[static_cast<std::size_t>(e.first)].insert(e.second);
    neighbours[static_cast<std::size_t>(e.second)].insert(e.first);
  }
  const double ue = static_cast<double>(undirected.size());
  r.values["num_nodes"] = n;
  r.values["num_edges"] = static_cast<double>(directed.size());
  r.values["avg_node_degree"] = n > 0 ? RoundTo(2.0 * ue / n, 3) : 0.0;
  r.values["network_density"] = n > 1 ? RoundTo(2.0 * ue / (static_cast<double>(n) * (n - 1)), 6) : 0.0;

  // networkx.average_clustering on the undirected projection (zeros included).
  double clustering = 0.0;
  for (int v = 0; v < n; ++v) {
    const auto& nb = neighbours[static_cast<std::size_t>(v)];
    const double k = static_cast<double>(nb.size());
    if (k < 2) continue;
    int triangles = 0;
    for (auto i = nb.begin(); i != nb.end(); ++i) {
      for (auto j = std::next(i); j != nb.end(); ++j) {
        if (neighbours[static_cast<std::size_t>(*i)].count(*j)) ++triangles;
      }
    }
    clustering += 2.0 * triangles / (k * (k - 1));
  }
  r.values["avg_clustering_coefficient"] = n > 0 ? RoundTo(clustering / n, 4) : 0.0;

  // Connected components of the undirected projection.
  std::vector<int> component(static_cast<std::size_t>(n), -1);
  std::vector<std::vector<int>> components;
  for (int v = 0; v < n; ++v) {
    if (component[static_cast<std::size_t>(v)] >= 0) continue;
    const auto dist = Bfs(und, v);
    components.emplace_back();
    for (int u = 0; u < n; ++u) {
      if (dist[static_cast<std::size_t>(u)] >= 0) {
        component[static_cast<std::size_t>(u)] = static_cast<int>(components.size() - 1);
        components.back().push_back(u);
      }
    }
  }
  const bool connected = components.size() == 1;
  int diameter = 0;
  if (n > 0 && n <= 100) {
    // Exact diameter of the graph, or of its largest component.
    std::size_t largest = 0;
    for (std::size_t c = 1; c < components.size(); ++c) {
      if (components[c].size() > components[largest].size()) largest = c;
    }
    for (int v : components[largest]) {
      for (int d : Bfs(und, v)) diameter = std::max(diameter, d);
    }
  } else if (n > 100) {
    // The simulator's 2-sweep BFS approximation from the first node.
    const auto d1 = Bfs(und, 0);
    int far = 0;
    for (int u = 0; u < n; ++u) {
      if (d1[static_cast<std::size_t>(u)] > d1[static_cast<std::size_t>(far)]) far = u;
    }
    for (int d : Bfs(und, far)) diameter = std::max(diameter, d);
  }
  r.values["diameter"] = diameter;

  // Algebraic connectivity: Fiedler eigenvalue for 2 <= n <= 150 when
  // connected, else 0 (the simulator's rule).
  double alg_conn = 0.0;
  if (n >= 2 && n <= 150 && connected) {
    std::vector<std::vector<double>> lap(static_cast<std::size_t>(n), std::vector<double>(static_cast<std::size_t>(n), 0.0));
    for (const auto& e : undirected) {
      lap[static_cast<std::size_t>(e.first)][static_cast<std::size_t>(e.second)] = -1.0;
      lap[static_cast<std::size_t>(e.second)][static_cast<std::size_t>(e.first)] = -1.0;
      lap[static_cast<std::size_t>(e.first)][static_cast<std::size_t>(e.first)] += 1.0;
      lap[static_cast<std::size_t>(e.second)][static_cast<std::size_t>(e.second)] += 1.0;
    }
    const auto eig = SymmetricEigenvalues(lap);
    alg_conn = eig.size() > 1 ? std::max(0.0, eig[1]) : 0.0;
  }
  r.values["algebraic_connectivity"] = RoundTo(alg_conn, 4);

  // Request features on the directed graph.
  const auto s = index.find(problem.source), d = index.find(problem.destination);
  if (s == index.end() || d == index.end()) {
    r.issues.push_back("source or destination is not an available node");
    return r;
  }
  r.values["source_in_degree"] = in_degree[static_cast<std::size_t>(s->second)];
  r.values["source_out_degree"] = static_cast<double>(out[static_cast<std::size_t>(s->second)].size());
  r.values["dest_in_degree"] = in_degree[static_cast<std::size_t>(d->second)];
  r.values["dest_out_degree"] = static_cast<double>(out[static_cast<std::size_t>(d->second)].size());
  const auto hops = Bfs(out, s->second)[static_cast<std::size_t>(d->second)];
  if (hops < 0) {
    r.issues.push_back("destination unreachable from source on the active graph");
  } else {
    r.values["req_shortest_hops"] = hops;
  }
  return r;
}

// ---------------------------------------------------------------- adapter

namespace {
std::string CheckValue(const RawFeatureSpec& spec, double v, const std::string& unit) {
  if (!std::isfinite(v)) return "non-finite value";
  if (unit != spec.unit) return "unit '" + unit + "' != contract unit '" + spec.unit + "'";
  if (v < spec.min_valid) return "below the valid minimum";
  if (spec.max_valid && v > *spec.max_valid) return "above the valid maximum";
  if (spec.integer && std::fabs(v - std::round(v)) > 1e-9) return "fractional value for an integer feature";
  return "";
}

struct SubstituteRule {
  const char* model_feature;
  const char* state_feature;
};
// Host-wide NetworkState values that can stand in, with a recorded semantic
// mismatch, for model features. Units: both sides ms or Mbps.
constexpr SubstituteRule kSubstitutes[] = {
    {"latency", "rtt.handshake.mean_ms"},     {"latency_p50", "rtt.handshake.median_ms"},
    {"latency_min", "rtt.handshake.min_ms"},  {"latency_max", "rtt.handshake.max_ms"},
    {"jitter", "ack_delay.jitter_ms"},        {"throughput", "throughput.mean_mbps"},
};

const char* UnitText(netstate::Unit u) {
  switch (u) {
    case netstate::Unit::Milliseconds: return "ms";
    case netstate::Unit::Mbps: return "mbps";
    case netstate::Unit::Ratio: return "ratio";
    case netstate::Unit::Count: return "count";
    case netstate::Unit::Bytes: return "bytes";
    case netstate::Unit::Percent: return "percent";
  }
  return "unknown";
}
}  // namespace

ModelFeatureVector ModelFeatureAdapter::Build(const fwd::ForwardingGraph& graph, const fwd::ForwardingProblem& problem,
                                              const netstate::NetworkState* state,
                                              const ITelemetryFeatureSource* telemetry) const {
  const auto started = std::chrono::steady_clock::now();
  ModelFeatureVector v;
  const auto& contract = RawFeatureContract();
  std::map<std::string, ModelFeatureValue> accepted;
  std::map<std::string, std::string> rejected;  // name -> issue
  std::set<std::string> stale_names;

  // 1. Graph and request features.
  const GraphFeatureResult g = ComputeGraphFeatures(graph, problem);
  const std::string graph_source = "forwarding_graph:" + graph.id() + "@v" + std::to_string(graph.version());
  for (const auto& e : g.values) {
    const RawFeatureSpec* spec = FindRawFeature(e.first);
    ModelFeatureValue f{e.first, spec->unit, e.second, FeatureProvenance::GraphDerived, graph_source, 0.0, ""};
    const std::string issue = CheckValue(*spec, e.second, spec->unit);
    if (issue.empty()) accepted[e.first] = f;
    else rejected[e.first] = issue;
  }
  for (const std::string& issue : g.issues) {
    if (issue.find("unreachable") != std::string::npos) rejected["req_shortest_hops"] = issue;
  }

  // 2. Telemetry source (measured or configured values).
  if (telemetry) {
    std::set<std::string> offered;
    for (const TelemetryFeatureInput& in : telemetry->Features()) {
      if (!offered.insert(in.name).second) {
        // Two values for one feature: ambiguous, so neither is used.
        accepted.erase(in.name);
        rejected[in.name] = "offered more than once by " + telemetry->Name();
        continue;
      }
      const RawFeatureSpec* spec = FindRawFeature(in.name);
      if (!spec) {
        rejected["unsupported:" + in.name] = "not a feature of " + std::string(kFeatureContractVersion);
        continue;
      }
      if (spec->group == FeatureGroup::Topology || spec->group == FeatureGroup::Request) {
        rejected["conflict:" + in.name] = "graph-derived feature offered by " + telemetry->Name();
        continue;
      }
      if (in.provenance != FeatureProvenance::Measured && in.provenance != FeatureProvenance::Configured) {
        rejected[in.name] = "telemetry provenance must be measured or configured";
        continue;
      }
      const std::string issue = CheckValue(*spec, in.value, in.unit);
      if (!issue.empty()) {
        rejected[in.name] = issue;
        continue;
      }
      if (in.provenance == FeatureProvenance::Measured &&
          (!in.age_ms || *in.age_ms > config_.max_feature_age_ms || *in.age_ms < 0.0)) {
        stale_names.insert(in.name);
        continue;
      }
      accepted[in.name] = {in.name, spec->unit, in.value, in.provenance, in.source, in.age_ms, ""};
    }
  }

  // 2b. V0.7.4: category-B features from validated NetworkState forwarding
  // observations, and category-C research estimates (reported, never used).
  const netstate::ForwardingFeatureSet* fwdset = state && state->forwarding ? state->forwarding.get() : nullptr;
  if (state) v.network_state_id = state->state_id;
  if (fwdset) v.forwarding_version = fwdset->version;
  std::set<std::string> insufficient_names;
  std::map<std::string, std::string> forwarding_issue;
  if (fwdset) {
    const std::string origin = "#" + std::to_string(state->state_id) + "/fwd v" + std::to_string(fwdset->version);
    for (const FeatureAcquisitionSpec& acq : FeatureAcquisitionContract()) {
      const netstate::ForwardingFeature* ff =
          acq.network_state_feature.empty() ? nullptr : fwdset->Find(acq.network_state_feature);
      if (!ff) continue;
      const netstate::Feature& f = ff->feature;
      const RawFeatureSpec* spec = FindRawFeature(acq.name);
      if (acq.category == FeatureCategory::Estimated) {
        if (!f.value) continue;
        ModelFeatureValue e;
        e.name = acq.name;
        e.unit = UnitText(f.unit);
        e.value = f.value;
        e.provenance = FeatureProvenance::Estimated;
        e.source = "network_state:" + acq.network_state_feature + origin;
        e.age_ms = f.age_ms;
        e.issue = std::string("estimate only (") + netstate::ToString(f.quality) + ", " +
                  std::to_string(f.sample_count) + " samples): " + acq.domain_compatibility;
        e.status = FeatureStatus::Incompatible;
        e.category = 'C';
        v.estimates.push_back(e);
        continue;
      }
      if (acq.category != FeatureCategory::DerivedFromObservations || !config_.use_forwarding_observations) continue;
      if (accepted.count(acq.name) || rejected.count(acq.name)) continue;
      switch (f.quality) {
        case netstate::FeatureQuality::Valid: {
          if (!f.value) break;
          const std::string issue = CheckValue(*spec, *f.value, UnitText(f.unit));
          if (!issue.empty()) {
            rejected[acq.name] = issue;
            break;
          }
          // A windowed count's freshness is its source's liveness over the
          // whole window (checked by the store), not the age of its newest
          // event: "no event for 30 s" is the observation 0.
          accepted[acq.name] = {acq.name, spec->unit, f.value, FeatureProvenance::Derived,
                                "network_state:" + acq.network_state_feature + origin, f.age_ms, ""};
          v.derived.push_back(acq.name);
          break;
        }
        case netstate::FeatureQuality::Stale:
          stale_names.insert(acq.name);
          forwarding_issue[acq.name] = "stale: " + f.quality_reason;
          break;
        case netstate::FeatureQuality::InsufficientSamples:
          insufficient_names.insert(acq.name);
          forwarding_issue[acq.name] = "insufficient: " + f.quality_reason;
          break;
        default:
          forwarding_issue[acq.name] = std::string(netstate::ToString(f.quality)) + ": " + f.quality_reason;
          break;
      }
    }
  }

  // 3. Explicitly allowed NetworkState substitutes for what is still missing.
  if (config_.allow_network_state_substitutes && state) {
    for (const SubstituteRule& rule : kSubstitutes) {
      if (accepted.count(rule.model_feature) || rejected.count(rule.model_feature)) continue;
      const netstate::Feature* f = state->FindFeature(rule.state_feature);
      if (!f) continue;
      const RawFeatureSpec* spec = FindRawFeature(rule.model_feature);
      if (!f->Usable()) continue;
      if (!f->age_ms || *f->age_ms > config_.max_feature_age_ms) {
        stale_names.insert(rule.model_feature);
        continue;
      }
      const std::string issue = CheckValue(*spec, *f->value, UnitText(f->unit));
      if (!issue.empty()) {
        rejected[rule.model_feature] = issue;
        continue;
      }
      accepted[rule.model_feature] = {rule.model_feature, spec->unit, f->value, FeatureProvenance::Substitute,
                                      std::string("network_state:") + rule.state_feature + "#" +
                                          std::to_string(state->state_id),
                                      f->age_ms, ""};
      v.substituted.push_back(rule.model_feature);
    }
  }

  // 4. Assemble in contract order.
  for (const RawFeatureSpec& spec : contract) {
    const FeatureAcquisitionSpec* acq = FindAcquisitionSpec(spec.name);
    const char category = acq ? Letter(acq->category) : '?';
    const auto a = accepted.find(spec.name);
    if (a != accepted.end()) {
      ModelFeatureValue f = a->second;
      f.status = FeatureStatus::Valid;
      f.category = category;
      v.features.push_back(f);
      continue;
    }
    ModelFeatureValue f;
    f.name = spec.name;
    f.unit = spec.unit;
    f.category = category;
    if (rejected.count(spec.name)) {
      f.issue = rejected[spec.name];
      if (f.issue.rfind("unit", 0) == 0) f.status = FeatureStatus::InvalidUnit;
      else if (f.issue.rfind("below", 0) == 0 || f.issue.rfind("above", 0) == 0) f.status = FeatureStatus::OutOfRange;
      else f.status = FeatureStatus::Invalid;
      v.invalid.push_back(spec.name);
    } else if (stale_names.count(spec.name)) {
      f.issue = forwarding_issue.count(spec.name) ? forwarding_issue[spec.name] : "stale";
      f.status = FeatureStatus::Stale;
      v.stale.push_back(spec.name);
    } else if (insufficient_names.count(spec.name)) {
      f.issue = forwarding_issue[spec.name];
      f.status = FeatureStatus::InsufficientSamples;
      v.insufficient.push_back(spec.name);
    } else {
      const bool unsupported = acq && acq->category == FeatureCategory::Unavailable;
      const bool incompatible =
          acq && (acq->category == FeatureCategory::Incompatible || acq->category == FeatureCategory::Estimated);
      f.issue = forwarding_issue.count(spec.name) ? forwarding_issue[spec.name] : "no source provides this feature";
      if (unsupported) f.detail = "unsupported: no AAFR measurement source (" + acq->measurement_method + ")";
      else if (incompatible) f.detail = "incompatible: " + acq->domain_compatibility;
      f.status = unsupported ? FeatureStatus::Unsupported
                             : (incompatible ? FeatureStatus::Incompatible : FeatureStatus::Missing);
      v.missing.push_back(spec.name);
    }
    v.features.push_back(f);
  }
  for (const auto& r : rejected) {
    if (!FindRawFeature(r.first)) v.invalid.push_back(r.first + " (" + r.second + ")");
  }
  v.build_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
  return v;
}

}  // namespace aafr::model
