# AAFR Requirements

## Adaptive Application-Layer Forwarding Runtime

This document defines the functional, AI, networking, routing, browser, runtime, stability, explainability, experimental, performance, and technical requirements for AAFR.

---

# 1. Core System Requirements

AAFR must provide three primary capabilities:

1. AI-Based Runtime Algorithm Selection
2. Adaptive Runtime Algorithm Switching
3. Stable and Explainable Switching

The system must operate while a user is actively using the Qt browser.

The browser must remain responsive while network monitoring, Expert Model inference, switching, routing, and decision recording occur in the background.

---

# 2. Core System Flow

The required end-to-end flow is:

```text
User
→ Qt Browser
→ Network Traffic
→ Network Monitoring
→ NetworkState
→ AI Expert Model
→ Recommended Algorithm + Confidence
→ AAFR Runtime
→ Stability Checks
→ Continue / Switch
→ Active Routing Algorithm
→ Routing Controller
→ Controlled Routing Environment
→ Network
→ Browser
→ User
```

The process must continue while the browser is running.

---

# 3. Routing Algorithm Requirements

AAFR must support ten routing and optimization algorithms.

## Classical Algorithms

1. Dijkstra
2. A*
3. Bellman-Ford

## Metaheuristic and Nature-Inspired Algorithms

4. Ant Colony Optimization
5. Particle Swarm Optimization
6. Genetic Algorithm
7. Artificial Bee Colony
8. Grey Wolf Optimizer
9. Firefly Algorithm
10. Whale Optimization Algorithm

All algorithms must expose a common routing interface.

Conceptually:

```text
Input

Topology
Source
Destination
Network State
Algorithm Configuration

        ↓

Routing Algorithm

        ↓

Output

Selected Path
Path Metrics
Algorithm Runtime
Objective / Performance Result
```

The common interface must allow AAFR to change the active algorithm without changing unrelated runtime components.

---

# 4. Dataset Requirements

AAFR uses three fixed datasets.

## Network Dataset

File:

`network_dataset_raw.csv`

Contains:

- 3,500 scenarios
- 45 columns
- Seven network scenario categories

Categories:

```text
A_Excellent
B_Congested
C_Unreliable
D_Sparse
E_Dense
F_Dynamic
G_Large
```

---

## Routing Benchmark Dataset

File:

`routing_benchmark_3500.csv`

Contains:

- 35,000 benchmark records
- 3,500 scenarios
- Ten algorithm results per scenario

Main fields:

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

The benchmark dataset must be used for algorithm-performance comparison.

---

## Expert Dataset

File:

`routing_expert_dataset.csv`

Contains:

- 3,500 records
- 58 columns

Target-related fields include:

```text
target_algo_eps0
target_algo_eps0_5
target_algo_eps1
target_algo_eps2
invoke_metaheuristic
```

Comparison-related fields include:

```text
dijkstra_objective
best_objective
qos_improvement_pct
dijkstra_runtime_ms
best_runtime_ms
```

The dataset must be validated before Expert Model training.

---

# 5. Dataset Integrity Requirements

Before model training:

- Dataset schemas must be validated.
- Missing values must be checked.
- Duplicate records must be checked.
- Data types must be validated.
- Scenario identifiers must be validated.
- Algorithm names must be normalized.
- Target distributions must be analyzed.
- Class imbalance must be documented.
- Potential data leakage must be identified.
- Training features must be explicitly documented.

Original datasets must not be silently modified.

All preprocessing must be reproducible.

---

# 6. Algorithm Suitability Requirements

The system must have a clearly defined method for determining why one algorithm is considered more suitable than another for a given network condition.

Suitability must be based on measurable benchmark performance.

Relevant benchmark measurements include:

- Latency
- Loss
- Bottleneck bandwidth
- Path delay variation
- Hop count
- Algorithm runtime
- Objective

The exact `objective` definition must be documented.

The exact algorithm target-generation and tie-breaking procedure must also be documented.

The system must not assume that one algorithm is universally best.

---

# 7. Expert Model Requirements

The Expert Model must perform supervised multiclass classification.

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

It must not directly perform routing or algorithm switching.

---

# 8. Expert Model Input Requirements

Model inputs must:

