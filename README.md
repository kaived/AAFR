# AAFR

### Adaptive Application-Layer Forwarding Runtime

AAFR is an intelligent browser networking research system that uses a trained AI Expert Model to recommend the most suitable routing algorithm according to the current network condition and dynamically manage algorithm switching at runtime.

The system combines real-time network monitoring, AI-based algorithm recommendation, controlled runtime switching, multiple routing algorithms, and explainable decision recording within a Qt-based browser networking environment.

The research investigates whether adaptive algorithm selection can improve browser network performance and reliability under changing network conditions compared with fixed routing approaches.

---

## Core Idea

Different routing algorithms can behave differently under different network conditions.

Instead of always using one fixed routing algorithm, AAFR observes the current network state and uses a trained AI Expert Model to recommend the most suitable algorithm.

```text
Network Condition
       ↓
Network Monitor
       ↓
NetworkState
       ↓
AI Expert Model
       ↓
Recommended Algorithm
+
Confidence
       ↓
AAFR Runtime
       ↓
Stability Checks
       ↓
Continue / Switch
       ↓
Active Routing Algorithm
       ↓
Routing Controller
       ↓
Controlled Routing Environment
```

This process continues while the user uses the browser.

---

## Research Question

> Can a trained AI Expert Model learn to select the most suitable routing algorithm from current network conditions, and can AAFR safely switch between routing algorithms at runtime to improve browser network performance and reliability?

---

## Core Capabilities

### 1. AI-Based Runtime Algorithm Selection

The trained AI Expert Model receives the current `NetworkState` and recommends the most suitable routing algorithm.

```text
NetworkState
      ↓
Expert Model
      ↓
Recommended Algorithm
+
Confidence
```

The Expert Model is responsible for algorithm recommendation.

---

### 2. Adaptive Runtime Algorithm Switching

When network conditions change meaningfully, the Expert Model can recommend a different algorithm.

AAFR receives the recommendation and manages the transition.

```text
Current Algorithm
       ↓
Network Changes
       ↓
Expert Model
       ↓
New Recommendation
       ↓
AAFR
       ↓
Continue / Switch
```

The browser continues operating while this process occurs in the background.

---

### 3. Stable and Explainable Switching

AAFR does not blindly switch every time the model produces a different prediction.

The runtime considers factors such as:

- Current active algorithm
- Recommended algorithm
- Model confidence
- Meaningful network change
- Minimum dwell time

Each significant runtime decision is recorded through a `DecisionRecord`.

---

## Supported Routing Algorithms

AAFR works with ten routing and optimization algorithms.

### Classical Algorithms

1. Dijkstra
2. A*
3. Bellman-Ford

### Metaheuristic and Nature-Inspired Algorithms

4. Ant Colony Optimization (ACO)
5. Particle Swarm Optimization (PSO)
6. Genetic Algorithm (GA)
7. Artificial Bee Colony (ABC)
8. Grey Wolf Optimizer (GWO)
9. Firefly Algorithm
10. Whale Optimization Algorithm (WOA)

All algorithms are designed to operate through a common routing interface.

---

## System Architecture

The complete runtime architecture is:

```text
                         User
                          ↓
                     Qt Browser
                          ↓
                   Browser Traffic
                          ↓
        ┌─────────────────┼─────────────────┐
        ↓                 ↓                 ↓
Browser Metrics         TShark       Controlled Network
        │                 │                 │
        └─────────────────┼─────────────────┘
                          ↓
                   Network Monitor
                          ↓
                     NetworkState
                          ↓
                  Meaningful Change?
                          ↓
                   AI Expert Model
                          ↓
            Algorithm + Confidence
                          ↓
                     AAFR Runtime
                          ↓
                   Stability Checks
                          ↓
                  Continue / Switch
                          ↓
                   Active Algorithm
                          ↓
                  Routing Controller
                          ↓
             Controlled Routing Environment
                          ↓
                       Network
                          ↓
                  Website / Server
                          ↓
                       Response
                          ↓
                     Qt Browser
```

---

## Component Responsibilities

| Component | Responsibility |
|---|---|
| Qt Browser | User-facing browser and application traffic generation |
| Browser Metrics | Application-level network observations |
| TShark | Automated packet and transport-level observations |
| Wireshark | Human packet inspection, debugging, and validation |
| Network Monitor | Collects and aggregates network measurements |
| NetworkState | Represents the current network condition |
| AI Expert Model | Recommends the most suitable routing algorithm |
| Confidence | Represents the strength of the model recommendation |
| AAFR Runtime | Controls runtime continuation and algorithm switching |
| Stability Logic | Prevents unnecessary or unstable switching |
| Routing Algorithm | Calculates a network path |
| Routing Controller | Applies the selected routing decision |
| Controlled Routing Environment | Provides controllable topology and forwarding |
| DecisionRecord | Records and explains runtime decisions |
| Experiment Layer | Evaluates AAFR against baseline approaches |

---

## Datasets

AAFR uses three fixed datasets.

### Network Dataset

```text
datasets/raw/network_dataset_raw.csv
```

Contains:

- 3,500 network scenarios
- 45 columns
- Seven network-condition categories

The seven scenario categories are:

```text
A_Excellent
B_Congested
C_Unreliable
D_Sparse
E_Dense
F_Dynamic
G_Large
```

Each category contains 500 scenarios.

---

### Routing Benchmark Dataset

```text
datasets/raw/routing_benchmark_3500.csv
```

Contains:

- 35,000 benchmark records
- 3,500 unique network scenarios
- 10 routing algorithm results for each scenario

The relationship is:

```text
3,500 Network Scenarios
        ×
10 Routing Algorithms
        =
35,000 Benchmark Records
```

The benchmark contains performance information such as:

```text
scenario_id
algorithm
source
destination
latency
bottleneck_bandwidth
loss
path_delay_std
hops
runtime
objective
```

The `3500` in the filename represents the number of network scenarios, not the total number of benchmark rows.

---

### Expert Dataset

```text
datasets/raw/routing_expert_dataset.csv
```

Contains:

- 3,500 scenario-level records
- 58 columns

It contains network information, algorithm-selection targets, and benchmark-derived comparison information used for Expert Model development.

Potential target fields include:

```text
target_algo_eps0
target_algo_eps0_5
target_algo_eps1
target_algo_eps2
```

The final Expert Model target is determined through dataset and research analysis rather than selected arbitrarily.

---

## Dataset Relationship

The three datasets represent different parts of the research pipeline.

```text
Network Dataset
      ↓
3,500 Network Scenarios
      ↓
Evaluate 10 Algorithms
      ↓
Routing Benchmark
      ↓
35,000 Algorithm Results
      ↓
Algorithm Comparison
      ↓
Expert Dataset
      ↓
Expert Model Training
```

Files inside:

```text
datasets/raw/
```

are treated as immutable source data and should not be modified directly.

Generated preprocessing artifacts, when required, belong in:

```text
datasets/processed/
```

---

## AI Expert Model

The Expert Model is a supervised multiclass classification system.

Conceptually:

```text
X = NetworkState Features

Y = Suitable Routing Algorithm
```

Runtime inference:

```text
NetworkState
      ↓
Expert Model
      ↓
Recommended Algorithm
+
Confidence
```

Example:

```json
{
  "recommended_algorithm": "PSO",
  "confidence": 0.87
}
```

Initial model candidates include:

- Majority-class baseline
- Logistic Regression
- Decision Tree
- Random Forest
- Gradient Boosting / XGBoost

Model selection is based on experimental evaluation.

---

## Model Evaluation

Because algorithm-selection targets may be imbalanced, accuracy alone is not sufficient.

Evaluation includes:

- Macro F1
- Balanced Accuracy
- Weighted F1
- Per-class Precision
- Per-class Recall
- Confusion Matrix
- Inference Latency
- Confidence Calibration

---

## NetworkState

`NetworkState` is the shared data contract between the Network Monitor and Expert Model.

```text
Network Sources
      ↓
Network Monitor
      ↓
NetworkState
      ↓
Expert Model
```

Potential feature groups include:

```text
Network Performance
Traffic State
Congestion State
Topology State
Dynamic / Failure State
Routing Context
```

Only features that can be consistently reproduced during runtime should be used by the final Expert Model.

**Implemented in V0.4:** the `NetworkState` contract and its builder
(`network/network_state/`), fed by V0.3 telemetry. It currently holds
host-wide capture-interface features and browser navigation features only.
Topology, congestion and routing-context groups above remain plans: no
forwarding topology exists yet, and the Expert Model that would consume the
state is not started.

---

## Network Monitoring

AAFR combines measurements from multiple sources.

```text
Qt Browser Measurements ─────┐
                              │
TShark Measurements ──────────┼──→ Network Monitor
                              │
Controlled Network State ─────┘
                              ↓
                         NetworkState
```

### TShark

TShark provides automated packet-level observations.

It does not select algorithms or perform routing.

### Wireshark

Wireshark is used for human inspection, debugging, measurement validation, traffic analysis, and experimental demonstration.

Wireshark is not the network simulator.

---

## Runtime Switching

The Expert Model recommends the algorithm.

AAFR controls whether that recommendation should be applied.

```text
Expert Model
      ↓
Recommended Algorithm
+
Confidence
      ↓
AAFR
      ↓
Same as Active?
   /        \
 YES         NO
  ↓           ↓
Continue   Stability Checks
              /       \
            FAIL      PASS
             ↓          ↓
          Continue    Switch
```

The newly activated algorithm affects subsequent routing decisions.

---

## Controlled Routing Environment

AAFR does not attempt to control arbitrary Internet routers.

Custom algorithms operate within a controlled routing and forwarding environment.

```text
Qt Browser
      ↓
Browser Traffic
      ↓
Controlled Routing Environment
      ↓
Network / Destination
```

AAFR controls the routing strategy through:

```text
Expert Model
      ↓
AAFR
      ↓
Active Algorithm
      ↓
Routing Controller
      ↓
Controlled Routing Environment
```

Simply changing an `active_algorithm` variable is not sufficient.

The selected algorithm must calculate a path and the routing controller must apply that path within the experimental environment.

---

## DecisionRecord

AAFR records significant runtime decisions.

A DecisionRecord contains information such as:

```text
timestamp
network_state
active_algorithm
recommended_algorithm
model_confidence
meaningful_change
stability_checks
decision
previous_algorithm
new_algorithm
reason
```

Possible decisions include:

```text
CONTINUE
SWITCH
REJECT_SWITCH
```

This provides explainability for runtime behaviour.

---

## Research Alignment

AAFR is being aligned with its research method: XGBoost link-weight
prediction, a four-algorithm pool, an interleaved probe tournament, dual
ANOVA arbitration and OpenFlow enforcement on an SDN testbed. **The sections
above and below this one describe the pre-alignment design** (a LightGBM
model that recommends one of ten algorithms) and are rewritten only when the
corresponding phase lands. Nothing in this section claims that AAFR
implements the method yet.

### Status

| Phase | Content | Status |
|---|---|---|
| 0 | Baseline protection, frozen specification, reference values | Done 2026-11-19 (see verification below) |
| 1 | Path metrics D, L, B, T, H; scores J, J_eng and G; Welford and frozen normalization | Implemented and verified 2026-11-19 as a standalone library (`research/`); not connected to the algorithms or the runtime |
| 2 | Probe tournament model, dual ANOVA, Holm, Wilson feasibility, confidence C and the decision as a reference implementation | Implemented and verified 2026-11-19 as a standalone library, with the research engine's incomplete-frame, paired-round and highest-C selection rules; not connected to probing, the runtime or the browser |
| 3 | Four-algorithm pool: Dijkstra, A*, ACO, GA as specified | Implemented and verified 2026-11-19 (`research/path_algorithms`): Dijkstra on start-up delays, A* on predicted link weights, ACO and GA on J_eng with the specified budgets (operators reused from production; non-specified choices documented), candidate grouping into arms. Tested with declared statistics and explicit weights only. Not connected to the runtime |
| 4 | XGBoost link-weight regressor through the C API | Implemented and verified 2026-11-19: the research's v3 model imported unchanged (`research/models/xgboost_v3`), XGBoost 3.0.4 C API, bitwise parity with Python, recorded metrics reproduced on the rebuilt test split, connected only to the research A*. Not connected to the runtime or the browser |
| 5A | Isolated research pipeline: telemetry contract → link weights → four algorithms → arms → probe observations → arbitration → decision record | Implemented and verified 2026-11-19 (`research/research_pipeline`), controlled telemetry and recorded or synthetic probes only, decision only |
| 5B.1 | Research pipeline attached to the runtime through a non-actuating adapter | Implemented and verified 2026-11-19 (`research/runtime_adapter`): explicit invocation, recommendations only, no forwarding change |
| 5B.2 | Research observability in the browser: browser-owned host, read-only Developer Tools view | Implemented and verified 2026-11-19 (`research/research_host`, Runtime → Research pipeline); decision only, evaluates only on "Evaluate now" |
| 5B.3 | Research records and demonstration readiness: bounded durable record store, records inspectable in Developer Tools, keyboard-focus test fixed | Implemented and verified 2026-11-19 (`research/record_store`): original record JSON kept byte-for-byte across restarts, at most 200 records, source labels kept, no measured telemetry; decision only |
| 5C | Runtime acting on the tournament and decision rule | Not started (approval pending) |
| 6 | Browser live-host mode and Developer Tools simplification | Not started |
| 7 | SDN testbed (Mininet, Open vSwitch, OpenFlow 1.3) on a separate Linux host | Not started; cannot run on the Windows build machine |
| 8 | Removal of isolated components; documentation rewrite | Not started |

