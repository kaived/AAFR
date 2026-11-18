# Expert Model

Feature extraction, training, evaluation, inference, and persisted model
artifacts for the expert decision model belong here.

Status: since V0.7.2 the runtime can consult the model through a persistent
local worker (`inference/aafr_model_worker.py`, artifacts pinned in
`models/model_manifest.json`); see
[V0.7.2 AI expert model integration](#v072-ai-expert-model-integration).
The V0.5 and V0.6 sections below are kept as written at the time.

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

## V0.7.2 AI expert model integration

V0.7.2 connects the existing, separately trained two-stage LightGBM selector
to the AAFR runtime. Nothing was retrained, no threshold was changed and no
algorithm implementation was replaced or duplicated. The model **recommends**;
the runtime's eligibility, validation, probing, statistical arbitration,
stability gates, activation confirmation and recovery stay the only
authority. This milestone's integration report, compatibility report and
inference-protocol specification live in this README (the repository keeps
no per-version documents).

### Verified model and artifacts

The artifacts stay outside the repository, in the model project's release
(`adaptive-routing-release/`), configured with `-DAAFR_MODEL_DIR=...` or the
`AAFR_MODEL_DIR` environment variable. In V0.7.2 they were loaded and run,
not only read:

| Artifact | Verified content |
|---|---|
| `models/stage1_gate.joblib` | `lightgbm.sklearn.LGBMClassifier`, binary, 55 inputs |
| `models/stage2_selector.joblib` | `LGBMClassifier`, multiclass, 55 inputs |
| `models/stage2_label_encoder.joblib` | scikit-learn `LabelEncoder`, classes `ACO, GWO, PSO, WOA` (pickled with scikit-learn 1.8.0) |
| `models/model_metadata.json` | timestamp `20261027_220608`, 55 ordered `feature_columns`, `optimal_threshold` 0.5100000000000001, stage metrics |
| `ml/inference.py`, `ml/features.py` | `RoutingAlgorithmSelector.predict_with_confidence`; 35 raw inputs, 13 engineered features, 7 scenario one-hots |

`expert_model/models/model_manifest.json` (committed, small) pins the
SHA-256 of all seven files, the model version (`lgbm2stage-20261027_220608`),
the feature contract (`aafr.model_features.v1`: names, order, units, groups,
training min/max), the stage semantics, the label map and the calibration
status. The worker refuses to import or unpickle anything whose hash differs.

Stage semantics, verified against the training data and code:

- **Stage 1** outputs `P(invoke_metaheuristic = 1)`. In the dataset
  `invoke_metaheuristic` is 1 exactly when some metaheuristic's objective was
  strictly lower than Dijkstra's (`target_algo_eps0 != Dijkstra`; 450 of 3500
  rows). Below the metadata threshold 0.51 (F1-maximised on out-of-fold
  predictions of a SMOTE-trained model) the answer is `Dijkstra`. It is not a
  switch/retain decision and not a quality score.
- **Stage 2** runs only at or above the threshold and returns the argmax of
  four classes trained on `target_algo_eps1` among gated rows; GA, ABC and
  Firefly were too rare and were dropped.
- **Probabilities are uncalibrated.** Calibration was never evaluated and the
  stage-1 model was trained on resampled data. AAFR records them as model
  scores and never uses them in a gate or as evidence.
- Reported training metrics: stage 1 accuracy 0.43, F1 0.27, ROC AUC 0.67;
  stage 2 accuracy 0.37, macro F1 0.35.

Verified environment: Python 3.15.2 in a local, git-ignored `.venv` with
numpy 2.5.5, pandas 3.1.1, scikit-learn 1.9.1, lightgbm 4.7.0, joblib 1.6.0.
scikit-learn 1.8.0 has no wheel for Python 3.15, so the label encoder loads
with an `InconsistentVersionWarning` (shown in the ready message and in
Developer Tools); the worker cross-checks its classes against the manifest.

```bash
py -3.15 -m venv .venv
```

```bash
.venv/Scripts/python.exe -m pip install --only-binary=:all: numpy pandas scikit-learn lightgbm joblib
```

