# Datasets

Traffic captures, browser workloads, generated telemetry extracts, and cleaned
experiment datasets belong here. Keep raw and derived data clearly separated.

## V0.7.1 external dataset assessment

Three CSV files from the external reference repository
(`oddproblem/Adaptive-runtime` at `6b6c008`, no license, see
[`runtime/README.md`](../runtime/README.md#v071-external-runtime-integration))
were profiled in place (`D:\AAFR-reference\Adaptive-runtime`). **None was
copied into this repository, merged into an AAFR dataset, or used for
training.** "Confirmed" below means computed from the files; "assumed" means
inferred from names or the reference's documents.

### Schemas and quality (confirmed)

| File | Rows × columns | Keys and structure | Missing / duplicates |
|---|---|---|---|
| `network_dataset_raw.csv` | 3 500 × 45 | `scenario_id` unique; `scenario_category` 7 classes × 500 (`A_Excellent`, `B_Congested`, `C_Unreliable`, `D_Sparse`, `E_Dense`, `F_Dynamic`, `G_Large`); `score` 0–100 | none / none |
| `routing_benchmark_3500.csv` | 35 000 × 11 | the same 3 500 `scenario_id`s × 10 algorithms (`Dijkstra`, `A*`, `Bellman-Ford`, `ACO`, `PSO`, `GA`, `ABC`, `GWO`, `Firefly`, `WOA`); columns `source, destination, latency, bottleneck_bandwidth, loss, path_delay_std, hops, runtime, objective` | none / none |
| `real_benchmark.csv` | 450 × 11 | 45 scenarios × the same 10 algorithms; one source (`Local-Host`), three destinations (`1.1.1.1` and two others) | none / none |

Ranges and units (units are not stated in the files; ms, Mb/s and ratios
are assumed from the column names):

- `network_dataset_raw`: latency 2.7–135 (median 21.8), throughput 3.9–119,
  loss 0–0.72, `packet_delivery_ratio` 0.27–1, graphs of 20–650 nodes.
  `mean_link_utilization` reaches 19.4 and `max_link_utilization` 106, so
  the "utilization" columns are not fractions (percent, or a different
  definition). `path_stretch` is constant 1.053 and
  `simulated_duration_sec` constant 1.5 on every row, which marks the data
  as **simulated**, not measured.
- `routing_benchmark_3500`: latency 2.1–135, bottleneck bandwidth 5–997,
  loss 2e-5–0.79, hops 1–18. In 2 920 of 3 500 scenarios all ten
  algorithms report identical path metrics. Dijkstra reaches the minimum
  `objective` in only 3 050 scenarios while ABC reaches it in 3 484, so
  `objective` is **not** the additive cost that Dijkstra minimizes: it is a
  different, non-additive objective, and "best algorithm" labels from it do
  not transfer to AAFR's J(P).
- `real_benchmark`: only 27 distinct latencies and 8 distinct bandwidths in
  450 rows; 44 of 45 scenarios have identical metrics for all algorithms.
  Paths toward public Internet hosts cannot be steered by a host, so the
  "path" differences cannot have been forwarded; the rows are at best
  traceroute-style observations of one path, labelled per algorithm.

### Leakage and provenance risks

- The reference's XGBoost regressor predicts `100 - score` from delay,
  loss and 1/bandwidth with R² 0.997: `score` is essentially a
  deterministic function of those inputs, so any model trained on it learns
  the formula, not network behaviour.
- `scenario_category` is a generator setting; using it as a feature or
  label encodes the simulator, not observations.
- The same `scenario_id`s appear in `network_dataset_raw` and
  `routing_benchmark_3500`; any split must be by scenario to avoid leakage.
- No seeds, generator code or capture provenance are included.

### Compatibility with AAFR measurements

AAFR measures per-request latency and failure over its own controlled
relays (V0.6 probes and outcomes) and host-wide TCP features (V0.4). None
of these datasets has per-request samples, provenance per value, or AAFR's
J(P) v1; utilization, queue and fairness columns have no AAFR counterpart
(see [`expert_model/README.md`](../expert_model/README.md)).

### Suitability

| Use | `network_dataset_raw` | `routing_benchmark_3500` | `real_benchmark` |
|---|---|---|---|
| Controlled benchmarks | No (simulated, other objective) | Limited: graph-free summary only; cannot be replayed without topologies | No |
| Statistical experiments | No (one aggregate row per scenario, no samples) | No (no repeated samples) | No |
| Runtime validation | No | No | No |
| Future model evaluation | Only as an out-of-distribution sanity check, split by scenario | Only for its own objective, not AAFR's | No (too few distinct values) |
| Future model training | Not recommended (label leakage, simulated, unknown units) | Not recommended (objective mismatch, 83% of scenarios uninformative) | No |

Restrictions: no license, so redistribution or inclusion in AAFR is not
permitted without the author's permission; use only by reference, for
comparison, and always labelled as external and simulated.
