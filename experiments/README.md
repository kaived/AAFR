# Experiments

This folder is for reproducible experiments.

## Subfolders

| Path | Purpose |
| --- | --- |
| `configs/` | experiment run definitions |
| `scenarios/` | network conditions and emulation profiles |
| `results/` | generated raw outputs, ignored by git |
| `analysis/` | generated summaries, charts, and processed metrics, ignored by git |

## Minimum Experiment Record

Every experiment should record:

- source commit
- algorithm version
- policy registry version
- browser mode: static or adaptive
- workload
- network scenario
- number of runs
- machine/environment details
- raw telemetry path
- summary metrics

## Comparison Rule

Every adaptive experiment needs a matching static baseline run under the same workload and network scenario.

## V0.6 runtime experiment

`aafr-runtime-experiment` (source `tools/runtime_experiment/`) compares, on
identical scenarios and seeds over real loopback relays, a fixed Dijkstra
path, the adaptive runtime with estimate-only evidence, the runtime with
controlled probing and statistical arbitration, and the runtime with every
stability gate disabled (isolated configuration). Scenarios: congestion that
appears and clears, and a link fluctuating faster than the gates.

```text
build\windows-msvc-qt\aafr-runtime-experiment.exe --out experiments\results\v0.6 --seconds 20 --seeds 1,2,3
```

It writes `runs.csv` (one row per run: latency mean / median / p95, success
rate, goodput, switches, failed and rejected switches, evaluations, decision
and computation time per evaluation, probe requests and time, switching
latency, adaptation and recovery delay, switches backed by probe evidence)
and `summary.json`. Results and their interpretation:
[browser README, performance measurements](../browser/qt_webengine/README.md#performance-measurements-debug-build-2026-11-16).