### Feature contract (`aafr.model_features.v1`)

AAFR sends the 35 raw inputs; the worker passes them to the model project's
own `engineer_features` (13 engineered columns + 7 scenario one-hots = 55
columns). AAFR never re-implements that transformation. `ModelFeatureAdapter`
(`runtime/model/model_feature_adapter.*`) builds the vector:

| Group (count) | Features | AAFR source | Status |
|---|---|---|---|
| Topology (7) | `num_nodes`, `num_edges`, `network_density`, `avg_node_degree`, `diameter`, `avg_clustering_coefficient`, `algebraic_connectivity` | the immutable forwarding graph (available nodes and edges) with the formulas of the model's `simulator/topology.py`: undirected projection for density, degree, clustering and diameter (exact up to 100 nodes, 2-sweep BFS above), Fiedler eigenvalue (Jacobi rotation) for 2..150 connected nodes else 0, same rounding | exact |
| Request (5) | `req_shortest_hops` (directed BFS), source/destination in/out degree | graph + `ForwardingProblem` | exact |
| Telemetry (13) | `latency`, `jitter`, `loss`, `throughput`, link utilization x3 (percent), queue x3, `offered_load_mbps`, `packet_delivery_ratio`, `jains_fairness_index` | an `ITelemetryFeatureSource` (measured or configured values with provenance) | **no AAFR measurement source yet** |
| Latency distribution (7) | `latency_p50/p90/p95/p99/min/max/std` | same | **no source yet** |
| Dynamics (3) | `link_failure_events`, `link_recovery_events`, `reroute_count` | same | **no source yet** |

