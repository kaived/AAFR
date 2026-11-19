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
| 2 | Probe tournament model, dual ANOVA, Holm, Wilson feasibility, confidence C and the decision as a reference implementation | Implemented and verified 2026-11-19 as a standalone library; not connected to probing, the runtime or the browser |
| 3 | Four-algorithm pool: Dijkstra, A*, ACO, GA as specified | Partly implemented 2026-11-19 (`research/path_algorithms`): Dijkstra on start-up delays, A* on predicted link weights, candidate grouping into arms. ACO and GA are declared and report *not implemented*: their operators are not defined by the specification (decision pending). Not connected to the runtime |
| 4 | XGBoost link-weight regressor through the C API | Not started; no compatible artifact exists |
| 5 | Runtime rewired to the tournament and decision rule | Not started |
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
decided: ACO and GA operators (pheromone exponents, evaporation, crossover,
mutation, selection); how a frame is aggregated when some of its blocks are
invalid; which training-set columns give the z-statistics of RTT, B and T in
G; the units of the regressor inputs (the dataset's `throughput` is a proxy
for bandwidth); whether the regressor is retrained from
`routing_expert_dataset.csv` (the original artifact is not available); the
future of the six algorithms and the LightGBM model outside the pool; whether
the V0.7.3 browser gateway and loopback relays stay as a labelled loopback
demonstration; and where the Linux testbed runs.

Phases 1 and 2 add these, also left open by the specification and not
assumed by the code (each is reported as undetermined or must be declared by
the caller):

- **Frames with invalid blocks.** A frame value is computed only when all 5
  blocks are valid. A frame with an invalid block has no value, and a
  challenger whose incumbent or own arm has such a frame gets no ANOVA, so the
  tournament verdict is *undetermined*. Candidate rules (drop the frame, mean
  of the valid blocks, mean of the returned echoes) need a decision.
- **Choice among several qualifying challengers.** When more than one
  challenger meets C, Holm p and dwell, the verdict is *undetermined*.
- **Standard deviation of the frozen statistics.** The contract stores the
  delta degrees of freedom (0 or 1) declared by the producer; there is no
  default.
- **Training-set columns for the z-statistics**, and the file format that
  will carry them with the regressor artifact (Phase 4).

Readings adopted where the specification's wording settles it:

- The frame value is the mean of its block means (the specification defines
  block means and their variance within a frame); block and frame variances
  use ddof 1.
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
| `research/arbitration_reference.h` | `RepeatedMeasuresAnova` (two arms × 10 rounds: SS_arm, SS_round, SS_error, F(1, 9), p, partial η², 95 % CI, RTT ratio); `FSurvival`; `HolmAdjusted`; `EvaluateFeasibility`; `ComputeConfidence`; `Decide`; `ArbitrateTournament` (verdict *keep*, *switch* or *undetermined*, with the reason) |

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
  tournaments, the one-sided limitation, and end-to-end verdicts.
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
- `AntColonyOptimization` and `GeneticAlgorithm` with the specified budgets
  (`AntColonyParameters`, `GeneticParameters`); they return `NotImplemented`.
- `RunPool` (the four, in a fixed order), `EvaluatePath` (J and J_eng on live
  observations, separate from each algorithm's optimization cost),
  `DistinctChallengers` (one arm per distinct path, labelled with every
  algorithm that returned it; the installed path is never an arm).

**Unresolved ACO and GA choices.** The specification fixes only the
objective (J_eng) and the budgets. Each choice below changes which path is
found on graphs with several near-optimal paths, how often the J_eng optimum
is reached and the run time; none is decided by the code.

| | Choice | Why it matters |
|---|---|---|
| ACO | Edge desirability η(e) | J_eng is a path-level, z-scored, non-additive cost with no per-edge decomposition; η = 1/delay, 1/W_link or none give different searches |
| ACO | Pheromone deposit | J_eng can be negative (z-scores), so the usual 1/J deposit is undefined; it needs a rank-based or shifted rule |
| ACO | α, β, evaporation ρ, initial pheromone, bounds, elitist reinforcement | Control exploration against convergence within 30 iterations |
| ACO / GA | Seed and random generator | Both are randomized; reproducible results need a declared seed policy |
| GA | Encoding and initialization | Node-sequence chromosomes (as in production) or random keys; how initial paths are drawn |
| GA | Selection | Fitness-proportional selection is undefined for a signed J_eng; tournament size or rank scheme needed |
| GA | Crossover and mutation operators and rates; loop repair; elitism; duplicates | Decide reachable paths and convergence in 40 generations |
| A* | Tie-breaking among equal-cost paths | When every W_link is 0 (the documented regressor limitation) all paths tie; the result then depends only on the tie rule (node order here, as in production) |

A practical option is to adopt the production ACO / GA operators unchanged
(with the specified ant / population counts and iteration budgets, and a
rank- or shift-based treatment of signed J_eng), recorded as AAFR
implementation choices rather than as part of the specification. That needs
approval.

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
  J_eng optimum on a congested testbed differs from the static Dijkstra path:
  the reference a future ACO / GA must reach. Without statistics J_eng is
  unavailable and J is still defined.
- **Builds (2026-11-19).** Debug and Release build; 41 CTest suites each. All
  37 non-GUI suites pass in both builds. GUI suites were run one at a time:
  all four pass in Release (15.2 s, 31.9 s, 54.3 s, 62.8 s). In Debug,
  observability (74.0 s) and forwarding (69.4 s) passed; interface
  (`lifecycleAndTabIsolation`, a page-load signal wait) and protocol
  (`wssBidirectionalMessagingAndTraversal`, a view wait) failed once and both
  passed on an immediate rerun (74.7 s, 77.4 s). Those Debug binaries were
  not rebuilt for Phase 3 and had passed alone earlier the same day, so this
  is Debug Qt WebEngine timing instability, recorded here rather than hidden.

**Remaining dependencies for the link-weight regressor (Phase 4).** A* and
J_eng need, and Phase 3 does not provide: an installed XGBoost runtime and C
API library (new dependency, approval pending); a regressor artifact (the
original is not available; retraining from `routing_expert_dataset.csv` to
the specified settings needs approval); the input contract (units of delay,
loss and 1/B, the dataset's `throughput` as the bandwidth proxy, feature
order); the frozen statistics for the regressor inputs and for J_eng / G (the
column mapping and ddof are open); the floor at 0 and its consequence for A*;
and per-link inputs at run time, which on the testbed come from per-link ICMP
probes and OpenFlow counters (Phase 7), not from host capture.

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