1. Be available before algorithm selection.
2. Represent the current network or routing context.
3. Not reveal the correct target.
4. Be reproducible in the runtime environment.
5. Use consistent definitions between training and runtime.

Potential feature groups include:

- Network performance
- Traffic state
- Congestion
- Topology
- Dynamic network state
- Routing context

The final feature list must be frozen before final model training.

---

# 9. Data Leakage Requirements

Target and post-decision information must not be used as normal model input.

Examples include:

```text
target_algo_eps0
target_algo_eps0_5
target_algo_eps1
target_algo_eps2
invoke_metaheuristic
best_objective
best_runtime_ms
qos_improvement_pct
```

Additional leakage-prone fields must be identified during feature auditing.

---

# 10. Expert Model Output Requirements

The Expert Model must return at least:

```text
recommended_algorithm
confidence
```

Example:

```json
{
    "recommended_algorithm": "PSO",
    "confidence": 0.87
}
```

The recommended algorithm must correspond to a supported algorithm class.

Confidence should use a consistent range:

```text
0.0 ≤ confidence ≤ 1.0
```

---

# 11. Confidence Requirements

Confidence represents the strength of the Expert Model's recommendation.

Conceptually:

```text
NetworkState
      ↓
Model
      ↓
Class Scores / Probabilities
      ↓
Highest Class
      ↓
Recommended Algorithm + Confidence
```

Confidence must be evaluated on unseen data.

Confidence calibration should be evaluated before interpreting model output as a reliable probability.

AAFR must use confidence as a stability signal rather than automatically switching for every highest-scoring prediction.

---

# 12. Expert Model Candidate Requirements

Initial model candidates should include:

- Majority-class baseline
- Logistic Regression
- Decision Tree
- Random Forest
- Gradient Boosting / XGBoost

Additional models may be introduced when experimentally justified.

---

# 13. Model Training Requirements

Training must:

- Use reproducible random seeds where applicable.
- Separate training and evaluation data.
- Prevent target leakage.
- Preserve preprocessing configuration.
- Preserve the feature list.
- Preserve target definition.
- Record model configuration.
- Record model version.
- Record evaluation results.
- Save the selected trained model.

Runtime inference must use the same preprocessing required by the trained model.

---

# 14. Model Evaluation Requirements

Because the target classes are imbalanced, model selection must not rely only on accuracy.

Evaluation must include:

- Macro F1
- Balanced Accuracy
- Weighted F1
- Per-class Precision
- Per-class Recall
- Confusion Matrix
- Inference Latency

Confidence calibration should also be evaluated.

---

# 15. NetworkState Requirements

NetworkState must be the standard data contract between the Network Monitor and Expert Model.

```text
Network Sources
      ↓
Network Monitor
      ↓
NetworkState
      ↓
Expert Model
```

NetworkState may represent:

```text
NetworkState

├── Network Performance
├── Traffic State
├── Congestion State
├── Topology State
├── Failure / Dynamic State
└── Routing Context
```

The final fields must correspond to the finalized model-input features.

---

# 16. Training-Runtime Consistency Requirements

For every model feature:

```text
Training Feature
      ↕
Runtime Feature
```

must have consistent:

- Meaning
- Unit
- Scale
- Calculation
- Preprocessing

A model must not depend on features that cannot be reliably reproduced in the runtime experimental environment.

---

# 17. Network Monitor Requirements

The Network Monitor must continuously observe relevant network conditions while the browser is running.

It must combine measurements from:

1. Qt/browser measurements
2. TShark measurements
3. Controlled network measurements where required

The Network Monitor must generate the latest valid NetworkState.

It must not select routing algorithms.

---

# 18. Qt Browser Monitoring Requirements

Browser-side monitoring may collect:

- Request timing
- Request completion
- Request failures
- Timeouts
- Bytes transferred
- Resource loading information
- Browser-level performance information

Monitoring must not unnecessarily block browser operation.

---

# 19. TShark Requirements

TShark must run as a background observation component where packet-level information is required.

It may provide:

- Packet timing
- Packet counts
- Traffic rate
- TCP behaviour
- Retransmission information
- Protocol information
- Source and destination information

TShark must not:

- Select routing algorithms
- Switch routing algorithms
- Act as the Expert Model
- Act as the network simulator
- Replace the controlled routing environment

---

# 20. Wireshark Requirements

