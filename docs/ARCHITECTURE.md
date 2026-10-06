# AAFR Architecture

## Adaptive Application-Layer Forwarding Runtime

This document defines the system architecture of AAFR, including the browser, network monitoring, AI Expert Model, runtime controller, routing algorithms, controlled routing environment, stability mechanism, and decision recording.

AAFR is designed as an adaptive browser networking system in which a trained AI Expert Model observes the current network state and recommends the most suitable routing algorithm.

AAFR manages the runtime execution of that recommendation by maintaining the active algorithm, controlling algorithm switching, and recording the reason behind each decision.

---

# 1. Architectural Goal

The architecture is designed around one continuous process:

```text
Observe
   ↓
Build Network State
   ↓
Predict Suitable Algorithm
   ↓
Evaluate Runtime Conditions
   ↓
Continue or Switch
   ↓
Execute Routing
   ↓
Observe Again
```

The user continues using the browser normally while monitoring, inference, switching, and logging operate in the background.

---

# 2. High-Level Architecture

```text
                         USER
                          │
                          ↓
                  ┌───────────────┐
                  │   Qt Browser  │
                  └───────┬───────┘
                          │
                    Browser Traffic
                          │
             ┌────────────┴────────────┐
             │                         │
             ↓                         ↓
    Browser Measurements            TShark
             │                    Packet Capture
             │                         │
             └────────────┬────────────┘
                          │
                          ↓
                ┌──────────────────┐
                │ Network Monitor  │
                └────────┬─────────┘
                         │
                         ↓
                  ┌─────────────┐
                  │NetworkState │
                  └──────┬──────┘
                         │
                         ↓
                Meaningful Change?
                    /          \
                  NO            YES
                  │              │
                  │              ↓
                  │     ┌────────────────┐
                  │     │ AI Expert Model│
                  │     └───────┬────────┘
                  │             │
                  │             ↓
                  │      Recommended Algorithm
                  │          + Confidence
                  │             │
                  │             ↓
                  │      ┌──────────────┐
                  └─────→│ AAFR Runtime │
                         └───────┬──────┘
                                 │
                         Same as Active?
                           /          \
                         YES           NO
                          │             │
                       Continue         ↓
                                Stability Checks
                                  /          \
                                FAIL         PASS
                                 │             │
                              Continue       Switch
                                               │
                                               ↓
                                      ┌────────────────┐
                                      │Active Algorithm│
                                      └───────┬────────┘
                                              │
                                              ↓
                                      Routing Decision
                                              │
                                              ↓
                               ┌────────────────────────┐
                               │ Controlled Routing     │
                               │ Environment            │
                               └────────────┬───────────┘
                                            │
                                            ↓
                                      Network Traffic
                                            │
                                            ↓
                                      Website/Server
                                            │
                                            ↓
                                        Response
                                            │
                                            ↓
                                        Qt Browser
                                            │
                                            ↓
                                           User
```

The process repeats continuously while the browser is running.

---

# 3. Architectural Layers

AAFR is divided into seven major architectural layers.

```text
┌──────────────────────────────────────────────┐
│ 1. Browser Layer                            │
├──────────────────────────────────────────────┤
│ 2. Network Observation Layer                │
├──────────────────────────────────────────────┤
│ 3. Network State Layer                      │
├──────────────────────────────────────────────┤
│ 4. Intelligence Layer                       │
├──────────────────────────────────────────────┤
│ 5. AAFR Runtime Layer                       │
├──────────────────────────────────────────────┤
│ 6. Routing and Forwarding Layer             │
├──────────────────────────────────────────────┤
│ 7. Observability and Experiment Layer       │
└──────────────────────────────────────────────┘
```

---

# 4. Browser Layer

The Browser Layer provides the user-facing application.

The browser is built using:

```text
Qt
+
Qt WebEngine
```

The browser allows normal activities such as:

- Searching
- Opening websites
- Clicking links
- Loading resources
- Opening multiple tabs
- Navigating between pages

The browser also generates the real application traffic used by the AAFR system.

Architecture:

```text
User
 ↓
Qt Browser
 ↓
Browser Requests
 ↓
Network
```

AAFR operates in the background and should not require the user to manually select routing algorithms.

---

# 5. Network Observation Layer

