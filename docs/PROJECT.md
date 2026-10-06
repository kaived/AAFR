# AAFR

## Adaptive Application-Layer Forwarding Runtime

AAFR is an intelligent browser networking research system that uses a trained AI Expert Model to select the most suitable routing algorithm according to the current network condition and dynamically manage algorithm switching at runtime.

The system integrates a Qt-based browser, network monitoring, TShark-based packet observation, an AI Expert Model, ten routing and optimization algorithms, a controlled routing environment, stability mechanisms, and explainable runtime decision recording.

The goal is to investigate whether AI-based adaptive routing algorithm selection can improve browser network performance and reliability under changing network conditions compared with fixed routing approaches.

---

# 1. Research Problem

Different routing and path-selection algorithms behave differently under different network conditions.

An algorithm that performs well under a stable network may not necessarily provide the best performance when the network becomes congested, unreliable, dynamic, sparse, dense, or large.

Using one fixed algorithm may therefore not always provide the most suitable performance trade-off.

AAFR investigates whether an AI Expert Model can learn the relationship between network conditions and routing algorithm performance and use that knowledge to recommend the most suitable algorithm at runtime.

AAFR then manages the runtime transition to the recommended algorithm while maintaining stability and recording the reason behind each decision.

---

# 2. Research Objective

The primary objective is to design, implement, and evaluate an intelligent runtime capable of:

1. Observing the current network condition.
2. Representing the condition as a consistent NetworkState.
3. Using a trained AI Expert Model to recommend the most suitable routing algorithm.
4. Producing a confidence score for the recommendation.
5. Dynamically switching the active routing algorithm when appropriate.
6. Preventing unnecessary or unstable algorithm switching.
7. Recording and explaining runtime decisions.
8. Evaluating whether adaptive algorithm selection improves performance compared with fixed routing approaches.

---

# 3. Core Research Question

Can a trained AI Expert Model learn to select the most suitable routing algorithm from current network conditions, and can AAFR safely switch between routing algorithms at runtime to improve browser network performance and reliability?

---

# 4. Core Capabilities

AAFR focuses on three core capabilities.

## Capability 1: AI-Based Runtime Algorithm Selection

The AI Expert Model analyzes the current NetworkState and recommends the most suitable routing algorithm.

Input:

```text
NetworkState
```

Output:

```text
Recommended Algorithm
+
Confidence
```

Flow:

```text
NetworkState
      ↓
AI Expert Model
      ↓
Recommended Algorithm
      +
Confidence
```

The Expert Model is responsible for algorithm selection.

AAFR does not independently select another algorithm.

---

## Capability 2: Adaptive Runtime Algorithm Switching

AAFR continuously receives updated network information.

When the network condition changes meaningfully, the Expert Model reevaluates the current NetworkState.

If the newly recommended algorithm differs from the currently active algorithm, AAFR determines whether the recommendation should be applied.

```text
Network Condition Changes
          ↓
NetworkState Updated
          ↓
Expert Model Reevaluation
          ↓
Recommended Algorithm
          ↓
AAFR
          ↓
Continue / Switch
```

Runtime switching applies the selected algorithm to subsequent routing decisions without requiring the browser to restart.

---

## Capability 3: Stable and Explainable Switching

AAFR must prevent unnecessary or unstable switching.

The runtime considers factors such as:

- Whether the recommended algorithm differs from the active algorithm
- Model confidence
- Whether the network condition changed meaningfully
- Minimum dwell time
- Other experimentally validated stability conditions

Every significant decision is stored in a DecisionRecord.

This allows the system to explain:

- What the network condition was
- Which algorithm was active
- Which algorithm was recommended
- What the model confidence was
- Whether a switch occurred
- Why the switch was accepted or rejected

---

# 5. Project Boundaries

AAFR focuses on adaptive routing and forwarding within a controlled experimental environment.

The project does not attempt to:

- Modify search-engine ranking or search results
- Train a large language model
- Replace Internet-wide routing
- Replace BGP
- Control arbitrary ISP routers
- Perform AI inference for every packet
- Switch algorithms for every individual browser request
- Automatically retrain the Expert Model during normal browsing
- Allow the AI model to create new routing algorithms autonomously

The Qt browser acts as the application and workload source.

The controlled routing environment provides the network layer in which custom routing decisions can actually be applied.

---

# 6. Supported Routing Algorithms

AAFR supports ten routing and optimization algorithms.

## Classical Algorithms

1. Dijkstra
2. A*
3. Bellman-Ford

## Metaheuristic and Nature-Inspired Algorithms

4. Ant Colony Optimization (ACO)
5. Particle Swarm Optimization (PSO)
6. Genetic Algorithm (GA)
7. Artificial Bee Colony (ABC)
8. Grey Wolf Optimizer (GWO)
9. Firefly Algorithm
10. Whale Optimization Algorithm (WOA)

