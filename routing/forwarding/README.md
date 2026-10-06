# Forwarding

This folder is for concrete forwarding strategies and browser-facing request
handling policies.

## Planned Strategies

| Folder | Examples |
| --- | --- |
| `connection_reuse/` | reuse policy, connection pool hints, baseline behavior |
| `multiplexing/` | HTTP/2 or HTTP/3 multiplexing preference experiments |
| `retry_backoff/` | retry timing, cooldown, and failure-class rules |
| `priority/` | browser request priority and scheduling policy |

## Strategy Requirements

Each strategy should document:

- what it does
- when it should help
- when it can hurt
- required executor support level
- telemetry fields needed to evaluate it
- baseline it should be compared against

## Implementation Rule

Each strategy should be measurable, gated by executor support, and traceable
through telemetry.