### Frozen specification

Every number below is recorded in
[`tests/fixtures/research/reference_values.h`](tests/fixtures/research/reference_values.h)
and checked by the `research_fixture` test.

- **Telemetry.** tshark streams time, addresses, TTL, TCP ACK RTT and frame
  length; an empty ACK RTT is missing, not an error. Hop count = nearest
  initial TTL (64, 128, 255) minus the observed TTL. Per-link delay, loss and
  jitter come from ICMP probes between monitor hosts, utilization from
  OpenFlow port counters. Sweeps every 2 s after 5 warm-up sweeps.
- **Link weight.** One XGBoost regressor maps the normalized delay, loss and
  inverse bottleneck bandwidth of a link to W_link (high = bad), floored at 0.
  Trained on the 3,500-run simulated dataset, target 100 - score, 400 trees,
  depth 5, learning rate 0.05, 2,800 / 700 split; recorded test R² 0.997 and
  MAE 1.04. It is a regressor: it does not select algorithms.
- **Path metrics.** D(P) = Σ d_e; L(P) = 1 - Π(1 - ε_e);
  B(P) = min c_e(1 - u_e); T(P) = B(P)(1 - L(P)); H(P) = number of links.
- **Three different scores, never merged.**
  J = 0.5 H + 100 / B + D + 100 L is an independent quality score used only to
  check tested paths. J_eng = z(D) + z(L) + z(1/B) + 0.3 z(H) is the engine
  cost of ACO and GA. G = -0.5 z(H) + z(B) - z(RTT) + z(T) is the measured
  goodness of an arm; only ΔG = G_challenger - G_incumbent is used. z uses
  training-set statistics.
- **Algorithms.** Exactly four. Dijkstra on the link delays measured at
  start-up (static, load-unaware baseline); A* on the live W_link with
  heuristic = hop distance × smallest weight; ACO (24 ants, 30 iterations) and
  GA (population 30, 40 generations) minimizing J_eng.
- **Triggers.** Link loss > 3 %, link utilization > 85 %, RTT > 2 × its
  reference, a 90 s timer, or a manual request; 15 s cooldown.
- **Tournament.** Each distinct path other than the installed one is an arm.
  Each arm gets 10 frames × 5 blocks × 8 ICMP echoes (400 probes), interleaved
  round by round with the arm order rotated, steered by ICMP-only rules so
  user traffic stays on the installed path. A block counts only with at least
  3 of 8 replies.
- **Dual ANOVA.** log10 RTT throughout. Step 1: variance within each block and
  of block means within each frame. Step 2: repeated-measures ANOVA across
  frames with the round as the repeated factor, giving F(1, 9), partial η² and
  a 95 % CI on the mean paired difference (reported as 10^d̄). Holm correction
  across challengers. F and t exact via the regularized incomplete beta
  function.
- **Decision rule (reference version).** s_η = partial η²;
  s_ci = min(1, -CI_hi / 0.05) if CI_hi < 0, -min(1, CI_lo / 0.05) if CI_lo > 0,
  else 0; s_g = clip(ΔG, -1, 1); **C = 0.40 s_η + 0.35 s_ci + 0.25 s_g**.
  Switch only if C > 0.70, Holm p < 0.05 and at least 30 s since the last
  switch. Feasibility: round-trip loss ≤ 3 % and mean RTT ≤ 150 ms; an arm is
  infeasible when the lower Wilson 95 % bound of its loss exceeds the budget;
  an infeasible challenger gets s_ci = -1; if the incumbent is infeasible and
  the challenger's upper Wilson bound is within budget, s_ci = +1 (repair).
- **Enforcement.** OpenFlow 1.3 flow-mods as JSON to a thin service that
  accepts the Ryu `ofctl_rest` format and programs Open vSwitch with
  `ovs-ofctl`. It is **not Ryu**. Make-before-break order.
- **Testbed.** Mininet, six Open vSwitch switches, veth links shaped with
  tc htb / netem. Congestion scenario: 14 Mb/s UDP h1 → h2 from 41.7 s. Loss
  scenario: 6 Mb/s UDP, 15 % loss each way on s1-s2 from 41.6 s. 300 s each.
- **Out of scope.** A traffic classifier is not part of the implemented
  method. The live host mode is a diagnostic mode and is not evaluated.

### Known defects of the reference rule

These are part of the reference behaviour and are **kept, not silently
fixed**. A correction is a separate proposal that needs approval.

1. The clipping of s_g makes the rule one-sided: with s_η = s_ci = 1, C > 0.70
   needs ΔG > -0.2, so a lower-goodness (lower-bandwidth) path can never win
   however strong its latency evidence (recorded congestion tournaments 2-4:
   C = 0.50).
2. The trained regressor predicts W_link ≈ 0 for every link under about
   10 ms, which covers the whole testbed, so A* effectively chooses on hop
   count.
3. A lossy path is not abandoned when its latency equals the alternative's.
4. Probe pairs are matched in one direction only.
5. A topology structure is read without a lock in two modules.
6. An experiment script can read its result file before the run ends.

Two later variants of the rule exist: one sets s_η = 1 and treats a repair as
significant, with a demand-drop trigger; a demand-aware one adds a bandwidth
budget against the measured demand. Neither is the reference rule, and
neither will be enabled without approval.

### Resolved conflicts in the method description

| Topic | Alternative description | Adopted |
|---|---|---|
| Normalization | Online Welford z-score | z-scores use training-set statistics. Proposed reading: Welford computes those statistics, which are then frozen; no z-scoring against live running statistics (decision pending) |
| Name "J(P)" | The z-scored cost minimized by ACO / GA | That cost is J_eng; J is the raw independent score |
| Confidence | 0.40·η² | 0.40·s_η with s_η = partial η² |
| Controller | "Ryu controller" | Ryu-compatible REST service, not Ryu |
| Timing | XGBoost < 15 µs; Dijkstra / A* 13-18 µs; ACO / GA ~1-2 ms | Recorded: Dijkstra 0.011-0.028 ms, A* 0.014-0.035 ms, ACO 0.8-2.7 ms, GA 2.0-3.5 ms; no XGBoost timing recorded. AAFR measures its own |
| Results | 2,475× throughput, 62.5 % loss eliminated, "mathematically proven" | Probe-arm figures of one tournament, not user-traffic results; at these effect sizes the ANOVA is trivially significant |
| Capture | tshark FIFO; Npcap in live mode | Testbed capture is tshark on Linux, not Npcap |
| Live mode | Process-I/O demand ranking, ICMP TTL hop discovery | Diagnostic only, not evaluated |

Earlier recorded results are not AAFR results and are not reported as such.

### Open decisions

The specification leaves these open; no value is assumed until they are
decided (the ACO and GA operators, also left open, are now AAFR
implementation choices recorded under Phase 3; the regressor, its input
contract and the J_eng / G statistics come from the imported research model
under Phase 4, with their scenario-level proxies): how a frame is aggregated when some of its blocks are
invalid; whether the regressor should later be retrained on per-link testbed
data; the
future of the six algorithms and the LightGBM model outside the pool; whether
the V0.7.3 browser gateway and loopback relays stay as a labelled loopback
demonstration; and where the Linux testbed runs.

Phases 1 and 2 add these, also left open by the specification and not
assumed by the code (each is reported as undetermined or must be declared by
the caller):

- **Frames with invalid blocks** and **several qualifying challengers**: first
  reported as undetermined; now resolved by adopting the research engine's own
  rules (see "Research-engine policies" under Phase 5A).
- **Standard deviation of the frozen statistics.** The contract stores the
  delta degrees of freedom (0 or 1) declared by the producer; there is no
  default.
- **Training-set columns for the z-statistics**, and the file format that
  will carry them with the regressor artifact (Phase 4).

Readings adopted where the specification's wording settles it:

- The frame value is the mean of its valid block means (the specification
  defines block means and their variance within a frame; the research engine
  discards invalid blocks); block and frame variances use ddof 1.
- The mean-RTT budget is checked on the arithmetic mean RTT of the returned
  probes as a point value (only loss has a Wilson interval). An arm with no
  returned probe is infeasible.
- With no earlier switch the dwell condition is met.
- Holm p is still required in a repair: the reference rule does not treat a
  repair as significant by itself.

### Phases 1 and 2: implementation

A standalone C++17 library, `aafr-research` (`research/`), of pure functions.
It links the adaptive runtime only to reuse its verified numerical
primitives (regularized incomplete beta, t quantile, Wilson interval, Holm
step-down). **Nothing in the browser, the live runtime, the algorithms, the
gateway or the relays calls it**, and the runtime's own arbitration
(`runtime/adaptive/statistical_arbitration`) is unchanged.

| File | Content |
|---|---|
| `research/path_metrics.h` | `ComputePathMetrics` (D, L, B, T, H with per-link validation and explicit issues); `QualityScore` (J); `EngineCost` (J_eng); `Goodness` (G from a measured `ArmOutcome`, throughput = B × measured delivery ratio); `GoodnessDifference` (ΔG) |
| `research/normalization.h` | `WelfordAccumulator`; `Freeze`; `NormalizationStatistics` (contract `aafr.research.normalization.v1`: source, per-feature count, mean, standard deviation, declared ddof); `ZScore` |
| `research/probe_tournament.h` | `ProbeTournament` / `ProbeArm` / `ProbeFrame` / `ProbeBlock` (10 × 5 × 8, a lost echo is an empty value); `ValidateTournament`; `SummarizeArm` (log10 block means and micro-variances, frame values and block-mean variances, delivery, mean RTT) |
| `research/arbitration_reference.h` | `RepeatedMeasuresAnova` (two arms × n paired rounds: SS_arm, SS_round, SS_error, F(1, n − 1), p, partial η², 95 % CI, RTT ratio; n = 10 with complete data); `FSurvival`; `HolmAdjusted`; `EvaluateFeasibility`; `ComputeConfidence`; `Decide`; `ArbitrateTournament` (paired-round exclusion, highest-C selection; verdict *keep*, *switch* or *undetermined*, with the reason) |

Rules the code enforces: missing or invalid values are never replaced by 0
or any default (the metric and every score that needs it are absent, with
the reason); B = 0 is a valid value that leaves J and J_eng undefined; z-scores
exist only for features with declared frozen statistics and are never
computed against live values or the current candidates; ΔG is an input, and
without it there is no confidence; the decision is a calculation only and
switches nothing.

**Verification.**

- `research_math` (14 cases) covers each metric, additive delay,
  multiplicative loss, the bottleneck, zero and missing bandwidth, invalid
  loss / utilization / delay / capacity, empty, disconnected and looping
  paths, missing data, J = 16.667 on the worked check, J_eng and G against
  hand-computed z-scores, Welford, and the frozen contract's refusals.
- `research_arbitration` (15 cases) covers tournament dimensions and
  structure, valid / invalid blocks and lost echoes, log10 block means,
  micro-variance, frame aggregation, the ANOVA, its sum-of-squares
  decomposition and degenerate inputs, F survival, Holm (single, multiple,
  equal, capped, ordering), Wilson feasibility, the repair rule, every s_ci
  branch, the s_g clip, the threshold and dwell boundaries, the recorded
  tournaments, the one-sided limitation, and end-to-end verdicts. Since the
  research-engine policies were adopted it has 19 cases (290 checks), adding
  incomplete frames (one invalid block; a frame with no valid block, 9 paired
  rounds, F(1, 8), against scipy), insufficient paired rounds (1 paired round:
  no test, Holm p = 1, keep; 2 rounds: F(1, 1); an untested challenger kept in
  the Holm family), highest-C selection (including a higher-C challenger that
  fails feasibility), and exact ties (earliest arm kept, tie recorded, choice
  follows arm order).