Rules: values must be finite, in the contract unit, physically valid (ratios
in [0, 1], counts integral and nonnegative) and fresh (measured values need
an age of at most 30 s). A feature offered twice, an unknown name, or a
telemetry value for a graph-derived feature makes the vector invalid. Any
missing, stale or invalid feature makes the model **abstain**
(`E_FEATURES_INCOMPLETE`): nothing is sent, and unknown never becomes 0 or
NaN (the model's own `inference.py` would silently turn NaN into 0).
Host-wide NetworkState values (`rtt.handshake.mean/median/min/max_ms`,
`ack_delay.jitter_ms`, `throughput.mean_mbps`) can stand in for six features
only with `allow_network_state_substitutes`, and are then labelled
`substitute`: they measure the host's TCP handshakes, not the simulator's
per-packet network latency.

### Python worker and process model

`expert_model/inference/aafr_model_worker.py` is a persistent local process,
started once and reused for every prediction. It:

1. redirects every stray `stdout` write (Python and native) to `stderr`;
   only protocol messages use the original `stdout`;
2. reads the manifest and verifies the SHA-256 of all pinned files **before**
   importing `ml/` or unpickling any joblib file (joblib files can execute
   code);
3. imports numpy, pandas, scikit-learn, lightgbm, joblib (`E_DEPENDENCY_MISSING`);
4. loads the model through `RoutingAlgorithmSelector.load` (unchanged code);
5. cross-checks raw features, model columns, stage-2 classes, threshold and
   input width against the manifest (`E_METADATA_UNSUPPORTED`);
6. sends `ready` and serves requests until `shutdown` or end of input.

It runs as `python -I -X utf8` (isolated: no `PYTHON*` variables, no user
site), opens no socket, executes nothing a request names and bounds every
input line.

`ModelWorkerClient` (`runtime/model/model_worker_client.*`, plain C++17)
owns it. The runtime is Qt-free and calls providers on its own worker
thread, so the worker is driven through native pipes (`ChildProcess`:
`CreateProcessW` with an explicit inherited-handle list, `CREATE_NO_WINDOW`,
and a Job Object that kills the worker with AAFR and caps each process at
2 GiB) rather than `QProcess`, which is bound to the thread and event loop
that created it. The GUI thread never waits on the worker: `Start()` returns
at once, a reader thread handles `hello`/`ready`, and `Infer()` runs on the
runtime thread with a hard deadline.

| Lifecycle | Behaviour |
|---|---|
| Interpreter | `AAFR_MODEL_PYTHON`, else CMake `AAFR_MODEL_PYTHON` (defaults to `.venv/Scripts/python.exe`); no PATH search |
| Readiness | `ready` validated in C++ against the manifest and `aafr.model_features.v1`; mismatch: `failed` |
| Requests in flight | one; no queue; a concurrent caller gets `busy` |
| Timeout | 500 ms per request (default); 2 consecutive timeouts: worker killed and restarted |
| Crash, corrupted output, oversized line | worker killed; restart after a 1 s backoff |
| Restart budget | at most 3 automatic restarts in 5 minutes, then `failed` until an explicit `Restart()` |
| Startup problems (`fatal`, contract mismatch, artifacts) | `failed`, never retried automatically |
| Shutdown | `shutdown` message, 2 s grace, then kill (guaranteed by the Job Object) |

### Inference protocol (version 1)

JSON Lines, UTF-8, one object per line, at most `--max-line-bytes` (default
65 536) per line. NaN/Infinity tokens, duplicate keys and trailing data are
rejected on both sides.

Worker to client at startup:

```json
{"type":"hello","protocol_version":1,"worker_version":"1.0.0","python_version":"3.15.2","pid":1234}
{"type":"ready","protocol_version":1,"model_version":"lgbm2stage-20261027_220608","feature_contract_version":"aafr.model_features.v1","raw_features":["num_nodes","..."],"model_feature_count":55,"labels":["Dijkstra","ACO","GWO","PSO","WOA"],"stage2_classes":["ACO","GWO","PSO","WOA"],"gate_threshold":0.5100000000000001,"artifact_sha256":{"...":"..."},"library_versions":{"lightgbm":"4.7.0"},"load_warnings":["..."],"verify_ms":12.4,"dependency_import_ms":1661.5,"load_ms":48.4,"pid":1234}
{"type":"fatal","protocol_version":1,"error_code":"E_ARTIFACT_HASH_MISMATCH","message":"..."}
```

Client to worker:

```json
{"type":"predict","protocol_version":1,"request_id":"inf-rt1-e7-3","model_version":"lgbm2stage-20261027_220608","feature_contract_version":"aafr.model_features.v1","evaluation_id":"rt1-e7","network_state_id":42,"graph_version":5,"state_version":"","features":{"num_nodes":4,"...":0.0}}
{"type":"ping","request_id":"ping-1"}
{"type":"shutdown"}
```

`features` must contain exactly the 35 contract names with finite numbers
(booleans and strings are rejected). Worker to client (probability values
illustrative):

```json
{"type":"prediction","protocol_version":1,"request_id":"inf-rt1-e7-3","status":"ok","model_version":"lgbm2stage-20261027_220608","feature_contract_version":"aafr.model_features.v1","model_label":"WOA","label_source":"stage2_argmax","gate_probability":0.823,"gate_threshold":0.5100000000000001,"stage2_ran":true,"stage2_probabilities":{"ACO":0.1,"GWO":0.2,"PSO":0.2,"WOA":0.5},"probabilities_calibrated":false,"out_of_training_range":["num_nodes"],"distribution_warnings":["scenario_category one-hot columns are all 0 (unseen in training)"],"inference_ms":7.3,"echo":{"network_state_id":42,"graph_version":5,"evaluation_id":"rt1-e7","state_version":""},"reasoning":"..."}
{"type":"prediction","protocol_version":1,"request_id":"inf-rt1-e7-4","status":"error","error_code":"E_MISSING_FEATURES","message":"1 required feature(s) missing","features":["jitter"]}
{"type":"pong","protocol_version":1,"request_id":"ping-1","requests_served":12,"pid":1234}
{"type":"bye","protocol_version":1,"requests_served":12}
```

Below the threshold `stage2_ran` is false, `label_source` is
`stage1_below_threshold` and `stage2_probabilities` is null: no stage-2
probabilities are invented. The model never abstains on its own; abstention
happens on the AAFR side when features are incomplete.

Error codes. Startup (`fatal`): `E_MANIFEST_MISSING`, `E_MANIFEST_INVALID`,
`E_PROTOCOL_VERSION`, `E_ARTIFACT_MISSING`, `E_ARTIFACT_HASH_MISMATCH`,
`E_DEPENDENCY_MISSING`, `E_MODEL_LOAD_FAILED`, `E_METADATA_UNSUPPORTED`.
Per request: `E_MALFORMED_JSON`, `E_MESSAGE_TOO_LARGE`, `E_UNKNOWN_TYPE`,
`E_MISSING_FIELD`, `E_PROTOCOL_VERSION`, `E_MODEL_VERSION_MISMATCH`,
`E_FEATURE_CONTRACT_MISMATCH`, `E_MISSING_FEATURES`, `E_UNEXPECTED_FEATURES`,
`E_INVALID_FEATURE_VALUE`, `E_INFERENCE_FAILED`. Client statuses: `ok`,
`worker_unavailable`, `busy`, `timeout`, `worker_error`, `invalid_response`,
`version_mismatch`, `stale`, `excessive_latency` (worker-reported model time
above 250 ms).

The client accepts a response only if its `request_id` is the one being
awaited; it echoes the request's evaluation id, NetworkState id and graph
version; the versions and threshold match the manifest; probabilities lie in
[0, 1], cover exactly the stage-2 classes, sum to 1 and their argmax is the
label; and `stage2_ran` agrees with the gate. A response for an older request
(after its deadline) is discarded and counted as stale; it can never be
matched to a newer request.

### Algorithm mapping and coverage

| Model label | AAFR id | Registry name | Display name |
|---|---|---|---|
| `Dijkstra` | `Dijkstra` | `dijkstra` | Dijkstra |
| none | `AStar` | `astar` | A* |
| none | `BellmanFord` | `bellman_ford` | Bellman-Ford |
| `ACO` | `AntColonyOptimization` | `aco` | Ant Colony Optimization (ACO) |
| `PSO` | `ParticleSwarmOptimization` | `pso` | Particle Swarm Optimization (PSO) |
| none | `GeneticAlgorithm` | `ga` | Genetic Algorithm (GA) |
| none | `ArtificialBeeColony` | `abc` | Artificial Bee Colony (ABC) |
| `GWO` | `GreyWolfOptimizer` | `gwo` | Grey Wolf Optimizer (GWO) |
| none | `FireflyAlgorithm` | `firefly` | Firefly Algorithm |
| `WOA` | `WhaleOptimizationAlgorithm` | `woa` | Whale Optimization Algorithm (WOA) |

The trained label space covers **5 of the 10** algorithms. The mapping
(`MapModelLabel`) is exact and case-sensitive; any other label (`pso`, `GA`,
`Other`, ...) is rejected, never mapped to a neighbour. All ten
implementations stay registered and are still computed at every evaluation;
A*, Bellman-Ford, GA, ABC and Firefly remain reachable through the
deterministic ranking.

### AI recommendation provider and runtime integration

`AIRecommendationProvider` (`runtime/model/ai_recommendation_provider.*`)
implements the existing `IRecommendationProvider`. The runtime order is
unchanged: change detection, eligibility, all ten algorithms compute, path
validation, **provider**, challengers, probing and arbitration (or the
estimate-only policy), stability gates, activation, confirmation,
DecisionRecord, outcome observation. The provider therefore only reorders
candidates that already passed eligibility and validation:

1. compute the deterministic ranking (always);
2. build the features; abstain if incomplete; otherwise infer, or reuse the
   previous answer for an identical feature vector (the model is a pure
   function of its inputs, so unchanged inputs cause no inference);
3. reject a stale answer (the browser's validator checks after inference that
   the graph version is still current);
4. map the label and require the mapped algorithm's candidate to be `Usable()`;
5. **AI preferred**: rank that candidate first and keep the deterministic
   order for the rest; **shadow**: return the deterministic ranking
   unchanged; **deterministic**: never call the model;
6. attach a `ModelRecommendationTrace`.

A model ranking cannot cause a switch by itself: the challenger must still
show measured improvement under arbitration (or pass the conservative
estimate margin when probing is unavailable), pass dwell, cooldown,
hysteresis and backoff, and be confirmed by the executor. The V0.7.1
measurement reviews and their schedule are unchanged; there is no second
runtime loop.

Fallback to the deterministic ranking is always recorded with its reason
(`fallback_reason`, reason code `MODEL_FALLBACK_DETERMINISTIC`, event
`model.deterministic_fallback`): interpreter missing, artifacts missing or
modified, load failure, missing packages, worker crash, timeout, busy,
malformed or inconsistent response, version or contract mismatch, excessive
model time, stale answer, unknown label, ineligible recommendation,
recommendation without a usable path, incomplete features. If the
deterministic ranking itself has nothing usable, its `unavailable` passes
through and the runtime retains or degrades exactly as in V0.7.1.

### Confidence semantics

Kept apart, never combined into one score:

| Quantity | Where | Use |
|---|---|---|
| Stage-1 gate probability, stage-2 class probabilities | trace `gate_probability`, `stage2_probabilities`, `probabilities_calibrated: false` | recorded only |
| Calibration quality | manifest `calibration`: not evaluated | none |
| Runtime evidence quality | DecisionRecord `evidence_source`, `evidence_validity` | gates |
| Statistical confidence interval | `ratio_ci_low/high`, `p_holm` | arbitration |
| Practical improvement margin | stability configuration | gates |

### Shadow mode

Shadow mode asks the model at every evaluation and records the prediction,
whether it was usable and whether it agrees with the deterministic choice
(`agrees_with_deterministic`, provider counters), but the ranking, and so
every switch, stays deterministic (`MODEL_SHADOW_ONLY`). It is the mode for
validating the model before it may influence forwarding.

### DecisionRecord, events and Developer Tools

`aafr.decision_record.v1` gains three additive fields (older readers ignore
them): `recommendation_mode`, `recommendation_source` (`deterministic`,
`expert_model`, `deterministic_fallback`, `deterministic_shadow`) and `model`
(the trace: model name and version, feature-contract version, request id,
evaluation id, NetworkState id, graph version, inference status and error,
cached flag, raw label, mapped `predicted_algorithm`, label source, gate
probability and threshold, stage-2 probabilities, calibration flag, worker
and round-trip latency, feature build time and provenance summary, missing,
substituted and out-of-range features, distribution warnings, candidate
check, whether it controlled the ranking, fallback reason, the deterministic
algorithm and agreement). The prediction (`model.predicted_algorithm`), the
runtime's candidate (`candidate_algorithm`) and the confirmed algorithm
(`confirmed_algorithm`) are separate fields, and `provider_id` names the
ranking actually used. New reason codes: `MODEL_RECOMMENDATION_USED`,
`MODEL_FALLBACK_DETERMINISTIC`, `MODEL_SHADOW_ONLY`. No browsing payload is
recorded.

Telemetry events (source `expert_model`): `model.worker_started`,
`model.loaded`, `model.ready`, `model.inference_requested`,
`model.inference_completed`, `model.inference_timed_out`,
`model.inference_failed`, `model.recommendation_rejected`,
`model.deterministic_fallback`, `model.worker_restarted`,
`model.stale_inference_discarded`, `model.worker_stopped`,
`model.worker_failed`. NetworkState ignores them (they are not network
observations).

Developer Tools, Runtime tab, adaptive pane, "AI expert model": the
recommendation mode (selectable), worker state and counters, model version,
threshold, labels, library versions and load warnings, the active
recommendation provider, the AI-recommended algorithm, the deterministic
recommendation, the runtime-selected candidate, the confirmed active
algorithm, the latest inference (gate, stage-2 probabilities, "uncalibrated",
feature provenance, errors), its latency, the fallback status,
model-versus-deterministic agreement, worker resources (Job Object working
set, peak commit, CPU) and recent model events. The evidence table has an
"AI recommendation" column and the Events tab an "Expert model" filter.

The browser enables the model only when asked, so its default behaviour is
exactly V0.7.1 (no Python process):

```bash
set AAFR_MODEL_MODE=shadow
```

`AAFR_MODEL_MODE` is `deterministic`, `shadow` or `ai_preferred`;
`AAFR_MODEL_DIR`, `AAFR_MODEL_PYTHON`, `AAFR_MODEL_MANIFEST`,
`AAFR_MODEL_WORKER` and `AAFR_MODEL_TIMEOUT_MS` override the build defaults;
`AAFR_MODEL_PROFILE` names a JSON object of configured values for the 23
telemetry features (labelled `configured`). Without such a profile the model
abstains in the browser, because AAFR has no measurement source for those
features yet.

### Compatibility report

- **Raw features.** 12 of 35 are computed exactly from AAFR's graph. 23 have
  no AAFR source: utilization, queue and fairness are not observable from a
  host; latency, jitter and throughput exist in NetworkState only as
  host-wide handshake-RTT and ACK-delay figures (semantic mismatch, opt-in
  substitutes); loss is not `tcp.retransmission_ratio`; the dynamics need a
  graph change history.
- **Engineered features.** All 20 derived columns come from the model's own
  `features.py` inside the worker (no second implementation). The 7 scenario
  one-hots are always 0 at inference, a combination no training row had
  (each had exactly one); every response carries this as a distribution
  warning.
- **Units.** Utilization in percent (0..106 in training), loss and delivery
  as ratios, latency in ms, throughput in Mbps; the adapter enforces them.
- **Class coverage.** 5 of 10 algorithms (mapping table above).
- **Output validity.** 60 sequential real requests: every label mappable,
  gate in [0, 1], `stage2_ran` consistent with the threshold; the worker's
  answer equals an in-process call of the model's own code (gate equal to 12
  decimals).
