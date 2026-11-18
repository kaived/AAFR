#pragma once

// Binds the V0.7.3 browser forwarding gateway to the V0.6 adaptive runtime.
// Plain C++17.
//
// The gateway reads the runtime's confirmed active configuration (never the
// candidate, never a model recommendation) and reports finished browser
// connections back as outcome observations. Nothing here can change the
// runtime's state: the runtime stays the only owner of the active
// configuration.

#include <functional>
#include <memory>

#include "routing/gateway/browser_forwarding_gateway.h"
#include "routing/topology/forwarding_graph.h"
#include "routing/topology/forwarding_problem.h"
#include "runtime/adaptive/aafr_runtime.h"

namespace aafr::gateway {

struct RuntimeBindingSources {
  runtime::AAFRRuntime* runtime{nullptr};  // Required.
  // Required: the current graph, to check a configuration is still usable.
  std::function<std::shared_ptr<const forwarding::ForwardingGraph>()> graph_source;
  std::function<forwarding::ForwardingProblem(const forwarding::ForwardingGraph&)> problem_for;
  forwarding::IStreamForwardingExecutor* executor{nullptr};  // Required.
  // Optional: relay-level evidence.
  const forwarding::LoopbackForwardingNetwork* network{nullptr};
};

PinnedConfiguration ToPinned(const runtime::ActiveConfiguration& active);
GatewayBindings MakeRuntimeBindings(const RuntimeBindingSources& sources);

}  // namespace aafr::gateway