All algorithms follow a common routing interface so that AAFR can change the active implementation without changing the overall runtime architecture.

---

# 7. Datasets

AAFR uses three fixed datasets.

## 7.1 Network Dataset

File:

`network_dataset_raw.csv`

Size:

- 3,500 network scenarios
- 45 columns

The dataset represents network conditions across seven scenario categories:

- A_Excellent
- B_Congested
- C_Unreliable
- D_Sparse
- E_Dense
- F_Dynamic
- G_Large

Each category contains 500 scenarios.

The dataset contains information related to:

- Latency
- Throughput
- Packet loss
- Jitter
- Packet statistics
- Packet delivery
- Queue behaviour
- Link utilization
- Network topology
- Link failures
- Recoveries
- Rerouting

---

## 7.2 Routing Benchmark Dataset

File:

`routing_benchmark_3500.csv`

Size:

- 35,000 rows
- 3,500 scenarios
- 10 algorithm results per scenario

Main columns:

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

The benchmark records how each of the ten algorithms performed for each network scenario.

Conceptually:

```text
One Network Scenario
        ↓
Evaluate 10 Algorithms
        ↓
Performance Results
        ↓
Algorithm Comparison
```

---

## 7.3 Expert Dataset

File:

`routing_expert_dataset.csv`

Size:

- 3,500 rows
- 58 columns

The Expert Dataset combines network information with algorithm-selection targets and comparison information.

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

The Expert Dataset is used for supervised Expert Model development after feature and target validation.

---

# 8. Dataset Relationship

The datasets form the following relationship:

```text
Network Dataset
      ↓
Network Conditions
      │
      │
      ↓
Routing Benchmark
      ↓
Performance of 10 Algorithms
      │
      │
      ↓
Expert Dataset
      ↓
Network Features + Algorithm Targets
      ↓
AI Expert Model Training
```

---

# 9. Algorithm Suitability

AAFR does not assume that one algorithm is universally best.

The most suitable algorithm depends on the current network condition and measured performance.

For each network scenario:

```text
Same Network Condition
        ↓
10 Algorithms
        ↓
Performance Measurements
        ↓
Objective Comparison
        ↓
Most Suitable Algorithm
```

The benchmark provides measurements including:

- Latency
- Loss
- Bottleneck bandwidth
- Path delay variation
- Hop count
- Algorithm runtime
- Objective

The exact objective definition and target-generation rules must be preserved from the dataset-generation methodology.

Where algorithms provide equivalent or sufficiently similar route quality, computational runtime may also be relevant to final algorithm suitability according to the dataset-generation procedure.

---

# 10. AI Expert Model

The Expert Model is formulated as a supervised multiclass classification problem.

```text
X = NetworkState Features

Y = Suitable Routing Algorithm
```

Conceptually:

```text
f(NetworkState)
       ↓
Recommended Algorithm
```

Runtime output:

```json
{
    "recommended_algorithm": "PSO",
    "confidence": 0.87
}
```

The Expert Model determines which algorithm should be recommended.

AAFR determines whether and when the recommendation should be applied.

---

# 11. Model Inputs

Only information available before algorithm selection and reproducible within the runtime experimental environment should be used as model input.

Potential feature groups include:

## Network Performance

- Latency
- Throughput
- Packet loss
- Jitter
- Packet delivery characteristics
- Traffic load

## Congestion

- Queue delay
- Queue occupancy
- Link utilization

## Topology

- Number of nodes
- Number of edges
- Network density
- Average node degree
- Diameter
- Clustering characteristics
- Connectivity characteristics

## Dynamic Network State

- Link failures
- Link recoveries
- Rerouting activity

## Routing Context

- Source
- Destination
- Source and destination properties
- Relevant route context

The final feature set must maintain consistency between training and runtime.

---

# 12. Data Leakage Prevention

Target or post-decision information must not be used as normal Expert Model input.

Examples include:

```text
target_algo_*
invoke_metaheuristic
best_objective
best_runtime_ms
qos_improvement_pct
```

The model must learn the relationship between network conditions and algorithm suitability rather than receiving information that directly reveals the answer.

---

# 13. Confidence

The Expert Model produces a confidence score with each recommendation.

Example:

```text
Dijkstra       0.05
A*             0.02
Bellman-Ford   0.01
ACO            0.06
PSO            0.78
GA             0.01
ABC            0.01
GWO            0.03
Firefly        0.01
WOA            0.03
```

Result:

```text
Recommended Algorithm = PSO
Confidence = 0.78
```

Confidence represents the strength of the model's prediction.

The confidence output must be evaluated and calibrated where necessary before being interpreted as a reliable probability.

AAFR consumes confidence as one signal within its stability logic.

---

