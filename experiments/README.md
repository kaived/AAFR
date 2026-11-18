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

## V0.7.3 browser forwarding experiments

`aafr-browser-experiment` (source `tools/browser_experiment/`) runs real Qt
WebEngine page loads against a local origin server. Qt WebEngine's proxy is
fixed per process, so the direct baseline and the forwarded runs are separate
child processes:

```text
build\windows-msvc-qt\aafr-browser-experiment.exe --all --trials=15 --out-dir=experiments\results\v0.7.3-debug
```

Experiments: A direct baseline; B fixed relay path (with and without injected
delay); C degradation of path A, before and after the runtime's switch; D
recovery; E relay failure and recovery; F expert model in shadow mode (no
profile, then a configured profile labelled as such). It writes
`direct.jsonl` and `forwarded.jsonl` (one line per trial with browser,
gateway and observed relay-path fields, per-phase summaries and switch
events) and `summary.md`. Results and interpretation:
[browser README, V0.7.3 controlled experiments](../browser/qt_webengine/README.md#v073-controlled-experiments-debug-build-2026-11-18).