- **Independent reference.** `tests/research/scipy_reference.py` (numpy 2.5.5,
  scipy 1.18.1, seeded) generates `tests/fixtures/research/scipy_reference_values.h`
  from synthetic tournaments; `research_reference_current` fails if the header
  is stale. The ANOVA is checked against `scipy.stats.ttest_rel` (F = t²,
  partial η² = t² / (t² + 9)) and `scipy.stats.f.sf`, the CI against
  `scipy.stats.t.ppf`, Wilson against `binomtest(...).proportion_ci(method="wilson")`,
  Welford against `numpy.var`, and Holm against a separate implementation.
  Enforced tolerances, all met: frame values, block micro-variances, partial
  η² and the mean paired difference 1e-12 relative; F 1e-9 relative; CI bounds
  1e-10 relative; p-values (F and t routes, down to 1e-18) 1e-9 relative;
  Wilson bounds 1e-12; Holm 1e-15; Welford 1e-12 on log-normal data and 1e-6
  on values near 1e9, where doubles are spaced 1.2e-7 apart (measured with
  the same algorithm in Python: 1.4e-8 relative; the naive E[x²] − E[x]²
  formula has relative error 1 there).
- **Recorded tournaments.** From their recorded summary statistics
  (partial η², the CI of the RTT ratio, probes delivered, mean RTT, ΔG),
  `ComputeConfidence` and `Decide` reproduce all seven recorded C values to
  three decimals and every swap / keep decision, including the repair in the
  first loss tournament and C = 0.50 in congestion tournaments 2-4. Their Holm
  p-values are not tabulated individually (all far below 0.05), so 1e-15
  stands in for them. Their per-frame probe data is not available, so their
  F, p, partial η² and CI are **not** recomputed; no per-frame data is
  reconstructed.
- **Builds (2026-11-19).** Debug and Release build; 40 CTest suites each (the
  37 earlier ones plus `research_math`, `research_arbitration` and
  `research_reference_current`). In the full runs all 36 non-GUI suites pass
  in both builds. The GUI suites failed only inside the full runs (Debug:
  `browser_interface` timeout and `browser_forwarding::switchingPinsConnectionsAndRecovers`,
  the intermittent case recorded under V0.7.4; Release:
  `browser_interface::windowNavigationAndShortcuts`, keyboard focus). Each GUI
  suite then passed run alone in both builds (Debug: interface 73.0 s,
  observability 73.6 s, forwarding 69.5 s, protocol 76.7 s; Release: 15.2 s,
  31.7 s, 53.9 s, 62.9 s). No file of the runtime, algorithms, routing,
  gateway, network or browser changed, and none of them links `aafr-research`.

### Phase 3: four-algorithm pool

**Assessment of the production algorithms** (`algorithms/`, unchanged). All
ten share one additive objective,
c(e) = latency + 100·(−ln(1 − p)) + 0.5 + policy cost, and every
metaheuristic is scored by it. Against the specification:

| Algorithm | Specified cost | Production | Reuse |
|---|---|---|---|
| Dijkstra | Sum of start-up link delays (static snapshot) | Current objective on the live graph | Search logic and tie-breaking identical; it reproduces the specified result when the objective is latency-only and the graph carries start-up delays |
| A* | Sum of predicted W_link; h = hops to destination × min W_link | Same heuristic (`hop_lower_bound`), applied to the shared objective | Heuristic and search identical; the production objective has no W_link term, so feeding it would mean storing W_link as a latency |
| ACO | Minimize non-additive J_eng; 24 ants, 30 iterations | Ant System with MAX-MIN bounds on the additive cost; 20 ants by default | Path construction and loop handling could be reused once the operators below are decided |
| GA | Minimize J_eng; population 30, 40 generations | Path chromosomes, tournament of 3, common-node crossover, regrow mutation, elitism on the additive cost; population 20 by default | Same |

**Implemented** (`research/path_algorithms.h`, a separate graph model that
keeps the start-up delay snapshot, the live observations and the predicted
W_link as distinct fields):

- `StaticDelayDijkstra` and `PredictedWeightAStar`, one shared search with
  the production queue order and tie-breaking. A missing cost on an available
  link is refused (`MissingCost`, naming the link), never defaulted; negative
  or non-finite costs are refused; unavailable links are excluded.
- `AntColonyOptimization` (24 ants × 30 iterations) and `GeneticAlgorithm`
  (population 30 × 40 generations) minimize J_eng of complete paths; J_eng is
  never split into edge weights. Operators below.
