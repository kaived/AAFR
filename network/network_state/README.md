# Network State

Converts V0.3 telemetry into validated `NetworkState` snapshots and
`NetworkConstraints` (V0.4). Plain C++17, no Qt.

| File | Purpose |
| --- | --- |
| `network_state.h` | Versioned snapshot contract: features with unit, kind, quality, scope and attribution |
| `execution_environment.h` | Topology, forwarding operations and budgets; unknown is an empty optional |
| `feature_validator.*` | Metric contracts and rejection of invalid measurements |
| `feature_normalizer.*` | Unit conversion to canonical units |
| `feature_extractor.*` | Bounded rolling series and the per-window feature definitions |
| `measurement_quality.*` | valid / missing / stale / insufficient_samples / invalid / unattributed |
| `network_state_builder.*` | Incremental ingest and snapshot building; capture and session policy |
| `network_constraints.*` | Constraint contract, derivation and conflict detection |
| `network_state_service.*` | Telemetry → state → constraints → eligibility, on a worker thread |
| `forwarding_observations.*` | V0.7.4: provenance-tagged gateway/relay/runtime/graph observations, validation, windowed `fwd.*` features with per-feature freshness and sample rules (`NetworkState::forwarding`, schema 2) |

V0.7.4 forwarding measurements, freshness rules and model use:
[expert model README, V0.7.4](../../expert_model/README.md#v074-real-network-feature-acquisition-and-model-compatibility).

Definitions and rules:
[browser README, V0.4](../../browser/qt_webengine/README.md#v04-networkstate-constraints-and-algorithm-eligibility).