# 14. Expert Model Development

Initial model candidates include:

- Majority-class baseline
- Logistic Regression
- Decision Tree
- Random Forest
- Gradient Boosting / XGBoost

The selected model will be determined experimentally.

Because the Expert Dataset is structured tabular data, deep neural networks are not required unless later experiments demonstrate a clear reason for using them.

---

# 15. Expert Model Evaluation

Because the target classes are imbalanced, accuracy alone is insufficient.

Model evaluation includes:

- Macro F1
- Balanced Accuracy
- Weighted F1
- Per-class Precision
- Per-class Recall
- Confusion Matrix
- Inference Latency
- Confidence Calibration

The model must be evaluated on unseen data.

---

# 16. NetworkState

NetworkState is the common representation connecting network monitoring with the Expert Model.

```text
Network Sources
      ↓
Network Monitor
      ↓
NetworkState
      ↓
Expert Model
```

Conceptually:

```text
NetworkState

├── Network Performance
├── Traffic State
├── Congestion State
├── Topology State
├── Dynamic / Failure State
└── Routing Context
```

The final fields must correspond to the finalized Expert Model features.

---

# 17. Network Monitoring

Network monitoring combines information from multiple sources.

```text
Qt Browser Measurements ─────┐
                              │
TShark Measurements ──────────┼──→ Network Monitor
                              │
Controlled Network State ─────┘
                              ↓
                         NetworkState
```

## Qt Browser

Provides application-level observations such as:

- Request timing
- Request failures
- Timeouts
- Bytes transferred
- Resource loading information
- Browser-level performance

## TShark

Provides packet and transport-level observations such as:

- Packet timing
- Packet counts
- Traffic rate
- TCP behaviour
- Retransmission information
- Protocol information
- Source and destination information

## Controlled Network

Provides network information that cannot reliably be obtained from ordinary browser traffic alone, such as controlled topology and internal routing state where required.

---

# 18. Wireshark

Wireshark GUI is used for:

- Packet inspection
- Debugging
- Measurement validation
- Traffic analysis
- Experimental demonstrations

Wireshark does not perform routing, algorithm selection, or network simulation.

TShark is used for automated packet-level runtime observation.

---

# 19. Monitoring Strategy

The Expert Model is not invoked for every packet.

Measurements are aggregated over a monitoring window.

```text
Raw Measurements
      ↓
Monitoring Window
      ↓
Feature Aggregation
      ↓
NetworkState
      ↓
Change Detection
```

The monitoring-window duration and meaningful-change criteria are determined experimentally.

---

# 20. AAFR Runtime

AAFR operates continuously while the user uses the browser.

The browser remains the foreground application.

Monitoring, AI inference, switching, and logging operate in the background.

```text
User
 ↓
Qt Browser
 ↓
Network Traffic
 ↓
Network Monitor
 ↓
NetworkState
 ↓
Expert Model
 ↓
Recommended Algorithm + Confidence
 ↓
AAFR
 ↓
Continue / Switch
 ↓
Active Algorithm
 ↓
Routing Decision
 ↓
Controlled Network
 ↓
Browser Continues
```

---

# 21. Runtime Switching Logic

```text
NetworkState
      ↓
Meaningful Change?
   /        \
 NO          YES
 ↓            ↓
Continue   Expert Model
                ↓
       Algorithm + Confidence
                ↓
             AAFR
                ↓
      Same as Active?
         /          \
       YES           NO
        ↓             ↓
    Continue     Stability Checks
                    /       \
                  FAIL      PASS
                   ↓          ↓
                Continue    Switch
                              ↓
                     Update Active Algorithm
                              ↓
                       DecisionRecord
```

The newly activated algorithm is used for subsequent routing decisions.

---

# 22. Stability Controls

AAFR prevents unnecessary or unstable switching.

Initial stability considerations include:

1. Recommended algorithm differs from the active algorithm.
2. Model confidence is sufficient.
3. Network change is meaningful.
4. Minimum dwell time has been satisfied.

These controls do not select another algorithm.

They determine whether the Expert Model's recommendation should be applied at that time.

---

# 23. Minimum Dwell Time

Minimum dwell time defines how long an algorithm remains active before another normal switch can occur.

```text
Dijkstra
    ↓
Switch
    ↓
PSO
    ↓
Minimum Dwell Period
    ↓
Another Switch May Be Considered
```

This reduces rapid algorithm oscillation.

The actual dwell time is determined experimentally.

---

# 24. Controlled Routing Environment

