#include "routing/gateway/runtime_gateway_binding.h"

namespace aafr::gateway {

PinnedConfiguration ToPinned(const runtime::ActiveConfiguration& active) {
  PinnedConfiguration p;
  p.generation = active.config_version;
  p.algorithm = algorithms::ToString(active.algorithm);
  p.path = active.path;
  p.graph_id = active.graph_id;
  p.graph_version = active.graph_version;
  p.decision_id = active.decision_id;
  p.activation_request_id = active.activation_request_id;
  return p;
}

GatewayBindings MakeRuntimeBindings(const RuntimeBindingSources& sources) {
  GatewayBindings b;
  runtime::AAFRRuntime* runtime = sources.runtime;
  b.executor = sources.executor;
  b.configuration = [runtime]() -> std::optional<PinnedConfiguration> {
    if (!runtime) return std::nullopt;
    const auto active = runtime->ConfirmedConfiguration();
    if (!active) return std::nullopt;
    return ToPinned(*active);
  };
  const auto graph_source = sources.graph_source;
  const auto problem_for = sources.problem_for;
  b.usable = [graph_source, problem_for](const PinnedConfiguration& pinned, std::uint64_t* graph_version,
                                         std::string* reason) {
    const auto graph = graph_source ? graph_source() : nullptr;
    if (!graph) {
      if (reason) *reason = "no forwarding graph";
      return false;
    }
    if (!pinned.graph_id.empty() && pinned.graph_id != graph->id()) {
      if (reason) *reason = "configuration belongs to graph '" + pinned.graph_id + "'";
      return false;
    }
    const forwarding::ForwardingProblem problem =
        problem_for ? problem_for(*graph) : forwarding::ForwardingProblem{};
    const forwarding::PathValidation validation = forwarding::ValidatePath(*graph, problem, pinned.path);
    if (!validation.valid) {
      if (reason) *reason = "path " + forwarding::PathText(pinned.path) + " is not valid on graph version " +
                            std::to_string(graph->version()) +
                            (validation.details.empty() ? std::string() : ": " + validation.details.front());
      return false;
    }
    if (graph_version) *graph_version = graph->version();
    return true;
  };
  if (sources.network) {
    const forwarding::LoopbackForwardingNetwork* network = sources.network;
    b.relay_evidence = [network](const std::string& id) { return network->StreamTraversals(id); };
  }
  b.on_finished = [runtime](const GatewayConnectionRecord& record) {
    if (!runtime || record.generation == 0) return;
    runtime::AAFRRuntime::BrowserStreamOutcome o;
    o.connection_id = record.connection_id;
    o.config_version = record.generation;
    o.decision_id = record.decision_id;
    o.established = record.path_confirmed;
    o.relay_failure = record.relay_failure;
    o.timeout = record.forwarding_status == "TIMEOUT";
    o.establish_ms = record.establish_ms.value_or(0.0);
    o.bytes = record.bytes_from_client + record.bytes_to_client;
    runtime->ObserveBrowserStream(o);
  };
  return b;
}

}  // namespace aafr::gateway