- `RunPool` (the four, in a fixed order), `EvaluatePath` (J and J_eng on live
  observations, separate from each algorithm's optimization cost),
  `DistinctChallengers` (one arm per distinct path, labelled with every
  algorithm that returned it; the installed path is never an arm).

**ACO and GA operators: AAFR implementation choices.** The specification
fixes only the objective (J_eng) and the budgets. Everything else below is
an AAFR choice, made by reusing the production operators (`algorithms/aco`,
`algorithms/ga`, version 1.0.0, unchanged) where they do not depend on an
additive edge cost, and the production helpers `RandomPath`, `Reachable` and
`Rng` as they are.

| | Choice | Source |
|---|---|---|
| ACO | Ant walk from the source, next hop ∝ τ(e)^α·η(e)^β among unvisited neighbours; a dead end fails the ant | Production |
| ACO | η(e) = 1 (no edge heuristic: J_eng has no per-edge term, and production's η = 1/(1 + c(e) − c_min) needs an additive cost); β therefore has no effect | AAFR choice |
| ACO | Deposit 1 / (1 + J_eng(P) − J_best), J_best = lowest J_eng seen so far in the run: positive for any sign, 1 for the best path, strictly decreasing in J_eng, so the ranking is the objective's | AAFR choice (production deposits 1 / J′ on a positive additive cost) |
| ACO | τ₀ = 1, evaporation then deposits by every scored ant then one extra best-so-far deposit, τ clamped to [0.01, 10], α = 1, ρ = 0.3 | Production |
| GA | Simple node-sequence chromosomes; initial population by randomized DFS | Production |
| GA | Tournament of 3 (comparisons only, valid for negative J_eng) | Production default |
| GA | Common-node crossover at rate 0.9 then loop removal; regrow mutation at rate 0.2; elitism of 1; duplicates allowed | Production |
| GA | Every child checked as a simple source-destination path over available links; an invalid child is rejected and replaced by its parent (none occurred in testing) | AAFR safeguard |
| Both | A path with undefined J_eng (e.g. B = 0) is rejected, never scored; a missing or invalid measurement on any available link refuses the run (`MissingCost` / `InvalidCost`, naming the link), as for Dijkstra and A* | AAFR choice |
| Both | Randomness only from `Rng(seed)` (seed 1 by default, a parameter); fixed iteration budget, no deadline or early stop | AAFR choice |
| A* | Ties among equal-cost paths broken by node order (relevant when every W_link is 0) | Production |

**Verification** (`research_algorithms`, 11 cases, 314 checks):

- Six-switch testbed: Dijkstra takes s1-s2-s6 (4 ms) and keeps it when the
  live delay and utilization of s1-s2 rise, because it routes on the start-up
  snapshot; J on the live values then ranks the detour better, showing that
  the optimization cost is not the quality score.
- A* with explicit predicted weights (no model): it avoids a link the weights
  mark as bad, matches brute force, ignores start-up delays, refuses missing,
  negative and NaN weights, and with all weights 0 returns a valid zero-cost
  path deterministically (decided by tie-breaking).
- On 40 seeded random 12-node graphs, research Dijkstra and A* return the
  same paths and costs as the production Dijkstra and A* (latency-only
  objective), A* equals Dijkstra on the same weights with no more
  expansions, and both match brute force.
- Dijkstra and A* returning the same path give one arm labelled with both
  (and nothing when that path is installed); distinct paths give distinct
  arms; same path, different optimization costs.
- With declared test statistics (not training statistics) the brute-force
  J_eng optimum on a congested testbed differs from the static Dijkstra path.
  Without statistics J_eng is unavailable and J is still defined.

**ACO and GA verification** (`research_metaheuristics`, 9 cases, 2,731
checks; declared test statistics, not training statistics):

- Refusals: malformed graph, unknown or equal endpoints, missing or
  incomplete statistics, missing and invalid link measurements (named),
  unreachable destination, invalid parameters. A link that is down needs no
  measurement.
- Paths through a zero-bandwidth link are never returned; if every path has
  undefined J_eng the result is `NoScoredPath`.
- The returned cost is exactly `EngineCost` of the returned path, never below
  the brute-force optimum, and differs from J.
- Negative J_eng: shifting the frozen delay mean by 256 standard deviations
  shifts every J_eng by exactly −256 (all values negative). With the same
  seeds ACO and GA return the same paths with the same evaluation counts,
  so the treatment of negative values does not change the ranking.
- Fixed seeds reproduce path, cost and counts; budgets are exact (ACO: 30
  iterations, 24 × 30 = 720 ant walks; GA: 40 generations, 30 + 40 × 29 =
  1,190 evaluations); every returned path is a valid simple path; across 27
  runs on random graphs no GA child was invalid (ACO ants hit 399 dead ends,
  which are rejected).
- Identical paths: each distinct non-installed path is exactly one arm,
  labelled with every algorithm that returned it; on the congested testbed
  ACO and GA both return s1-s3-s6 and form one arm.

**Comparative results** (simulation on fixtures, not measurements of the
testbed). Rate of reaching the brute-force J_eng optimum, 20 seeds per
instance:

| Instances | Simple paths (mean) | ACO optimum rate | ACO mean gap | GA optimum rate | GA mean gap |
|---|---|---|---|---|---|
| Congested six-switch testbed (1) | 9 | 100 % | 0 | 100 % | 0 |
| Random 10-node digraphs (28 with a path) | 57 | 97.7 % | 0.031 | 99.8 % | 0.003 |

On the congested testbed with explicit predicted weights (10 on s1-s2, 1
elsewhere): Dijkstra s1-s2-s6 (start-up delay 4 ms; J = 205, J_eng = 36.2);
A* s1-s3-s6 (W = 2); ACO and GA s1-s3-s6 (J_eng = −2.1; J = 12). Each
algorithm reports its own optimization cost; J and J_eng are evaluated
separately. Release run time per request: ACO 0.87 ms and GA 1.87 ms on the
testbed, 0.70 ms and 1.88 ms on average over the random 10-node graphs
(one workstation; Dijkstra and A* below the 0.01 ms timer resolution shown).

- **Builds after ACO / GA (2026-11-19).** Debug and Release build; 42 CTest
  suites each (adds `research_metaheuristics`). All 38 non-GUI suites pass in
  both builds. Each GUI suite run alone passes in both builds (Debug:
  interface 73.8 s, observability 75.1 s, forwarding 69.7 s, protocol 81.2 s;
  Release: 18.3 s, 35.0 s, 54.5 s, 62.9 s). No intermittent failure occurred
  in this run. No file of the runtime, algorithms, routing, gateway, network
  or browser changed.
- **Builds (2026-11-19).** Debug and Release build; 41 CTest suites each. All
  37 non-GUI suites pass in both builds. GUI suites were run one at a time:
  all four pass in Release (15.2 s, 31.9 s, 54.3 s, 62.8 s). In Debug,
  observability (74.0 s) and forwarding (69.4 s) passed; interface
  (`lifecycleAndTabIsolation`, a page-load signal wait) and protocol
  (`wssBidirectionalMessagingAndTraversal`, a view wait) failed once and both
  passed on an immediate rerun (74.7 s, 77.4 s). Those Debug binaries were
  not rebuilt for Phase 3 and had passed alone earlier the same day, so this
  is Debug Qt WebEngine timing instability, recorded here rather than hidden.

The link-weight regressor and the frozen statistics Phase 3 lacked are
provided by Phase 4 below, except per-link inputs at run time, which on the
testbed come from per-link ICMP probes and OpenFlow counters (Phase 7), not
from host capture.

### Phase 4: XGBoost v3 link-weight model

The research's trained regressor is **imported, not retrained**: the five
files of `engine/models/v3/` from
`https://github.com/oddproblem/Adaptive-runtime` at commit `6b6c008`, copied
byte-for-byte from the git blobs (LF line endings) into
`research/models/xgboost_v3/`. `.gitattributes` marks the folder `-text` so no
checkout converts them. `provenance.json` (AAFR's manifest) records the
source, each file's git blob id, SHA-256 and size, the model, feature and
training contracts, the statistics' proxy mappings and the known
limitations. The source repository declares no license.

| File | SHA-256 | Role |
|---|---|---|
| `regressor.json` | `a5384851…4325b87` | The model: XGBoost JSON, saved by 3.0.4, gbtree, `reg:squarederror`, 400 trees, 3 features, base score 26.38 |
| `scaler_regressor.json` | `1c30d576…2f2efa` | Training-scaler mean and scale of the 3 inputs (fitted on the 2,800 training rows) |
| `features_regressor.txt` | `4bc9898e…2e46c7` | Input order |
| `normalization.json` | `29f39d37…68a16` | Frozen statistics for J_eng and G |
| `regressor_metrics.json` | `483e5647…77d5b` | Recorded R², MAE, split sizes, gain importances |

(Full hashes in `provenance.json`; the four text files' hashes differ from a
CRLF checkout of the source, which is why the blobs were imported.)

**Dependency.** XGBoost 3.0.4 (the version that saved the model): the
official Windows wheel `xgboost-3.0.4-py3-none-win_amd64.whl` (SHA-256
`b57d05fb…ac30d600`, equal to PyPI's published hash), installed in the
project `.venv` for the Python reference and supplying `xgboost.dll`; the
official header `include/xgboost/c_api.h` of tag `v3.0.4` (commit `0915f5f`,
Apache-2.0, `third_party/xgboost/` with its LICENSE). The wheel ships no import
library or header, so `LinkWeightModel` loads `xgboost.dll` at run time from
`AAFR_XGBOOST_LIBRARY` (CMake cache path, default the `.venv` copy), with every
function typed from the official header. Nothing links against it, and a
missing library is a reported error. Research verification requires it:
`research_xgboost` and `research_pipeline` **fail** when the library is not
available (CMake also warns at configure time); they never skip.

**Feature contract** (`aafr.research.link_weight.v1`): inputs in the order
`delay_ms` (ms), `loss_ratio` (0..1), `inv_bottleneck_bw` (1/(Mb/s)); z = (x −
mean) / scale from `scaler_regressor.json`, computed in double and passed as
float32; output the raw float32 prediction, W_link = max(0, raw). The inverse
bandwidth is built two ways, kept apart:

- `TrainingInput`: 1 / max(throughput, 1e-6), the training preprocessing,
  where throughput is a scenario's **aggregate delivered traffic**;
- `LinkInput`: 1 / (c (1 − u)), a link's **available bandwidth**, with no floor
  (B = 0 gives no input).

They share a unit, not a physical meaning. Missing, non-finite or
out-of-range inputs (negative delay, loss outside [0, 1], inverse bandwidth
≤ 0) give `MissingInput` / `InvalidInput` for that row and no weight; there is
no fallback. (The source engine substitutes 1 ms for a missing delay and
floors available bandwidth at 0.1 Mb/s; AAFR does neither.)

**Normalization contract.** Inference uses only `scaler_regressor.json`.
`PathObjectiveStatistics` reads `normalization.json`, after checking its hash,
as the frozen statistics of J_eng and G, unchanged: computed over **all 3,500
scenarios, the 700 test rows included**, population standard deviation
(ddof 0), so they are not unbiased validation estimates. Mappings (all
scenario-level proxies): Delay ← `latency`; Loss ← `loss`;
InverseBandwidth ← 1/`throughput`; Hops ← `avg_hop_count`;
Bandwidth ← `offered_load_mbps` (offered traffic, not capacity);
Rtt ← `latency` (one-way scenario latency, not an RTT);
Throughput ← `throughput` (delivered traffic).

**Verification with Python XGBoost** (`tests/research/xgboost_reference.py`,
xgboost 3.0.4, numpy 2.5.5, scikit-learn 1.9.1):

- The five files match `provenance.json` and agree on the feature order; the
  model has 3 features and 400 rounds.
- **Original split rebuilt exactly**: `train_test_split(test_size=0.2,
  random_state=42)` on the training dataset (same 3,500 scenarios as
  `routing_expert_dataset.csv`); refitting the scaler on the rebuilt training
  rows reproduces the shipped means and scales bit for bit.
- **Imported model on the rebuilt test split** (executed here): R² =
  0.9971980602, MAE = 1.0365576301; recorded in `regressor_metrics.json`:
  R² = 0.9971980602, MAE = 1.0365576301 (difference 0). With the floor at 0:
  R² 0.997199, MAE 1.0325. Training rows: R² 0.999329, MAE 0.556.
- `normalization.json` recomputed over all 3,500 rows with ddof 0: identical
  (its delay mean 31.961 vs 32.067 over the training rows only).
- Prediction distribution: test raw predictions from −0.332 to 101.1
  (median 6.77); 54 of 700 negative, 148 in [0, 0.5), none non-finite. Test
  rows with latency < 10 ms: 213, of which 184 have target 0; their mean raw
  prediction is 0.061 (max 0.91), 54 negative.

**C++ parity** (`research_xgboost`): on 113 inputs (normal range, low delay
down to 0.1 ms, high loss up to 1.0, low throughput down to 0.1 Mb/s, the 1e-6
throughput floor, extremes, 64 seeded in-range points, the testbed links)
the normalized float32 features and the raw predictions are **bitwise
identical** to Python's (maximum difference 0; the enforced tolerance is
4 float32 ulp, allowing for summation order). Three of them are negative and
floored to 0. Predictions are deterministic, batch and single calls agree,
and invalid rows do not affect valid ones in a batch.

**Low-delay behaviour on the six-switch testbed** (link inputs: one-way delay,
no loss, 1/capacity):

| Links | Delay | inv_bw | Raw | W_link |
|---|---|---|---|---|
| s1-s2, s2-s6 | 2 ms | 0.1 | 0.000431 | 0.000431 |
| s1-s3, s3-s6, s1-s4, s4-s5, s5-s6 | 5-7 ms | 0.01 | 0.004236 | 0.004236 |
| s2-s3, s3-s5 | 3-4 ms | 0.02 | −0.066314 | 0 |

4 of 18 directed links have weight 0 and the largest weight is 0.0042: the
model treats the whole idle testbed as nearly free, as the research reports.
A* on these weights takes s1-s2-s6 (W = 0.00086), the unique brute-force
minimum of the 9 simple paths, so tie-breaking does not decide it here; with
the zero-weight links s2-s3 and s3-s5 a tie is possible on other demands.
With 15 % loss on s1-s2 its weight becomes 17.58 and A* takes s1-s3-s2-s6
(W = 0.0047), using the zero-weight s3-s2. A link without a prediction (e.g.
missing utilization, or zero available bandwidth) leaves A* with
`MissingCost`, naming the link.

**Performance** (Release, one workstation, `research_xgboost`):

| Step | Time |
|---|---|
| Model load (5 SHA-256 checks, JSON, `XGBoosterLoadModel`; library already resident) | 108 ms |
| Feature preparation (`LinkInput` + normalization), per link | 0.47 µs |
| Inference, one link per call (median / p99) | 0.54 ms / 0.88 ms |
| Inference, 18 testbed links in one call (median) | 0.56 ms |
| Inference, 1,000 rows in one call | 2.3 ms (2.3 µs per row) |
| A* on predicted weights, search only (median) | 0.013 ms |

Each inference call is dominated by building the XGBoost matrix and the call
itself, not by the 400 trees, so batching all links of a sweep costs about
the same as one link. The presentation's "< 15 µs" inference figure is **not**
reproduced by this measurement. Working-set growth for a second model load in
the same process was 0.9 MB (the library was already loaded, so this is not
the library's footprint).

- **Builds (2026-11-19).** Debug and Release build; 44 CTest suites each (adds
  `research_xgboost` and `research_xgboost_reference_current`). All 40
  non-GUI suites pass in both builds. Each GUI suite run alone passes in both
  builds (Debug: 74.5 s, 76.3 s, 68.2 s, 77.0 s; Release: 15.0 s, 30.7 s,
  53.6 s, 63.6 s); no intermittent failure occurred. No file of the runtime,
  production algorithms, routing, gateway, network capture or browser
  changed, and none of them links `aafr-research`.

**Limitations carried forward.** The model is a scenario-level regressor
used on links; its inverse-bandwidth input changes meaning between training
(delivered traffic) and links (available bandwidth); its learned weight is
about 0 for every idle testbed link, so A* there is decided by tiny
differences; gain importance of the bandwidth input is 0.004; the J_eng / G
statistics include the test rows and map RTT, bandwidth and throughput to
scenario proxies; and the source repository has no license.

### Phase 5A: isolated research pipeline

`research/research_pipeline.h` joins the research components into one
decision-only pipeline. It measures nothing, sends no probe, installs no rule
and switches no traffic; every record says so (`"action": "none: decision
only, no forwarding change"`). Nothing in the runtime, gateway, browser,
capture pipeline or forwarding calls it, and the production LightGBM provider
and arbitration are untouched.

```text
Stage 1  PlanTournament(TelemetrySnapshot, LinkWeightModel, NormalizationStatistics)
   TelemetrySnapshot  (origin: controlled | recorded | synthetic, never live)
     -> AssignPredictedWeights      XGBoost v3 W_link per available link
     -> Dijkstra / A* / ACO / GA    on the predicted graph (pool order)
     -> EvaluatePath                J and J_eng of each returned path
     -> DistinctChallengers         arms: "incumbent" (installed path) + "challenger-N"
Stage 2  DecideTournament(TelemetrySnapshot, TournamentPlan, ProbeObservations?)
   ProbeObservations  (origin: recorded | synthetic; 10 x 5 x 8 per planned arm)
     -> observations must match the plan (arm ids, paths, roles)
     -> per arm: SummarizeArm; G from path hops, snapshot B(P), probe mean RTT
        and delivery ratio; dG per challenger
     -> ArbitrateTournament         RM-ANOVA, Holm, Wilson feasibility, C, rule
     -> ResearchDecisionRecord      keep | switch | undetermined, with reasons
        ToJson                      deterministic JSON (timing optional)
```

**Input contract.** `TelemetrySnapshot`: a labelled origin and a required
description; the research graph (per link: live delay, loss, capacity,
utilization; the start-up delay snapshot; availability); source, destination
and the installed path (a simple path over available links); seconds since
the last switch (absent: none yet). `ProbeObservations`: a labelled origin, a
required description and one `ProbeArm` per planned arm with the planned id,
path and role. Statistics: `PathObjectiveStatistics` of the imported model.
**Output contract** (`aafr.research.decision_record.v1`): the inputs' origins
and descriptions, model id and source commit, statistics source, every link's
prediction (status, raw, W_link), every algorithm's status, path, own
optimization cost, J and J_eng (ACO / GA with seed and counts), the arms with
their algorithms, per-arm probe outcome and goodness, per challenger dG,
ANOVA (F, df, p, partial η², CI, ratio), Holm p, Wilson feasibility,
confidence terms, decision flags, the verdict, the selected arm and path,
and the reason.

**Research-engine policies** (adopted 2026-11-19). The source repository has
two commits: `a0ce0ed` (2026-11-14), which added the recorded results, and
`6b6c008` (2026-11-15). `anova.cpp` / `anova.h` and the model artifacts are
identical in both. `decision.cpp` differs: at `a0ce0ed` it is the reference
rule AAFR implements (s_η = partial η², significant only if Holm p < α),
while `6b6c008` adds s_η = 1 on a repair and treats a repair as significant
by itself, a later variant that AAFR does not adopt. The recorded
tournaments follow the `a0ce0ed` rule (e.g. s_η = 0.991 on the loss repair).
AAFR's incomplete-data and selection rules now correspond to `a0ce0ed`:

| Situation | Research engine (`a0ce0ed`) | AAFR (`research/`) |
|---|---|---|
| Block | valid with ≥ 3 of 8 replies (`blockStats`) | `kMinReplies = 3` |
| Frame | invalid blocks discarded; valid with ≥ 1 valid block; value = mean of valid block means (`frameStats`) | `kMinValidBlocksPerFrame = 1`, `FrameSummary::valid`, same value |
| Pairing | per challenger, rounds where either frame is not valid are dropped (`pairVsIncumbent`) | same; `ChallengerEvaluation::paired_rounds` |
| Test size | `rmAnova` needs ≥ 2 subjects; df = (1, n − 1); t interval with n − 1 df | `kMinPairedRounds = 2`; F(1, n − 1), t(n − 1); F(1, 9) only with 10 paired rounds |
| Fewer than 2 paired rounds | default result F = 0, p = 1, η² = 0 (cannot pass) | test not computed, p = 1 in Holm, cannot qualify; verdict keep unless another qualifies |
| Holm | over every challenger (`holmBonferroni`) | same, untested challengers with p = 1 |
| Several qualifying | highest C (`c.passes && c.C > bestC.C`) | highest C (`qualifying_arms`) |
| Exact tie on C | strict `>`: the earliest challenger in arm order is kept | same, and the tie is recorded (`tied_arms`) |
| Arm order | incumbent, then distinct paths in algorithm order | incumbent, then distinct paths in pool order |
| Dwell | checked after selection: a blocked winner gives keep | part of qualification: same verdict (keep) |

Differences kept on purpose (AAFR does not default values): a missing dG
(the engine uses s_g = 0) and a non-finite η² (the engine uses 0) leave the
tournament undetermined; an ANOVA with no variation at all is not computed
(the engine reports F = 0, p = 1), which gives the same outcome, no switch.
Probe observations that do not match the plan are also undetermined.

**Provenance and redistribution.** The imported model artifacts and the
recorded-tournament fixture come from `https://github.com/oddproblem/Adaptive-runtime`,
a public user repository that declares **no license** (no license file in
either commit; GitHub reports none). Default copyright therefore applies and
**redistribution permission is not established**: it must be obtained from
the repository owner (a license added to the repository, or written
permission) before these files are published with AAFR. The vendored
XGBoost header and its LICENSE are Apache-2.0 (v3.0.4 has no NOTICE file).

**Verification** (`research_pipeline`, 10 cases, 252 checks; needs XGBoost):

- Invalid telemetry (no description, installed path off the graph or not
  from the source, negative dwell time) refuses the plan; the record is
  undetermined.
- Plan on the idle testbed: link weights equal Python XGBoost's for every
  link; the four algorithms run in pool order; A* equals the brute-force
  minimum of the predicted weights; ACO and GA never beat the brute-force
  J_eng minimum; arms are the incumbent plus each distinct other path. When
  all four algorithms return the installed path, there is no tournament and
  the verdict is keep.
- Congestion state of the first recorded congestion tournament (s1-s2: 22 %
  loss, 99 % utilization), with **synthetic** probes shaped like it
  (incumbent 3 of 8 echoes per block at ~81.5 ms, i.e. 150 of 400):
  Dijkstra keeps s1-s2-s6, A* takes s1-s3-s2-s6, ACO and GA s1-s3-s6 (one arm,
  both labels). With s1-s3-s6 clean (~20.4 ms) and s1-s3-s2-s6 lossy and slow
  the verdict is **switch to s1-s3-s6 by the repair rule** (C = 0.999995),
  identical to calling the reference arbitration directly; inside the dwell
  time it is keep. With both challengers clean, both qualify (C = 0.999994
  and 0.999995) and the **higher C is selected** (s1-s3-s6).
- One incumbent block with 2 replies and one incumbent frame with no valid
  block: the block is discarded, the empty frame's round is dropped, both
  challengers are tested on 9 paired rounds, F(1, 8), and the clean
  challenger is selected. With the incumbent valid in one round only, no
  test is computed and the verdict is keep.
- Observations for a wrong path, a missing arm, no observations or no
  description: undetermined with the mismatch recorded.
- Missing utilization on s3-s5: no prediction there, so A*, ACO and GA refuse
  (`MissingCost`); Dijkstra still runs; no challenger, keep.
- The JSON record is identical for identical inputs, parses, carries the
  schema, origins `controlled` / `synthetic`, the model commit, 18 link
  weights and 4 candidates, and no `live` origin.
- **Recorded research tournaments as an independent reference** (seven, from
  `results/{congestion,loss}/tournaments.jsonl` of the source commit, copied
  into `tests/fixtures/research/recorded_tournaments.h` by
  `tests/research/recorded_tournaments.py`): from the recorded per-frame log10
  RTT, AAFR's RM-ANOVA reproduces the recorded F (largest relative difference
  2.2e-11), p (1.0e-10), partial η² (1.3e-15), CI (0) and RTT ratio; Holm
  equals the recorded Holm p; AAFR's goodness G from each arm's recorded
  outcome and `normalization.json` equals the recorded goodness exactly (all
  14 arms); with AAFR's Wilson feasibility, ComputeConfidence reproduces the
  recorded s_η, s_ci, s_g and C (largest difference 5.6e-16), the repair
  flags, and `Decide` reproduces every recorded swap or keep. These
  tournaments are frame-level records, so they verify the statistics layer,
  not the echo-level stage of the pipeline.
- `research_xgboost` and `research_pipeline` were run with `xgboost.dll`
  temporarily renamed: both exit 1 (fail), as required.
- **Builds (2026-11-19, after adopting the research-engine policies).** Debug
  and Release build; 45 CTest suites each. All 41 non-GUI suites pass in both
  builds, including every research suite and both reference-staleness
  checks. GUI suites run one at a time: Debug observability (88.1 s),
  forwarding (73.6 s) and protocol (82.8 s) and Release interface (16.3 s),
  observability (32.1 s) and forwarding (54.5 s) passed; Debug
  `browser_interface` (90 s timeout, `windowNavigationAndShortcuts`) and
  Release `browser_protocol` (`wsPlainBidirectional`, WebSocket close code)
  failed once and both passed on an immediate rerun (75.4 s, 63.7 s). Neither
  binary was rebuilt by Phases 4-5A (built 14:57 and 15:09 the same day), so
  these are recorded as GUI timing instability, not regressions. An earlier
  run of this phase also had one Debug `browser_interface` timeout that
  passed on rerun. No file of the runtime, production algorithms, routing,
  gateway, network capture or browser changed.

**Phase 5B dependencies** (not started): approval to touch the live
runtime; a source of per-link delay, loss and utilization (per-link ICMP
probes and OpenFlow counters on the testbed, Phase 7; host capture does not
provide them); a probe scheduler producing interleaved 10 × 5 × 8
tournaments; the triggers
(loss > 3 %, utilization > 85 %, RTT > 2 × reference, 90 s timer, manual;
15 s cooldown) and the 2 s telemetry sweep with 5 warm-up sweeps; how the
production arbitration and LightGBM provider are isolated or retired; and
where DecisionRecords of the research pipeline are stored and shown.

**Research alignment.** The pipeline follows the frozen specification end
to end: learned link weights feed A*, ACO and GA optimize J_eng, identical
paths share an arm, the installed path is the incumbent, and the decision is
the reference rule with its one-sided limitation and its repair rule. It
reproduces the recorded research statistics and goodness exactly, and its
incomplete-data and selection rules are the research engine's (`a0ce0ed`).
It differs from the engine only where the engine substitutes values (a
missing delay becomes 1 ms, available bandwidth is floored at 0.1 Mb/s, a
missing dG or η² becomes 0): AAFR reports those cases as unavailable or
undetermined instead.

### Phase 5B.1: research runtime adapter (non-actuating)

**Runtime audit.** `AAFRRuntime` (`runtime/adaptive`) is the only owner of the
active forwarding configuration. `Step()` (on its worker thread every tick,
serialized by one mutex) runs change detection, eligibility, the
algorithms, the injected `IRecommendationProvider` (deterministic, or the
V0.7.2 LightGBM `AIRecommendationProvider`), controlled probing with its own
arbitration (`statistical_arbitration`: paired t / bootstrap, Holm, Wilson),
the stability gates (dwell, cooldown, backoff, margin, hysteresis) and
activation through the forwarding executor, and writes a `DecisionRecord`.
Its read interface is `Status()` (`RuntimeStatus` with the
`ActiveConfiguration`: path, `config_version`, `activated_monotonic_ns`),
`Decisions()` and `Events()`, all thread-safe. Developer Tools read it only
through `IRuntimeObservabilityProvider` (`setRuntimeProvider`) and
`setAdaptiveRuntime`.

**Integration boundary.** `research/runtime_adapter.h` sits beside the
runtime, not inside it:

- it **reads** the runtime through `AAFRRuntime::Status()` only
  (`ReadRuntime`: the installed path and the seconds since the runtime last
  *applied* a switch, i.e. `config_version > 1`) and, when asked, converts the
  runtime's forwarding graph into a labelled controlled snapshot
  (`SnapshotFromRuntimeGraph`, with `CaptureStartupDelays` for Dijkstra's
  start-up costs; utilization is not in that graph and stays absent);
- it **holds no** forwarding executor, recommendation provider or runtime
  control reference, so it cannot activate a path, send a probe or change the
  runtime's decisions;
- no file of the runtime, its arbitration, the LightGBM provider, the
  gateway, the relays, the capture pipeline or the browser changed; the
  browser did not link `aafr-research` in 5B.1 (it hosts the adapter from
  5B.2 on, read-only).

**Contracts.** Inputs are the Phase 5A `TelemetrySnapshot` and
`ProbeObservations` (labelled origin, required description; no live
origin exists). `Plan()` runs stage 1 without changing state (to learn the
arms); `Evaluate()` runs both stages and records a `ResearchRecommendation`:
sequence, verdict, installed path, recommended path (switch only), origins,
model id and source commit, the full `ResearchDecisionRecord` and its JSON.
`ResearchRecommendation::kApplied` and `ResearchRuntimeStatus::kActuationEnabled`
are compile-time `false`; the record's action reads "none: decision only, no
forwarding change"; it is a different type from the runtime's
`DecisionRecord`, so the two cannot be confused.

**Lifecycle** (`ResearchRuntimeState`): `unavailable` (model or statistics
not loaded, e.g. no XGBoost library; every request refused and counted) →
`idle` (ready) → `evaluating` (one at a time) → `decided` (keep, switch or
undetermined recorded, "not applied") or `error` (input contract violated: no
recommendation, reasons kept in `last_errors`); from `decided` or `error` the
next request goes through `evaluating` again. History is bounded
(`Options::history`). The 30 s dwell and every threshold are the research
library's; the adapter adds no stability rule, and because a recommendation
changes nothing it never restarts the dwell, which counts from the runtime's
own applied switches.

**Verification** (`research_runtime_adapter`, 7 cases, 71 checks; needs
XGBoost):

- No XGBoost library or no model directory: `unavailable`, requests refused,
  `Plan()` refused, nothing recorded.
- `idle` → `Plan()` leaves the state and counters unchanged → `Evaluate()`
  on controlled congested-testbed telemetry with synthetic probes gives
  `decided` with a switch recommendation to the J_eng path, `applied()` false,
  origins and model commit preserved, and a record byte-identical to calling
  `DecideTournament` directly.
- Two adapters give identical records for identical inputs; history keeps
  the last 4 of 7 with consistent sequence numbers.
- Contract violations (no description, installed path off the graph,
  negative dwell time, unlabelled probes): `error`, no recommendation. Missing
  utilization on one link: A*, ACO and GA refuse, keep. Tournament needed but
  no probes: undetermined.
- 4 threads × 3 concurrent evaluations: serialized, 12 consistent
  recommendations.
- **Real `AAFRRuntime`** (deterministic test executor, manual clock): a
  snapshot built from the runtime's own graph and active path (utilization
  absent, so keep) and a switch recommendation on testbed telemetry leave the
  runtime's configuration version, path, counters, decisions, events and the
  executor's request count **unchanged**; the runtime keeps stepping normally.
- After a switch the runtime applied itself (`config_version` 2): at 10 s
  the research dwell blocks (keep), at 30 s the same evidence gives a switch
  recommendation; the runtime's configuration stays version 2.
- **Builds (2026-11-19).** Debug and Release build; 46 CTest suites each (adds
  `research_runtime_adapter`). All 42 non-GUI suites pass in both builds,
  including every research and existing runtime suite. GUI suites run one at
  a time: observability, forwarding and protocol pass in both builds (Debug
  86.0 s, 71.8 s, 82.7 s; Release 35.8 s, 55.0 s, 62.1 s). `browser_interface`
  failed once in both builds in `windowNavigationAndShortcuts` (text typed
  into the address bar did not arrive: keyboard focus was elsewhere on the
  desktop) and passed on an immediate rerun in both (Release 15.5 s, Debug
  74.6 s). Its binaries were not rebuilt by Phases 4-5B.1 (the browser does not
  link the research library), so this is recorded as desktop focus
  instability, not a regression.

**Remaining after 5B.1** (the browser host and the Developer Tools view are
Phase 5B.2, below): approval to let research recommendations act; a live
telemetry source with per-link utilization and an origin for measured inputs;
real probe tournaments from a probe scheduler and ICMP steering (Phase 7);
the triggers and cooldown; an activation policy; persistence of research
records.

### Phase 5B.2: research observability in the browser (non-actuating)

**Browser lifecycle audit.** `BrowserWindow` builds the telemetry pipeline,
the NetworkState service, the forwarding service (loopback relays), the
optional LightGBM worker / `AIRecommendationProvider` and `AAFRRuntime` in its
constructor; `startForwarding()` starts the relays and, on the forwarding
worker, the runtime; Developer Tools attach to the runtime by a queued call
on the GUI thread. The destructor deletes Developer Tools, detaches the
gateway, stops the runtime, the forwarding service, the model worker and the
state service, then shuts the pipeline down. Developer Tools read the runtime
through `IRuntimeObservabilityProvider` / `setAdaptiveRuntime` on a 250 ms
refresh timer; their only runtime action is "Evaluate now" (a diagnostic
evaluation that still passes every gate) and the loopback condition buttons.

**Integration** (the minimum needed):

| Point | Change |
|---|---|
| `research/research_host.{h,cpp}` (new) | `ResearchHost`: owns one `ResearchRuntimeAdapter` and one worker thread; `Start()` loads the model on the worker; `RequestEvaluation()` queues one controlled evaluation (coalesced while one is pending); `Stop()` / destructor join the worker and release the adapter. `IResearchObservabilityProvider::ResearchSnapshot()` returns a copy of host state, adapter status, the latest records and the request log; it has no method that evaluates or changes anything. |
| `BrowserWindow` (constructor, destructor, header) | Creates the host after the runtime with a snapshot source that reads only `ForwardingService::GraphSnapshot()` and `AAFRRuntime::Status()` (`ReadRuntime`, steady clock) and labels the snapshot *controlled: browser runtime loopback relay graph (configured relay values; not per-link SDN telemetry)*, start-up delays captured from the first graph; starts it immediately (background load). Destructor: Developer Tools first, then `researchHost_->Stop()` and release, before the runtime and forwarding service stop. The model directory and XGBoost library are build settings; `AAFR_XGBOOST_LIBRARY` overrides the library. |
| `DevToolsPanel` | `setResearchObservability(provider, request)`: one read-only sub-tab, Runtime → **Research pipeline**, appended after the existing seven (their order and contents are unchanged); refreshed with the other views. The existing **Evaluate now** also calls `request`, queuing one research evaluation; no new control was added. |
| `CMakeLists.txt` | `aafr-browser-ui` links `aafr-research`; the two path settings; the `research_host` test. |

The host holds no forwarding executor, provider or runtime control
reference (the adapter it owns holds none either), so neither it nor the
panel can activate a path, send a probe or change the runtime's
configuration. The LightGBM provider and the production arbitration are
untouched.

**Developer Tools mapping** (Runtime → Research pipeline, read-only):

| Shown | Source |
|---|---|
| Banner "Research pipeline: decision only. Its recommendations are never applied; …; not per-link SDN telemetry, and has no utilization." | `DevToolsPanel::researchDecisionOnlyText()` |
| Research host (not started / loading / running / stopped), pipeline state (unavailable / idle / evaluating / decided / error) and detail, "Actuation: disabled: decision only" | host and adapter status |
| Link-weight model id and source commit; frozen-statistics source (incl. test rows, ddof 0, proxies) | adapter status |
| Counters (keep / switch recommended / undetermined / input errors / refused), request counters, last input errors | adapter and host |
| Latest recommendation "#n verdict (not applied)", telemetry input with its origin label and description, probe observations ("none supplied" today) | last record |
| Tables: recommendations (Applied = "no (decision only)"), candidate paths (algorithm, status, path, own cost and kind, J, J_eng, note), arbitration (arm, path, algorithms, paired rounds, F, Holm p, partial η², RTT ratio CI, C, qualifies), XGBoost link weights (status, raw, W_link, note), request log | last record and host log |

**Verification.**

- `research_host` (6 cases, 35 checks; needs XGBoost): not-started → running
  → stopped; requests before start and after stop refused; nothing evaluates
  without a request (60 s of clock time pass with no record); an explicit
  request on the runtime's own graph gives a keep recommendation (only
  Dijkstra can run without utilization), controlled origin, no probes, not
  applied; a runtime without a configuration gives a *skipped* request and
  never reaches the adapter; while the worker is busy, the second request
  waits and the third and fourth are coalesced; an unavailable model keeps
  the host running and refuses requests; destruction without `Stop()`, with a
  pending request, is clean; two hosts give identical records, and against a
  real `AAFRRuntime` the configuration, counters, decisions and the
  executor's request count are unchanged.
- Browser (`browser_observability`, new cases
  `researchPipelineIsDecisionOnlyInBrowser` and
  `researchPipelineUnavailableWithoutXGBoost`, real Qt WebEngine): the Runtime
  sub-tabs are the existing seven plus "Research pipeline"; the banner is the
  decision-only text; the model loads in the background (state `idle`, model
  `xgboost-link-weight-v3` / `6b6c008`, statistics "incl. test rows");
  a page load records nothing (0 rows, 0 requests); after the runtime is
  ready, **Evaluate now** queues one research evaluation: "#1 keep (not
  applied)", telemetry "controlled: browser runtime loopback relay graph
  (configured relay values; not per-link SDN telemetry); … utilization not
  measured", probes "none supplied", Applied "no (decision only)", candidates
  Dijkstra `path_found` and A* / ACO / GA `missing_cost` with the links named,
  link weights `missing_input`, request "evaluated: recommendation #1"; the
  runtime's active path and configuration version are unchanged. With
  `AAFR_XGBOOST_LIBRARY` pointing at a missing file the pane shows
  `unavailable`, pages still load, Developer Tools open and close, and the
  window shuts down cleanly. Both cases passed in 8 of 8 consecutive Release
  runs (after the test waited for Developer Tools to attach the runtime, as
  the existing adaptive test does; one earlier run had clicked before that
  queued attach).
