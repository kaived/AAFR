# Evaluation Tools

This folder will contain scripts that convert telemetry logs into evaluation metrics.

Planned outputs:

- page-load summary
- request latency distribution
- strategy switch timeline
- adaptive-vs-static comparison table
- worst-case degradation report
- reproducibility manifest

## Planned Inputs

- telemetry JSONL files
- experiment config
- network scenario file
- policy registry version
- source commit id

## Planned Metrics

- median page-load time
- p95 page-load time
- request latency distribution
- failed request count
- retry count
- cache hit rate
- decision overhead
- switch rate per subsystem
- adaptive degradation against static baseline

## Rule

Evaluation scripts must never silently drop failed runs. Failed or incomplete runs should appear in the summary.