- **Distribution shift (measured).** AAFR's controlled graph has 4 nodes; the
  training graphs have 20 to 650. With AAFR's graph features and each of the
  3500 training rows' own telemetry, the model answers `Dijkstra` for 3499
  rows and `WOA` for one (row 2614, gate 0.82); the next-highest gate is
  0.35. On AAFR's topology the model is effectively an always-Dijkstra
  selector, and a metaheuristic recommendation there is an extrapolation.
- **Objective mismatch.** Labels come from the simulator's objective
  (bottleneck-bandwidth term, multiplicative loss); AAFR's J(P) uses additive
  `-ln(1-p)` loss and treats bandwidth as a constraint. "Best algorithm" is
  objective-specific.
- **Performance.** Measured below; inference is fast enough for every
  evaluation.
- **Required for model improvement (not done in V0.7.2):** a measured
  per-path telemetry source (probe and outcome statistics for the latency
  distribution, jitter, loss and delivery ratio; graph change history for
  the dynamics); retraining on AAFR-sized topologies with AAFR's objective
  and its own `aafr.measured_forwarding.v1` outcome data; an explicit
  unknown-scenario encoding instead of all-zero one-hots; calibration (for
  example isotonic regression on held-out data) before any probability is
  shown as a confidence; coverage of the five missing algorithms or an
  explicit abstain class; re-pickling with the scikit-learn version in use.