- **Builds (2026-11-19).** Debug and Release build; 47 CTest suites each (adds
  `research_host`). All 43 non-GUI suites pass in both builds. GUI suites run
  one at a time: observability (with the two new cases), forwarding and
  protocol pass in both builds (Debug 108.4 s, 71.9 s, 81.3 s; Release 36.8 s,
  55.0 s, 61.5 s). `browser_interface` fails in both, in
  `windowNavigationAndShortcuts` only (53 of 54 cases pass).
- **Keyboard-focus investigation.** `windowNavigationAndShortcuts` shows the
  window without waiting for it to become active, then relies on keyboard
  focus: the address-bar edit is preserved only while the address bar has
  focus, and Ctrl+T / Ctrl+W go to the active window. On this desktop the
  case then failed 13 of 13 times in Release (10 alone, 3 in the full suite),
  also with the XGBoost library disabled (3 of 3). The committed baseline
  without any Phase 5B.2 change (`364a013`, built separately in Release) was
  run alternately with this build: both failed 5 of 5, each 4 times at Ctrl+T
  (2 tabs instead of 3) and once at the preserved address. The failure is the
  desktop's foreground window, not this change; it also passed alone on
  reruns in earlier phases when the window did get focus. No browser test
  assertion was changed.

**Remaining for live research integration:** a measured telemetry origin
and source with per-link delay, loss and utilization (the runtime's loopback
graph is configured relay data without utilization, so A*, ACO and GA refuse
on it and every browser recommendation today is keep from Dijkstra alone);
real probe tournaments (10 × 5 × 8, interleaved) from a probe scheduler with
ICMP steering (Phase 7 testbed); the triggers (loss, utilization, RTT,
90 s timer, manual) and 15 s cooldown instead of the manual request only;
persistence of research records; approval and a policy before any research
recommendation may reach the runtime's activation path (Phase 5C), with the
production LightGBM provider and arbitration isolated or retired.

