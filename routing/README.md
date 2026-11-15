# Routing

Topology, forwarding, and controller code for route selection and application.

V0.5 contents:

| Folder | Files | Purpose |
| --- | --- | --- |
| `topology/` | `forwarding_graph.*`, `forwarding_problem.*`, `scenario_catalog.*` | controlled graph with provenance, problem, objective, the single path validator, benchmark scenarios and the loopback demo topology |
| `forwarding/` | `forwarding_executor.*`, `loopback_forwarding.*` | executor contract, simulated executor, real loopback TCP relays |
| `controller/` | `forwarding_service.*` | what the browser owns: relays, verification, execution engine |

Details: [browser README, V0.5](../browser/qt_webengine/README.md#v05-controlled-forwarding-engine-and-algorithms).
