# Algorithms

Algorithm-specific implementations and notes live here. Each subfolder should
state the baseline, inputs, outputs, tunable parameters, and experiment status.

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