### Phase 5B.3: research records and demonstration readiness

**Storage audit.** The browser stored no research data before this phase:
`ResearchRuntimeAdapter` keeps a bounded in-memory history (lost at exit),
`QSettings` holds only the window and Developer Tools layout, and the
telemetry JSONL export (`--telemetry-dir`, off by default) is a redacted,
size-bounded event log, not a record store. A
small store was therefore added.

**Architecture changes.**

| Point | Change |
|---|---|
| `research/record_store.{h,cpp}` (new) | `ResearchRecordStore`: plain C++17, thread-safe, no Qt. JSON Lines file, schema `aafr.research.record_store.v1`, one line per record, oldest first: `{"schema":…,"session":"<id>","sequence":n,"record":<decision record>}`. `<decision record>` is the `aafr.research.decision_record.v1` JSON exactly as `ToJson()` produced it, embedded byte-for-byte; it is never re-serialized or re-evaluated. Every write replaces the whole file (temporary file, then rename) and keeps at most `capacity` records, dropping the oldest. |
| `research/research_host.{h,cpp}` | `SetRecordStore(path, capacity, session)` (before `Start()` only). The worker loads the store after the adapter and appends each produced recommendation; a failed write is added to that request's outcome ("not stored: …") and to the store status, and the evaluation itself still completes. The snapshot adds the store status, the stored records (this and earlier sessions) and `kMeasuredTelemetryAvailable = false`. |
| `BrowserWindow` | Configures the store at `QStandardPaths::AppDataLocation/research/research_records.jsonl`, capacity 200, session id `<UTC start time>-<process id>`. |
| `DevToolsPanel` (Runtime → Research pipeline) | Two read-only rows (**Record store**, **Measured telemetry**), a **Session** column ("this session" / "earlier: <id>") in the recommendations table, which now lists the stored records, and a read-only **Selected record** box showing the selected row's original record JSON. No tab, control or layout of the existing views was changed. |

**Persistence behaviour.**

- A missing file is an empty store. A line is accepted only when its envelope
  schema, session, sequence and embedded record schema are valid and its
  telemetry and probe origins are `controlled`, `recorded` or `synthetic`;
  there is no measured or live origin. Any other line (bad JSON, other
  schema, origin `live`, a truncated line) is skipped and counted, never
  repaired, and the count is shown.
- A record is stored only if its line can be read back. Source labels
  (origin and description of telemetry and probes), the model source commit,
  the installed and selected paths and all calculations stay as computed,
  because the original record bytes are kept.
- The store holds no timing data, so the same inputs give the same stored
  record; session and sequence are outside the record.
- Nothing reads the store except Developer Tools; it never feeds the runtime,
  the forwarding service or a later evaluation.

**Reporting.** Developer Tools state "Measured telemetry: none: no research
input in this build is measured live telemetry (controlled, recorded or
synthetic only; host capture is not per-link telemetry)". The existing
unavailable-model state, input errors and the "utilization not measured"
telemetry label are unchanged and also appear in stored records.

**Keyboard-focus test.** `browser_interface::windowNavigationAndShortcuts`
showed the window without waiting for it to become active, then relied on
keyboard focus (the preserved address-bar edit and Ctrl+T / Ctrl+W / Ctrl+Tab
go to the active window). The case now activates the window and waits for it
(`activateWindow()`, `QTest::qWaitForWindowActive`), as the other keyboard
cases in the suite do. No assertion was changed or removed. Before: 0 of 13
Release runs passed; after: 10 of 10.

**Verification (2026-11-19).**

- `research_record_store` (new, 6 cases, 61 checks; needs XGBoost; real
  pipeline records: a switch with seeded synthetic probes, a keep with a
  missing measurement, an undetermined one without probes): each line round
  trips with the record byte-identical and its labels, paths and model commit
  intact; records written by one store are read by a new one (restart) with
  their session, the new session's records following; capacity 3 keeps the
  newest 3, and a reload with capacity 2 reads 3 and keeps 2; of five stored
  lines (bad JSON, other schema, origin `live`, valid, truncated) only the
  valid one is loaded, 4 are counted as skipped; a path that cannot be
  created reports the error and keeps the record in memory; no temporary file
  remains; two runs give identical stored records.
- `research_host` (7 cases, 51 checks): adds a host restart: the first host
  stores its keep recommendation; a second host on the same file shows it
  (session-1, byte-identical, controlled, "utilization not measured") with no
  evaluation of its own, then appends its own record after it; an unwritable
  store gives "not stored:" and the evaluation still completes.
- Browser (`browser_observability`, new cases `researchRecordsSurviveRestart`
  and `researchDevToolsOpenCloseRepeatedly`): a fresh window shows "0 records
  kept … at most 200" with the file path and the measured-telemetry statement;
  after **Evaluate now** the record is listed as "this session", the file has
  one line embedding exactly the JSON shown, and the runtime's path and
  configuration version are unchanged; after the window is destroyed, a new
  window shows "1 records kept (1 from earlier sessions …), 1 read at start, 0
  unreadable", no evaluation of its own, the row as "earlier: <id>", keep,
  "no (decision only)", controlled, no probes, and the identical JSON.
  Developer Tools are opened, switched to the research view, refreshed and
  closed 12 times while pages keep loading, then the window is destroyed with
  the panel open. Each test removes the record file first.
- **Builds (2026-11-19).** Debug and Release build; 48 CTest suites each
  (adds `research_record_store`). All 44 non-GUI suites pass in both builds.
  In the first full run, Release passed all four GUI suites and Debug three:
  Debug `browser_interface` was killed by its 90 s CTest limit while still
  passing (50 of 54 cases done, none failed). That limit dated from V0.2; the
  suite takes 19 s in Release and 95-126 s in Debug, so it is now 300 s (the
  other GUI suites have 480-600 s). No assertion was changed.