### Measurements (Debug build, 2026-11-18, this workstation)

| Quantity | Value |
|---|---|
| Worker spawn to validated ready | 1.86 s (artifact hashing 12 ms, package imports 1.66 s, model load 48 ms) |
| Manual restart to ready | 1.8 s; automatic restart after a crash, failure to ready: 2.3 s (includes 100 ms backoff) |
| Model inference (worker-reported), 60 requests | p50 7.3 ms, p95 8.1 ms |
| Round trip (request write to validated response) | p50 8.3 ms, p95 9.0 ms, max 15.4 ms |
| Feature construction (C++) | 0.6 ms on the controlled graph |
| End-to-end first recommendation in the runtime | 12.4 ms round trip (model 11.2 ms) |
| Worker memory (Job Object) | working set 169 MiB, peak commit 586 MiB |
| Worker CPU, startup + 62 requests + restart | 3.4 s user, 2.2 s kernel |
| Fallback latency | none added beyond the failed attempt: unavailable fails immediately, a timeout costs the 500 ms budget |

### Tests

| Test | Covers |
|---|---|
| `model_contract` | strict JSON codec; 35-feature contract; label mapping of all ten algorithms and rejection of unknown labels; repository manifest vs contract; manifest tampering; graph features against hand-computed values (cycle, complete graph, path, triangle, disconnected, unavailable nodes); Jacobi eigenvalues |
| `model_feature_adapter` | mapping and order, provenance, missing, stale, NaN, infinity, wrong units, out-of-range, fractional counts, unsupported names, duplicates, graph-feature conflicts, opt-in substitutes, unreachable destination, serialization to the worker's contract |
| `model_ai_provider` | three modes; ranking; shadow comparison; unknown label; ineligible and pathless recommendations; nine failure statuses; abstention without inference; no worker; stale context; caching; fallback that fails safely; mode changes; trace JSON |
| `model_ai_runtime` | the provider inside `AAFRRuntime`: all ten algorithms computed, initial activation, a different prediction correctly retained, evidence + hysteresis before a switch, dwell/cooldown, improvement margin, ineligible recommendation not executed, activation failure keeps the previous configuration, safety recovery, stale inference cannot switch, shadow never controls, worker failure fallback and recovery, AI disabled equals deterministic |
| `model_worker` | real processes: not configured, correlation and sequential requests, timeout and late (stale) response, repeated timeouts restart, crash recovery, restart budget, malformed/wrong-version/bad-echo/error/bad-probabilities/slow/unknown-label responses, oversized output, startup failures (fatal, missing package, contract, version, hang), busy; real model: load, 60 requests with latency, worker-side validation, restart time; missing artifact, corrupt artifact, unsupported metadata |
| `model_e2e` | real worker + real loopback relays: steps 1-17 of the controlled scenario, including a retained valid prediction and fallback/recovery |
| `model_worker_python` | the worker protocol in Python: ready contents, predictions, equality with the model's own code, every request rejection, oversized request, startup failures |
| `browser_observability` (new case `devToolsShowsExpertModel`) | Developer Tools model section, separation of prediction / candidate / confirmed algorithm, mode selector, Expert model event filter |

