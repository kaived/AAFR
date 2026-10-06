# Browser Layer

This folder is for the custom browser product.

The browser layer should provide the user-facing application while using an existing browser engine for standards-compatible rendering.

## Planned Responsibilities

- main browser window
- address bar
- tabs
- back, forward, reload
- static/adaptive mode toggle
- status panel for strategy and telemetry
- Qt WebEngine view
- request observation/interception wiring

## What Not To Build

Do not build a custom HTML renderer, JavaScript engine, CSS parser, DOM implementation, TLS stack, or complete HTTP stack. Those belong to Qt WebEngine/Chromium.

## First Milestone

Build a minimal Qt WebEngine browser shell:

```text
address bar -> QWebEngineView -> rendered page
```

Then add:

- basic navigation controls
- one tab
- telemetry-only request observation
- adaptive/static mode indicator

## Integration Boundary

Before implementing a browser action, check:

- `runtime/execution/action_space.h`
- `configs/policy_registry.yaml`

Only execute strategies whose support level is implemented.