- **Repeated GUI runs** (after that change; each suite alone, three times per
  build, no reruns): `browser_interface` 6/6, `browser_observability` 6/6
  (with the four research cases), `browser_protocol` 6/6, `browser_forwarding`
  4/6. The two failures (one Debug, one Release, each the first run) are the
  intermittent failure already recorded for V0.7.4: in Debug,
  `switchingPinsConnectionsAndRecovers` saw 1 switch instead of 2 because the
  production arbitration refused the probe evidence ("serial correlation
  leaves too few effective pairs"). The Release results file was overwritten by
  the later passing runs, so its failing case is not recorded. That case runs
  on the test's own runtime and gateway, without a `BrowserWindow`, so no
  research host or record store exists in it. Its cause is analysed below.

**Verification closure: `browser_forwarding` instability.**

- *Where it fails.* `switchingPinsConnectionsAndRecovers` drives two switches
  (A → B when A congests, B → A when it clears), each with exactly two runtime
  evaluations (`Step`, then a manual "confirm"). With
  `sustain_evaluations = 2`, a switch commits only when **both** evaluations
  find the challenger better. Every failure (8 analysed) was one of those four
  evaluations not doing so: 7 rejected for serial correlation (lag-1
  r = 0.43–0.58, n_eff 3.2–4.8 < 5; the decision is then *retain* or, after
  one valid evaluation, *switch_rejected* with `HYSTERESIS_NOT_SATISFIED`)
  and 1 *no significant difference* (r = 0.378, n_eff = 5.4, ratio CI upper
  bound 1.0015, Holm p = 0.0506). In every rejected campaign all or nearly
  all 12 rounds favoured the challenger; the arbitration refused the
  evidence's independence, not its direction.
- *Cause: the measurement, not the arbitration.* The loopback relays realise
  each configured edge delay with 5 ms `sleep_for` slices. On Windows a
  sleep lasts until the next timer tick: about 15.6 ms at the default timer
  resolution and 5-6 ms at 1 ms (measured: 15.53 ms vs 5.60 ms for a 5 ms
  sleep). The probe RTTs are therefore whole numbers of ticks plus overhead
  (recorded values cluster at 47, 62, 78, 93, 109 ms) instead of the
  configured 10 ms (A) and 50 ms (B). Which tick count a probe gets depends
  on the phase of the timer when it starts; consecutive rounds run back to
  back, so the phase, and the tick count, persists for several rounds. The
  log-ratio d then moves in runs between a few levels, which is serial
  correlation by construction. Changes of the timer resolution during a
  campaign (Chromium raises it in the same process; seen in 59 of 122
  recorded campaigns) add level shifts; they make rejection more likely but
  are not required (4 of the 7 rejected campaigns had no change). A standalone
  diagnostic with the production probe engine and arbitration on real relays
  reproduced it without Qt: rejected campaigns 0/30 at a fixed default
  resolution, 1/30 at a fixed 1 ms, 3/30 when the resolution toggled.
- *Not the cause.* Not the research pipeline or record store (that case has
  neither; the committed baseline `ac56c23`, without any Phase 5B.3 change,
  built separately and run alternately with this build, failed 1 of 30
  against 2 of 30, Fisher p = 1.0; the production forwarding, runtime and
  arbitration sources are identical in both). Not concurrency or a runtime
  defect: every failing decision is internally consistent (gates, reason
  codes and statistics agree), no switch was ever made on invalid evidence,
  and the recorded rounds reproduce the reported r1 and n_eff exactly.
- *Rates.* This case alone: HEAD 6 of 60 runs failed (Debug 2/30, Release
  4/30), baseline 1 of 30. With all campaigns recorded, 2-3 % of campaigns
  are not "challenger better"; four consecutive required gives a predicted
  8-13 % of runs failing, consistent with the 10 % observed.
- *Test expectation.* The case assumes that every one of four consecutive
  evaluations produces valid, significant evidence. On this host that holds
  for about 90 % of runs; when it does not, the runtime correctly retains and
  would succeed at a later evaluation (its periodic scheduler re-evaluates).
  The expectation that the switch commits within exactly two evaluations is
  therefore too strict for timer-quantized measurements.
- *Correction (approved and applied; test harness only).* Each stage
  (A → B, B → A) now evaluates as the runtime's scheduler would keep doing:
  the change-triggered evaluation, then manual evaluations 150 ms apart, at
  most 6, stopping at the committed switch. Only three outcomes let a stage
  continue: *retain* with `invalid_evidence` (`STATISTICAL_EVIDENCE_INVALID`)
  or `no_significant_difference` (`STATISTICAL_EVIDENCE_INSUFFICIENT`) for
  the expected challenger with no gate assessed, and *switch_rejected* with
  valid `challenger_better` evidence where hysteresis is the only blocking
  gate. The runtime's own rule decides the sustain count (a retain resets
  its streak, an assessed proposal adds one); the test predicts that streak
  and requires the runtime's reported "preferred in k of 2" to match it,
  and the committing switch to report k = 2 with every gate passed, a
  confirmed activation on the expected path, the configuration version
  advanced by exactly one and the switch counter by exactly one. Any other
  decision type or reason, a second decision per evaluation, or any change of
  path, version or switch counter before the switch fails at once;
  exhausting 6 evaluations fails. Every evaluation is logged (decision,
  evidence and its summary, reasons, hysteresis state, activation, confirmed
  path and version, and the campaign's probe rounds with durations and the
  timer setting); a failure also logs the stage trail and the full decision
  record. A negative control (`AAFR_TEST_MAX_STAGE_EVALUATIONS=1`, which can
  only lower the limit) fails as it must: the runtime does not switch on a
  single qualifying evaluation. The runtime, arbitration, thresholds, probe
  engine, forwarding executor and research pipeline are unchanged.
- *Corrected results (2026-11-20; the case alone, Release and Debug
  alternating, one results file per run, none overwritten).* Release 30/30,
  Debug 30/30. Stages needing more than two evaluations: Release 1 of 60
  (B → A, 3 evaluations), Debug 3 of 60 (A → B, 4 evaluations); each was
  preceded by a *retain* with `invalid_evidence`, after which the runtime
  reported "preferred in 1 of 2" again, as its rule requires. Before the
  correction the same case failed 6 of 60 (HEAD) and 1 of 30 (baseline);
  the 4 extended stages are the runs the old two-evaluation expectation
  would have failed (4 of 60). No evaluation exhausted the limit; the
  observed reliability is 60 of 60, not a proven failure rate.
- *Diagnostics kept.* The case now records, for every run, each decision of
  each switch step (type, verdict, assumption summary) and the 48 latest
  runtime requests with their durations and the system timer resolution
  (`RecordingExecutor`, a pass-through around the loopback executor that
  never alters a result); on a failed switch it also prints the full
  decision record.