Verification record (2026-11-18, Debug, MSVC 14.44, Qt 6.12.0): baseline 21
CTest suites passed before any change; after V0.7.2, 28 of 28 passed (7 new
suites; 2 opt-in Qt cases skipped as before: live TShark capture and UI
screenshots). In one intermediate full run `browser_interface` failed one
address-bar comparison under load and passed 54/54 on the immediate rerun and
in the final full run; that suite is known to be timing-sensitive.

### Known limitations

- Ordinary browser page loads still do not pass through AAFR's controlled
  forwarding relays (V0.7.1), so nothing here shows that AI-assisted
  selection improves real browser performance.
- In the browser the model abstains unless a configured telemetry profile is
  supplied; the end-to-end test uses two profiles copied from training rows,
  labelled `configured`.
- On the 4-node controlled topology the model nearly always answers Dijkstra
  (distribution shift above).
- Probabilities are uncalibrated; the label encoder was pickled with
  scikit-learn 1.8.0 and runs under 1.9.1.
- Resource figures come from the worker's Job Object (the venv `python.exe`
  is a launcher that starts the real interpreter as a child); the reported
  pid is the launcher's.
- `ChildProcess` has a POSIX implementation that compiles only on
  non-Windows systems and was not exercised.

## V0.7.3 feature readiness from real browser forwarding