The Network Observation Layer determines what is currently happening in the network.

It combines information from multiple sources.

```text
                   Network Observation

          ┌──────────────┼───────────────┐
          ↓              ↓               ↓
   Qt Browser          TShark       Controlled
   Measurements        Capture       Network
          │              │               │
          └──────────────┼───────────────┘
                         ↓
                  Network Monitor
```

---

# 6. Browser Measurements

The Qt browser provides application-level network information.

Potential measurements include:

- Request timing
- Request completion
- Request failures
- Timeouts
- Bytes transferred
- Resource loading information
- Browser-level performance information

These measurements describe network behaviour from the application's perspective.

---

# 7. TShark Integration

TShark runs as a background packet-observation component.

Conceptually:

```text
Browser Traffic
      ↓
Network Interface
      ↓
TShark
      ↓
Packet Measurements
      ↓
Network Monitor
```

TShark may provide information such as:

- Packet timing
- Packet counts
- Traffic rate
- TCP behaviour
- Retransmission information
- Protocol information
- Source and destination information

TShark does not make algorithm-selection decisions.

TShark does not perform routing.

Its responsibility is network observation.

---

# 8. Wireshark

Wireshark GUI is separate from the automated runtime.

It is primarily used for:

- Packet inspection
- Debugging
- Traffic analysis
- Measurement verification
- Experimental validation
- Demonstrations

The relationship is:

```text
Wireshark
    ↓
Human Analysis


TShark
    ↓
Automated Runtime Monitoring
```

Wireshark is not used as a network simulator.

---

# 9. Network Monitor

The Network Monitor combines raw measurements from the available network-observation sources.

```text
Browser Measurements ──────┐
                            │
TShark Measurements ────────┼──→ Network Monitor
                            │
Controlled Network State ───┘
```

The Network Monitor is responsible for:

- Collecting measurements
- Aggregating measurements
- Maintaining monitoring windows
- Calculating required features
- Detecting network changes
- Producing NetworkState

It does not select routing algorithms.

---

# 10. Monitoring Window

AAFR does not perform AI inference for every packet.

Measurements are collected over a monitoring window.

```text
Raw Measurements
      ↓
Monitoring Window
      ↓
Feature Aggregation
      ↓
NetworkState
```

This prevents individual packet fluctuations from unnecessarily triggering Expert Model inference.

The monitoring-window duration is configurable and should be determined experimentally.

---

# 11. Network State Layer

`NetworkState` provides the common representation of the current network condition.

It acts as the interface between:

```text
Network Monitor
      ↓
NetworkState
      ↓
AI Expert Model
```

Potential information represented by NetworkState includes:

```text
NetworkState

├── Network Performance
│   ├── Latency
│   ├── Throughput
│   ├── Packet Loss
│   └── Jitter
│
├── Traffic State
│   ├── Packet Delivery
│   └── Traffic Load
│
├── Congestion State
│   ├── Queue Delay
│   ├── Queue Occupancy
│   └── Link Utilization
│
├── Topology State
│   ├── Nodes
│   ├── Edges
│   ├── Density
│   └── Connectivity
│
├── Dynamic State
│   ├── Link Failures
│   ├── Link Recoveries
│   └── Rerouting Activity
│
└── Routing Context
    ├── Source
    ├── Destination
    └── Relevant Path Context
```

The final NetworkState fields must match the finalized Expert Model input features.

---

# 12. Training and Runtime Consistency

Every network feature used by the Expert Model must have a consistent definition between training and runtime.

```text
Training Feature
      ↕
Runtime Feature
```

The following should remain consistent:

- Meaning
- Unit
- Scale
- Calculation
- Preprocessing

For example:

```text
Training:
latency = defined measurement

Runtime:
latency = same defined measurement
```

Features that cannot be reproduced reliably at runtime should not be used as normal runtime model inputs.

---

# 13. Network Change Detection

The Network Monitor continuously updates NetworkState.

However, small fluctuations should not cause constant Expert Model reevaluation.

The architecture therefore includes meaningful-change detection.

```text
Previous NetworkState
         +
Current NetworkState
         ↓
Compare
         ↓
Meaningful Change?
     /          \
   NO            YES
   ↓              ↓
Continue      Expert Model
```

