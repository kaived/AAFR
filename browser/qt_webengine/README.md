# AAFR Qt WebEngine browser V0.3

The browser UI runs independently of the adaptive forwarding runtime. V0.3
adds observability: browser event capture, optional TShark network telemetry,
a typed telemetry pipeline, and a native Developer Tools panel. It does not
add an AI model, algorithm selection, or forwarding/switching behaviour. See
[V0.3 observability](#v03-observability-and-developer-tools) below.

## Architecture

- `main.cpp`: application startup, top-level window lifetime, and the opt-in
  `--telemetry-dir` JSONL option.
- `browser_window.h/.cpp`: navigation toolbar, address bar, shortcuts, window
  title, loading progress, errors, integration status, and ownership of the
  V0.3 telemetry pipeline, collector, TShark monitor and Developer Tools dock.
- `browser_event_collector.h/.cpp`: turns Qt WebEngine page signals into
  telemetry events with stable tab and navigation ids.
- `devtools_panel.h/.cpp`: the read-only Developer Tools dock.
- `tab_manager.h/.cpp`: tab ownership, per-page loading state, WebEngine signal
  wiring, new-window requests, and safe tab closure. An internal `BrowserView`
  adapter routes links requesting a new window into tabs.
- `browser_theme.h/.cpp`: centralized dark-neutral/teal chrome tokens and local
  navigation icons. `browser_assets.qrc` bundles the Google search-provider mark.

## UI design

On Windows the tab strip is the title bar, as in Chrome. The native frame is
kept, so resize borders, the shadow, rounded corners, Aero Snap and Win+arrow
keys still work, but Qt's private `QWindowsWindow::setCustomMargins` folds the
caption into the client area. `BrowserWindow::titleBarHit` answers Windows'
hit-test: empty strip space acts as the caption (drag, double-click to
maximize, right-click for the system menu), a band along the top edge resizes,
and tabs and buttons stay interactive. AAFR draws its own minimize, maximize
and close buttons. Windows 11's Snap Layouts pop-up on hovering maximize is not
supported; Win+Z still opens it. Other platforms keep their native title bar.

A layout-owned tab strip mirrors the existing QTabWidget model; TabManager
still owns the views and histories. Tabs use a consistent height, capped
widths, elided titles, and opaque overflow controls. The minimum client size is
800x600. The tab bar paints Chrome-style tabs itself: the active tab has
rounded top corners and curved feet flaring into the toolbar, in the toolbar's
colour; a hovered tab gets a rounded highlight; inactive tabs are divided by
thin separators; and each tab shows a 16 px icon at the left of its body with
the title directly after it. New tabs show the AAFR "A" mark, pages show their
favicon, and pages without one show a globe. The window and taskbar icon is
the same "A" mark. The address field expands with the window; Reload and Stop
share one button. Home has no toolbar button and is available as Alt+Home.

The new-tab surface is a lightweight Qt widget with the AAFR mark and a
centered search field, not an extra WebEngine document. It uses the address bar's URL/search handling
without introducing an internal URL or synthetic history entry. The default
new tab and Home display this surface. Page styling remains independent of the
browser's chrome stylesheet.

Google's bundled G mark identifies the default search provider in both search
fields. A non-Google search template uses the neutral search icon. There are no
remote image requests for these icons. The native title shows the page title
without an AAFR suffix; the duplicate tab-strip brand and permanent standalone
badge are removed. Loading and error messages remain in the status area.

Ctrl+L, Ctrl+T, Ctrl+W, Ctrl+Tab, Ctrl+Shift+Tab, Alt+Left/Right, F5, Ctrl+R,
and Esc remain available. Search fields and buttons have accessible names and
visible keyboard focus. V0.3 adds one toolbar button (Developer Tools,
Ctrl+Shift+M) at the right of the address bar; nothing else in the V0.2
chrome changed.

The UI suite checks relative widget bounds at 1920x1080, 1440x900, 1280x800,
1024x768, and 800x600, plus overflow, ordering, focus, shortcuts, and ownership.
Set `AAFR_UI_CAPTURE_DIR` before CTest to save actual Qt client-area captures
at those sizes. These omit the native frame and are separate from standalone
desktop screenshots. Generated images and test reports live under `build/`.

The Google-icon/branding update built successfully on 2026-11-03. CTest passed
1/1 suite in 35.19 seconds; Qt Test reported 41 passed, 0 failed, 0 skipped.
Updated rendered captures are in `build/ui-verification/google-search/`.

## Features

Back, Forward, Refresh, Stop, and Home (Alt+Home) operate on the active tab.
Home opens the local new-tab screen by default. Enter a URL or search terms and press Enter. Qt's
`QUrl::fromUserInput` handles hostnames and local file paths. Bare hostnames use
HTTPS, except loopback hosts (`localhost`, `*.localhost`, `127.x.x.x`, `::1`),
which keep HTTP so local development servers work; explicit HTTP and HTTPS URLs
retain their scheme. Words and phrases
without a recognizable hostname use the configured search URL, with the query
percent-encoded. Invalid URLs display a message without navigating.

`BrowserWindow(parent, homeUrl, searchUrlTemplate)` configures the home page and
search endpoint. Defaults are `about:blank` (the local new-tab screen) and
`https://www.google.com/search?q=%1`; search templates must use HTTP(S) and
contain `%1`. `BrowserWindow::urlFromInput` provides the same normalization
without starting WebEngine navigation. The only persisted setting is the
Developer Tools layout (V0.3); there is no search-provider preferences dialog.

Use **New Tab**, the tab close buttons, or these keyboard shortcuts:

| Action | Windows shortcut |
| --- | --- |
| Focus/select address | Ctrl+L |
| New tab | Ctrl+T |
| Close current tab | Ctrl+W |
| Next / previous tab | Ctrl+Tab / Ctrl+Shift+Tab |
| Back / forward | Alt+Left / Alt+Right |
| Refresh | F5 |
| Stop | Escape |
| Home | Alt+Home |
| Developer Tools | Ctrl+Shift+M |
| Search tabs | Ctrl+Shift+A |
| Reopen closed tab | Ctrl+Shift+T |

**Tab search.** The chevron button at the left of the tab strip, or
Ctrl+Shift+A, opens a Chrome-style popup (`tab_search_popup.h/.cpp`). Typing
filters open and recently closed tabs by title or site; Up/Down move the
selection, Enter switches to it, and Esc closes the popup. Each open tab shows
its icon, title, site and when it was last viewed, plus a close button on
hover; the popup stays open after closing a tab. The collapsible Recently
Closed section lists up to 10 closed pages, newest first; choosing one reopens
it in a new tab, as does Ctrl+Shift+T for the latest. Empty new tabs are not
remembered. The list lives in memory only and is never written to disk.

Tabs can be dragged to reorder. Closing the final tab creates a blank tab.
Each tab retains its own title, URL, favicon, history, progress, and error state.
Background loads do not change the active address or status. Failed loads keep
Qt's error page and show a status message; Refresh retries the URL. Explicitly
stopped loads are not displayed as failures. Renderer termination displays a
recovery message. An address being edited is preserved until navigation or a
tab switch.

## Build and run

Requires Qt 6 with Widgets and WebEngineWidgets, CMake 3.25 or newer for the
version 6 presets, and a matching C++17 compiler. The checked-in Windows preset
uses Qt 6.12.0 MSVC x64 at `C:/Qt/6.12.0/msvc2022_64` and Qt's Ninja tool.
Run from a Visual Studio x64 developer shell matching MSVC 19.44. When multiple
toolsets are installed, initialize `VsDevCmd.bat` with
`-arch=x64 -host_arch=x64 -vcvars_ver=14.44` before configuring and building:

```powershell
cmake --preset windows-msvc-qt
cmake --build --preset windows-msvc-qt
$env:PATH = "C:\Qt\6.12.0\msvc2022_64\bin;$env:PATH"
.\build\windows-msvc-qt\aafr-browser.exe
```

For a different Qt installation, override `CMAKE_PREFIX_PATH` or configure
directly with `cmake -S . -B build/local -DCMAKE_PREFIX_PATH=<Qt-prefix>`.
Qt WebEngine's helper executable, resources, plugins, and libraries must be
available at runtime; use Qt's deployment tooling when distributing the app.

## Verification

Optional Qt Test checks use temporary local HTML pages. They cover lifecycle
events, tab isolation/reordering/closure, failed-load recovery, links opening
tabs, address entry, navigation history, and keyboard shortcuts:

```powershell
cmake --preset windows-msvc-qt -DAAFR_BUILD_BROWSER_TESTS=ON
cmake --build --preset windows-msvc-qt
ctest --test-dir build/windows-msvc-qt --output-on-failure
```

CTest prepends the imported Qt Core DLL directory for the selected build
configuration to PATH automatically through `TARGET_FILE_DIR` and
`ENVIRONMENT_MODIFICATION` (supported by the CMake 3.22 minimum).
Launching the browser executable directly still uses the PATH setup above.
Tests require Qt Test and a desktop environment capable of running WebEngine.
Tests configure a local home page or `about:blank`; none require public network
access. URL normalization is tested without page loading. WebEngine integration
tests use temporary HTML and loopback HTTP servers, including an intentionally
pending response to exercise Stop and closure during loading. They also cover
search submission, active/background control state, repeated closure, and
external view destruction. Closing the final tab creates a blank replacement;
external destruction schedules replacement after Qt removes the old widget.

If the installed MSVC toolchain reports C1902 (program database manager
mismatch), a local verification build can use embedded debug symbols:

```powershell
cmake --preset windows-msvc-qt -B build/browser-interface-check -DAAFR_BUILD_BROWSER_TESTS=ON '-DCMAKE_CXX_FLAGS_DEBUG=/Z7 /Od /RTC1'
cmake --build build/browser-interface-check
ctest --test-dir build/browser-interface-check --output-on-failure
```

WebEngine tests must run outside a process sandbox that prevents Chromium's
own sandbox from initializing. Do not disable the browser's Chromium sandbox.
Detailed Qt Test results are also written to `browser-interface-results.txt`
inside the build directory.

## Known limitations and interactive verification

Runtime verification before the UI refinement on 2026-11-03 using MSVC 19.44
and Qt 6.12.0 Debug:
`cmake --build build/windows-msvc-qt --parallel 4` succeeded;
`ctest --test-dir build/windows-msvc-qt --output-on-failure` passed 1/1 suite
in 19.55 seconds. Qt Test reported 32 passed, 0 failed, 0 skipped (including
initialization and cleanup). This run used the normal developer environment
outside the agent process sandbox, which denied temporary HTML file writes.
Chromium's sandbox remained enabled. Tests validate local-page behavior and
URL normalization, not availability of public websites.

The browser executable was previously blocked by Windows Smart App Control's
`VerifiedAndReputableDesktop` policy. Interactive acceptance of the standalone
application remains pending; automated tests do not establish that it can be
launched under that policy. Do not disable Smart App Control, Defender, Code
Integrity, or Chromium's sandbox to run this project. Public HTTPS availability,
certificate errors, visual layout, and standalone keyboard/mouse behavior still
require interactive verification in an environment that permits execution.
The nonfatal `WrapVulkanHeaders` warning alone is not a build blocker.
The passing Debug test run also logs Chromium frame-shutdown and media
URL-safety diagnostics. Both reproduce with plain Qt views without AAFR
classes; the media messages disappear in a control that disables only its
built-in error page. The renderer shutdown cause remains unresolved. Tests
confirm QObject destruction, not native/renderer memory-leak freedom. See
[Runtime verification and stability](RUNTIME_VERIFICATION.md) for the ownership
review, isolated controls, reproducible commands, and evidence.

V0.2 is functionally implemented and automated-test verified, but not yet fully
accepted. Standalone visual acceptance remains explicitly pending.

V0.2 does not include adaptive routing, interception, or inference. The V0.3
section below records what has and has not been verified for observability.

## AAFR integration boundary

`BrowserWindow::tabManager()` exposes the tab lifecycle interface:

- `webViewCreated(view)` fires before a new view's initial load. Connect future
  page observers here; adapters attached after startup should also inspect
  existing views through the manager's `count()` and `widget(index)` methods.
- `navigationStarted(view, url)` and
  `navigationFinished(view, url, success, error)` include background tabs.
  A stopped navigation emits `success=false` with `Navigation stopped`, while
  the UI treats it as a cancellation. These are page-level events, not network
  request metrics.
- `BrowserWindow::setIntegrationStatus(text)` lets an adapter display a status
  message when explicitly called. There is no permanent standalone badge.

View pointers are owned by the tab manager; external adapters should use
`QPointer` or QObject lifetime connections and must not retain raw pointers
after a tab closes. Update widgets only on the GUI thread.

No forwarding policy, request interceptor, or AI model is installed by this UI.
The only sensor is the optional, off-by-default TShark monitor (V0.3). Future
adapters should use `runtime/interfaces/interfaces.h`, validate support in
`runtime/execution/action_space.h` and `configs/policy_registry.yaml`, and keep
policy selection and execution outside the browser UI.

## V0.3 observability and Developer Tools

V0.3 makes browser and network behaviour observable. It does not load, train
or call an Expert Model, produce confidence scores, select algorithms, or
switch forwarding. Every value shown is either measured or labelled
unavailable.

### Components

```text
Qt WebEngine signals ──► BrowserEventCollector ─┐
                                                ├─► TelemetryPipeline ─┬─► in-memory history (bounded)
tshark (QProcess) ──► TSharkMonitor ────────────┘   (stamps id, time)   ├─► latest value per metric
                      └ IntervalAggregator                              └─► JSONL writer thread (opt-in)
                                                         │
                                   DevToolsPanel ◄───────┘ polls at 4 Hz while visible
                                   IRuntimeObservabilityProvider (none in V0.3)
```

| Component | Location | Responsibility |
| --- | --- | --- |
| `TelemetryEvent` | `network/telemetry/telemetry_event.h` | Typed, versioned record (schema 1) and JSON serialization |
| `TelemetryPipeline` | `network/telemetry/telemetry_pipeline.h` | Stamps event id, session id, UTC and monotonic time; keeps a 2,000-event ring buffer and the latest value per metric |
| `BoundedQueue`, `JsonlTelemetryWriter` | `network/telemetry/` | Non-blocking hand-off to a writer thread with size-based rotation |
| `tshark_fields` | `network/tshark/tshark_fields.h` | TShark field parsing, per-interval aggregation, metric events |
| `NetworkMonitor`, `TSharkMonitor` | `network/monitor/`, `network/tshark/` | Monitor interface and the optional asynchronous TShark adapter |
| `BrowserEventCollector` | `browser/qt_webengine/` | Browser lifecycle events with tab and navigation ids |
| `DevToolsPanel` | `browser/qt_webengine/` | Read-only Developer Tools dock |
| Runtime contracts | `runtime/observability/runtime_contracts.h` | `AlgorithmStatus`, `AlgorithmCapability`, `DecisionRecord`, provider interface |

`aafr-telemetry` (telemetry core, TShark parsing, runtime contracts) is plain
C++17 with no Qt dependency, so a future runtime can produce and consume the
same records without linking the browser or the Developer Tools UI.
`aafr-network-monitor` needs only Qt Core. `aafr-browser-ui` holds the widgets
and is shared by the application and the Qt tests.

### Browser event model

| Event type | Qt source | Value |
| --- | --- | --- |
| `tab.created`, `tab.activated`, `tab.closed` | `TabManager` lifecycle and `currentChanged` | — |
| `navigation.started` | `QWebEnginePage::loadingChanged` (started) | — |
| `navigation.succeeded` / `navigation.failed` / `navigation.stopped` | `loadingChanged` (finished) | `navigation.duration` in ms |
| `navigation.progress` | `loadProgress`, recorded at 25 % steps | percent |
| `navigation.url_changed` | `urlChanged` | — |
| `renderer.terminated` | `renderProcessTerminated` | exit code attribute |

Tab ids are assigned once per view and never reused in a session. Each
`navigation.started` gets a new navigation id; the completion event carries
the same id. Durations use `std::chrono::steady_clock`, so wall-clock changes
cannot affect them. If a completion arrives without an observed start (a
collector attached mid-load), it has no navigation id, no duration, and
`availability=unavailable`. Failed navigations add the Qt error domain, code
and message. `TabManager::tabClosed` reports both user closure and external
view destruction exactly once.

These are page-level navigation events. Qt's signals do not expose individual
HTTP requests, subresources, redirects, or socket timing, and V0.3 makes no
claim about them.

### Network telemetry and TShark

TShark is optional and off by default. The browser works normally without
it: the Network tab shows `Unavailable` and the capture button is disabled.
Capture starts only when the user clicks **Start capture**. AAFR never
requests elevation, installs drivers, or changes Windows security settings.
If Npcap denies access, the error is shown and the browser carries on.

Configuration:

| Setting | Effect |
| --- | --- |
| `AAFR_TSHARK_PATH` | TShark executable. Otherwise `tshark` on `PATH`, then `C:/Program Files/Wireshark/tshark.exe` |
| `AAFR_TSHARK_INTERFACE` | Default capture interface (also editable in the Network tab). Empty lets TShark choose |

The monitor runs `tshark -l -n -Q -T fields` with capture filter `tcp or udp`
and requests only these fields: `frame.time_epoch`, `frame.len`,
`frame.protocols`, `tcp.analysis.ack_rtt`, `tcp.analysis.retransmission`,
`dns.flags.response`, `dns.time`, `quic.version`. Payloads, IP addresses and
ports are never requested. Output is aggregated and published once per second:

| Metric | Unit | Notes |
| --- | --- | --- |
| `net.tcp.ack_rtt.latest/min/mean/max` | ms | TShark's ACK RTT for segments seen on the interface; omitted when no samples |
| `net.tcp.retransmissions` | segments per interval | TShark's retransmission heuristic |
| `net.dns.response_time.mean/max` | ms | `dns.time` on responses; omitted when no samples |
| `net.dns.responses` | per interval | |
| `net.packets`, `net.bytes`, `net.tcp.packets`, `net.udp.packets`, `net.quic.packets` | per interval | QUIC versions seen are an attribute |
| `net.capture.packets_total`, `net.capture.bytes_total` | since capture start | |

Every metric event carries its source (`tshark`), unit, timestamps, scope
(`capture_interface`), availability, and attribution (`unattributed`).

Measurement limitations:

- Capture metrics are host-wide for the interface. They are not attributed to
  tabs or navigations, because nothing links packets to a tab.
- Retransmissions are not an exact packet-loss rate; V0.3 reports no loss
  percentage.
- ACK RTT is measured at this host and only for TCP; QUIC RTT is encrypted and
  not reported.
- Metrics with no samples are shown as `Not observed`, never as zero. Values
  older than 10 s are marked stale.

Process handling: TShark runs asynchronously through `QProcess`. A missing
executable, a failed start, a non-zero exit (with the last stderr line) and an
unexpected end are each reported as `capture.error` or `capture.stopped`
events. Stop and window shutdown kill the process with a bounded 1.5 s wait.

### Telemetry pipeline, JSONL and privacy

`TelemetryPipeline::Publish` never waits on disk: it stamps the event, stores
it in the bounded history, and offers its JSON line to a bounded queue
(4,096 lines). A background thread writes the queue. When the queue is full,
lines are dropped and counted; the Overview tab shows the count.

JSONL output is off by default. Enable it with `--telemetry-dir <dir>` or
`AAFR_TELEMETRY_DIR`. Files are named `aafr-telemetry-<session>.jsonl` and
rotate at 4 MiB, keeping at most 4 files (about 16 MiB) per session.

Privacy defaults:

- URLs drop credentials, query and fragment (`https://host:port/path` only).
- `file:` URLs keep only the file name (`file:.../page.html`), because local
  paths contain account names.
- `data:`, `blob:` and `javascript:` URLs keep only the scheme.
- No packet payloads, addresses or ports are captured or stored.
- Page titles are not recorded.

### Developer Tools panel

A `QDockWidget` outside the central widget, so webpage layout and rendering
are unaffected. It is hidden at startup and opens from the toolbar button or
Ctrl+Shift+M. It docks at the bottom by default; **Dock right** moves it to the
right side, the float button detaches it, and the splitter resizes it. The dock
side and size are saved to `aafr-browser-layout.ini` in the user config
folder, but the panel always starts hidden. It polls the pipeline at 4 Hz only
while visible.

| Tab | Content |
| --- | --- |
| Overview | Active tab and navigation status, last page-load duration, collection status (events, JSONL, drops), session id, capture state, latest metric summary |
| Network | Capture controls, every metric with value, unit, source, scope and freshness, QUIC versions, and a scope note |
| Runtime | Active algorithm, activation timestamp and reason, runtime state, last evaluation. V0.3 has no provider, so every field shows `Not available — runtime not integrated` |
| Decisions | Decision history table (type, previous → candidate, reason code, failed checks, switch outcome, explanation). Empty with the same message in V0.3 |
| Events | Last 500 browser and network events with time, session offset, type, source, tab, navigation, value and details; source filter, pause and clear |

### Future runtime observability contracts

`runtime/observability/runtime_contracts.h` defines, without implementing a
runtime:

- `AlgorithmStatus`: algorithm id, activation time, activating decision id,
  execution state, last confirmed time.
- `AlgorithmCapability`: supported characteristics, required inputs,
  operational restrictions, implementation availability, resource
  requirements.
- `DecisionRecord`: decision id, timestamp, NetworkState reference, previous
  and candidate algorithm, constraint results, stability-gate results,
  decision type, reason code, explanation, execution confirmation.
- `IRuntimeObservabilityProvider`: a read-only snapshot and decision history.

A `SwitchApproved` decision is not a committed switch. `ExecutionConfirmation`
(`Pending`, `Committed`, `Failed`, `RolledBack`) records what actually took
effect, and the panel shows "Switch approved, awaiting commit" separately from
"Switch committed". `DevToolsPanel::setRuntimeProvider` accepts a provider; the
browser passes none.

### Automated tests

```powershell
cmake --preset windows-msvc-qt -DAAFR_BUILD_BROWSER_TESTS=ON
cmake --build build\windows-msvc-qt --parallel 4
ctest --test-dir build\windows-msvc-qt --output-on-failure
```

| CTest suite | Covers |
| --- | --- |
| `runtime_core_smoke` | Existing runtime decision and transition gates |
| `telemetry_core` | Serialization, UTC formatting, bounded queues under concurrent producers, pipeline ids/history/eviction, JSONL rotation and failure handling, TShark parsing with fixtures, runtime contracts (381 checks) |
| `browser_interface` | The V0.2 browser suite, unchanged apart from two new address rows (43 passed) |
| `browser_observability` | Navigation lifecycle and monotonic timing, multiple tabs, activation and closure, failed/stopped navigation, renderer termination, URL redaction, TShark missing/failed start/non-zero exit/fixture capture/stop/shutdown, panel open/close/shortcut, docking/resizing/floating, layout persistence, event display and bounding, missing metrics, measured metrics, runtime and decisions unavailable, contract display, browser use with panel open, window shutdown (31 passed, 1 opt-in skip) |

TShark tests use `fake-tshark` (`tests/tools/fake_tshark.cpp`), which replays
fixtures from `tests/fixtures/tshark/`, writes stderr, exits with a code, or
hangs until killed. No capture driver or network access is required. If
Windows application control blocks the helper, those tests skip with the
reason rather than pass. Set `AAFR_UI_CAPTURE_DIR` to save real Qt renders of
each panel tab (bottom and right docked) from `captureDevToolsScreenshots`.

### V0.3 verification record (2026-11-10)

Windows 11, MSVC 19.44 x64, Qt 6.12.0 Debug, Ninja, in `build\windows-msvc-qt`:

- The build succeeded with no compiler warnings.
- CTest passed 4/4 suites. Qt Test reported 31 passed, 1 skipped (opt-in
  screenshots) in `browser_observability`, and 43 passed in
  `browser_interface`. `telemetry_core` passed 381 checks. The observability
  suite takes about 16 s when run directly and up to 112 s on the first CTest
  run after a rebuild.
- Smart App Control intermittently blocked newly built unsigned test
  executables (`browser-interface-tests.exe` in one run, `telemetry-core-tests.exe`
  in another; Code Integrity events 3033/3077) and allowed them on a later
  run. No policy was changed.
- Developer Tools captures were rendered and inspected in
  `build/ui-verification/v0.3-devtools/`: all five tabs bottom-docked, the
  Network tab right-docked, and the hidden state. Their network values come
  from the `capture_basic.tsv` fixture published by the test, not live capture.

Not verified:

- Live capture with real TShark/Npcap. TShark is not installed on the
  verification machine; parsing and process handling are verified only with
  fixtures and the fake helper. Field output (for example `True`/`1` for
  flags) may differ between TShark versions.
- Interactive use of the standalone `aafr-browser.exe`, including real mouse
  drag-docking, floating-window behaviour and native-frame appearance. The
  captures are client-area renders, not desktop screenshots.
- Renderer termination from a real crash (the test emits the signal).