The ten algorithms require a network environment in which their routing decisions can actually be applied.

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
Selected Network Path
```

Qt WebEngine cannot instruct arbitrary Internet routers to use Dijkstra, PSO, ACO, GWO, or other custom algorithms.

The controlled routing environment therefore provides the topology and forwarding control required for experimentation.

---

# 25. DecisionRecord

Every significant runtime decision is represented by a DecisionRecord.

It contains information such as:

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

---

# 26. Explainability

AAFR should be able to explain:

1. What was the network condition?
2. Which algorithm was active?
3. Which algorithm did the Expert Model recommend?
4. What was the confidence?
5. Did the network meaningfully change?
6. Were stability conditions satisfied?
7. Was the recommendation applied?
8. Why was the switch accepted or rejected?

Explainability is therefore built from:

```text
NetworkState
      +
Model Recommendation
      +
Confidence
      +
Current Algorithm
      +
Stability Checks
      +
Final Decision
      ↓
DecisionRecord
```

---

# 27. Experiment Scenarios

The primary network scenario families are:

1. Excellent
2. Congested
3. Unreliable
4. Sparse
5. Dense
6. Dynamic
7. Large

Runtime experiments can also transition between conditions.

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

This allows the system's adaptive behaviour to be evaluated.

---

# 28. Evaluation

AAFR is evaluated at three levels.

## AI Model

- Macro F1
- Balanced Accuracy
- Weighted F1
- Per-class Precision
- Per-class Recall
- Confusion Matrix
- Inference Latency
- Confidence Calibration

## Routing and Network

- Objective
- Latency
- Packet loss
- Bandwidth / Throughput
- Path delay variation
- Hop count
- Algorithm runtime

## Runtime and Browser

- Number of algorithm switches
- Rejected switches
- Unnecessary switches
- Switching overhead
- Runtime stability
- Request completion
- Request failures
- Resource loading performance
- Browser loading performance

---

# 29. Baseline Comparison

AAFR must be experimentally compared against fixed or baseline routing approaches.

```text
Fixed / Baseline Routing
           VS
      Adaptive AAFR
```

The research does not assume that AAFR is automatically better.

Improvement must be demonstrated through experimental results.

---

# 30. End-to-End Architecture

```text
User
 ↓
Qt Browser
 ↓
Browser Traffic
 ↓
Browser Measurements + TShark + Controlled Network State
 ↓
Network Monitor
 ↓
NetworkState
 ↓
Meaningful Change Detection
 ↓
AI Expert Model
 ↓
Recommended Algorithm + Confidence
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
 ↓
User

             +
             ↓
       DecisionRecord
             ↓
    Experiments / Analysis
```

---

# 31. Codebase Architecture

```text
aafr/
│
├── datasets/
│
├── algorithms/
│   ├── dijkstra/
│   ├── astar/
│   ├── bellman_ford/
│   ├── aco/
│   ├── pso/
│   ├── ga/
│   ├── abc/
│   ├── gwo/
│   ├── firefly/
│   └── woa/
│
├── expert_model/
│   ├── features/
│   ├── training/
│   ├── evaluation/
│   ├── inference/
│   └── models/
│
├── network/
│   ├── monitor/
│   ├── tshark/
│   ├── browser_metrics/
│   └── network_state/
│
├── runtime/
│   ├── aafr_runtime.py
│   ├── switching.py
│   ├── stability.py
│   └── decision_record.py
│
├── routing/
│   ├── topology/
│   ├── forwarding/
│   └── controller/
│
├── browser/
│   └── qt_browser/
│
├── experiments/
│
├── tests/
│
└── configs/
```

---

# 32. Development Order

```text
Research Requirements
        ↓
Dataset Audit
        ↓
Algorithm Suitability Definition
        ↓
Expert Model Target Selection
        ↓
Feature Selection
        ↓
NetworkState Definition
        ↓
Routing Algorithm Implementation
        ↓
Algorithm Validation
        ↓
Expert Model Training
        ↓
Expert Model Evaluation
        ↓
Network Monitor
        ↓
AAFR Runtime
        ↓
Stability Controls
        ↓
DecisionRecord
        ↓
Controlled Routing Environment
        ↓
Qt Browser
        ↓
TShark Integration
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

# 33. Decisions To Finalize

The following decisions must be finalized from the dataset methodology or through experimentation:

1. Exact benchmark objective definition.
2. Exact target-generation and tie-breaking procedure.
3. Expert Model target selection:
   - target_algo_eps0
   - target_algo_eps0_5
   - target_algo_eps1
   - target_algo_eps2
4. Final runtime-measurable model feature set.
5. Confidence calibration method.
6. Meaningful network-change criteria.
7. Minimum dwell time.
8. Confidence threshold.
9. Monitoring-window duration.
10. Exact controlled routing environment.

These values should not be selected arbitrarily.

---

# 34. Project Principle

AAFR follows the cycle:

```text
Observe
→ Represent
→ Predict
→ Stabilize
→ Switch
→ Route
→ Measure
→ Repeat
```

The responsibilities remain separated:

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
```