V0.7.3 routes real browser connections through the relays (see
[browser README, V0.7.3](../browser/qt_webengine/README.md#v073-real-browser-forwarding)).
That makes some of the 23 telemetry inputs that have no AAFR source
*measurable in principle*, per path. **None of them is fed to the model
yet**: building those sources, with windows, provenance and freshness, is
V0.7.4. The model still abstains in the browser unless a configured profile
is supplied. Nothing was retrained, no label or threshold (Stage 1: 0.51)
changed, and no value is manufactured to make inference run.

Legend: *direct* = measurable from real forwarding without modelling;
*estimator* = needs a documented estimator and validation first;
*unavailable* = not observable from an application-layer host.

| Feature | Unit | Existing source | Gateway measurement | Relay measurement | Status |
|---|---|---|---|---|---|
| `latency` | ms | NetworkState handshake RTT (host-wide, semantic mismatch) | `establish_ms` per path (tunnel set-up incl. destination connect) | per-hop set-up time (not yet recorded) | estimator: application set-up latency ≠ the simulator's packet latency |
| `jitter` | ms | ACK-delay jitter (host-wide) | spread of `establish_ms` / `first_byte_ms` per path | — | estimator |
| `loss` | ratio | TCP retransmission heuristic | failed tunnels / attempts per path | per-node refusals | estimator: connection failure ratio ≠ packet loss |
| `throughput` | Mbps | host-wide TShark throughput | bytes ÷ duration per connection | `StreamTraversal` bytes per node | direct (per connection; aggregation window still to define) |
| `mean_link_utilization` | % | none | — | bytes per edge ÷ configured capacity | estimator (capacity is configured, not measured) |
| `max_link_utilization` | % | none | — | as above | estimator |
| `link_utilization_std` | % | none | — | as above | estimator |
| `avg_queue_delay_ms` | ms | none | — | — | unavailable (no queue visibility at the application layer) |
| `max_queue_depth` | packets | none | — | — | unavailable |
| `avg_queue_occupancy` | ratio | none | — | — | unavailable |
| `offered_load_mbps` | Mbps | none | bytes from the browser ÷ window | per-node bytes toward the destination | direct for AAFR's own load (not the network's) |
| `packet_delivery_ratio` | ratio | none | completed ÷ forwarded connections | — | estimator (connection ratio, not packets) |
| `jains_fairness_index` | ratio | none | per-connection throughput across active connections | — | direct formula, but the meaning differs from the simulator's flows |
| `latency_p50` … `latency_p99` (4) | ms | partial (handshake median) | percentiles of `establish_ms` / `first_byte_ms` per path | — | estimator (same semantic gap as `latency`) |
| `latency_min`, `latency_max`, `latency_std` (3) | ms | handshake min/max/variation | same | — | estimator |
| `link_failure_events` | count | graph change history | — | relay failures seen by browser connections | direct for the controlled topology |
| `link_recovery_events` | count | graph change history | — | relays back in service | direct for the controlled topology |
| `reroute_count` | count | runtime switches | `RelayPathChanged` events | — | direct (gateway-observed switches) |

Implemented in V0.7.3, because forwarding and its validation need them: the
gateway's per-connection `establish_ms`, `first_byte_ms`, `duration_ms`,
bytes each way, forwarding status and relay-failure flag; per-node
`StreamTraversal` bytes and open/close times; `RelayPathChanged`,
`ConfigurationDraining` and `ConfigurationRetired` events; and browser-stream
outcomes per decision. These are gateway- and relay-observed values with
provenance. They are deliberately **not** passed to `ConfiguredTelemetrySource`
or the feature adapter.

Topology mismatch (unchanged): the model was trained on much larger
networks than AAFR's 4-node controlled graph, and nearly always answers
Dijkstra on it. The deterministic runtime keeps working whenever features
are incomplete. The AI regression with the gateway (`gateway_model`, 6 cases)
confirms that deterministic, shadow and AI-preferred modes all forward
browser traffic over the runtime's confirmed configuration only.

Release build (V0.7.3 verification closure, 2026-11-18): the Python worker
starts from the Release binaries with the same interpreter
(`.venv\Scripts\python.exe`) and manifest-checked artifacts; `model_worker`,
`model_e2e`, `model_worker_python`, `model_ai_runtime`, `model_ai_provider`
and `gateway_model` pass in Release. In the Release experiment's shadow
phase, the real model abstained on every decision without measured inputs
(deterministic fallback in control), and with the configured profile agreed
3/3 with the deterministic choice. No artifact, label or threshold changed.
