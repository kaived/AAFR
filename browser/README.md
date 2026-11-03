# Browser Layer

This folder is for the custom browser product.

The browser entry point is `qt_webengine/main.cpp`. See
[`qt_webengine/README.md`](qt_webengine/README.md) for its current status and
build instructions.

The browser layer should provide the user-facing application while using an existing browser engine for standards-compatible rendering.

## Implemented Interface

The Qt shell provides navigation controls, an address bar, multiple reorderable
tabs, per-page titles and URLs, loading progress, and load-failure status.
`BrowserWindow` owns the UI and `TabManager` owns WebEngine views. See the
Qt WebEngine README for build instructions, shortcuts, and integration signals.

## Planned Integration

- static/adaptive mode toggle
- status panel for strategy and telemetry
- request observation/interception wiring

## What Not To Build

Do not build a custom HTML renderer, JavaScript engine, CSS parser, DOM implementation, TLS stack, or complete HTTP stack. Those belong to Qt WebEngine/Chromium.

## Integration Boundary

Before implementing a browser action, check:

- `runtime/execution/action_space.h`
- `configs/policy_registry.yaml`

Only execute strategies whose support level is implemented.
