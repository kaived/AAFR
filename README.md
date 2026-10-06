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

## Repository Structure

```text
aafr/
│
├── datasets/
│   ├── raw/
│   │   ├── network_dataset_raw.csv
│   │   ├── routing_benchmark_3500.csv
│   │   └── routing_expert_dataset.csv
│   │
│   ├── processed/
│   └── README.md
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
├── configs/
│
├── PROJECT.md
├── REQUIREMENTS.md
├── ARCHITECTURE.md
└── README.md
```

Directories are added as their corresponding components are implemented.

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