Wireshark GUI should be used for:

- Packet inspection
- Debugging
- Measurement validation
- Traffic analysis
- Experimental demonstrations

Wireshark is not the runtime algorithm-selection component.

Wireshark is not the network simulator.

---

# 21. Monitoring Window Requirements

Expert Model inference must not occur for every packet.

Measurements should be aggregated over a monitoring window.

```text
Raw Measurements
      ↓
Monitoring Window
      ↓
Feature Aggregation
      ↓
NetworkState
```

The monitoring-window duration must be configurable and experimentally determined.

---

# 22. Network Change Detection Requirements

The system must distinguish meaningful network changes from small measurement fluctuations.

```text
Previous NetworkState
        +
Current NetworkState
        ↓
Meaningful Change?
```

Small measurement noise should not automatically cause switching.

Change criteria must be configurable and experimentally evaluated.

---

# 23. AAFR Runtime Requirements

AAFR Runtime must:

- Maintain the currently active algorithm.
- Receive Expert Model recommendations.
- Receive model confidence.
- Compare the recommendation with the active algorithm.
- Evaluate stability conditions.
- Continue the active algorithm when appropriate.
- Activate the recommended algorithm when switching is approved.
- Maintain runtime state.
- Generate DecisionRecords.

AAFR must not independently replace the Expert Model's recommendation with another algorithm.

---

# 24. Runtime State Requirements

AAFR must maintain at least:

```text
active_algorithm
latest_network_state
recommended_algorithm
recommendation_confidence
last_switch_time
stability_state
latest_decision
```

There must normally be a valid active algorithm while the runtime is operating.

---

# 25. Runtime Switching Requirements

The switching process must follow:

```text
Expert Model Recommendation
        ↓
Same as Active?
     /        \
   YES         NO
    ↓           ↓
Continue   Stability Checks
                ↓
            Pass / Fail
             /      \
           FAIL     PASS
            ↓        ↓
        Continue    Switch
```

The newly selected algorithm must apply to subsequent routing decisions.

The browser must not need to restart when the algorithm changes.

---

# 26. Switching Stability Requirements

Initial switching conditions include:

1. Recommended algorithm differs from the active algorithm.
2. Model confidence is sufficient.
3. Network change is meaningful.
4. Minimum dwell time is satisfied.

Additional stability mechanisms may be introduced when experimentally justified.

---

# 27. Minimum Dwell Time Requirements

After an algorithm becomes active, it must remain active for a configurable minimum period before another normal switch is permitted.

Purpose:

- Prevent rapid switching
- Reduce oscillation
- Allow the algorithm time to operate
- Improve runtime stability

The dwell duration must be experimentally determined.

---

# 28. Switching Configuration Requirements

The following values must remain configurable:

- Confidence threshold
- Meaningful-change threshold
- Minimum dwell time
- Monitoring interval
- Model path
- Algorithm configuration

These values must not be permanently hard-coded without experimental justification.

---

# 29. DecisionRecord Requirements

Significant runtime decisions must generate a DecisionRecord.

It should contain:

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

Possible decision states include:

```text
CONTINUE
SWITCH
REJECT_SWITCH
```

---

# 30. Explainability Requirements

For each significant algorithm decision, the system should be able to answer:

1. What was the network condition?
2. Which algorithm was active?
3. Which algorithm was recommended?
4. What was the confidence?
5. Did the network meaningfully change?
6. Were the stability checks satisfied?
7. Was a switch performed?
8. Why was the switch accepted or rejected?

The explanation must not simply state:

```text
"The AI selected it."
```

It should be connected to the recorded runtime state and decision process.

---

# 31. Controlled Routing Environment Requirements

AAFR requires a controllable routing and forwarding environment.

```text
Qt Browser
      ↓
AAFR
      ↓
Active Algorithm
      ↓
Routing Controller
      ↓
Controlled Routing Environment
      ↓
Network Path
```

The environment must expose sufficient topology and network-state information for the supported algorithms.

It must provide a mechanism for applying selected paths to experimental traffic.

---

# 32. Routing Execution Requirements

The active algorithm must receive the required network information and calculate a usable routing/path decision.

The Routing Controller must apply the resulting path within the controlled environment.

Changing:

```text
active_algorithm = "PSO"
```

alone does not constitute successful runtime switching.

