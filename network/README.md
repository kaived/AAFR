# Network

Network observation, capture, browser metrics, and state-estimation modules.

V0.3 contents:

- `telemetry/`: typed, versioned telemetry events, the bounded telemetry
  pipeline and the background JSONL writer (plain C++17, no Qt).
- `tshark/`: TShark field parsing and per-interval aggregation (plain C++17)
  and the optional `QProcess` capture adapter (Qt Core).
- `monitor/`: the `NetworkMonitor` interface that capture sources implement.

`network_state/` and `browser_metrics/` are still placeholders: V0.3 does not
derive a `NetworkState` from these measurements. See the
[browser README](../browser/qt_webengine/README.md#v03-observability-and-developer-tools)
for metrics, limitations and configuration.