The exact thresholds are configurable and determined experimentally.

---

# 14. Intelligence Layer

The Intelligence Layer contains the trained AI Expert Model.

Its responsibility is:

> Given the current NetworkState, recommend the most suitable routing algorithm.

Architecture:

```text
NetworkState
      ↓
AI Expert Model
      ↓
Class Prediction
      +
Confidence
```

Example output:

```json
{
    "recommended_algorithm": "PSO",
    "confidence": 0.87
}
```

The Expert Model recommends the algorithm.

It does not perform the actual switch.

---

# 15. Expert Model

The Expert Model is trained as a supervised multiclass classification model.

Conceptually:

```text
f(NetworkState)
        ↓
Routing Algorithm
```

Possible algorithm classes are determined by the finalized training target and supported routing algorithms.

Model candidates include:

- Logistic Regression
- Decision Tree
- Random Forest
- Gradient Boosting / XGBoost

The selected trained model is loaded by the runtime inference component.

---

# 16. Expert Model Training Architecture

Model training occurs separately from the live browser runtime.

```text
Network Dataset
       +
Routing Benchmark Dataset
       +
Expert Dataset
       ↓
Dataset Validation
       ↓
Feature Selection
       ↓
Target Selection
       ↓
Preprocessing
       ↓
Train Candidate Models
       ↓
Evaluate Models
       ↓
Select Expert Model
       ↓
Save Model
       ↓
Runtime Inference
```

The live browser does not retrain the model during normal operation.

---

# 17. Dataset Architecture

AAFR uses three primary datasets.

```text
network_dataset_raw
        ↓
Network Conditions
        │
        │
        ├───────────────┐
        │               │
        ↓               ↓
Routing Benchmark   Network Features
        │               │
        ↓               │
10 Algorithm Results    │
        │               │
        └───────┬───────┘
                ↓
        Expert Dataset
                ↓
       Expert Model Training
```

The routing benchmark contains the comparative performance of the ten algorithms.

The Expert Dataset contains the network information and algorithm-selection targets used for model development.

---

# 18. Confidence

The Expert Model produces a confidence value with its recommendation.

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

Confidence is passed to AAFR as part of the recommendation.

```text
Expert Model
      ↓
PSO
+
0.78 Confidence
      ↓
AAFR Runtime
```

AAFR uses confidence as one of its stability signals.

---

# 19. AAFR Runtime Layer

The AAFR Runtime is the central runtime controller.

It connects:

```text
NetworkState
     ↓
Expert Model
     ↓
Recommendation
     ↓
AAFR Runtime
     ↓
Active Algorithm
```

The AAFR Runtime is responsible for:

- Maintaining the active algorithm
- Receiving model recommendations
- Receiving confidence
- Checking whether the recommendation differs from the active algorithm
- Applying stability rules
- Performing approved algorithm switches
- Maintaining runtime state
- Creating DecisionRecords

AAFR does not independently choose a different algorithm from the one recommended by the Expert Model.

---

# 20. AAFR Runtime State

The runtime maintains information such as:

```text
AAFRState

├── active_algorithm
├── latest_network_state
├── recommended_algorithm
├── recommendation_confidence
├── last_switch_time
├── stability_state
└── latest_decision
```

There should always be a valid active algorithm during normal operation.

---

# 21. Runtime Decision Flow

The core runtime flow is:

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
      Same as Active Algorithm?
          /             \
        YES              NO
         ↓                ↓
      Continue      Stability Checks
                        /       \
                      FAIL      PASS
                       ↓          ↓
                    Continue    Switch
                                  ↓
                         Update Active Algorithm
                                  ↓
                           DecisionRecord
```

---

# 22. Stability Layer

The Stability Layer prevents unnecessary or unstable algorithm switching.

Initial stability signals include:

```text
Recommended Algorithm
        ↓
Different from active?
        ↓
Confidence sufficient?
        ↓
Network change meaningful?
        ↓
Minimum dwell time satisfied?
        ↓
Switch allowed
```

The stability layer does not select another routing algorithm.

Its responsibility is only to determine whether the Expert Model's recommendation should be applied immediately.

---

# 23. Minimum Dwell Time

Minimum dwell time defines how long an algorithm should remain active before another normal switch is allowed.

Example:

```text
Dijkstra
    ↓
