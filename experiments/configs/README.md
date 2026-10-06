# Experiment Configs

Store reproducible experiment configuration files here.

Each experiment config should identify:

- experiment id
- workload
- network scenario
- browser mode: static or adaptive
- policy registry version
- algorithm version
- number of runs
- random seed, if applicable
- expected output directory
- notes about hardware or OS assumptions

## Suggested Template

```yaml
experiment_id: "stable-broadband-static-001"
mode: "static"
algorithm_version: "none"
policy_file: "config/policy_static_baseline.yaml"
scenario_file: "experiments/scenarios/stable_broadband.yaml"
workload: "controlled_news_page"
runs: 10
seed: 42
metrics:
  - page_load_time_ms
  - request_latency_ms
  - failed_requests
  - cache_hit_rate
  - decision_overhead_ms
```

## Rule

Configs should be small, readable, and committed. Generated outputs should go to `experiments/results/`.
