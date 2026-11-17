# Expert Model

Feature extraction, training, evaluation, inference, and persisted model
artifacts for the expert decision model belong here.

## AI model integration readiness (V0.5 assessment)

The Expert Model was developed separately. It is **not** in this
repository, AAFR does not load or call it, and V0.5 changed nothing about
it. Integration is V0.7 work. This section records what was inspected and
what an adapter will need.

### Where the model is and what was inspected

The artifacts were found outside the repository, in a local copy of the
separate project (`adaptive-routing-release/`). They were read, not copied
or modified:

| Item | Content |
|---|---|
| `models/stage1_gate.joblib`, `stage2_selector.joblib`, `stage2_label_encoder.joblib` | LightGBM classifiers and a scikit-learn `LabelEncoder` (binary; not opened, only loaded by `inference.py`) |
| `models/model_metadata.json` | 55 ordered `feature_columns`, `optimal_threshold` 0.51, stage metrics, `stage2_class_names` |
| `ml/inference.py` | `RoutingAlgorithmSelector` with `predict`, `predict_with_confidence`, `predict_batch` |
| `ml/features.py` | the 35 raw inputs, 13 engineered features, 7 scenario one-hots |
| `ml/train.py` | trains through `features.prepare_dataset`; SMOTE inside stratified CV; LightGBM |
| `routing/evaluator.py`, `data/feature_metadata.json` | the objective the labels were derived from; value ranges |

### Prediction semantics

- **Stage 1 (gate):** `P = stage1.predict_proba(X)[0, 1]`, the probability
  that a metaheuristic is needed. If `P < 0.51` the answer is `"Dijkstra"`.
- **Stage 2 (selector):** otherwise the selector's class is decoded by the
  label encoder; the classes are `ACO`, `GWO`, `PSO`, `WOA`. Without stage 2
  the answer falls back to `"Dijkstra"`.
- `predict` returns a string; `predict_with_confidence` adds
  `gate_confidence`, `threshold`, per-class probabilities (stage 2 only) and
  a reasoning text.
- Reported metrics: stage 1 accuracy 0.43, F1 0.27, ROC AUC 0.67 (recall
  0.81 at the chosen threshold); stage 2 accuracy 0.37, macro F1 0.35. The
  metadata also reports an end-to-end selection accuracy of 0.976 and a mean
  objective improvement of 0.66 % over always-Dijkstra; how that figure is
  computed was not re-derived here.

### Algorithm label compatibility

| Model output | AAFR `AlgorithmId` | Note |
|---|---|---|
| `Dijkstra` | `dijkstra` | case differs |
| `ACO`, `GWO`, `PSO`, `WOA` | `aco`, `gwo`, `pso`, `woa` | case differs |
| (never produced) | `astar`, `bellman_ford`, `ga`, `abc`, `firefly` | the model cannot recommend these |

A recommendation is not a decision: the V0.4 eligibility checks still apply,
and an ineligible recommendation must be rejected with its reason codes.

### Input features against the V0.4 NetworkState

The model expects 35 raw features (`RAW_INPUT_FEATURES`). Status for AAFR:

| Model group | Features | Source in AAFR | Status |
|---|---|---|---|
| Topology (7) | `num_nodes`, `num_edges`, `network_density`, `avg_node_degree`, `diameter`, `avg_clustering_coefficient`, `algebraic_connectivity` | the controlled `ForwardingGraph`, not NetworkState | derivable; algebraic connectivity needs a Laplacian eigenvalue solver (not implemented) |
| Request (5) | `req_shortest_hops`, `source_in/out_degree`, `dest_in/out_degree` | the `ForwardingProblem` and graph | derivable |
| Latency (1) | `latency` | NetworkState has `rtt.handshake.*_ms` (host-wide TCP handshake RTT) | **semantic mismatch**: the model's latency is network/path latency in its simulator (2.7-135 ms in training); AAFR's is host-wide and not per path |
| Jitter (1) | `jitter` | `ack_delay.jitter_ms`, `rtt.handshake.variation_ms` | **semantic mismatch** (ACK-delay jitter includes the host's own ACKs) |
| Loss (1) | `loss` (ratio 0-0.72) | `tcp.retransmission_ratio` | **not equivalent**: a retransmission heuristic, not a loss rate |
| Throughput (1) | `throughput` | `throughput.mean_mbps` | units match (Mbps); host-wide, both directions |
| Utilization and queues (6) | `mean/max_link_utilization`, `link_utilization_std`, `avg_queue_delay_ms`, `max_queue_depth`, `avg_queue_occupancy` | none | **missing**: not observable from a host |
| Load and fairness (3) | `offered_load_mbps`, `packet_delivery_ratio`, `jains_fairness_index` | none (delivery ratio could be estimated from forwarding outcomes) | **missing** |
| Latency distribution (7) | `latency_p50/p90/p95/p99/min/max/std` | `rtt.handshake.median/min/max_ms`, `rtt.handshake.variation_ms` | partial: no p90/p95/p99; same semantic mismatch as `latency` |
| Dynamics (3) | `link_failure_events`, `link_recovery_events`, `reroute_count` | the graph's change history | derivable for the controlled topology only |

Engineered features (13) are computed from the raw ones by
`engineer_features`; they need no separate data but inherit every gap above.

### Issues an adapter must handle

1. **Scenario one-hots.** Training builds `cat_A_Excellent` ...
   `cat_G_Large` from the dataset's `scenario_category`, so every training
   row has exactly one set. At inference `engineer_features` sets all seven
   to 0 unless a `scenario_category` is supplied, a combination never seen
   in training. AAFR has no such category.
2. **Unknown becomes zero.** `inference.py` replaces NaN and infinity with
   0.0. AAFR's rule is that unknown is never zero; the adapter must refuse
   (or explicitly flag) a prediction when required inputs are unknown
   instead of passing NaN.
3. **Objective mismatch.** The labels come from
   `J = w_lat L + w_loss Loss + w_bw / B + w_hops H` (weights 1, 100, 100,
   0.5; bottleneck-bandwidth term; loss as `1 - prod(1 - p)`). AAFR's V0.5
   objective uses additive `-ln(1 - p)` loss and treats bandwidth as a hard
   constraint. "Best algorithm" labels are objective-specific.
4. **Distribution shift.** Training data is simulated (3500 scenarios, seven
   categories). AAFR's controlled topology is 4 nodes with configured values;
   its benchmark graphs are 8-152 nodes with a different generator.
5. **Scope.** The model reasons about a routing topology; NetworkState
   describes the host's traffic. Topology and request features must come
   from the controlled graph, telemetry features from per-path measurement
   that does not exist yet.

### Adapter requirements for V0.7

- Run the existing `inference.py` unchanged, out of process (a Python
  worker over a local pipe or loopback socket), so the browser build gains
  no Python dependency and the model files stay where they are.
- Build the 35 raw inputs in C++ from the graph, the problem and the
  NetworkState; send them with per-feature provenance; let `features.py` do
  the engineering. Do not reimplement the 13 engineered features in C++
  unless the Python worker is ruled out.
- Refuse to predict when a required input is unknown; record which.
- Map labels to `AlgorithmId`, keep gate probability, class probabilities
  and threshold, and pass the recommendation through V0.4 eligibility.
- Log every recommendation with its inputs, NetworkState id and graph
  version, and keep it apart from the V0.6 decision and its execution.

### Data V0.5 can provide

`aafr-benchmark` exports per-run JSONL and CSV with schema version, scenario
configuration, algorithm configuration, feature definitions, units and
provenance (see the browser README, "Benchmark methodology"). Topology and
request features for those scenarios are derivable from the exported
configurations. They may serve later validation or retraining; V0.5 does not
retrain or alter the model.

## V0.7 model integration contract (prepared in V0.6)

V0.6 runs without any model. It defines the seam a model will plug into and
the rules it must obey; nothing below is implemented yet. The model meant
here is the separately trained two-stage LightGBM selector assessed above,
not the XGBoost link-weight regressor of the external reference repository
(that repository's current code uses XGBoost only to weight links; it has no
algorithm classifier).

### Provider interface

A model is added as one more `aafr::runtime::IRecommendationProvider`
(`runtime/adaptive/recommendation_provider.h`) and injected through
`RuntimeDependencies::provider`. The runtime calls `Recommend()` on its own
worker thread, once per evaluation:

| Input (`RecommendationInput`) | Meaning |
|---|---|
| `state`, `constraints` | the V0.4 snapshot (may be null: no NetworkState) |
| `graph`, `problem` | the immutable graph version and the problem posed on it |
| `candidates` | one `CandidateEvaluation` per algorithm: eligibility status and reasons, overridden host checks, computed path, validity, feasibility, J(P), computation time |
| `active`, `active_path` | the confirmed configuration, if any |
| `benchmark` | optional offline `BenchmarkEvidence` |
| `context` | evaluation id, trigger, NetworkState id, graph version |

| Output (`Recommendation`) | Rule for a model provider |
|---|---|
| `status` | `Recommended`, `InsufficientEvidence` (required inputs unknown), `Unavailable`, or `Error` (timeout, worker failure) |
| `recommended`, `ranking` | only algorithms whose candidate is `Usable()`; a model label without a usable candidate is dropped, never forced |
| `provider_id` | `<model name>/<model version>/<feature schema version>` |
| `source`, `evidence` | what the ranking came from; never "measured" |
| `estimated_improvement` | J(P) difference on the same graph and objective version only, or empty |
| `explanation` | gate probability, threshold, class probabilities and the label mapping, as numbers |

A model's probabilities are the model's own scores. They are recorded in the
explanation and are not used as a confidence in any gate.

### Required input features and their quality

The 35 raw inputs listed above split into three groups. An adapter must send
each value with its unit and provenance and must refuse to predict
(`InsufficientEvidence`) when a required one is unknown, instead of passing
0 or NaN:

- Derivable from the graph and problem (topology and request groups,
  dynamics from the graph's version history): available, but computed on
  AAFR's 4-node controlled graph, far outside the training distribution.
- Available with a semantic mismatch (latency, jitter, throughput, latency
  distribution): the NetworkState values are host-wide TCP handshake RTT
  and ACK-delay figures, not per-path latency. V0.6 probe and outcome
  measurements are per path but cover only AAFR's own loopback relays.
- Missing (utilization, queues, offered load, fairness, loss): not
  observable from a host. `tcp.retransmission_ratio` is not a loss rate.

### Labels, versions, timeout, fallback and safety

- Labels: `Dijkstra`, `ACO`, `GWO`, `PSO`, `WOA` map to `dijkstra`, `aco`,
  `gwo`, `pso`, `woa`. The model cannot name the other five algorithms.
- Versioning: every DecisionRecord already carries `provider_id`; the model
  version, feature schema version and `model_metadata.json` hash belong in
  it. A provider whose metadata does not match its expected feature list
  must report `Error`.
- Timeout: inference runs out of process (see "Adapter requirements") and
  must answer within a fixed budget (for example 50 ms). A late answer is
  `Error`, never awaited past the budget.
- Fallback: on `Error`, `InsufficientEvidence` or `Unavailable` the runtime
  uses `DeterministicRecommendationProvider` for that evaluation and records
  both providers' results.
- Safety boundaries, unchanged by any provider: eligibility filtering,
  path validation, controlled probing and statistical arbitration, the
  stability gates, activation confirmation, recovery, and the DecisionRecord.
  A model can change which candidate is proposed first; it cannot activate,
  bypass a gate, or mark anything as measured.

## V0.7.2 readiness after V0.7.1

V0.7.1 integrated no model and retrained nothing. The contract above still
holds; V0.7.1 changes or confirms the following for the adapter.

- **Provider contract unchanged.** `IRecommendationProvider::Recommend()`
  and `RecommendationInput` / `Recommendation` are as described above.
  `context.trigger` may now also be `review` (the measurement-led review).
  A provider must treat it like any other trigger; a review switch still
  needs measured evidence, so a model ranking alone can never cause one.
- **What the model can influence.** The order of candidates and therefore
  which challengers are probed (at most `max_challengers`). Probing,
  arbitration, the gates, confirmation and recovery are unchanged and stay
  outside the provider.
- **Input schema.** The 35 raw features listed above, each with unit,
  provenance and quality. Required quality: `valid`, not stale beyond the
  NetworkState window; otherwise `InsufficientEvidence`. Per-path measured
  inputs, if a model wants them, must come from AAFR probe and outcome data
  (`aafr.measured_forwarding.v1`), never from the reference datasets.
- **Label mapping.** `Dijkstra, ACO, GWO, PSO, WOA` → `dijkstra, aco, gwo,
  pso, woa`; the model cannot name the other five, which stay reachable
  through the deterministic ranking.
- **Timeout and fallback.** Out-of-process inference with a fixed budget
  (for example 50 ms); late answers are `Error`; `Error`,
  `InsufficientEvidence` or `Unavailable` fall back to
  `DeterministicRecommendationProvider` for that evaluation, both results
  recorded.
- **Versioning.** `provider_id = <model>/<model version>/<feature schema
  version>`; a metadata mismatch is `Error`.
- **External model.** The reference repository's XGBoost link-weight
  regressor is not an algorithm selector, is trained on a target derived
  from its inputs, and saturates below 10 ms. It is out of scope for V0.7.2
  (see the dataset assessment in
  [`datasets/README.md`](../datasets/README.md#v071-external-dataset-assessment)).

Open questions for V0.7.2: where the model process runs and how it is
started without an unrestricted process-control API; whether the model
should rank only the five labels it knows or abstain on the others; how
outcome data from reviews is versioned before any retraining is considered.
