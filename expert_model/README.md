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
