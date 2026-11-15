# AAFR Qt WebEngine browser V0.5

The browser UI runs independently of the adaptive forwarding runtime. V0.3
adds observability: browser event capture, optional TShark network telemetry,
a typed telemetry pipeline, and a native Developer Tools panel. It does not
add an AI model, algorithm selection, or forwarding/switching behaviour. See
[V0.3 observability](#v03-observability-and-developer-tools) below.

V0.4 derives a validated `NetworkState`, network constraints, capability
profiles for the ten planned algorithms and explainable eligibility results
from that telemetry. It implements no algorithm and selects or switches
nothing. See
[V0.4 NetworkState, constraints and algorithm eligibility](#v04-networkstate-constraints-and-algorithm-eligibility).

V0.5 adds a controlled application-layer forwarding engine: a typed
forwarding graph and objective, implementations of all ten algorithms behind
one interface, real forwarding through loopback relays, implementation
verification that drives the capability registry, manual execution from
Developer Tools, and a reproducible benchmark. Browser page loads are **not**
routed through it, and nothing is selected or switched automatically. See
[V0.5 controlled forwarding engine and algorithms](#v05-controlled-forwarding-engine-and-algorithms).

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

That is the Debug build, meant for development and the tests. It loads the
debug Qt WebEngine (debug Chromium), which is much slower. Measured on
2026-11-14 with a local page of 400 search-style results, Debug took 2.5–3.9 s
to load and 3.5–5.8 s of CPU, against 1.0–1.5 s and 1.1 s for Release, and
kept using CPU while the page was idle (about 2 s per 10 s against 0.1 s).
Searching therefore feels slow in a Debug build, and the Debug browser shows
"Debug build — pages load slowly" in its status bar. For everyday browsing,
use the Release preset:

```powershell
cmake --preset windows-msvc-qt-release
cmake --build --preset windows-msvc-qt-release
$env:PATH = "C:\Qt\6.12.0\msvc2022_64\bin;$env:PATH"
.\build\windows-msvc-qt-release\aafr-browser.exe
```

The browser keeps its HTTP cache, cookies and site data on disk in a Qt
WebEngine profile named `aafr`, under the user's application data folder, so
pages are not downloaded from scratch after every start. Start it with
`--private` to keep nothing on disk for that run. A URL or search terms given
on the command line open in the first tab, for example
`aafr-browser.exe https://example.com`.

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
| `AAFR_TSHARK_INTERFACE` | Default capture interface, preselected in the Network tab's dropdown. Empty means Automatic |

**Choosing the interface.** The Network tab's dropdown lists the interfaces
from `tshark -D`, which runs in the background when the panel first opens
(listing only, not capturing). The first entry, **Automatic**, captures on the
connected Wi-Fi or Ethernet adapter, found with `QNetworkInterface` and shown
in the entry's label, e.g. "Automatic (Wi-Fi)". TShark's own default, its
first listed interface, is often a virtual adapter with no traffic. Names from
`AAFR_TSHARK_INTERFACE` match loosely: `wifi`, `Wi-Fi`, `WI FI` or the number
`5` all select Wi-Fi, and an ambiguous loose match is refused rather than
guessed. An unknown name fails before TShark starts, with a message listing
the real interface names. AAFR passes TShark the resolved device path
(`\Device\NPF_{…}`).

**No measurements before data.** Until TShark delivers its first packet, no
interval is published, so a capture that never starts (for example a wrong
interface) leaves every metric "Not observed" instead of showing zeros.

The monitor runs `tshark -l -n -Q -T fields` with capture filter `tcp or udp`
and requests only these fields: `frame.time_epoch`, `frame.len`,
`frame.protocols`, `tcp.analysis.ack_rtt`, `tcp.analysis.retransmission`,
`dns.flags.response`, `dns.time`, `quic.version`, `tcp.stream`,
`tcp.analysis.initial_rtt`. `tcp.stream` is TShark's per-capture connection
counter, not an address. Payloads, IP addresses and ports are never requested.
Output is aggregated and published once per second:

| Metric | Unit | Notes |
| --- | --- | --- |
| `net.tcp.handshake_rtt.min/mean/max` | ms | Network latency, shown as "Latency" in the panel: TShark's handshake RTT (`tcp.analysis.initial_rtt`), one sample per connection with traffic in the interval; the `connections` attribute is the sample count; omitted when no samples |
| `net.tcp.ack_rtt.latest/min/mean/max` | ms | Shown as "ACK delay, both directions": TShark's ACK RTT for segments seen on the interface; omitted when no samples |
| `net.tcp.ack_rtt.jitter` | ms | Mean absolute difference between consecutive ACK RTT samples in the interval; omitted with fewer than two samples |
| `net.throughput` | Mbps | Frame bytes in the interval × 8 ÷ the measured interval length; both directions combined |
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
- Latency is the handshake RTT, so it is fixed when a connection opens and
  does not follow later changes on that path. Connections opened before the
  capture started have no handshake to measure and are left out. It is TCP
  only; QUIC RTT is encrypted and not reported.
- ACK RTT is not path latency. It times every data segment against its ACK in
  both directions, and this host acknowledges received data within
  microseconds, so on a download its mean falls far below the real round trip
  (a 2.67 ms mean was seen live on a link whose handshakes took tens of ms).
- Jitter is taken over those ACK RTT samples on all TCP connections, so it
  mixes the two directions and different servers. It is not the per-stream
  RFC 3550 jitter.
- Throughput is the traffic actually seen, not link capacity, and counts whole
  frames including headers. Upload and download are not separated, because
  addresses are never captured.
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
| `telemetry_core` | Serialization, UTC formatting, bounded queues under concurrent producers, pipeline ids/history/eviction, JSONL rotation and failure handling, TShark field parsing and aggregation with inline sample lines, `tshark -D` parsing and interface name matching, runtime contracts (400 checks) |
| `browser_interface` | The V0.2 browser suite plus the Chrome-style title bar, tab search, Refresh window stability, focus rings and tab close-button alignment (51 passed) |
| `browser_observability` | Navigation lifecycle and monotonic timing, multiple tabs, activation and closure, failed/stopped navigation, renderer termination, URL redaction, TShark missing and failed start, capture off by default, interface listing and unknown-name refusal with an installed TShark (skipped without it), interface dropdown, panel open/close/shortcut, docking/resizing/floating, layout persistence, event display and bounding, missing metrics, measured metrics, runtime and decisions unavailable, contract display, browser use with panel open, window shutdown (30 passed, 2 opt-in skips) |

TShark parsing and the Network tab are tested with sample `-T fields` lines
embedded in the test sources; no fixture files or helper program is needed.
Exit codes and stderr from a failing TShark process have no automated test,
because no stand-in executable is kept in the repository.

Two tests run only on request:
- `tsharkLiveCaptureOptIn` performs a real 8-second capture when
  `AAFR_LIVE_CAPTURE=1` is set (interface from `AAFR_TSHARK_INTERFACE`,
  default `wifi`). It records AAFR's metadata fields only and writes nothing to
  disk; it needs some network traffic, and makes a DNS lookup and a TCP
  connection to example.com itself.
- `captureDevToolsScreenshots` saves real Qt renders of each panel tab
  (bottom and right docked) when `AAFR_UI_CAPTURE_DIR` is set.

### V0.3 verification record (2026-11-10)

Windows 11, MSVC 19.44 x64, Qt 6.12.0 Debug, Ninja, in `build\windows-msvc-qt`:

- The build succeeded with no compiler warnings.
- CTest passed 4/4 suites. Qt Test reported 30 passed, 2 skipped (opt-in
  live capture and screenshots) in `browser_observability`, and 51 passed in
  `browser_interface`. `telemetry_core` passed 400 checks. The observability
  suite takes about 16 s when run directly and up to 112 s on the first CTest
  run after a rebuild.
- Smart App Control intermittently blocked newly built unsigned test
  executables (`browser-interface-tests.exe` in one run, `telemetry-core-tests.exe`
  in another; Code Integrity events 3033/3077) and allowed them on a later
  run. No policy was changed.
- Developer Tools captures were rendered and inspected in
  `build/ui-verification/v0.3-devtools/`: all five tabs bottom-docked, the
  Network tab right-docked, and the hidden state. Their network values come
  from the sample lines published by the test, not live capture.
- Live capture with TShark 4.6.9 and Npcap (opt-in `tsharkLiveCaptureOptIn`):
  `wifi` resolved to Wi-Fi; 53 packets (34,009 bytes) in about 4 s; TCP ACK
  RTT 52.7–95.5 ms (mean 88.4 ms); one DNS response at 377.6 ms; no capture
  errors; stop returned the monitor to Off. This exercised real TShark output,
  listing, start and stop. While capturing, two processes ran (`tshark.exe`
  and its `dumpcap.exe` capture child); after Stop, and after closing a
  browser window mid-capture, neither was left running.

Not verified:

- TShark exit codes and stderr from a failing capture (for example Npcap
  denying access) against real TShark; only an unknown interface name and a
  missing or unstartable executable are tested. Field output (for example
  `True`/`1` for flags) may differ in other TShark versions.
- Interactive use of the standalone `aafr-browser.exe`, including real mouse
  drag-docking, floating-window behaviour and native-frame appearance. The
  captures are client-area renders, not desktop screenshots.
- Renderer termination from a real crash (the test emits the signal).

## V0.4 NetworkState, constraints and algorithm eligibility

V0.4 turns V0.3 telemetry into validated, structured inputs for a future
runtime: a `NetworkState`, a set of `NetworkConstraints`, capability profiles
for the ten planned algorithms, and a deterministic eligibility evaluation
with reason codes. It implements **no** routing algorithm, **no** Expert
Model, and **no** algorithm selection or switching. All ten algorithms are
reported as not implemented, because none is.

### Architecture

```text
TelemetryPipeline (V0.3)
   │  events with id > last seen (incremental, bounded batches)
   ▼
NetworkStateService ── worker thread, 1 s tick ───────────────────────────────┐
   │ validation        feature_validator    reject, count, report; never repair│
   │ normalization     feature_normalizer   canonical units                    │
   │ aggregation       feature_extractor    bounded rolling series             │
   │ feature extraction                     per observation window             │
   │ quality           measurement_quality  valid / missing / stale / ...      │
   ▼                                                                           │
NetworkState ─► ConstraintEvaluator ─► AlgorithmEligibilityEvaluator           │
                (+ ExecutionEnvironment)   (+ CapabilityRegistry)              │
   ▼                                                                           │
StateSnapshot (immutable, shared_ptr) ─► DevToolsPanel reads the latest ◄──────┘
   └─► diagnostic events back into the TelemetryPipeline (on change only)
```

| Component | Location | Responsibility |
| --- | --- | --- |
| `NetworkState`, `Feature` | `network/network_state/network_state.h` | Versioned snapshot contract (schema 1) |
| `ExecutionEnvironment` | `network/network_state/execution_environment.h` | Topology, forwarding operations and budgets; every unknown is an empty optional |
| `FeatureValidator`, `FeatureNormalizer` | `network/network_state/feature_validator.*`, `feature_normalizer.*` | Metric contracts, sanity bounds, unit conversion |
| `RollingSeries`, `FeatureExtractor` | `network/network_state/feature_extractor.*` | Bounded time-ordered series, window statistics, the 25 features |
| `MeasurementQualityEvaluator` | `network/network_state/measurement_quality.*` | Quality classification and scope checks |
| `NetworkStateBuilder` | `network/network_state/network_state_builder.*` | Incremental ingest, session policy, snapshot building |
| `NetworkConstraints`, `ConstraintEvaluator` | `network/network_state/network_constraints.*` | Constraint contract, derivation and conflict detection |
| `NetworkStateService` | `network/network_state/network_state_service.*` | The pipeline above, its worker thread and diagnostics |
| `AlgorithmId`, `AlgorithmCapability` | `algorithms/algorithm_id.h`, `algorithm_capability.h` | Ids and the typed capability contract (schema 1) |
| `CapabilityRegistry` | `algorithms/capability_registry.*` | The ten profiles, profile validation, availability tracking |
| `ReasonCode` | `algorithms/eligibility_reason.*` | The single reason-code catalog |
| `AlgorithmEligibilityEvaluator` | `algorithms/eligibility_evaluator.*` | The ten eligibility rules |

Everything above is in the `aafr-network-state` library: plain C++17, no Qt,
linked against `aafr-telemetry` only. The V0.3 viewer contract
`observability::AlgorithmCapability` is kept; `ToObservabilityCapability()`
converts the typed profile to it.

### NetworkState schema

| Group | Fields |
| --- | --- |
| Identification | `state_id` (increments per snapshot), `schema_version`, `telemetry_session_id`, `capture_session_id` (`<session>/c<n>`), `observed_at`, `observed_monotonic_ns`, `windows[]` with name and duration, `primary_window`, `freshness_ms`, `validity` (`valid` / `degraded` / `no_data`) with a reason |
| Features | Per window, the 25 features below, each with `value` (optional), `unit`, `kind` (`measured` / `derived` / `configured` / `unknown`), `quality`, `quality_reason`, `sample_count`, `age_ms`, `source`, `scope`, `attribution` |
| Sources and scope | `sources.browser_events`, `sources.capture` (`never_started` / `active` / `stopped` / `error`), capture interface, `network_scope` (always `capture_interface`), observed QUIC versions |
| Browser context | Active tab id, its load progress, last navigation outcome and tab, time of the newest browser event |
| Execution context | Forwarding topology availability, supported forwarding operations, computation deadline, memory and CPU budgets, copied from the `ExecutionEnvironment`; empty optional = unknown |
| Quality and counters | Quality summary of the primary window; accepted, ignored, rejected, duplicate and late event counts; capture sessions; session resets |

A missing measurement is an empty optional with a quality and a reason. Zero
always means that zero was observed.

### Features and units

V0.3 publishes one summary per capture interval, not one event per packet, so
every feature is defined over interval summaries. "Samples" are intervals or
navigation events.

| Feature | Unit | Kind | Definition |
| --- | --- | --- | --- |
| `rtt.handshake.mean_ms` | ms | measured | Interval means of TCP handshake RTT, weighted by connections per interval: the exact mean over connections |
| `rtt.handshake.median_ms` | ms | derived | Median of the interval means |
| `rtt.handshake.min_ms`, `.max_ms` | ms | measured | Smallest interval minimum, largest interval maximum |
| `rtt.handshake.variation_ms` | ms | derived | Sample standard deviation (n − 1) of the interval means; needs two |
| `ack_delay.mean_ms`, `ack_delay.jitter_ms` | ms | measured | TShark ACK RTT (both directions, not path latency), weighted by samples |
| `dns.response_time.mean_ms`, `.max_ms` | ms | measured | Weighted mean and maximum of DNS response time |
| `dns.responses` | count | measured | Sum over the window |
| `tcp.retransmissions` | count | measured | Sum of TShark's retransmission flags |
| `tcp.retransmission_ratio` | ratio | derived | Retransmissions ÷ TCP packets. An indicator, not a packet-loss probability; undefined (missing) with no TCP packets |
| `throughput.mean_mbps` | Mbps | derived | Bytes × 8 ÷ summed interval lengths (time-weighted) |
| `throughput.peak_mbps` | Mbps | measured | Largest interval rate |
| `traffic.packets`, `traffic.bytes`, `tcp.packets`, `udp.packets`, `quic.packets` | count / bytes | measured | Sums over the window |
| `tcp.connection_observations` | count | measured | Connections with traffic, summed per interval (a connection active in three intervals counts three times) |
| `nav.duration.median_ms`, `.mean_ms` | ms | measured | Durations of **successful** navigations. Page-load time; never an RTT |
| `nav.succeeded`, `nav.failed` | count | measured | Navigation outcomes; an observed zero once browser events exist |
| `nav.failure_ratio` | ratio | derived | Failed ÷ (succeeded + failed); stopped navigations excluded |

Network features have scope `capture_interface` and attribution
`unattributed`: they cover all host traffic and are never presented as one
tab's traffic. `MeasurementQualityEvaluator::ForScope` returns
`unattributed` with no value when a tab-level view of one is requested.

Normalization is unit conversion only (`kNormalizationVersion = 1`): ms,
Mbps, ratio 0..1, percent 0..100, integer counts. There is no min-max or
z-score scaling, because V0.4 has no justified reference ranges.

Validation rejects a measurement that has no value, is not finite, is
negative, carries a unit of another quantity, has a fractional count, or
exceeds a sanity bound (durations 3,600,000 ms; throughput 1,000,000 Mbps;
counts 2^53). Rejected values are counted and reported, never repaired.

### Rolling windows

Windows are configuration (`BuilderConfig::windows`). The defaults are
starting points for experiments, not universal constants: `short` 10 s,
`medium` 60 s (primary), `baseline` 300 s. A window covers
`now − duration < t ≤ now` on the telemetry monotonic clock.

- Memory is bounded twice: samples older than the longest window are pruned,
  and each series holds at most `max_samples_per_series` (1,024).
- Out-of-order events inside the horizon are inserted at their own time.
  Events older than the longest window are refused as late.
- A repeated event id is refused as a duplicate.
- A new capture (`capture.started`) discards all capture-interface samples:
  the interface or network may have changed, so captures are never averaged
  together. Browser samples are kept.
- A stopped or failed capture keeps its samples; they age into `stale`.
- A new telemetry session id discards everything.
- Network metrics without an observed start event imply a capture session.

### Data-quality rules

| Quality | When |
| --- | --- |
| `valid` | Value in range, fresh, and enough samples |
| `missing` | Nothing observed (capture never started, metric not reported, or an undefined ratio) |
| `stale` | Newest observation older than the stale limit (10 s by default; the value is kept but not usable), or observed earlier in this capture and not inside the window |
| `insufficient_samples` | Fewer samples than needed: 3 by default for means, medians and ratios, 2 at least for variation, 1 for sums and extremes |
| `invalid` | Inputs were rejected inside the window, or the computed value is outside its unit's range |
| `unattributed` | A capture-interface feature requested at tab or navigation scope |

Browser features have no separate stale limit: navigations are sparse, so the
window bounds their age. Only `valid` features are usable by constraints and
eligibility.

### Network constraints

`ConstraintEvaluator` produces 28 constraints from a `NetworkState` and an
`ExecutionEnvironment`. Each has an id, category (`network`, `performance`,
`execution`, `operational`), type, value, unit, source, timestamp (measured
and derived), validity and explanation.

| Source | Meaning | Examples |
| --- | --- | --- |
| `measured` | Read from the state | usable features, monitoring available, capture interrupted |
| `configured` | From the environment | topology, edge weights, protocols, deadline, budgets, forwarding operations, problem, objective |
| `derived` | Computed | required protocols supported, reliability within limit, state fresh, state validity |
| `unknown` | Nobody supplied it | any unset environment field |

An unknown constraint has no value, and the typed readers (`Bool`, `Number`,
`Text`, `List`) return nothing for unknown or invalid constraints, so neither
can be read as satisfied. Contradictions mark the affected constraints
`invalid`: non-positive budgets, CPU above 100 %, a ratio limit outside 0..1,
a computation deadline longer than the latency budget, an available topology
with no representation, a graph with no nodes, an empty candidate path set, a
negative cycle without negative weights, candidate paths to an unreachable
destination.

The browser's own environment (`BrowserDefaultEnvironment`) states what is
true today: no forwarding topology exists and no forwarding operation can be
executed. Budgets, problem and objective are unknown.

### Algorithm capability model and profiles

`AlgorithmCapability` separates `AlgorithmProperties` (inherent to the
published algorithm) from `ImplementationProfile` (measured facts about
AAFR's implementation: worst-case runtime, peak memory, covered topology
size, execution environment, verification note). Every implementation field
is empty today.

Availability states: `profile_defined`, `implementation_unavailable`,
`implementation_present`, `verified_executable`, `unsupported`. The registry
refuses `verified_executable` without measured runtime and memory, an
execution environment and a verification note. All ten built-in profiles are
`implementation_unavailable`.

| Algorithm | Problems | Topology | Edge weights | Guarantee | Time | Required configuration |
| --- | --- | --- | --- | --- | --- | --- |
| Dijkstra | single-source, single-pair shortest path | weighted directed graph | nonnegative | exact | O((V + E) log V), binary heap | none |
| A* | single-pair shortest path | weighted directed graph | nonnegative | exact only with an admissible heuristic | O((V + E) log V) worst case with a consistent heuristic | `heuristic` |
| Bellman-Ford | single-source, single-pair shortest path | weighted directed graph | negative allowed, no reachable negative cycle (detected) | exact | O(V · E) | none |
| ACO | candidate-solution optimization | graph or candidate path set | set by the objective | none, stochastic | O(iterations · ants · path construction) | common + `ant_count`, `evaporation_rate`, `pheromone_weight`, `heuristic_weight` |
| PSO | candidate-solution optimization | candidate path set | set by the objective | none, stochastic | O(iterations · particles · evaluation) | common + `swarm_size`, `inertia_weight`, `cognitive_coefficient`, `social_coefficient` |
| GA | candidate-solution optimization | graph or candidate path set | set by the objective | none, stochastic | O(generations · population · evaluation) | common + `population_size`, `crossover_rate`, `mutation_rate`, `selection_method` |
| ABC | candidate-solution optimization | candidate path set | set by the objective | none, stochastic | O(iterations · colony · evaluation) | common + `colony_size`, `abandonment_limit` |
| GWO | candidate-solution optimization | candidate path set | set by the objective | none, stochastic | O(iterations · wolves · evaluation) | common + `pack_size` |
| Firefly | candidate-solution optimization | candidate path set | set by the objective | none, stochastic | O(iterations · fireflies² · evaluation) | common + `firefly_count`, `attractiveness`, `absorption_coefficient`, `randomization` |
| WOA | candidate-solution optimization | candidate path set | set by the objective | none, stochastic | O(iterations · whales · evaluation) | common + `whale_count`, `spiral_constant` |

"Common" is `solution_encoding`, `objective_function`, `max_iterations`,
`random_seed`. PSO, ABC, GWO, Firefly and WOA were formulated for continuous
search spaces and need a discrete encoding of candidate paths. No profile
claims that an algorithm performs better under latency, loss or congestion;
that needs experiments.

Every profile lists `rtt.handshake.mean_ms` as its required NetworkState
feature. This is **provisional**: V0.4 has no forwarding-problem definition,
and host-wide latency evidence is not a per-path edge weight.

### Eligibility rules

`AlgorithmEligibilityEvaluator` runs ten checks per algorithm, in this order.
It uses no clock, randomness or I/O, so equal inputs give equal results.

| # | Check | Passes when | Otherwise |
| --- | --- | --- | --- |
| 1 | `implementation_availability` | `verified_executable` | `IMPLEMENTATION_UNAVAILABLE`, `IMPLEMENTATION_UNVERIFIED`, `IMPLEMENTATION_UNSUPPORTED` |
| 2 | `required_features` | Every required feature is `valid` in the primary window | `REQUIRED_FEATURE_MISSING`, `REQUIRED_FEATURE_UNUSABLE` |
| 3 | `topology_information` | A topology exists in a representation the algorithm accepts | `TOPOLOGY_UNAVAILABLE`, `TOPOLOGY_UNKNOWN`, `UNSUPPORTED_TOPOLOGY` |
| 4 | `problem_representation` | The algorithm solves the defined problem and objective | `PROBLEM_UNDEFINED`, `UNSUPPORTED_PROBLEM_REPRESENTATION`, `OBJECTIVE_UNDEFINED`, `UNSUPPORTED_OBJECTIVE` |
| 5 | `edge_weights` | Weights meet the algorithm's requirement | `UNSUPPORTED_EDGE_WEIGHTS`, `EDGE_WEIGHTS_UNKNOWN`, `NEGATIVE_CYCLE_PRESENT` |
| 6 | `required_configuration` | Every required parameter is supplied; a heuristic is available if needed | `REQUIRED_CONFIGURATION_MISSING`, `HEURISTIC_UNAVAILABLE`, `HEURISTIC_UNKNOWN` |
| 7 | `computation_budget` | Measured worst-case runtime ≤ deadline, at a covered topology size | `COMPUTATION_BUDGET_EXCEEDED`, `COMPUTATION_BUDGET_UNKNOWN`, `COMPUTATION_COST_UNKNOWN` |
| 8 | `memory_budget` | Measured peak memory ≤ budget | `MEMORY_BUDGET_EXCEEDED`, `MEMORY_BUDGET_UNKNOWN`, `MEMORY_COST_UNKNOWN` |
| 9 | `forwarding_execution` | The environment can execute an operation the result needs, and required protocols are supported | `FORWARDING_CAPABILITY_UNAVAILABLE`, `FORWARDING_CAPABILITY_UNKNOWN`, `UNSUPPORTED_PROTOCOL`, `PROTOCOL_SUPPORT_UNKNOWN` |
| 10 | `data_quality` | The state has usable features and is fresh | `NETWORK_STATE_UNUSABLE`, `STALE_NETWORK_STATE` |

A check that depends on an `invalid` constraint fails with
`CONSTRAINT_INVALID`.

Status: `not_implemented` if no implementation exists (the other checks are
still reported); otherwise `ineligible` if any check failed; otherwise
`unknown` if any check could not be decided; otherwise `eligible` with
`ELIGIBILITY_CONFIRMED`.

**Policy for unknown information.** A check whose inputs are unknown has the
outcome `unknown`; it never counts as passed. An unconfigured budget is
unknown, not unlimited. `EligibilityPolicy::unknown_is_ineligible` (default
off) reports such algorithms as `ineligible` instead of `unknown`.

Each result carries the algorithm id, status, all ten check results, reason
codes (failed first, then unknown), the missing information (constraint ids,
feature ids, `config:<name>`), and an explanation assembled from the values
that were compared. Eligible does not mean best: nothing is ranked, selected
or switched.

### Reason-code catalog

Defined once in `algorithms/eligibility_reason.cpp`, with a description per
code (`Describe`). The texts are stable.

`ELIGIBILITY_CONFIRMED`, `CHECK_PASSED`, `IMPLEMENTATION_UNAVAILABLE`,
`IMPLEMENTATION_UNVERIFIED`, `IMPLEMENTATION_UNSUPPORTED`,
`REQUIRED_FEATURE_MISSING`, `REQUIRED_FEATURE_UNUSABLE`,
`TOPOLOGY_UNAVAILABLE`, `TOPOLOGY_UNKNOWN`, `UNSUPPORTED_TOPOLOGY`,
`PROBLEM_UNDEFINED`, `UNSUPPORTED_PROBLEM_REPRESENTATION`,
`OBJECTIVE_UNDEFINED`, `UNSUPPORTED_OBJECTIVE`, `UNSUPPORTED_EDGE_WEIGHTS`,
`EDGE_WEIGHTS_UNKNOWN`, `NEGATIVE_CYCLE_PRESENT`,
`REQUIRED_CONFIGURATION_MISSING`, `HEURISTIC_UNAVAILABLE`,
`HEURISTIC_UNKNOWN`, `COMPUTATION_BUDGET_EXCEEDED`,
`COMPUTATION_BUDGET_UNKNOWN`, `COMPUTATION_COST_UNKNOWN`,
`MEMORY_BUDGET_EXCEEDED`, `MEMORY_BUDGET_UNKNOWN`, `MEMORY_COST_UNKNOWN`,
`FORWARDING_CAPABILITY_UNAVAILABLE`, `FORWARDING_CAPABILITY_UNKNOWN`,
`UNSUPPORTED_PROTOCOL`, `PROTOCOL_SUPPORT_UNKNOWN`, `STALE_NETWORK_STATE`,
`NETWORK_STATE_UNUSABLE`, `CONSTRAINT_INVALID`.

### Update pipeline and threading

`BrowserWindow` owns one `NetworkStateService`. `Start()` runs the first step
on the calling thread, so a snapshot exists immediately, then a worker thread
runs one step per second. A step reads only events newer than the last id it
saw (at most 8,192 per step), so nothing is rebuilt from history. The result
is published as an immutable `StateSnapshot` behind a `shared_ptr`; the GUI
thread only copies that pointer. The worker is joined before the telemetry
pipeline is destroyed.

Diagnostic events are published on change only, so an idle browser adds
nothing to the bounded telemetry history:

| Event type | Published when |
| --- | --- |
| `state.updated` | Validity, quality summary or capture status changed; otherwise at most every 5 s while measurements arrive |
| `state.feature_validation_failed` | Measurements were rejected in the step (one event per metric and issue, with a count) |
| `state.measurement_stale` | Features of the primary window became stale |
| `constraints.evaluated` | The constraint set changed |
| `eligibility.evaluated` | Any algorithm's status or reason codes changed |
| `eligibility.algorithm_excluded` | One per algorithm whose result changed and is not eligible |

They carry source `state_pipeline`. None is a runtime decision. The
telemetry schema version stays 1: event types and a source were added, no
field changed.

### Developer Tools

- **Overview** gains NetworkState status, latest state update, feature
  completeness, data-quality summary and the number of eligible algorithms.
  The page now scrolls instead of clipping.
- **Network** has two inner tabs. *Latest interval* is the V0.3 table.
  *NetworkState features* shows the features of a selectable observation
  window with value, unit, kind, quality, samples, freshness,
  source/scope/attribution and the quality reason. A missing value is shown
  as "—", never 0.
- **Runtime** has four inner tabs. *Runtime state* is the V0.3 form and still
  reads "Not available — runtime not integrated". *Eligibility*, *Capability
  registry* and *Constraints* are V0.4 diagnostics under the banner "Runtime
  not integrated — active algorithm unavailable." No active algorithm is
  displayed.
- **Decisions** is unchanged: empty until a runtime provider exists.
  Eligibility results are not shown there.
- **Events** gains a *Diagnostics* source filter for the event types above.

### Security and privacy

Unchanged from V0.3: capture stays off until started in the panel, no packet
payloads, addresses or ports are requested, URLs stay redacted, no elevation
is requested, and no Windows security setting is touched. V0.4 stores only
numeric aggregates and tab ids. Without TShark the browser works as before
and the state reports reduced observability (`packet capture is
never_started`, network features `missing`).

### Tests

| CTest suite | Covers |
| --- | --- |
| `network_state` | Median, weighted mean, standard deviation, window expiry and boundaries, capacity, duplicates, late and out-of-order samples; unit conversion; validation of each issue; aggregation with exact expected values; empty windows and missing values; stale measurements and capture interruption; capture sessions and telemetry-session reset; browser features; quality classification and scope; configuration and bounded memory; configured, unknown and conflicting constraints, unsupported protocols (922 checks) |
| `algorithm_eligibility` | All ten profiles registered with unique ids and valid metadata; unknown algorithms; profile validation; availability tracking and refusal of unevidenced executable claims; reason-code catalog; eligible, ineligible, unknown and not-implemented results; missing, stale and thin features; topology, problem, objective, edge weights, heuristic, configuration, budgets, topology scale, forwarding operations, protocols; invalid constraints; multiple simultaneous failures and code ordering; determinism (780 checks) |
| `state_pipeline` | Real `TelemetryPipeline` and TShark parser with fixture lines through state, constraints and eligibility; diagnostics on change only; incremental reads; rejection reporting; stale reporting; a verified registry fixture reaching `eligible`; concurrent readers against the worker thread; stop, restart and destruction; history overflow and memory bounds (387 checks) |
| `browser_observability` | V0.3 cases plus: panel without a state pipeline, state and eligibility without capture, state from published telemetry, diagnostics filter, no fabricated active algorithm or decisions, snapshot reads not blocking, worker shutdown with the window (34 passed, 2 opt-in skips) |

Tests that need an executable algorithm build a registry fixture with
invented measurements inside the test. The shipped registry has none.
No test needs live packet capture.

### V0.4 verification record (2026-11-14)

Windows 11, MSVC 19.44 x64, Qt 6.12.0 Debug, Ninja, in `build\windows-msvc-qt`:

- The build succeeded with no compiler warnings or errors.
- CTest passed 7/7 suites: `runtime_core_smoke`, `telemetry_core`,
  `network_state` (922 checks), `algorithm_eligibility` (780 checks),
  `state_pipeline` (387 checks), `browser_interface` (52 passed),
  `browser_observability` (34 passed, 2 opt-in skips).
- Before any V0.4 change, the V0.3 baseline passed 3/4 suites: one
  `browser_observability` case (`navigationLifecycleAndTiming`) failed once
  on a 1 ms timing tolerance and passed on three reruns and in every later
  run. It was left unchanged.
- Developer Tools captures were rendered with `captureDevToolsScreenshots`
  and inspected: Overview, both Network inner tabs and the Eligibility,
  Capability registry and Constraints tabs, bottom-docked at 1280×800. Their
  network values come from fixture lines, not live capture.
- Measured step time of the state pipeline in this Debug build: 3.7 to 9.0 ms
  per step in the tests. Not measured in a Release build.

Not verified:

- The V0.4 pipeline against a live TShark capture, and the Release preset
  build.
- Interactive use of the standalone `aafr-browser.exe` with the new tabs.
- Behaviour over hours of continuous capture; memory bounds are tested with
  thousands of intervals, not long real runs.

### Known limitations

- Statistics are over per-interval summaries. The median and variation are
  of interval means, not of per-connection samples, which V0.3 does not
  publish.
- Network features are host-wide. There is no per-tab, per-connection or
  per-path measurement, so nothing can serve as a per-path edge weight.
- The retransmission ratio is TShark's heuristic over all TCP packets, not a
  loss rate. No QUIC loss or RTT is observable from encrypted traffic.
- The required-feature list of every profile is provisional (see above).
- The `ExecutionEnvironment` is configured in code. The browser has no way
  to load one from a file, and no forwarding topology exists.
- Computation and memory checks need measured implementation profiles, which
  do not exist, so those checks are `unknown` for every algorithm.
- The state shown in the panel can be up to one tick (1 s) behind the V0.3
  rows next to it.
- Windows and thresholds have default values that are not experimentally
  tuned.

### V0.5 prerequisites

Items 1, 3, 4 and 5 are addressed by V0.5 for the controlled topology (see
below); item 2 is addressed only for that topology, whose edge values are
configured, not measured; item 6 remains V0.6 work.

1. A forwarding-problem definition: what a node, an edge and a path are for
   application-layer forwarding, and where a topology comes from.
2. Per-path or per-endpoint measurements to derive edge weights or objective
   inputs, replacing the provisional host-wide latency requirement.
3. A forwarding execution layer that can carry out at least one
   `ForwardingOperation`, with verification.
4. At least one algorithm implementation behind a common interface, with
   measured runtime and memory to fill its `ImplementationProfile`.
5. Configured budgets (deadline, memory) and a way to load an
   `ExecutionEnvironment`.
6. Only then: selection among eligible algorithms and `DecisionRecord`
   production through `IRuntimeObservabilityProvider`.

## V0.5 controlled forwarding engine and algorithms

V0.5 separates four things that later milestones must not blur:
**recommendation** (V0.7, the Expert Model), **algorithm computation**
(V0.5), **forwarding execution** (V0.5) and **runtime switching** (V0.6).
V0.5 computes a path with an explicitly named algorithm, validates it,
forwards one request along it and reports what happened. It never picks an
algorithm and never re-runs anything by itself.

### Architecture

```
ForwardingGraph (versioned, provenance per value)      routing/topology/
  + ForwardingProblem (source, destination, objective, hard constraints)
        |
        v
AlgorithmExecutionEngine::Submit(request)              algorithms/execution/
  1. validate request and problem
  2. V0.4 eligibility (environment derived from the graph and executor)
  3. IPathAlgorithm::Run  (one of ten)                  algorithms/<name>/
  4. ValidatePath (the single validator)               routing/topology/
  5. IForwardingExecutor::Execute                      routing/forwarding/
       LoopbackForwardingExecutor (real TCP on 127.0.0.1)
       SimulatedForwardingExecutor (tests, benchmark)
  6. telemetry events -> TelemetryPipeline -> Developer Tools / JSONL
```

All of it is plain C++17 in the `aafr-forwarding` library (no Qt).
`ForwardingService` (`routing/controller/`) is what the browser owns: the
loopback topology, its relays, verification and the engine. It starts when
Developer Tools is first shown, not with the browser. The engine runs
requests on one worker thread; the GUI thread only submits and reads
snapshots.

### Forwarding problem, graph and objective

`G = (V, E)` contains only nodes AAFR controls: one or more `entry` nodes,
`relay` nodes and `destination` nodes. `E` holds the forwarding transitions
those nodes are configured to allow. The graph is not inferred from TShark and
says nothing about Internet routing.

Every edge value is a `Measurement` with a provenance: `measured`,
`configured`, `estimated`, `simulated` or `unknown`. A missing value stays
unknown; it never becomes zero. `ForwardingGraph::Validate` rejects
duplicate or malformed ids, dangling edges, self-loops, non-finite or
negative latency, loss outside [0, 1), zero bandwidth and values without a
provenance. Every mutation bumps the graph version; a problem pinned to a
version (and a path computed for it) is rejected after the graph changes.

Objective (version 1), identical for all ten algorithms:

```
c(e) = w_latency * latency_ms(e) + w_loss * -ln(1 - loss(e)) + w_hop + w_policy * policy_cost(e)
J(P) = sum over the edges of P of c(e)          defaults: w_latency 1, w_loss 100, w_hop 0.5, w_policy 1
```

`-ln(1 - p)` makes loss additive: the sum over a path equals
`-ln(delivery probability)`. Because J is additive, Dijkstra, A* and
Bellman-Ford solve it exactly and every metaheuristic is scored by the same
function. `policy_cost` is a signed, operator-configured preference; it is
the only source of negative costs (no negative latency is ever invented).

Hard constraints are never folded into J:

| Kind | Constraint | Effect |
|---|---|---|
| per edge | edge and node availability, `allowed_relays`, `forbidden_nodes`, protocol, `min_bandwidth_mbps`, known values for every weighted term | the edge is not usable at all (`BuildView`) |
| per path | `latency_budget_ms`, `max_hops` | a path that breaks them is reported **infeasible**, with its score |
| computation | `computation_deadline_ms`, `max_objective_evaluations`, `memory_budget_bytes` | budget for the algorithm and inputs to eligibility |

Exact algorithms optimize J over usable edges and then check the path-level
limits; when the optimum breaks one they report `no_feasible_path` rather
than claiming an answer (constrained shortest path is NP-hard). The
metaheuristics include a penalty for path-level violations in their internal
fitness, scaled so every infeasible path ranks below every feasible one;
reported scores always come from `ValidatePath`, never from the penalized
fitness.

### Execution contract

`IPathAlgorithm::Run(AlgorithmRequest)` receives the graph, the problem
(with its objective and constraints), an `ExecutionBudget`
(iterations, evaluations, deadline), algorithm parameters, a seed and a
cancellation flag. It returns an `AlgorithmResult`: algorithm id and
version, status (`path_found`, `no_path`, `no_feasible_path`,
`negative_cycle`, `invalid_input`, `cancelled`, `error`), path, feasibility,
J(P), computation time, iterations, evaluations, termination reason, whether
optimality is claimed and why, effective parameters, seed, graph version and
accounted memory (bytes held by the algorithm's own containers, not process
memory). Unknown parameter names are rejected, not ignored.

`IForwardingExecutor::Execute(ForwardingRequest, ForwardingExecutionContext)`
returns the requested path, the **confirmed** path, `path_confirmed`,
response status and body, end-to-end time, the node where forwarding
stopped, the provenance of the timing and a text describing the evidence. A
path counts as used only when `path_confirmed` is true.

### Real local forwarding (loopback proof of concept)

`LoopbackForwardingNetwork` starts one TCP server per graph node on
`127.0.0.1` (ephemeral ports). The browser's topology is:

```
client -> entry -> relay_a -> dest        (latency 2 + 2 ms configured)
               \-> relay_b -> dest        (latency 4 + 1 ms configured)
           relay_a -> relay_b             (latency 1 ms configured)
```

Protocol (one line each way plus one payload line):

```
AAFR1 REQ <request_id> <budget_ms> <remaining route|-> <trace|->
AAFR1 RSP <request_id> <status> <trace>
```

Each node appends itself to the trace, refuses a request it has already
handled (`LOOP_DETECTED`) or one longer than 16 hops, forwards only to a
neighbour that the graph configures and that is enabled, connects only to
ports of its own network on 127.0.0.1, and relays the downstream answer
unchanged. The destination answers `OK`. The executor reports a path as
confirmed only when the response is `OK`, the returned trace equals the
requested path, **and** every node's own bounded request log contains the
request id. Tests check that choosing `relay_a` or `relay_b` changes which
relay's counter increases.

Safety: loopback only; no arbitrary destination can be named (there is no
host field); header and payload sizes are bounded; at most 32 concurrent
connections per node; every read has a deadline and polls the cancellation
and shutdown flags every 20 ms; `Stop()` closes the listening sockets and
joins every thread. Relay unavailability is real (the node's server is
stopped, or a link disabled). Optional `inject_edge_latency` sleeps each
configured edge latency inside the relay; timings are then labelled as
including injected (simulated) delay.

### Browser integration status

Browser traffic is **not** forwarded through the relays. Findings:

- Qt WebEngine has no per-tab or per-request proxy API. Chromium reads the
  proxy once, from `QNetworkProxy::applicationProxy()` or the
  `--proxy-server` flag in `QTWEBENGINE_CHROMIUM_FLAGS`, before the first
  profile is created; it applies to every profile and tab of the process.
  This is from the Qt documentation and was **not verified experimentally**
  in V0.5.
- Pointing that application-wide proxy at the entry node would require the
  entry to act as an HTTP proxy for arbitrary hosts (including `CONNECT`
  tunnels for HTTPS), which contradicts the "no unrestricted proxy" rule,
  and would route all browsing through it with no per-tab control.
- AAFR does not touch the Windows (system) proxy.

What runs instead: the engine forwards one AAFR request per manual execution
through the relays, and the result is shown in Developer Tools. A
standalone simulation is not presented as live forwarding; the loopback
relays carry real TCP traffic, but only AAFR's own requests.

### Algorithm implementations

| Algorithm | Representation | Search and update | Constraint handling | Optimality |
|---|---|---|---|---|
| Dijkstra | graph search | binary heap, lazy deletion, ties by node index | per-edge by the view; path limits checked after | exact for nonnegative costs; refuses negative costs |
| A* | graph search | `heuristic`: `zero` or `hop_lower_bound` (fewest hops to the destination x smallest edge cost; admissible and consistent); `heuristic_weight` w | as Dijkstra | claimed only for w = 1; w > 1 (weighted A*) claims none; `hop_lower_bound` falls back to `zero` when the smallest cost is not positive |
| Bellman-Ford | graph search | up to V-1 relaxation rounds, early stop, one extra round for detection | as Dijkstra | exact without a reachable negative cycle; otherwise `negative_cycle` with the cycle's nodes |
| ACO | pheromone on usable edges | ants choose unvisited neighbours with probability ~ tau^alpha eta^beta, eta = 1/(1 + c - c_min); evaporation rho; feasible ants and the best-so-far path deposit 1/J'; tau clamped to [0.01, 10] | dead-end ants fail; infeasible paths deposit nothing | none |
| PSO | random keys in [0,1]^V | inertia-weight velocity update, velocity clamp | penalty | none |
| GA | node sequence (variable-length path) | tournament or rank-roulette selection; common-node crossover with loop repair; mutation regrows a suffix by random walk; elitism | penalty; repair keeps paths simple | none |
| ABC | random keys | employed, onlooker (fitness-proportional) and scout phases; v_ij = x_ij + phi (x_ij - x_kj) | penalty | none |
| GWO | random keys | alpha, beta, delta leaders; a decreases 2 -> 0; A = 2ar1 - a, C = 2r2 | penalty | none |
| Firefly | random keys | pairwise attraction beta0 exp(-gamma r^2) with r^2 normalized by V; random step alpha decaying 0.97 per iteration | penalty | none |
| WOA | random keys | encircling, random search and logarithmic spiral (b = `spiral_constant`), a decreases 2 -> 0 | penalty | none |

**Discrete adaptation.** PSO, ABC, GWO, Firefly and WOA were published for
continuous spaces. Their update equations are kept unchanged and act on a
random-key vector (one real key per node). `DecodePriorities` turns keys into
a path with a depth-first walk from the source that always tries the
unvisited neighbour with the highest key and backtracks out of dead ends.
Every key vector decodes to a simple path when the destination is reachable,
and equal keys always decode to the same path. GA works on paths directly
and ACO constructs paths on the graph, so neither needs the encoding.

All stochastic algorithms use one portable generator (xoshiro256** seeded by
splitmix64), so a seed gives the same result on every platform. Every run is
bounded by its iteration budget, its evaluation budget, its deadline and the
cancellation flag. When the destination is unreachable over usable edges,
every algorithm returns `no_path` without searching.

### Capability registry integration

The V0.4 registry still registers every profile as
`implementation_unavailable`. `VerifyImplementations` then runs each
implementation on fixed fixtures (the 4-node topology, two 10-node graphs
checked against brute force over all simple paths, one with a latency
budget, a 42-node graph, an unreachable graph and a negative-cycle graph)
and checks validity, reported scores, optimality (exact) or never beating
the optimum (metaheuristics), reproducibility, budgets, `no_path` and
negative-cost behaviour. It then forwards each algorithm's path through the
executor and requires a confirmed path.

`ApplyVerification` records the result:

| Level | Meaning in V0.5 |
|---|---|
| profile registered | `CapabilityRegistry::BuiltIn()` |
| implementation available | implementation checks passed: `implementation_present` |
| verified | checks passed **and** forwarding confirmed over **real** traffic: `verified_executable`, with measured worst runtime, peak accounted memory, largest fixture (42 nodes) and a note. Simulated forwarding is recorded but never counts. |
| executable here | eligibility check `forwarding_execution` passes (an executor is ready) |
| eligible now | all ten V0.4 checks pass for the request |

The engine derives the V0.4 `ExecutionEnvironment` from the request: the
graph (node and usable-edge counts, reachability, sign of costs, a negative
cycle check when needed, and the candidate-path count capped at 100000),
the executor's operations and protocols, the problem representation by
family (`single_pair_shortest_path` for exact algorithms,
`candidate_solution_optimization` for metaheuristics), budgets, and every
algorithm's configuration including `solution_encoding`,
`objective_function`, `max_iterations` and `random_seed`. The reason codes
are the V0.4 catalog, unchanged.

The required-feature list of every profile still names the host-wide
`rtt.handshake.mean_ms`, and `data_quality` still requires a fresh
NetworkState. Neither describes the controlled topology, whose costs come
from the graph. An execution request may set
`enforce_host_state_checks = false`; those two checks may then fail without
blocking, every other check must pass, and the record lists them as
overridden. The default is to enforce.

### Benchmark methodology

`aafr-benchmark` runs `experiments/benchmark` over twelve seeded scenarios
(`routing/topology/scenario_catalog.cpp`): small (8 nodes), medium (42),
large (152), sparse, dense, multiple near-equal candidates, disconnected,
high-latency edges with a 150 ms budget, a 600 Mbps bandwidth floor, 20 %
link failures, a limited budget (3 iterations, 30 evaluations) and a dynamic
topology (4 epochs of latency drift and failures). All graph values are
`simulated`. Each scenario epoch is solved by every algorithm with the same
objective and budget (default 100 iterations, 4000 evaluations, 5 s);
deterministic algorithms once, stochastic ones once per seed (default 1-5).
The reference is the exact algorithm's feasible optimum; gap =
(J - J_ref) / |J_ref|. Feasible paths are passed through the simulated
executor (labelled `simulated`). Computation time is measured on the host.

```
aafr-benchmark --out build\benchmark-results --seeds 5
```

writes `benchmark_runs.jsonl` (a header record with schema version, budget,
seeds, provenance, feature definitions with units, scenario configurations
and algorithm configurations; one record per run; one summary per scenario
and algorithm with count, mean, sample standard deviation, min, median and
max) and `benchmark_runs.csv` (one row per run).

### Benchmark results (Debug build, 2026-11-14)

570 runs, 5 seeds, default budget, 43 s total, Debug (unoptimized) build, so
absolute times are high. `at ref` counts runs that reached the exact
reference.

| Scenario | Exact (3) | ACO | GA | GWO | PSO | ABC | WOA | Firefly |
|---|---|---|---|---|---|---|---|---|
| small, sparse | reference | 5/5 at ref | 5/5 | 5/5 | 5/5 | 5/5 | 5/5 | 5/5 |
| medium (mean gap) | reference | 0 % | 5.2 % | 12.3 % | 25.1 % | 9.6 % | 18.3 % | 31.4 % |
| large (mean gap) | reference | 0 % | 0 % | 23.9 % | 30.1 % | 118.9 % | 91.4 % | 54.6 % |
| dense (mean gap) | reference | 0.9 % | 3.2 % | 0.5 % | 6.1 % | 3.9 % | 17.7 % | 2.3 % |
| high_latency, feasible runs | reference | 5/5 | 5/5 | 5/5 | 5/5 | 4/5 | 1/5 | 1/5 |
| constrained_bandwidth, feasible runs | reference | 2/5 | 5/5 | 5/5 | 5/5 | 5/5 | 5/5 | 5/5 |
| limited_budget (mean gap) | reference | 23.8 % | 49.5 % | 34.0 % | 62.6 % | 73.8 % | 38.6 % | 52.7 % |
| disconnected | `no_path` | `no_path` (all algorithms, all seeds) | | | | | | |

Mean computation time on `large`: exact 14-16 ms; ACO 54 ms; Firefly 54 ms;
WOA 180 ms; PSO 227 ms; GWO 276 ms; GA 294 ms; ABC 1032 ms.

These numbers describe these implementations, this objective, these
budgets and these simulated graphs only. On an additive objective the exact
algorithms are optimal by construction, so the metaheuristics can at best
tie them here; the comparison is a correctness and cost baseline, not
evidence about which algorithm suits real networks. ACO's constraint
handling by rejection (dead-end ants fail, infeasible ants deposit nothing)
finds feasible paths less reliably under the bandwidth floor, where many
edges are unusable. Firefly and WOA rarely found a feasible path under the
latency budget at this budget.

### Telemetry

Source `forwarding_engine`; additive to schema 1:
`algorithm.execution_started`, `algorithm.execution_completed`,
`algorithm.execution_failed`, `algorithm.candidate_path_rejected`,
`forwarding.execution_started`, `forwarding.execution_completed`,
`forwarding.execution_failed`. Every event carries `execution_id`,
`algorithm`, `scenario_id`, `network_state_id`, `graph_id` and
`graph_version`; computation events add status, termination, computation
time, iterations, evaluations, objective, feasibility and path; forwarding
events add `request_id`, executor, requested and confirmed paths,
`path_confirmed`, response status, end-to-end time and provenance.
Computation time and forwarding time are separate fields and are never
combined. The NetworkState builder ignores these events, as it ignores its
own diagnostics.

### Developer Tools

- **Overview:** forwarding engine state, verified algorithm count, latest
  execution and latest forwarding outcome.
- **Network > Controlled forwarding:** topology summary (graph id and
  version, constraints, node ports), edge table (latency, loss, bandwidth,
  provenance, availability, edge cost, requests handled) and every
  available path with hops, latency, J(P) and feasibility.
- **Runtime:** `Adaptive Runtime: Not integrated` at the top, kept apart from
  everything else. **Algorithm execution** tab: per-algorithm registry
  status, implementation checks, real-forwarding verification, worst
  runtime, eligibility on the controlled topology with reason codes;
  controls (algorithm, seed, iterations, forward or not, host-state
  override, Run, Cancel); progress and result (stage and outcome,
  eligibility and overridden checks, candidate path, computation time,
  iterations and evaluations, J(P) with optimality note, forwarding result,
  confirmed path, end-to-end time with provenance, evidence).
- **Decisions:** unchanged. Reserved for V0.6 `DecisionRecord`s; manual
  executions are not decisions and are not shown there.
- **Events:** a fifth filter, **Forwarding**, for the seven new events. The
  table stays bounded at 500 rows.

The panel computes eligibility for the table only when the NetworkState,
the service state or the override changes; algorithms and forwarding run on
the engine's worker thread.

### Security and resource use

The relays listen on 127.0.0.1 only and forward only within their own
network; there is no host or URL field to abuse. No packet capture, no
system proxy or routing change, no firewall rule and no elevated privilege
is used. Requests carry a short payload and a request id; nothing is stored
on disk. Engine history is bounded (64 records), the queue is bounded (8),
forwarding is serialized, and every computation is bounded by iterations,
evaluations and a deadline.

### V0.5 tests

| Program | Covers |
|---|---|
| `forwarding-graph-tests` | valid and invalid graphs, problem validation, objective values, every path issue, problem view, simulated executor (delivery, unavailability, seeded loss, cancellation), scenario catalog determinism |
| `path-algorithms-tests` | known shortest paths, exact algorithms against brute force on 12 graphs, A* heuristics and weighted A*, invalid inputs, negative edges and cycles, unreachable destinations, metaheuristic validity and seed reproducibility, iteration/evaluation/deadline/cancel budgets, latency budget, impossible budgets and allowed relays, verifier and registry levels |
| `loopback-forwarding-tests` | relay selection proven by node counters, refusal of non-neighbours, loops, unknown nodes and malformed input, link down, relay unavailable and restarted, timeout, cancellation, concurrent requests, shutdown and restart, stale graph, verification over real traffic (10/10) |
| `benchmark-tests` | scenario loading, statistics, fixed-seed reproducibility, scenario outcomes, JSONL and CSV export |
| `forwarding-pipeline-tests` | NetworkState -> eligibility -> computation -> validation -> loopback forwarding -> telemetry for all ten algorithms; ineligible, overridden, candidate rejected, relay failure, async, cancellation, queue bound |
| `browser-observability-tests` (2 new cases) | panel without a forwarding service; nothing starts before Developer Tools opens; the browser verifies 10/10, the V0.4 registry shows them as verified executable, and a Dijkstra run from the panel is forwarded and confirmed |

### V0.5 verification record (2026-11-14)

Debug preset (`build\windows-msvc-qt`, MSVC 19.44, Qt 6.12.0, browser tests
on). `ctest --test-dir build\windows-msvc-qt --output-on-failure`: **12 of 12
passed** (the 7 existing programs and the 5 new ones). Times: forwarding_graph
0.5 s, path_algorithms 6.2 s, loopback_forwarding 3.7 s, benchmark 3.3 s,
forwarding_pipeline 14.2 s, browser_interface 65.1 s (54 passed),
browser_observability 46.8 s (36 passed, 2 skipped as before).
`browser_interface` ran in 54 s before V0.5 and 65-75 s after on this
machine, inside its 90 s limit; the cause was not isolated. Not verified: a
Release build of V0.5, interactive use of the new panel sections by a person,
and browser traffic through the relays (not implemented).

The forwarding service starts when Developer Tools is first shown (or
`BrowserWindow::startForwarding()` is called): it opens four loopback ports
and runs verification on a worker thread. Until then it holds no socket and
no thread.

### Known limitations

- Browser traffic is not routed through the forwarding layer (see above).
- The controlled topology's values are configured, and the benchmark's are
  simulated. No edge value in V0.5 is a measurement of a real network path;
  per-relay RTT probing is not implemented.
- Verified runtimes come from the Debug build and from graphs up to 42
  nodes; eligibility reports the computation check as unknown for larger
  graphs.
- Peak memory is the algorithm's accounted container memory, not process
  memory.
- The relays are a proof of concept (a thread per connection, a text
  protocol, IPv4 loopback only), not a production forwarder.
- The topology is fixed in code; there is no file loader for graphs or
  `ExecutionEnvironment`s.
- Metaheuristic parameters are defaults, not tuned.
- Interactive use of the new panel sections was exercised by the automated
  Qt test, not by a person.

### V0.6 prerequisites

1. A deterministic decision loop that turns eligibility, NetworkState and
   execution outcomes into `DecisionRecord`s, with the stability gates
   (dwell time, confidence, meaningful change) and a provider for the
   Decisions tab.
2. Measured edge values for the controlled topology (per-relay probing), or
   an explicit decision to keep configured values, so that decisions react
   to something real.
3. A switching protocol in the forwarding layer: activate a path, confirm
   it, roll back on failure, and record "approved" separately from
   "committed".
4. Release-build runtime measurements for the registry profiles.
5. A decision on browser integration: either a restricted HTTP adapter for a
   controlled test origin, or keeping forwarding scoped to AAFR's own
   requests.
