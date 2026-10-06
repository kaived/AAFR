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
