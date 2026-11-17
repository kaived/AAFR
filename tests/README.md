# Tests

This folder contains automated checks.

## Test Levels

| Folder | Purpose |
| --- | --- |
| `unit/` | small tests for individual runtime components |
| `integration/` | tests for full sense-decide-gate-act-log behavior |
| `system/` | browser-level adaptive-vs-static checks |

## Current Test

`tests/unit/runtime_core_smoke_test.cpp` verifies that the seed decision model and transition manager can run together from the `runtime/` architecture.

`tests/unit/browser_interface_test.cpp` exercises the Qt browser using local
HTML pages. Enable it with `-DAAFR_BUILD_BROWSER_TESTS=ON`; see
[`browser/qt_webengine/README.md`](../browser/qt_webengine/README.md) for commands.

`tests/unit/network_state_test.cpp`, `tests/unit/algorithm_eligibility_test.cpp`
and `tests/integration/state_pipeline_test.cpp` cover V0.4 (NetworkState,
feature engineering, constraints, the capability registry, eligibility and
the update pipeline). They are plain C++ with fixed-timestamp fixtures from
`tests/support/test_support.h` and need neither Qt nor packet capture.

V0.5 adds `unit/forwarding_graph_test.cpp`, `unit/path_algorithms_test.cpp`,
`unit/loopback_forwarding_test.cpp`, `unit/benchmark_test.cpp` and
`integration/forwarding_pipeline_test.cpp` (plain C++, no Qt), plus two
Developer Tools cases in `unit/browser_observability_test.cpp`. The loopback
tests open sockets on 127.0.0.1 only; none needs Internet access. Coverage
table: [browser README, V0.5 tests](../browser/qt_webengine/README.md#v05-tests).

V0.6 adds `unit/runtime_change_detection_test.cpp`,
`unit/runtime_statistics_test.cpp`, `unit/runtime_stability_test.cpp`,
`unit/runtime_recommendation_test.cpp`, `integration/adaptive_runtime_test.cpp`
(deterministic: a `ManualClock` and a seeded test executor) and
`integration/runtime_loopback_test.cpp` (real relays on 127.0.0.1), with
fixtures in `support/runtime_test_support.h`, plus a browser case
(`adaptiveRuntimeSwitchesInBrowser`). Coverage table:
[browser README, V0.6 tests](../browser/qt_webengine/README.md#v06-tests).

## What To Add

Add tests for:

- feature normalization
- decision ranking
- transition gates
- telemetry JSON-lines format
- policy registry loading
- executor support-level validation

## Rule

Every algorithm change should include a test that fails if the ranking or gate behavior regresses.
