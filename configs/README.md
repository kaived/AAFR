# Configuration

This folder contains runtime and experiment configuration.

## Main File

`policy_registry.yaml` defines:

- decision model version
- available subsystems
- default strategies
- candidate strategies
- scoring weights
- transition thresholds
- executor support level for each strategy

## What To Add Here

Add:

- policy variants for experiments
- tuned weight sets
- static baseline policies
- adaptive candidate policies
- validation notes for config changes

Use clear names:

```text
policy_registry.yaml
policy_static_baseline.yaml
policy_caaf_seed.yaml
policy_experiment_mobile_loss.yaml
```

## Rule

Changing strategy behavior through configuration is preferred over hardcoding weights in C++.