Switch
    ↓
PSO
    ↓
Start Dwell Period
    ↓
PSO remains active
    ↓
Dwell Period Satisfied
    ↓
Another switch may be considered
```

This reduces rapid oscillation such as:

```text
Dijkstra
→ PSO
→ GWO
→ PSO
→ ACO
→ PSO
```

The actual dwell duration is experimentally determined.

---

# 24. Algorithm Registry

AAFR maintains access to all supported algorithms through an Algorithm Registry.

Conceptually:

```text
Algorithm Registry

"dijkstra"      → DijkstraRouter
"astar"         → AStarRouter
"bellman_ford"  → BellmanFordRouter
"aco"           → ACORouter
"pso"           → PSORouter
"ga"            → GARouter
"abc"           → ABCRouter
"gwo"           → GWORouter
"firefly"       → FireflyRouter
"woa"           → WOARouter
```

The runtime maintains:

```text
active_algorithm
```

When an approved switch occurs:

```text
Previous:
active_algorithm = "dijkstra"

Expert Model:
recommended_algorithm = "pso"

AAFR approves switch

New:
active_algorithm = "pso"
```

---

# 25. Common Routing Interface

All ten algorithms should expose a common interface.

Conceptually:

```text
route(
    topology,
    source,
    destination,
    network_state
)
```

The result should contain information such as:

```text
RouteResult

├── selected_path
├── path_cost
├── latency
├── loss
├── bottleneck_bandwidth
├── hops
├── algorithm_runtime
└── objective
```

The exact implementation depends on the controlled routing environment and experimental methodology.

---

# 26. Routing Algorithms

The system supports ten algorithms.

## Classical

```text
Dijkstra
A*
Bellman-Ford
```

## Metaheuristic and Nature-Inspired

```text
ACO
PSO
GA
ABC
GWO
Firefly
WOA
```

The Expert Model determines which algorithm is recommended.

AAFR determines whether an approved runtime transition should occur.

The selected algorithm performs the actual path-selection computation.

---

# 27. Routing and Forwarding Layer

The Routing and Forwarding Layer executes the selected routing strategy.

```text
AAFR
 ↓
Active Algorithm
 ↓
Path Calculation
 ↓
Selected Path
 ↓
Forwarding Controller
 ↓
Controlled Network
```

This distinction is important.

Changing:

```text
active_algorithm = "PSO"
```

is not sufficient by itself.

PSO must actually calculate a path and the controlled routing environment must apply that path to subsequent experimental traffic.

---

# 28. Controlled Routing Environment

AAFR requires a network environment in which routing decisions can actually be controlled.

```text
Qt Browser
      ↓
Browser Traffic
      ↓
Controlled Routing Environment
      ↑
      │
AAFR → Active Algorithm
```

The controlled environment must provide the information required by the routing algorithms, such as:

- Topology
- Nodes
- Edges
- Link properties
- Current link/network state
- Source
- Destination

It must also provide a mechanism for applying the selected path.

AAFR does not attempt to replace Internet-wide routing or control arbitrary ISP routers.

---

# 29. Runtime Algorithm Switching

Runtime switching changes the algorithm used for subsequent routing decisions.

Example:

```text
Current Algorithm
Dijkstra

       ↓

Network changes

       ↓

Expert Model
PSO + 0.87 confidence

       ↓

AAFR checks stability

       ↓

SWITCH

       ↓

active_algorithm = PSO

       ↓

Next routing decision

       ↓

PSO
```

Existing requests do not need to be forcibly migrated in the initial architecture.

The new algorithm applies to subsequent routing decisions.

---

# 30. DecisionRecord Architecture

Every significant decision is represented by a DecisionRecord.

Conceptually:

```text
DecisionRecord

├── timestamp
├── network_state
├── active_algorithm
├── recommended_algorithm
├── model_confidence
├── meaningful_change
├── stability_checks
├── decision
├── previous_algorithm
├── new_algorithm
└── reason
```

Possible decisions include:

```text
CONTINUE
SWITCH
REJECT_SWITCH
```

---

# 31. Explainability Flow

Explainability is produced from the complete decision context.

```text
NetworkState
      +
Expert Model Prediction
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

Example:

```text
Current Algorithm:
Dijkstra

Network:
Latency increased
Loss increased
Jitter increased

Expert Model:
PSO

Confidence:
0.87

Minimum Dwell:
Satisfied

Decision:
SWITCH

Transition:
Dijkstra → PSO
```

---

# 32. Background Runtime Architecture

The user-facing browser should not wait for the entire intelligence pipeline to finish before remaining usable.

Conceptually, the application contains independent runtime activities.

```text
┌───────────────────────────────────┐
│ Browser / UI                      │
│ User interaction                  │
└───────────────────────────────────┘

┌───────────────────────────────────┐
│ Network Monitoring                │
│ Browser + TShark + network state  │
└───────────────────────────────────┘

┌───────────────────────────────────┐
│ AI Inference                      │
│ Expert Model prediction           │
└───────────────────────────────────┘

┌───────────────────────────────────┐
│ AAFR Runtime                      │
│ Stability + switching             │
└───────────────────────────────────┘
```

These are logical execution responsibilities and do not require exactly four operating-system threads.

The implementation may use:

- Background workers
- Threads
- Processes
- Event-driven execution
- Asynchronous communication

depending on the component.

---

# 33. Browser Request Flow

When a user performs a search or opens a website:

```text
User Action
    ↓
Qt Browser
    ↓
Browser Request
    ↓
Active Routing Environment
    ↓
Network
    ↓
Server
    ↓
Response
    ↓
Qt Browser
    ↓
Render
```

At the same time:

```text
Traffic
   ↓
Network Monitor
   ↓
NetworkState
   ↓
Expert Model when required
   ↓
AAFR
```

The monitoring and decision process therefore operates alongside normal browsing.

---

# 34. Example End-to-End Runtime

Assume:

```text
Active Algorithm = Dijkstra
```

The user searches:

```text
"latest artificial intelligence research"
```

The browser generates traffic.

The Network Monitor observes:

```text
Latency     = 165 ms
Loss        = 4.2%
Jitter      = 29 ms
Throughput  = 17 Mbps
```

A meaningful network change is detected.

```text
NetworkState
      ↓
Expert Model
```

The model returns:

```text
Recommended Algorithm = PSO
Confidence = 0.88
```

AAFR receives:

```text
Current = Dijkstra
Recommended = PSO
```

Stability checks pass.

AAFR performs:

```text
Dijkstra → PSO
```

PSO becomes the active algorithm.

The next routing decision uses PSO.

The browser continues operating without restarting.

---

# 35. Failure Architecture

AAFR should maintain a valid operating state when individual components fail.

## Expert Model Failure

```text
Inference Failure
      ↓
Keep Current Algorithm
      ↓
Record Failure
```

## Recommendation Failure

```text
Invalid Recommendation
      ↓
Reject
      ↓
Keep Current Algorithm
```

## Switching Failure

```text
Switch Attempt
      ↓
Failure
      ↓
Retain / Restore Valid Algorithm
      ↓
Record Failure
```

## TShark Failure

```text
TShark unavailable
      ↓
Record monitoring failure
      ↓
Use remaining valid measurement sources
where possible
```

The system should not intentionally enter a state with no valid active routing algorithm.

---

# 36. Observability Architecture

The system should provide runtime visibility into:

```text
Current NetworkState

Current Algorithm

Expert Model Recommendation

Confidence

Last Switch

Switching Decision

Decision Reason

Routing Result

Runtime Errors
```

This information supports:

- Development
- Debugging
- Research analysis
- Demonstration
- Experiment validation

---

# 37. Experiment Architecture

Experiments evaluate the complete pipeline.

```text
Network Scenario
      ↓
Browser Workload
      ↓
Routing Configuration
      ↓
Network Measurements
      ↓
AAFR Decisions
      ↓
Routing Results
      ↓
Browser Results
      ↓
Experiment Record
```

The primary network scenario families are:

```text
Excellent
Congested
Unreliable
Sparse
Dense
Dynamic
Large
```

Experiments may also transition between scenarios:

```text
Excellent
    ↓
Congested
    ↓
Unreliable
    ↓
Excellent
```

This evaluates whether AAFR adapts when conditions change.

---

# 38. Baseline Architecture

The experimental architecture should support both:

```text
Fixed Routing
```

and:

```text
AAFR Adaptive Routing
```

This allows:

```text
Same Scenario
     ↓
Same Browser Workload
     ↓
┌────────────────┬────────────────┐
│ Fixed Routing  │ AAFR Adaptive  │
└───────┬────────┴────────┬───────┘
        ↓                 ↓
     Results           Results
        └────────┬────────┘
                 ↓
              Compare
```

The purpose is to experimentally determine whether adaptive selection provides measurable improvement.

---

# 39. Codebase Architecture

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

# 40. Component Communication

The primary interfaces are:

```text
Qt Browser
      ↓
Network Monitor

TShark
      ↓
Network Monitor

Controlled Network
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
Algorithm Recommendation + Confidence

Recommendation
      ↓
AAFR Runtime

AAFR Runtime
      ↓
Algorithm Registry

Active Algorithm
      ↓
Routing Controller

Routing Controller
      ↓
Controlled Network

AAFR Runtime
      ↓
DecisionRecord
```

Each component should have one clearly defined responsibility.

---

# 41. Complete End-to-End Architecture

```text
                           USER
                            │
                            ↓
                       Qt Browser
                            │
                            ↓
                      Browser Traffic
                            │
              ┌─────────────┼─────────────┐
              │             │             │
              ↓             ↓             ↓
         Browser         TShark      Controlled
         Metrics         Capture     Network State
              │             │             │
              └─────────────┼─────────────┘
                            ↓
                     Network Monitor
                            │
                            ↓
                       NetworkState
                            │
                            ↓
                   Meaningful Change?
                       /          \
                     NO            YES
                     │              │
                     │              ↓
                     │       AI Expert Model
                     │              │
                     │              ↓
                     │      Algorithm Prediction
                     │        + Confidence
                     │              │
                     │              ↓
                     │         AAFR Runtime
                     │              │
                     │       Same as Active?
                     │          /        \
                     │        YES         NO
                     │         │           │
                     │         │      Stability Check
                     │         │         /       \
                     │         │       FAIL      PASS
                     │         │        │          │
                     │         │        │        SWITCH
                     │         │        │          │
                     └─────────┴────────┴──────────┘
                                      │
                                      ↓
                               Active Algorithm
                                      │
                                      ↓
                              Routing Controller
                                      │
                                      ↓
                              Selected Network Path
                                      │
                                      ↓
                          Controlled Routing Environment
                                      │
                                      ↓
                                  Destination
                                      │
                                      ↓
                                   Response
                                      │
                                      ↓
                                  Qt Browser
                                      │
                                      ↓
                                     USER

                                      +
                                      │
                                      ↓
                               DecisionRecord
                                      │
                                      ↓
                         Logs / Experiments / Analysis
```

---

# 42. Architectural Responsibility Summary

| Component | Responsibility |
|---|---|
| Qt Browser | User-facing browsing and application traffic |
| Browser Metrics | Application-level network observations |
| TShark | Packet and transport-level observations |
| Wireshark | Human packet inspection and validation |
| Controlled Network | Provides controllable topology, state, and forwarding |
| Network Monitor | Collects and aggregates network measurements |
| NetworkState | Standard representation of current network condition |
| Expert Model | Recommends the most suitable algorithm |
| Confidence | Represents strength of model recommendation |
| AAFR Runtime | Manages runtime continuation and switching |
| Stability Layer | Prevents unnecessary switching |
| Algorithm Registry | Provides access to supported algorithms |
| Routing Algorithm | Calculates the selected path |
| Routing Controller | Applies the routing decision |
| DecisionRecord | Records and explains runtime decisions |
| Experiment Layer | Evaluates and compares system behaviour |

---

# 43. Architectural Principle

The architecture follows a strict separation of responsibilities:

```text
Network Monitor
      ↓
OBSERVES

NetworkState
      ↓
REPRESENTS

Expert Model
      ↓
SELECTS / RECOMMENDS

AAFR
      ↓
CONTROLS THE TRANSITION

Routing Algorithm
      ↓
CALCULATES THE PATH

Routing Controller
      ↓
APPLIES THE PATH

DecisionRecord
      ↓
EXPLAINS

Qt Browser
      ↓
USES THE NETWORK
```

The central runtime cycle is:

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

This cycle continues while the user uses the browser.