**Record store with several browser instances.** The first 5B.3 store kept
records in memory and rewrote the whole file, so two browser instances on one
file overwrote each other's records. Each read and write now holds an
exclusive cross-process lock on `research_records.jsonl.lock` (released by
the operating system if the holder exits), and each write re-reads the file,
keeps every record in it, adds this session's records that are not there yet
(a record is identified by session and sequence; a duplicate is not stored
again), trims to capacity and replaces the file. Lines this instance already
holds are matched byte-for-byte instead of being parsed again. If the lock
cannot be taken within 10 s (the research worker thread waits, never the GUI),
the record stays in memory, is counted as *unsaved* (Developer Tools: "N of
this session not yet written"), the error is shown, and the next successful
write stores it. When a window closes, its research host flushes unsaved
records on its worker thread (lock wait 2 s). **Limitations:** an unsaved
record is lost if the browser crashes before a later write or the shutdown
flush, or if the flush cannot take the lock within 2 s; a written record
survives a crash (the file is replaced whole), but writes are not forced to
disk, so a power loss right after a write may still lose it. Tests:
two stores interleaving writes keep all three records in order; 4 threads × 15
appends and 3 processes × 8 appends to one file lose nothing (60 and 24
records, no duplicates, no temporary file); a held lock gives "busy", keeps
the record and writes it with the next append; a read-only file gives
"cannot replace", leaves the file unchanged and recovers when writable; two
research hosts (two browser windows) evaluating at the same time store all 6
records and leave the runtime's configuration, switches and executor requests
unchanged. With the store locked by a third instance, two windows each keep
their record unsaved (request "not stored: record store busy", nothing on
disk) and both records are in the file after the windows close. A browser
process whose write found the store busy writes its record when it exits
normally and loses it when it crashes (`_Exit`, no flush), as documented.
(`research_record_store` 12 cases, 184 checks; `research_host` 9 cases.)

**Combined-tree integration corrections.** A shared bounded store now merges
only unwritten records from its own session; it does not resurrect records
another instance already trimmed. Its unsaved count includes only records
still retained within capacity. Each browser window has a UUID in its research
session ID, so windows created in the same process and second do not collide.
Research observability publishes the last completed evaluation and persistence
snapshot together, with a separate pending flag, so Developer Tools does not
wait on a worker's cross-process file lock. Shutdown still flushes with the
existing 2 s lock wait. Regression coverage includes shared-capacity trimming,
bounded unsaved counts, responsive snapshots under lock contention, two actual
browser windows storing distinct records, and the unsaved count in the
expandable research diagnostics.

**`research_metaheuristics` timeout.** Debug durations on the build machine
over 16 measurements: 43.7-87.7 s (uncapped: 81.2, 77.2, 87.7 s, all 2731
checks passing); Release about 2 s. Its 60 s limit was therefore exceeded
without any failing check. Its limit alone is now 240 s; the algorithms,
budgets and assertions are unchanged.

**Final verification (2026-11-20).** Debug and Release build. Non-GUI: 44/44
in each. GUI suites, each alone: Release 4/4; Debug observability, forwarding
and protocol pass. Debug `browser_interface` failed once in
`windowNavigationAndShortcuts`: Windows did not make the test window active
(`qWaitForWindowActive` returned false) while the desktop was in use by
another application. The case requires an active window and now fails at
that point instead of at a later keyboard check; run alone afterwards it
passed (54/54), but that rerun does not remove the dependency on the desktop
foreground.

**Closure runs (2026-11-20).** Debug and Release build. Non-GUI: Release
44/44; Debug 43/44 in the full run, where `research_metaheuristics` (frozen
research code, unchanged since the baseline) reached its 60 s limit while the
machine was busy (the same run took `research_host` 28 s instead of 7 s);
alone it passes 3/3 at 44 s, so its limit leaves little margin in Debug. GUI
suites, each alone: all 8 pass (4 suites × 2 builds).

**Remaining Phase 6 dependencies:** a measured telemetry origin with per-link
delay, loss and utilization (today A*, ACO and GA refuse on the loopback
graph and every browser recommendation is keep from Dijkstra alone); real
interleaved probe tournaments (10 × 5 × 8) from a probe scheduler, which
needs the Phase 7 testbed; the triggers and cooldown instead of the manual
request; the live-host mode itself and the Developer Tools simplification;
and, separately, approval and a policy before any research recommendation may
act (Phase 5C).

### Phase 0 verification (2026-11-19)

### Phase 0 verification (2026-11-19)

- Baseline tag `baseline-v0.7.4` (local, annotated) on commit `1604bbf`.
- Full suites on the unmodified tree: Debug 35/36 and Release 35/36. In both,
  the failure was `browser_interface`: a Debug timeout at its 90 s limit, and
  in Release a focus assertion in `clickedControlsKeepNoFocusRing`. Rerun
  alone on an idle desktop, it passes in both builds (Debug 87.3 s, close to
  its limit; Release 16.1 s). This is the known focus and load sensitivity,
  not a regression.
- New `research_fixture` suite: the six-switch testbed graph is well formed;
  the existing Dijkstra with a delay-only cost picks s1-s2-s6 (4 ms) over
  s1-s3-s6 (10 ms); J gives 16.667 on the worked check; the confidence
  equation with the feasibility rule and the existing Wilson interval
  reproduces the recorded C of all seven recorded tournaments (to 3 decimals)
  and their swap / keep decisions, including the loss-scenario repair and the
  one-sided defect in congestion tournaments 2-4. These are consistency
  checks of the reference data, not tests of an AAFR implementation of the
  method.
- After Phase 0: Debug 37/37 and Release 37/37 CTest suites pass
  (`browser_interface` 79.2 s Debug, 16.4 s Release). No product code changed.

---

## Implementation Status

Research-alignment phases are tracked in [Research Alignment](#research-alignment).

| Milestone | Status |
|---|---|
| V0.2 Qt browser interface | Implemented and automated-test verified; standalone interactive acceptance pending |
| V0.3 network observability and Developer Tools | Implemented and automated-test verified; live TShark capture and standalone interactive use not yet verified |
| V0.4 NetworkState, feature engineering, constraints, capability registry, eligibility | Implemented and automated-test verified; not yet verified against live capture or in a Release build |
| V0.5 controlled forwarding engine, ten algorithm implementations, loopback forwarding, benchmarking | Implemented and automated-test verified (Debug build); browser traffic is not routed through it; Release-build measurements and interactive use pending |
| V0.6 adaptive runtime: change detection, deterministic recommendation, controlled probing, statistical arbitration, stability gates, confirmed switching, recovery, DecisionRecords | Implemented and automated-test verified (Debug build), including live loopback switching; no ML; browser page loads are not routed through it; Release build and interactive use by a person not verified |
| V0.7.1 external runtime integration: reference audit, measurement-led review (bidirectional recovery when only measurements change), stale-recovery reporting, probe/scoring/scenario regression suites | Implemented and automated-test verified (Debug build, simulated executor for the new scenarios; live loopback suite unchanged and passing); no code copied from the unlicensed reference; no ML |
| V0.7.2 AI expert model integration: persistent local Python worker (pinned artifacts), feature adapter, label mapping, `AIRecommendationProvider` (deterministic / shadow / AI preferred), fallback, stale protection, DecisionRecord and Developer Tools | Implemented and automated-test verified (Debug build, 28/28 CTest suites) with the real model, including a live loopback end-to-end switch; no retraining; the model covers 5 of 10 algorithms, lacks an AAFR source for 23 of its 35 inputs and nearly always answers Dijkstra on the 4-node topology; off by default in the browser. Details: [`expert_model/README.md`](expert_model/README.md#v072-ai-expert-model-integration) |
| V0.7.3 real browser forwarding: loopback-only HTTP/CONNECT forwarding gateway, byte-stream tunnels through the V0.5 relays, connection pinning to the runtime's confirmed configuration, draining, relay-failure recovery, Developer Tools, controlled experiments | Implemented and automated-test verified in **Debug and Release (33/33 CTest suites each)** with real Qt WebEngine traffic over relay-confirmed paths: HTTP/1.1, HTTPS (TLS validation intact), **HTTP/2 (ALPN h2, concurrent streams)** and **WebSocket (ws/wss)**, including pinning of a long-lived page and a WebSocket across an A→B switch and B→A recovery; Release performance measured (configured delays, one workstation); direct mode stays the default; HTTP/3/QUIC excluded; the expert model still lacks measured sources for 23 inputs. Details: [`browser/qt_webengine/README.md`](browser/qt_webengine/README.md#v073-real-browser-forwarding) |
| V0.7.4 real network feature acquisition: provenance-tagged gateway/relay/runtime/graph observations in NetworkState (`fwd.*`, schema 2), per-feature freshness and sample rules, 35-feature acquisition contract (12 direct, 3 derived, 12 estimate-only, 5 unavailable, 3 incompatible), training-distribution profile and compatibility check, explicit model abstention, generation-aware change detection, Developer Tools diagnostics | Implemented; **verification not closed** (2026-11-19). Last full runs: Release 35/36 (`measurement_pipeline` failed on a noise-sensitive experiment threshold, since strengthened: 18/18 targeted passes), Debug 35/36 (`browser_interface` timeout at its 90 s limit; 54/54 in 86 s on rerun). The existing V0.7.3 case `browser_forwarding::switchingPinsConnectionsAndRecovers` fails intermittently in Release (3/12 standalone with V0.7.4, 0/14 with an unmodified V0.7.3 build, Fisher p ≈ 0.085; arbitration refused serially correlated probe evidence; cause not identified). New suites `forwarding_measurement`, `model_compatibility` and the real Qt WebEngine case `realPageLoadsReachNetworkState` pass in both builds; no retraining. **The existing model cannot make valid recommendations from actual AAFR measurements**: only 15 of 35 inputs have a measured source, the 4-node graph is outside the 20–650-node training support, and inference sets the scenario one-hots to 0 (never seen in training); with measured inputs it abstains and the runtime adapts deterministically. Details: [`expert_model/README.md`](expert_model/README.md#v074-real-network-feature-acquisition-and-model-compatibility) |

V0.3 adds browser event capture with stable tab and navigation ids, an optional
TShark adapter (off by default), a typed and versioned telemetry pipeline with
opt-in, privacy-redacted JSONL output, a native Developer Tools panel
(Ctrl+Shift+M), and read-only data contracts for future runtime state and
`DecisionRecord`s. No model, algorithm selection or switching is implemented;
the panel's Runtime and Decisions tabs show "Not available — runtime not
integrated". Details, limitations and the verification record are in
[`browser/qt_webengine/README.md`](browser/qt_webengine/README.md#v03-observability-and-developer-tools).

V0.4 converts that telemetry into a versioned `NetworkState` (25 features
per configurable rolling window, each with a unit, a measured/derived kind and
a quality), a `NetworkConstraints` set that keeps measured, configured,
derived and unknown values apart, capability profiles for all ten algorithms,
and a deterministic eligibility evaluation with stable reason codes. **None
of the ten algorithms is implemented**, so every one is reported as
`not_implemented`, and the registry refuses an "executable" claim without
measured evidence. Eligibility is a diagnostic: it is not a performance
prediction, and nothing is selected or switched. The schema, feature
definitions, rules, reason codes, limitations and V0.5 prerequisites are in
[`browser/qt_webengine/README.md`](browser/qt_webengine/README.md#v04-networkstate-constraints-and-algorithm-eligibility).

V0.5 defines the forwarding problem on a controlled, versioned graph of
AAFR-owned relays (every value carries its provenance), one additive
objective shared by all algorithms, and a single path validator. **All ten
algorithms are implemented** behind one interface: Dijkstra, A* and
Bellman-Ford exactly, and ACO, PSO, GA, ABC, GWO, Firefly and WOA as
documented discrete adaptations (random-key decoding for the continuous
ones). Paths are forwarded for real through TCP relays on 127.0.0.1 and
counted as used only when node evidence confirms them. Each implementation
is verified on fixtures and through real forwarding before the capability
registry marks it `verified_executable`. Algorithms run only when started by
hand from Developer Tools; nothing is chosen or switched automatically, and
browser page loads do not pass through the relays. `aafr-benchmark` exports
reproducible JSONL and CSV results over twelve simulated scenarios. Details
are in
[`browser/qt_webengine/README.md`](browser/qt_webengine/README.md#v05-controlled-forwarding-engine-and-algorithms).

V0.6 adds `AAFRRuntime` (`runtime/adaptive/`), the central orchestrator. It
detects meaningful changes in the NetworkState and the controlled graph,
ranks every eligible algorithm's validated path with a deterministic,
replaceable recommendation provider, measures the active path against up to
two challengers with interleaved probes over the real relays, and judges the
difference statistically (paired log-latency test or bootstrap, Holm,
Wilson intervals for failures, explicit insufficient or invalid evidence).
Switching passes dwell, cooldown, backoff, margin and hysteresis gates
(bypassed only by documented safety overrides) and is committed only after
the executor confirms the new path; failures keep or re-establish a
confirmed configuration or enter a degraded state. Every evaluation leaves a
DecisionRecord with stable reason codes, and later outcomes are linked by
revision. The runtime switches in both directions and never locks onto one
algorithm. It contains no ML: the Expert Model plugs in as another provider
in V0.7. Details, methodology, tests and measurements are in
[`browser/qt_webengine/README.md`](browser/qt_webengine/README.md#v06-adaptive-runtime).

V0.7.1 audits an external adaptive-runtime reference (no license: concepts
only, nothing copied) against V0.6. Most of its lessons were already
covered by V0.6; the one real gap was recovery when only measurements
change. The runtime now runs a measurement-led review every 30 s that
probes the incumbent against the best alternative even when the graph
objective prefers the incumbent, so it leaves a path that degrades only in
measurement and returns once it recovers, through the usual gates. The
audit, compatibility matrix and test results are in
[`runtime/README.md`](runtime/README.md#v071-external-runtime-integration),
the dataset assessment in
[`datasets/README.md`](datasets/README.md#v071-external-dataset-assessment)
and V0.7.2 readiness in
[`expert_model/README.md`](expert_model/README.md#v072-readiness-after-v071).

V0.7.3 routes real browser traffic through AAFR. Run the browser with
`--forwarding=controlled` and Qt WebEngine is pointed, through Chromium's
supported `--proxy-server` switch set before Qt starts, at
`AAFRBrowserForwardingGateway`. This is a local HTTP/CONNECT proxy bound to
127.0.0.1 and restricted to the browser's own process. For every browser
connection it reads the runtime's **confirmed** active configuration and pins
the connection to it. It then opens a byte-stream tunnel along that path
through the existing V0.5 relays (a new `AAFR1 TUN` mode, confirmed by the
relays' own trace and logs exactly like a request), and only then relays the
bytes. HTTPS stays end to end and the browser validates certificates as
usual. New connections follow switches, existing ones finish on the path
they started on, relay failures seen by browser connections trigger the
runtime's recovery, and without a confirmed configuration the gateway answers
503 instead of going direct. `aafr-browser-experiment` compares direct and
forwarded page loads and exercises switching, recovery, relay failure and
model shadow mode. Details:
[`browser/qt_webengine/README.md`](browser/qt_webengine/README.md#v073-real-browser-forwarding);
model feature readiness:
[`expert_model/README.md`](expert_model/README.md#v073-feature-readiness-from-real-browser-forwarding).

---

## Repository Structure

```text
AAFR/
├── algorithms/
├── browser/
│   ├── qt_webengine/
│   │   ├── main.cpp
│   │   ├── browser_window.h/.cpp
│   │   ├── tab_manager.h/.cpp
│   │   ├── browser_theme.h/.cpp
│   │   ├── browser_event_collector.h/.cpp
│   │   ├── devtools_panel.h/.cpp
│   │   └── README.md
│   └── README.md
├── configs/
│   └── policy_registry.yaml
├── datasets/
│   ├── processed/
│   └── raw/
├── docs/
├── experiments/
├── expert_model/
├── network/
│   ├── monitor/        # NetworkMonitor interface
│   ├── telemetry/      # Telemetry events, pipeline, JSONL writer
│   └── tshark/         # TShark field parsing and QProcess adapter
├── routing/
├── runtime/
│   └── observability/  # Future runtime observability contracts
├── tests/
│   └── unit/
├── tools/
├── .gitignore
├── CMakeLists.txt
├── CMakePresets.json
├── LICENSE
├── README.md
└── vcpkg.json
```

Existing implementation files and component documentation live within these directories.
---

## Development Flow

AAFR is developed incrementally rather than implementing the complete system at once.

```text
Dataset Audit
      ↓
Target Analysis
      ↓
Feature Audit
      ↓
NetworkState Definition
      ↓
Routing Algorithms
      ↓
Algorithm Validation
      ↓
Expert Model Training
      ↓
Expert Model Evaluation
      ↓
Runtime Inference
      ↓
Network Monitoring
      ↓
AAFR Runtime
      ↓
Stability Logic
      ↓
DecisionRecord
      ↓
Controlled Routing Environment
      ↓
Qt Browser Integration
      ↓
End-to-End Integration
      ↓
Baseline Experiments
      ↓
AAFR Experiments
      ↓
Results and Analysis
```

---

## Experimental Scenarios

Experiments cover the seven network-condition families:

```text
Excellent
Congested
Unreliable
Sparse
Dense
Dynamic
Large
```

Dynamic experiments may transition between conditions.

Example:

```text
Excellent
    ↓
Congested
    ↓
Unreliable
    ↓
Excellent
```

This allows AAFR's adaptive behaviour to be evaluated.

---

## Evaluation

The complete system is evaluated at three levels.

### Expert Model

Model-selection quality, class-level performance, inference latency, and confidence quality.

### Routing and Network

Metrics such as:

- Objective
- Latency
- Loss
- Throughput / bottleneck bandwidth
- Path delay variation
- Hop count
- Algorithm runtime

### AAFR and Browser

Metrics such as:

- Algorithm switches
- Rejected switches
- Unnecessary switches
- Switching overhead
- Runtime stability
- Request completion
- Request failures
- Resource loading performance
- Browser loading performance

---

## Baseline Comparison

The main experimental comparison is:

```text
Fixed / Baseline Routing
           VS
      Adaptive AAFR
```

The project does not assume that adaptive routing will automatically outperform fixed routing.

Performance improvement must be demonstrated experimentally.

---

## Documentation

Detailed project documentation is available in:

### `PROJECT.md`

Defines:

- Research problem
- Research objective
- Core capabilities
- Datasets
- Expert Model
- Runtime behaviour
- Evaluation approach

### `REQUIREMENTS.md`

Defines:

- Functional requirements
- AI requirements
- Network requirements
- Routing requirements
- Runtime requirements
- Stability requirements
- Testing requirements
- Experimental requirements

### `ARCHITECTURE.md`

Defines:

- System architecture
- Architectural layers
- Component responsibilities
- Component communication
- Runtime flow
- Routing architecture
- Codebase architecture

---

## Current Research Decisions

Several parameters are intentionally not hard-coded before experimentation.

These include:

- Exact benchmark objective definition
- Target-generation and tie-breaking procedure
- Expert Model target selection
- Final model feature set
- Final NetworkState fields
- Confidence calibration
- Confidence switching threshold
- Meaningful network-change criteria
- Monitoring-window duration
- Minimum dwell time
- Exact controlled routing environment
- Final baseline configuration

These decisions are finalized through dataset analysis, methodology validation, or controlled experiments rather than arbitrary assumptions.

---

## Project Principles

AAFR follows a simple continuous cycle:

```text
Observe
→ Represent
→ Predict
→ Stabilize
→ Continue / Switch
→ Route
→ Measure
→ Repeat
```

Each component has one primary responsibility:

```text
Network Monitor
→ Observes

NetworkState
→ Represents

Expert Model
→ Recommends

AAFR
→ Controls the transition

Routing Algorithm
→ Calculates the path

Routing Controller
→ Applies the path

DecisionRecord
→ Explains

Qt Browser
→ Uses the network

TShark
→ Provides packet-level observations
```

The central principle is:

> **The AI Expert Model recommends which routing algorithm is most suitable for the current network condition. AAFR manages when and how that recommendation is applied at runtime.**
