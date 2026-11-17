# Runtime

This folder contains the adaptive runtime that makes the browser intelligent.

## Main Loop

```text
Sensing
  -> EnvironmentSnapshot
  -> AAFR Decision Engine
  -> Stability Gate
  -> Policy Executor
  -> Decision Logger
  -> Telemetry Feedback
```

## Current Modules

| Path | Purpose |
| --- | --- |
| `common/` | shared data structures such as `EnvironmentSnapshot` and `DecisionResult` |
| `interfaces/` | runtime interfaces such as `ISensor`, `IDecisionModel`, and `ITelemetrySink` |
| `decision/` | weighted scoring seed model and policy registry types |
| `transition/` | stability gates for strategy switching |
| `telemetry/` | JSON-lines decision logging |
| `execution/` | policy executor action-space and support levels |
| `aafr_runtime.py` | Python orchestration placeholder for future experiment-facing runtime glue |
| `switching.py` | Python placeholder for switch orchestration experiments |
| `stability.py` | Python placeholder for stability-gate experiments |
| `decision_record.py` | Python placeholder for serialized decision records |
| `adaptive/` | **V0.6 adaptive runtime** (`aafr-adaptive-runtime`, plain C++17): `AAFRRuntime` orchestrator, `NetworkChangeDetector` and `EvaluationScheduler`, `IRecommendationProvider` with `DeterministicRecommendationProvider`, `ControlledProbeEngine`, `StatisticalArbitrationEngine`, `ForwardingPerformanceEvaluator`, `SwitchingStabilityController`, `DecisionRecord` (`aafr.decision_record.v1`), `RuntimeOutcomeObserver`, reason codes and the lifecycle |
| `observability/` | V0.3 viewer contracts; `AAFRRuntime` implements `IRuntimeObservabilityProvider` |

The V0.6 runtime is documented in
[`browser/qt_webengine/README.md`](../browser/qt_webengine/README.md#v06-adaptive-runtime);
the older `ngr::` seed model and transition manager above predate it and are
not used by the browser.

## Future Modules

| Path | Purpose |
| --- | --- |
| `sensing/` | network, workload, cache, and system monitors |
| `registry/` | YAML loading and validation for policy files |
| `concurrency/` | safe strategy publication and resource management |

## Development Rule

Keep the runtime independent from Qt when possible. Qt-specific code should live in `browser/qt_webengine/` or a clearly separated adapter.

## Testing Rule

Every runtime behavior should have a focused test under `tests/unit/` or `tests/integration/`.
