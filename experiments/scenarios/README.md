# Network Scenarios

Store network emulation scenarios here.

Initial scenario set:

- stable broadband
- high latency
- low throughput
- packet loss
- high jitter
- fluctuating mobile-like network

## What Each Scenario Should Define

- scenario id
- latency range
- throughput limit
- packet loss
- jitter
- duration
- whether conditions are stable or time-varying
- tool used to apply the scenario

## Suggested Template

```yaml
scenario_id: "fluctuating-mobile"
description: "Mobile-like network with varying latency and loss."
latency_ms:
  base: 80
  jitter: 40
throughput_mbps:
  min: 2
  max: 12
packet_loss_pct:
  min: 0.0
  max: 3.0
duration_s: 120
```

## Rule

Scenarios should be reusable. Do not tune a scenario only to make the adaptive algorithm look good.
