# Algorithms

Algorithm-specific implementations and notes live here. Each subfolder should
state the baseline, inputs, outputs, tunable parameters, and experiment status.

## V0.5: implementations, verification and manual execution

All ten algorithms are implemented behind `IPathAlgorithm`
(`common/path_algorithm.h`), one file per folder (`dijkstra/dijkstra.cpp`,
`aco/aco.cpp`, ...), in the `aafr-forwarding` library. They solve the shared
forwarding problem (`routing/topology/forwarding_problem.h`) with one
objective. `execution/implementation_verifier.*` verifies them and updates
the registry; `execution/algorithm_execution_engine.*` runs one named
algorithm on request, never on its own. Representations, discrete
adaptations, constraint handling, registry levels and benchmark results:
[browser README, V0.5](../browser/qt_webengine/README.md#v05-controlled-forwarding-engine-and-algorithms).

## V0.4: capability registry and eligibility (no implementations)

No algorithm in this folder is implemented. V0.4 adds only metadata and
checks, in the `aafr-network-state` library:

| File | Purpose |
| --- | --- |
| `algorithm_id.h` | Ids of the ten planned algorithms |
| `algorithm_capability.h` | Typed capability contract: inherent properties kept apart from implementation facts |
| `capability_registry.*` | The ten profiles, profile validation, availability tracking |
| `eligibility_reason.*` | The reason-code catalog |
| `eligibility_evaluator.*` | Ten deterministic eligibility checks per algorithm |

Every built-in profile is `implementation_unavailable`. The registry refuses
to mark an algorithm `verified_executable` without measured runtime, measured
memory, an execution environment and a verification note. Profiles, rules and
reason codes:
[browser README, V0.4](../browser/qt_webengine/README.md#v04-networkstate-constraints-and-algorithm-eligibility).
