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

## Implemented (V0.5 – V0.7.3)

| File | Role |
| --- | --- |
| `forwarding_executor.*` | Execution contract: `IForwardingExecutor` (messages) and, since V0.7.3, `IStreamForwardingExecutor` (byte-stream tunnels), plus the simulated executor |
| `loopback_forwarding.*` | Relays on 127.0.0.1: `AAFR1 REQ` messages and V0.7.3 `AAFR1 TUN` tunnels, with per-node logs and `StreamTraversal` evidence |
| `socket_io.*` | V0.7.3 shared bounded socket primitives (connect, listen, line/head readers, bidirectional pump, abortive close) |
| `egress_policy.*` | V0.7.3 destination policy for tunnels leaving the controlled layer (address classes, ports, DNS-rebinding-safe per-address checks) |

Browser traffic reaches these relays through the browser forwarding gateway
in `routing/gateway/`. See
[browser README, V0.7.3](../../browser/qt_webengine/README.md#v073-real-browser-forwarding).
