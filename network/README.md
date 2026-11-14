# Network

Network observation, capture, browser metrics, and state-estimation modules.

V0.3 contents:

- `telemetry/`: typed, versioned telemetry events, the bounded telemetry
  pipeline and the background JSONL writer (plain C++17, no Qt).
- `tshark/`: TShark field parsing and per-interval aggregation (plain C++17)
  and the optional `QProcess` capture adapter (Qt Core).
- `monitor/`: the `NetworkMonitor` interface that capture sources implement.

V0.4 contents:

- `network_state/`: the `NetworkState` contract, feature validation,
  normalization, rolling-window extraction, quality classification, the
  incremental state builder, `NetworkConstraints`, and the
  `NetworkStateService` that runs the update pipeline on a worker thread
  (plain C++17, no Qt; library `aafr-network-state`).

`browser_metrics/` is still a placeholder: browser measurements reach the
state through V0.3 telemetry events. See the browser README for
[V0.3 metrics and configuration](../browser/qt_webengine/README.md#v03-observability-and-developer-tools)
and the
[V0.4 schema, features and rules](../browser/qt_webengine/README.md#v04-networkstate-constraints-and-algorithm-eligibility).
