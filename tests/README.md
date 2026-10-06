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
