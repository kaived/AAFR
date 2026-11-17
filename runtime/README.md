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

## V0.7.1 external runtime integration

V0.7.1 compares AAFR V0.6 with an external reference repository and adopts
only what improves AAFR. AAFR stays the authoritative system; nothing from
the reference is linked, vendored or copied. This section is the integration
audit and the integration report (kept here rather than in separate
`docs/V0.7_*` files). The dataset assessment is in
[`datasets/README.md`](../datasets/README.md#v071-external-dataset-assessment)
and V0.7.2 readiness is in
[`expert_model/README.md`](../expert_model/README.md#v072-readiness-after-v071).

### Reference inspected

| Item | Value |
|---|---|
| Repository | https://github.com/oddproblem/Adaptive-runtime (cloned to `D:\AAFR-reference\Adaptive-runtime`, outside this tree; not a Git remote of AAFR) |
| Branch / commit | `main` at `6b6c008fc484bcbadfb8d8447acf36fe5f39e10d` (2026-11-15, "Audit fixes: demand-aware Goodness and recovery, equal-latency repair, hardened dashboard and FIFO, flow rollback"); previous commit `a0ce0ed` |
| Retrieved | 2026-11-18 |
| License | **None.** There is no LICENSE file and no license statement, so the default is all rights reserved. Only `engine/third_party/nlohmann/json.hpp` (MIT) carries a license. Consequence: **no reference code was copied**; every adopted idea below is an independent implementation in AAFR's own code |
| Source read | `engine/src/{anova,decision,goodness,engine,probing,ml_engine,netmap,ingestion,live_capture,telemetry,sdn,topology,algorithms}.cpp` and their headers, `engine/tests/unit_tests.cpp`, `engine/tests/check_stats.py`, `engine/testbed/{testbed,run_experiment,make_report}.py`, `engine/dashboard/server.py`, `engine/models/v3/*.json`, `implementation.md`, `issues.md`, `motive.txt`, `results/README.md`, the three CSV datasets |
| Lead files that do not exist | `capture.cpp` (the capture code is `live_capture.cpp`), although `issues.md` names it |

What the current reference actually does (from its code, not its documents):
Mininet + Ryu emulation on Linux; an XGBoost **link-weight regressor**
(`ml_engine.cpp`, target `100 - score`, inputs delay, loss and 1/bandwidth,
R² 0.997 on its own synthetic data). It is not an algorithm classifier. Each
tournament runs four algorithms (Dijkstra, A*, ACO, GA), with ICMP probes
steered by OpenFlow "shadow" rules. Arbitration is a repeated-measures ANOVA
on log10 RTT frames with Holm correction, plus a weighted confidence
`C = 0.40 eta² + 0.35 s_ci + 0.25 s_g` with Wilson-interval loss feasibility.
A 30 s dwell limits switching, and swaps are hot swaps by OpenFlow cookie.
`implementation.md` and `motive.txt` describe an older plan (public ping
targets such as 8.8.8.8, FIFO zero-copy) that the code no longer follows.

### AAFR V0.6 baseline (audited before any change)

Baseline build: up to date, no errors. Baseline tests: **18/18 passed**
(206.7 s, including both Qt browser tests).

| Area | Finding |
|---|---|
| Lifecycle, scheduler, change detection, eligibility, deterministic recommendation | Implemented and tested |
| Controlled probing (balanced, randomized interleaved rounds, provenance, timeout, deadline, cancellation, executability check) | Implemented, but tested only indirectly through runtime tests (now tested directly, see below) |
| Statistical arbitration (paired log-latency t or block bootstrap of the median, lag-1 n_eff deflation, Holm, Bonferroni CIs, Wilson failure intervals, reliability repair, explicit insufficient or invalid evidence) | Implemented and tested |
| Stability gates (dwell, cooldown, failed-activation cooldown, expiring backoff, margin with CI bound and ms floor, hysteresis, safety override) | Implemented and tested; symmetric, with no permanent exclusion |
| Activation, confirmation before commit, stale-graph discard, recovery (re-confirm previous, ranked candidates, detour), degraded mode | Implemented and tested |
| Bidirectional switching | Verified **only when the graph shows the change**. **Defect:** degradation and recovery that showed up only in measurements (graph unchanged) were never acted on, because no trigger fired while Ready and an evaluation whose J(P) preferred the incumbent retained it without probing. This is the same class of failure as the reference's critical "never switches back" issue |
| Recovery explanation | **Defect (minor):** when recovery confirmed a replacement but the graph changed before commit, the record said "no configuration could be confirmed" instead of reporting a stale evaluation, and requested no re-evaluation |
| Concurrency | `Step()`/`Initialize()` are serialized by one mutex; graphs are immutable `shared_ptr<const ForwardingGraph>` snapshots; each record carries `graph_version` and `network_state_id`; evaluation never runs on the GUI thread. No defect found |
| Developer Tools | Shows runtime state, gates, decisions and events; no defect |

### Compatibility matrix

Decisions: Already implemented, Adapt, Independently implement, Improve existing, Defer, Reject. Licensing is the same for every reference component: no license, so only concepts can be reused.

| # | Reference component (file) | AAFR equivalent | Assessment | Benefit / risk | Decision | Status and test evidence |
|---|---|---|---|---|---|---|
| 1 | Statistical arbitration: RM-ANOVA on log10 frames + Holm (`anova.cpp`) | `StatisticalArbitrationEngine` | With two arms, RM-ANOVA is the paired t test (F = t²). The reference drops any frame with a NaN (listwise deletion), ignores autocorrelation and non-normality, and its eta² becomes 1 when the error variance is near zero. AAFR already covers pairing, Holm, CIs and robustness | No gain; ANOVA would only add assumptions | Already implemented (dual ANOVA rejected) | `runtime_statistics` (12 tests) unchanged and passing |
| 2 | Controlled active probing: interleaved frames, rotating order, at least 3 replies per block (`probing.cpp`) | `ControlledProbeEngine` | AAFR already interleaves and balances (order randomized per round). Block/frame grouping only matters for ICMP loss; AAFR's per-request probes are confirmed per path | SDN shadow rules: forbidden dependency | Already implemented; SDN steering rejected | New `runtime_probe` (9 tests) |
| 3 | Confidence score C = weighted eta², CI, ΔGoodness (`decision.cpp`) | Arbitration verdict + margin gate | Mixes unitless and z-scored terms. Clamping `s_g` caused the reference's own recovery bug, and a weighted sum is not a probability | Would import the same bug | Reject | AAFR keeps verdict + CI bound + relative and ms margin |
| 4 | Multi-metric Goodness, z-scored with dataset statistics, demand-capped bandwidth (`goodness.cpp`) | J(P) v1 (`forwarding_problem.h`) + `ForwardingPerformanceEvaluator` | Different units, normalization and direction (maximize vs minimize), and the z-statistics come from a synthetic dataset. AAFR treats bandwidth as a hard per-edge constraint against demand, so a low-bandwidth path that still covers the demand is never penalized. That is what the reference's "demand awareness" fix tried to achieve | Importing it would mix incompatible scores | Already implemented (objective unchanged, now regression-tested) | New `runtime_scoring` (8 scenarios) |
| 5 | Dynamic link weights by XGBoost (`ml_engine.cpp`, `train_regressor.py`) | J(P) edge cost | ML; saturates to 0 below 10 ms; trained on mismatched ranges | Out of scope for V0.7.1 | Defer (no ML import). No deterministic re-weighting is needed: J(P) is linear and separates sub-ms paths | `runtime_scoring::very_low_latency_differentiation` |
| 6 | Traffic-demand trigger: a tournament when demand halves (`engine.cpp`) | `min_bandwidth_mbps` constraint + change detection | AAFR uses demand as a constraint, and a demand change reaches the runtime as a ConstraintChange | — | Already implemented | `runtime_scenarios::network_constraint_change` |
| 7 | Anti-flapping: 30 s dwell (`decision.h`) | `SwitchingStabilityController` | AAFR's gates are a superset (dwell, cooldown, hysteresis, backoff, margin) | — | Already implemented | `runtime_stability`, `runtime_scenarios::intermittent_congestion` |
| 8 | Recovery switching: periodic tournaments + demand-drop trigger (`engine.cpp`) | **Missing in AAFR for measurement-only changes** | The reference's lesson holds: re-evaluation must not depend on the graph changing | Fixes the "never switches back" class | **Independently implement**: measurement-led review | `adaptive_runtime` (6 new tests), `runtime_scenarios` |
| 9 | Topology concurrency: mutex on the shared `topo_` (`engine.cpp`, `telemetry.cpp`) | Immutable versioned graph snapshots | AAFR's design avoids the race the reference had | — | Already implemented | `adaptive_runtime::concurrent_updates_with_worker`, `no_overlapping_switches`, `runtime_scenarios::topology_change_during_evaluation` |
| 10 | Probe pair matching in both directions (`engine.cpp`) | Probe ids per request; the executor returns the confirmed path | There is no reverse-flow matching to get wrong: each probe is one request/response on one path | — | Already implemented (not applicable) | `runtime_probe::balanced_interleaved_rounds` |
| 11 | Flow rollback with an RAII cookie guard (`probing.cpp`, `engine.cpp::swapTo`) | Commit after confirmation, re-confirm previous, degraded mode | AAFR installs no persistent state, so there is nothing to roll back. The lesson (never report a swap that was not confirmed) is already an AAFR rule | — | Already implemented; stale-recovery reporting improved | `adaptive_runtime::failed_switch_keeps_previous`, `runtime_scenarios::forwarding_activation_failure` |
| 12 | Telemetry z-score normalization (`goodness.cpp`), TTL hop estimate (`netmap.cpp`, `live_capture.cpp`) | V0.4 `FeatureNormalizer`, TShark adapter | AAFR features keep their units and quality. No TTL hop feature is added: it would be an estimate on host traffic, unrelated to controlled paths. AAFR does no textual IPv6 comparison (checked `network/`), so the reference's IPv6 bug has no counterpart here | — | TTL hops rejected; otherwise already implemented | Existing `network_state`, `telemetry_core` |
| 13 | Experiment runner and atomic result writing (`run_experiment.py`, `engine.cpp::writeStatus`) | `aafr-benchmark`, `aafr-runtime-experiment` | Useful ideas: versioned scenarios, seeds, files complete before they are read, traceable switches | — | Adapt | New `runtime_scenarios` writes `runtime_scenarios.jsonl` (`aafr.runtime_scenarios.v1`) |
| 14 | Result recording and replay (`tournaments.jsonl`, `timeline.json`, dashboard) | DecisionRecords (JSONL), events, Developer Tools | AAFR already records more per decision, and no second dashboard is needed | The dashboard has unauthenticated POST control endpoints | Reject the dashboard and `server.py` | — |
| 15 | Datasets | — | See the dataset assessment | — | Defer (not used in V0.7.1) | `datasets/README.md` |
| — | SDN (`sdn.cpp`, Ryu REST), Mininet testbed, Linux FIFO ingestion (`ingestion.cpp`), fork/exec | V0.5 executor | Forbidden runtime dependencies, and insecure in the reference (world-writable FIFO, unauthenticated control) | — | Reject | — |

### Implemented changes

1. **Measurement-led review** (`runtime/adaptive/aafr_runtime.{h,cpp}`,
   `runtime_types.{h,cpp}`, `reason_codes.{h,cpp}`). Adds
   `RuntimeConfig::review_interval_ms` (default 30 000; 0 disables; valid
   range 1 000..3 600 000), a new trigger `EvaluationTrigger::Review`
   ("review") and a new reason `MEASUREMENT_REVIEW` (appended; existing
   codes unchanged). While Ready, at every interval the runtime probes the
   incumbent against the best alternative, even when J(P) prefers the
   incumbent. If every algorithm agrees on the active path, the alternative
   is the existing detour (all relays of the active path forbidden). Rules:
   - only measured evidence can make a review switch; without probing, a
     review retains and records why;
   - the ordinary gates still apply (dwell, cooldown, margin, hysteresis,
     backoff). A time-blocked preference found by a review is re-checked by
     another review, so it is measured again instead of being retained on
     J(P);
   - after a commit, or after any evaluation that probed, the next review is
     a full interval away. Overhead is bounded to one campaign per interval:
     24 probe requests with the default 12 rounds and two arms;
   - algorithm names are never hard-coded, and previously rejected paths are
     subject only to the expiring backoff.
2. **Stale recovery is reported as stale** (`Recover()`): a replacement
   confirmed on the relays after the graph changed is not committed. The
   record gets `EVALUATION_STALE` with an accurate explanation, the
   configuration stays unverified (degraded), and a TopologyChange
   evaluation is requested.
3. **Status and Developer Tools** (`RuntimeStatus::review_due_ms`,
   `last_review`, `counters.reviews`; `devtools_panel.cpp`): the Adaptive
   runtime pane gains a "Measurement review" row (interval, time to the next
   review, latest review decision; "disabled" or "probing is off" when they
   apply) and shows the review count. The browser uses the default interval.
4. **Test support** (`tests/support/runtime_test_support.h`): per-path
   measurement-only latency (`extra_latency_ms`) and loss (`loss_ratio`,
   deterministic per request id), and activation-only confirmation failure
   (`unconfirmed_activation_paths`).

Unchanged: the ten algorithms, J(P) v1, the V0.5 executor, the arbitration
method, the gate rules, and the DecisionRecord schema (the new trigger and
reason are new values in existing fields).

### Tests added

| Program | Tests | What it proves |
|---|---|---|
| `adaptive_runtime` (+6) | `measurement_only_congestion_and_recovery` | A is preferred; A degrades only in measurement; a review switches to B; while A stays congested, B is retained (J(P) alone would return to A); A recovers; a review switches back to A. The graph version never changes |
| | `review_disabled_misses_measurement_only_congestion` | reproduces the V0.6 defect with `review_interval_ms = 0` |
| | `review_without_probing_never_switches`, `equal_latency_lossy_incumbent_left_by_review`, `review_schedule_in_status`, `invalid_review_interval_rejected` | an estimate never drives a review switch; loss is not masked by latency parity (Wilson reliability repair); status timers; config validation |
| `runtime_probe` (new, 9) | balanced interleaved rounds and reproducibility, probe isolation (scenario, payload tag, only the arm paths), unexecutable path refused, timeout, cancellation and cleanup (no request left in flight), deadline, partial failure, stale measurement giving invalid evidence, invalid configuration | probing is safe and bounded |
| `runtime_scoring` (new, 8) | the eight required scoring scenarios | see matrix row 4 |
| `runtime_scenarios` (new, 10) | stable, gradual, sudden, recovery, intermittent, equal latency with unequal loss, constraint change, activation failure, topology change during evaluation, rapid telemetry | versioned, seeded scenarios with a JSONL switching trace |
| `browser_observability` (+1 check) | the review row reflects the runtime's real schedule | |

Scenario results (ManualClock, simulated executor, seeds 101–110; the
trace is written to `runtime_scenarios.jsonl` in the test working directory):

| Scenario | Evaluations | Switches (to) | Rejected | Failed activations | Discarded | Reviews | Probe requests |
|---|---|---|---|---|---|---|---|
| stable_network (40 s) | 9 | 0 | 0 | 0 | 0 | 8 | 192 |
| gradual_congestion (40 s) | 15 | 1 (B at 12.0 s) | 1 | 0 | 0 | 7 | 192 |
| sudden_congestion (20 s, at 5 s) | 5 | 1 (B at 6.25 s) | 1 | 0 | 0 | 3 | 96 |
| congestion_recovery (45 s) | 11 | 2 (B at 6.25 s, A at 21.25 s) | 2 | 0 | 0 | 7 | 240 |
| intermittent_congestion (toggles every 1.5 s) | 12 | 0 | 5 | 0 | 0 | 11 | 264 |
| equal_latency_unequal_loss (loss 0.6 on A from 2 s) | 8 | 1 (B at 17.5 s, trigger review) | 2 | 0 | 0 | 7 | 168 |
| network_constraint_change | 8 | 2 (B safety switch at 3 s, A at 16.25 s) | 1 | 0 | 0 | 4 | 96 |
| forwarding_activation_failure | 11 | 1 (B at 16.75 s, after B confirms) | 3 | 2 | 0 | 6 | 240 |
| topology_change_during_evaluation | 8 | 1 (B at 5.25 s) | 1 | 0 | 1 | 4 | 168 |
| rapid_telemetry_updates (new graph every 250 ms) | 7 | 0 | 0 | 0 | 0 | 6 | 144 |

These are runtime-logic results under simulation, not performance
measurements, and no benchmark improvement is claimed. Congestion that
toggles faster than the hysteresis window produces no switch at all: the
runtime stays stable but does not adapt. That is the intended anti-flapping
trade-off.

Final build: clean, no warnings in the changed files. Final tests (Debug,
2026-11-18): **21/21 passed** in 222.6 s. These are the 18 baseline programs
plus `runtime_probe`, `runtime_scoring` and `runtime_scenarios`; none were
skipped and none failed.

Live forwarding is still covered by `runtime_loopback` (real 127.0.0.1
relays). The scenario suite is simulation and is not used as proof of live
activation.

### Known risks and limitations

- A review costs one probe campaign per interval on the controlled relays
  (24 requests by default). At the 30 s default this is small, and the
  runtime records it (`probe_overhead_ms`, `probe_requests`).
- A review can only find alternatives the graph allows; a single-path graph
  has nothing to compare against.
- Reviews judge AAFR's own loopback relays. Browser page loads still do not
  pass through them (unchanged since V0.5).
- The new runtime tests and the scenario suite use the simulated executor.
  Release-build and interactive verification remain open, as for V0.6.