The newly active algorithm must actually influence subsequent routing decisions.

---

# 33. Qt Browser Requirements

The browser must be implemented using Qt/Qt WebEngine.

It should support normal browser operations including:

- Search
- URL navigation
- Page loading
- Link navigation
- Resource loading
- Multiple requests
- Multiple tabs where implemented

The user must not need to manually choose routing algorithms.

---

# 34. Browser Responsiveness Requirements

The following operations should execute without unnecessarily blocking the browser UI:

- Network monitoring
- TShark processing
- Expert Model inference
- Stability evaluation
- Decision recording
- Runtime switching

The implementation should use suitable asynchronous, event-driven, threaded, or process-based execution where necessary.

---

# 35. Active Algorithm Availability Requirements

AAFR must maintain a valid active algorithm during normal operation.

During Expert Model inference:

```text
Current Algorithm
      ↓
Remains Active
      ↓
Expert Model Calculates Recommendation
      ↓
AAFR Evaluates Recommendation
```

There should not normally be a period where the routing runtime has no valid algorithm.

---

# 36. Failure Handling Requirements

AAFR should fail safely.

## Expert Model Failure

```text
Inference Failure
→ Keep Current Algorithm
→ Record Failure
```

## Invalid Recommendation

```text
Invalid Algorithm
→ Reject Recommendation
→ Keep Current Algorithm
```

## Switching Failure

```text
Switch Failure
→ Retain / Restore Valid Algorithm
→ Record Failure
```

## Monitoring Failure

```text
Measurement Source Failure
→ Record Failure
→ Continue with remaining valid measurements where possible
```

Failures must not silently leave the routing system in an invalid state.

---

# 37. Logging Requirements

The system must record important runtime events including:

- Runtime startup
- Browser startup
- Network Monitor startup
- TShark startup
- Model loading
- Model inference
- Algorithm recommendations
- Confidence
- Switch attempts
- Successful switches
- Rejected switches
- Algorithm failures
- Monitoring failures
- Runtime shutdown

Logs should contain timestamps.

---

# 38. Configuration Requirements

Configuration should contain parameters such as:

```text
model_path
target_variant
monitoring_interval
confidence_threshold
minimum_dwell_time
meaningful_change_threshold
algorithm_parameters
experiment_configuration
logging_configuration
```

Experimental parameters should not be scattered as hard-coded constants throughout the codebase.

---

# 39. Experiment Scenario Requirements

Experiments should cover:

1. Excellent
2. Congested
3. Unreliable
4. Sparse
5. Dense
6. Dynamic
7. Large

Experiments should also evaluate transitions between conditions.

Example:

```text
Excellent
→ Congested
→ Unreliable
→ Excellent
```

---

# 40. Routing Evaluation Requirements

Routing evaluation should include relevant measurements such as:

- Objective
- Latency
- Loss
- Bottleneck bandwidth / throughput
- Path delay variation
- Hop count
- Algorithm runtime

---

# 41. AAFR Runtime Evaluation Requirements

Runtime evaluation should include:

- Number of algorithm switches
- Switch frequency
- Rejected switches
- Unnecessary switches where measurable
- Switching overhead
- Runtime stability
- Response time to network changes
- Runtime failures

---

# 42. Browser Evaluation Requirements

Browser-level evaluation should include relevant measurements such as:

- Request completion
- Request failures
- Resource loading performance
- Page loading performance
- Browser responsiveness

The exact browser metrics must be defined before final experiments.

---

# 43. Baseline Requirements

AAFR must be compared with clearly defined baseline routing configurations.

At minimum:

```text
Fixed / Baseline Routing
           VS
      Adaptive AAFR
```

Comparisons should use equivalent experimental conditions.

AAFR must not be assumed to outperform the baseline.

Any improvement must be demonstrated experimentally.

---

# 44. Reproducibility Requirements

Experiments should record:

- Dataset version
- Model version
- Feature set
- Target definition
- Model configuration
- Algorithm configuration
- Random seed where applicable
- Network scenario
- Network configuration
- Switching configuration
- Experiment timestamp
- Evaluation results

Stochastic algorithms should use controlled seeds and repeated runs where required.

---

# 45. Performance Requirements

AAFR should measure and minimize overhead introduced by:

- Network monitoring
- TShark processing
- Expert Model inference
- Stability evaluation
- Decision recording
- Algorithm switching

Model inference must be fast enough for runtime use without unnecessarily blocking browser operation.

---

# 46. Modularity Requirements

The following components must remain logically separated:

```text
Browser

Network Monitor

NetworkState

TShark Collector

Expert Model

AAFR Runtime

Stability Logic

Routing Algorithms

Routing Controller

Controlled Routing Environment

DecisionRecord

Experiment Framework
```

Changes to one component should require minimal changes to unrelated components.

---

# 47. Interface Requirements

Clear interfaces must exist between:

```text
Browser / TShark / Network
            ↓
      Network Monitor

Network Monitor
      ↓
NetworkState

NetworkState
      ↓
Expert Model

Expert Model
      ↓
AAFR Runtime

AAFR Runtime
      ↓
Algorithm Registry

Routing Algorithm
      ↓
Routing Controller

Routing Controller
      ↓
Controlled Network

AAFR Runtime
      ↓
DecisionRecord
```

---

# 48. Testing Requirements

## Unit Testing

Tests should cover:

- Individual routing algorithms
- Feature calculations
- NetworkState validation
- Expert Model inference
- Confidence handling
- Stability rules
- DecisionRecord generation

## Integration Testing

Tests should cover:

```text
Monitor → NetworkState

NetworkState → Expert Model

Expert Model → AAFR

AAFR → Algorithm Switch

Algorithm → Routing Controller

Routing Controller → Controlled Network

TShark → Network Monitor

Browser → Runtime
```

## End-to-End Testing

The complete flow must be testable:

```text
User Browsing
      ↓
Network Changes
      ↓
NetworkState
      ↓
Expert Model
      ↓
Recommendation
      ↓
AAFR Decision
      ↓
Algorithm Switch
      ↓
Routing Execution
      ↓
Browser Continues
```

---

# 49. System Success Requirements

The system should demonstrate that:

1. All ten algorithms operate through a common interface.
2. Algorithm performance can be evaluated under controlled scenarios.
3. The Expert Model can be trained using validated features and targets.
4. The Expert Model can recommend an algorithm and confidence.
5. Runtime NetworkState can be generated consistently.
6. AAFR can maintain an active algorithm.
7. AAFR can switch algorithms without restarting the browser.
8. Stability logic prevents uncontrolled rapid switching.
9. DecisionRecord explains runtime decisions.
10. The selected algorithm affects actual routing decisions.
11. Browser operation continues while AAFR runs in the background.
12. TShark can provide required packet-level observations.
13. Experiments are reproducible.
14. Baseline and adaptive routing can be compared.
15. Experimental results can determine whether AAFR provides measurable improvement.

---

# 50. Requirements To Finalize Before Implementation

## Before Expert Model Training

Finalize:

- Exact objective definition
- Exact target-generation procedure
- Tie-breaking procedure
- Target selection:
  - target_algo_eps0
  - target_algo_eps0_5
  - target_algo_eps1
  - target_algo_eps2
- Final Expert Model feature list
- Training and evaluation methodology

## Before Network Monitor Implementation

Finalize:

- Runtime feature list
- Source of every feature
- Units
- Feature calculation methods
- Aggregation methods

## Before AAFR Switching Implementation

Finalize:

- Meaningful network-change definition
- Confidence handling
- Minimum dwell-time configuration
- Switching-state transitions

## Before End-to-End Experiments

Finalize:

- Controlled routing environment
- Baseline configuration
- Monitoring window
- Experiment scenarios
- Evaluation metrics
- Repetition and random-seed strategy

---

# 51. Requirements Principle

Every component has one primary responsibility:

```text
Qt Browser
→ Generates application traffic and provides browsing

Network Monitor
→ Observes network conditions

NetworkState
→ Represents current network conditions

Expert Model
→ Recommends the most suitable algorithm

Confidence
→ Represents recommendation strength

AAFR
→ Controls runtime continuation or switching

Routing Algorithm
→ Calculates the path

Routing Controller
→ Applies the path

Controlled Network
→ Executes the experimental routing behaviour

DecisionRecord
→ Records and explains decisions

TShark
→ Provides packet-level observations

Wireshark
→ Supports human inspection and validation
```

The complete runtime follows:

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