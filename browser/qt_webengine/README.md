# AAFR Qt WebEngine browser V0.6

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

V0.6 adds the adaptive runtime: change detection, deterministic
recommendation, controlled incumbent/challenger probing with statistical
arbitration, stability gates, switching that commits only after the
forwarding executor confirms the path, failure recovery, explainable
DecisionRecords and outcome observation, shown in Developer Tools. It uses
no ML, and browser page loads are still not routed through the relays. See
[V0.6 adaptive runtime](#v06-adaptive-runtime).

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

> **Superseded by V0.7.3.** Browser traffic can now traverse the relays in
> the browser's controlled forwarding mode (default: direct). See
> [V0.7.3 real browser forwarding](#v073-real-browser-forwarding). The
> V0.5 findings below are kept as the record of that milestone.

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

## V0.6 adaptive runtime

V0.6 adds `AAFRRuntime`, the central orchestrator that decides, without any
ML, whether the controlled forwarding layer keeps its active algorithm or
switches, and that commits a switch only after the forwarding executor has
confirmed it. Code: `runtime/adaptive/` (library `aafr-adaptive-runtime`,
plain C++17, no Qt). The browser creates the runtime with the window and
starts it once V0.5 forwarding verification finishes.

### Architecture

```text
NetworkState (V0.4)    forwarding graph (V0.5, versioned)
        \                    /
         NetworkChangeDetector -> EvaluationScheduler (coalescing, spacing)
                     |
              AAFRRuntime::Evaluate (one at a time)
                     |
   eligibility (V0.5 engine) -> 10 algorithms -> ValidatePath
                     |
   IRecommendationProvider (DeterministicRecommendationProvider)
                     |
   incumbent (active path) vs challengers (distinct paths)
                     |
   ControlledProbeEngine -> StatisticalArbitrationEngine   (when possible)
                     |                    otherwise: estimate-only policy
   SwitchingStabilityController (dwell, cooldown, backoff, evidence,
                                 margin, hysteresis; safety override)
             /                         \
        RETAIN                      SWITCH -> activation request over the
                                     V0.5 executor -> path confirmed?
                                     -> commit | previous re-confirmed |
                                        recovery | degraded
                     |
   DecisionRecord -> RuntimeOutcomeObserver -> telemetry + Developer Tools
```

Roles are fixed: the provider recommends, arbitration supplies evidence, the
executor forwards, and only the runtime owns the active configuration
(`ActiveConfiguration`: algorithm, path, graph version, J(P), configuration
version, activation request, decision id). Everything is replaceable through
`RuntimeDependencies` (graph source, problem factory, NetworkState source,
executor, eligibility engine, provider, monotonic clock, telemetry pipeline).

Browser wiring: `BrowserWindow` builds the runtime over
`ForwardingService::GraphSnapshot()`, `LoopbackDemoProblem`,
`NetworkStateService::Latest()`, the loopback executor and the V0.5
engine's eligibility, and calls `Start()` from the verification callback.
The relays now sleep each edge's configured delay (`inject_edge_latency`),
so probing measures the configured topology instead of bare loopback speed;
those delays are labelled simulated. Browser page loads still do not pass
through the relays.

### Runtime lifecycle

| State | Meaning | Valid next states |
|---|---|---|
| `uninitialized` | constructed | `initializing`, `stopping` |
| `initializing` | validating configuration, selecting and confirming the first configuration | `ready`, `degraded`, `failed`, `stopping` |
| `ready` | a confirmed configuration is active | `evaluating`, `degraded`, `stopping` |
| `evaluating` | comparing active and challengers | `ready`, `switching`, `degraded`, `stopping` |
| `switching` | activation requested, waiting for confirmation | `ready`, `degraded`, `stopping` |
| `degraded` | no verified configuration; periodic recovery evaluations | `evaluating`, `stopping`, `failed` |
| `stopping` / `stopped` | cancelling probes and activations, joining the worker | `stopped` / none |
| `failed` | invalid configuration or missing dependency | `stopping` |

Invalid transitions are refused and recorded as runtime errors.
`Step()` and `Initialize()` share one mutex, so evaluations and switches can
never overlap; `Start()` runs `Step()` on a worker thread every tick (never
the GUI thread). `Stop()` sets a cancellation flag that the probe engine,
the algorithms and every executor request poll, joins the worker and ends in
`stopped`.

Initial selection is not a switch: it evaluates every algorithm, activates
the best usable candidate, and records `INITIAL_ACTIVATION` only after the
executor confirms the path. If nothing can be confirmed the runtime enters
`degraded` and retries every `degraded_retry_ms`.

### Network change detection

`NetworkChangeDetector` compares each new observation (built from immutable
snapshots) with per-measurement baselines:

| Input | Rule |
|---|---|
| NetworkState features (default: `rtt.handshake.median_ms` 10 ms / 30 %, `ack_delay.jitter_ms` 5 ms / 50 %, `dns.response_time.mean_ms` 20 ms / 50 %, `tcp.retransmission_ratio` 0.02 / 50 %, `throughput.mean_mbps` 1 Mb/s / 50 %, `nav.failure_ratio` 0.10 / 50 %) | change >= max(absolute, relative x baseline) on 2 consecutive new snapshots; only `valid` features no older than 10 s count; the absolute floor makes a zero baseline safe |
| Missing, stale, insufficient, invalid features | never trigger; reported per feature and reset the streak |
| Same NetworkState id and graph version | duplicate, ignored (fast ticks) |
| Capture interface change | meaningful; all feature baselines restart |
| Constraint signature (id, validity, value; the list of usable measurements excluded) | any change triggers `constraint_change` |
| Graph | node/edge availability, edge latency beyond max(2 ms, 25 %) and any operator policy-cost change trigger `topology_change` (no sustain: configured values are not noisy) |

`EvaluationScheduler` keeps at most one pending evaluation, folds every
request into it with the highest-priority trigger (`failure_recovery` >
`startup` > `topology_change` > `constraint_change` > `meaningful_change` >
`manual` > `periodic`), and starts it no sooner than `min_interval_ms` after
the previous one unless a safety trigger is pending. A challenger that only
time-based gates blocked (dwell, cooldown, hysteresis, backoff) gets a
scheduled `periodic` re-check, so a valid switch does not wait for an
unrelated trigger.

### Recommendation provider

`IRecommendationProvider` takes the NetworkState, constraints, graph,
problem, one `CandidateEvaluation` per algorithm (eligibility, overridden
host checks, computed path, validity, feasibility, J(P), computation time),
the active configuration, optional benchmark evidence and the evaluation
context; it returns a status, the recommended algorithm, a ranking, a
provider id, the evidence source, J(P) estimates and an explanation. It
never activates anything.

`DeterministicRecommendationProvider` ranks usable candidates (eligible,
computed, valid, feasible) by the V0.5 objective J(P) on the current graph
version. With benchmark evidence for the same objective version, algorithms
whose feasible rate is below 0.9 rank after the others. Ties (within 1e-9)
prefer a candidate on the active path, then `AlgorithmId` order (exact
algorithms first), then lower computation time. The active algorithm gets
no tie preference of its own, so an algorithm left for a safety reason is
chosen again once it is eligible. With no usable candidate it returns
`unavailable` or `insufficient_evidence`; it never invents an improvement:
`estimated_improvement` is the active path's J(P) minus the recommended one
on the same graph and objective version, or empty.

Eligibility reuses the V0.5 engine (`AlgorithmExecutionEngine::Eligibility`).
The two host-state checks (`RequiredFeatures`, `DataQuality`) judge the
host's captured traffic, which the controlled graph does not use, so by
default they may fail without excluding an algorithm; every DecisionRecord
lists them under `overridden_checks` (`enforce_host_state_checks` turns
this off).

### Controlled probing

`ControlledProbeEngine` measures the incumbent (active path) and up to two
challengers (distinct paths) with ordinary forwarding requests over the
executor, tagged `runtime_probe` with an `aafr-probe` payload so they are
never mixed with application traffic. Each round sends one probe per arm in
an order shuffled by a seeded portable RNG, so slow drift affects all arms
of a round alike and observations pair by round. Defaults: 12 rounds,
64-byte payload, 1.5 s per probe, 20 s per campaign, at most 4 arms and 64
rounds, cancellation before every probe. Every observation records probe
id, scenario id, round, order, arm, path id, algorithm, start and end
(monotonic ns), duration, success (delivered over a confirmed path),
timeout, status, payload size, provenance, graph version and executor.

Probing is refused, with the reason in the record, when the executor is not
ready, cannot select a path per request, the arms share a path, a path does
not validate on the pinned graph version, or a path exceeds the executor's
hop limit. A computed path is never assumed executable.

When probing is unavailable or disabled, the documented conservative
policy applies: only the J(P) estimate on the current graph version counts,
it must exceed the larger estimate margin (20 %), and the hysteresis streak
still applies (`IMPROVEMENT_ESTIMATE_ONLY` in the record).

### Statistical arbitration methodology

The external repository's current code (`engine/src/anova.cpp`,
`decision.cpp`, `probing.cpp`) runs a repeated-measures ANOVA on log10 RTT
per arm with interleaved rounds as subjects, Holm-Bonferroni, a paired CI
and a weighted "confidence" `C = 0.40 eta^2 + 0.35 s_ci + 0.25 s_g`. With
one incumbent and one challenger per comparison that ANOVA is a paired t
test (F = t^2), and `C` mixes an effect size, a CI score and a Goodness
difference into a number that is not a probability. AAFR keeps the sound
parts and drops the composite:

1. **Pairing.** Round r of the incumbent and of a challenger form a pair:
   d_r = ln(challenger_r) - ln(incumbent_r) over rounds where both
   succeeded. exp(mean d) is the latency ratio; negative d means faster.
2. **Independence.** Lag-1 autocorrelation r1 of d in round order. If
   r1 > 0.3 the sample size is deflated to n_eff = n (1 - r1) / (1 + r1) for
   the standard error and degrees of freedom; below `min_effective_pairs`
   (5) the evidence is `invalid_evidence`.
3. **Sample size.** At least `min_pairs` (8) valid pairs, else
   `insufficient_evidence`.
4. **Distribution.** If |skewness| <= 1 and excess kurtosis <= 3: paired t on
   the mean of d (exact Student t via the regularized incomplete beta
   function). Otherwise a seeded percentile bootstrap of the median of d
   (2000 resamples; a circular block bootstrap when autocorrelated).
5. **Multiple challengers.** Holm step-down adjusted p-values; intervals at
   the Bonferroni level 1 - alpha/m so the direction claim holds for all m.
6. **Reliability.** Failure ratio per arm over all its probes with Wilson
   intervals. Latency of surviving probes cannot show loss, so if the
   incumbent's interval lies above the failure budget (5 %) and the
   challenger's lies entirely below the incumbent's, the challenger is
   better even at equal latency (`reliability repair`). A challenger above
   the budget, or entirely above the incumbent's interval, is never better.
   (Proving a challenger *within* 5 % would need at least 73 failure-free
   probes, more than a bounded campaign sends.)
7. **Design.** All arms must come from one campaign (graph version,
   executor, payload) and be interleaved; a blocked design is invalid.

Verdicts: `challenger_better` (Holm p < alpha and the whole interval on the
faster side, or a reliability repair), `challenger_not_better`,
`no_significant_difference`, `insufficient_evidence`, `invalid_evidence`.
A significant p-value alone never switches: the stability controller
separately requires a practically meaningful improvement.

### Multi-metric performance evaluation

Two kinds of numbers are never mixed (`ForwardingPerformanceEvaluator`):

- **Estimate:** the V0.5 objective J(P) (`ForwardingObjective` v1) from
  configured or simulated edge values. Two values are compared only for the
  same problem, graph version and objective version (`CompatibleObjectives`).
- **Measured** (`aafr.measured_forwarding.v1`): latency median / p90 / mean
  of successful requests (ms, lower is better), jitter as mean absolute
  difference of consecutive successful durations (ms, lower), failure ratio
  with Wilson interval (0..1, lower), and goodput as application payload
  bytes delivered per second of request time (B/s, higher; loopback goodput
  of this request pattern, not capacity). Missing values stay empty with a
  reason.

Hard constraints are checked, never weighted: the problem's latency budget
against the measured median, the failure budget against the Wilson lower
bound. No weighted composite of measured metrics is formed. Algorithm
computation time, runtime decision time, probe overhead and activation
latency are recorded separately in every DecisionRecord.

### Stability controls

`SwitchingStabilityController` (time passed in from the monotonic clock):

| Gate | Rule (browser defaults) |
|---|---|
| Minimum dwell | at least 5 s since the active configuration's confirmed activation |
| Cooldown | 8 s after a committed switch; separately 5 s after any failed activation |
| Candidate backoff | a candidate whose activation failed is skipped for 5 s, doubling up to 60 s; it always expires (no permanent blacklist) |
| Evidence | valid evidence (measured, reliability repair, or a compatible estimate) |
| Improvement margin | measured: relative latency improvement >= 10 %, its interval bound > 0, and >= 0.5 ms per request; estimate only: >= 20 % |
| Hysteresis | the same candidate path preferred in 2 consecutive evaluations; the streak resets when the preferred candidate changes or no challenger is preferred |

The gates are symmetric: returning to an earlier algorithm passes the same
rules as leaving it. Safety overrides (`ACTIVE_PATH_FAILED` after 2
consecutive failed health or application requests, `ACTIVE_ALGORITHM_INELIGIBLE`,
`ACTIVE_CONFIGURATION_INVALID`, `CONSTRAINT_VIOLATED`) bypass dwell,
cooldown, margin and hysteresis; evidence validity and candidate backoff
still apply, the replacement is validated, and activation is still
confirmed. Every override is recorded with `SAFETY_OVERRIDE` and its reason.

When the active path fails in measurement while the graph shows nothing,
every algorithm would compute the same path again. The runtime then
recomputes candidates with the failed path's relays forbidden
(`ForwardingConstraints::forbidden_nodes`) and records the detour.

### Bidirectional switching

Nothing in the runtime remembers a direction: dwell and cooldown expire,
backoff expires, hysteresis restarts per candidate, and the provider ranks
every eligible algorithm on every evaluation. Tested both ways:

- path level, deterministic (`bidirectional_recovery`): path A congested ->
  switch to B -> congestion clears -> blocked by cooldown and hysteresis ->
  switch back to A -> A congested again -> switch to B again;
- algorithm level (`algorithm_level_bidirectional`): a negative operator
  policy cost makes Dijkstra and A* ineligible -> safety switch to
  Bellman-Ford -> the cost is removed -> Dijkstra is chosen again;
- real relays (`runtime_loopback`): A -> B -> A over TCP relays on
  127.0.0.1, each activation proven by the relays' own request logs;
- browser (`adaptiveRuntimeSwitchesInBrowser`): through the Developer Tools
  buttons.

### Forwarding activation and failure recovery

V0.5 forwarding is source-routed per request, so activating a configuration
means proving that the path works: the runtime sends an activation request
(`aafr-activation`) along it and accepts only `delivered` with
`path_confirmed` and a confirmed path equal to the request. The stages are
recorded separately: recommendation, candidate, gates, `SWITCH_REQUESTED`,
activation pending, confirmed or failed, `SWITCH_COMMITTED`, recovery.

- The graph version is checked before activation and again after
  confirmation; if it changed, the evaluation is discarded
  (`EVALUATION_STALE`), nothing is committed and a re-evaluation is queued.
- On failure (`SWITCH_FAILED`, plus `ACTIVATION_TIMEOUT` when it timed
  out; a delivered response without path proof is a failure too): the
  candidate enters backoff, then (1) the previous configuration is
  re-confirmed with a fresh request and kept
  (`PREVIOUS_CONFIGURATION_RETAINED`), or (2) the remaining ranked
  candidates, then (3) a detour around the failed path are tried
  (`RECOVERY_REQUESTED`, `RECOVERY_SUCCEEDED`), or (4) the active
  configuration is marked unverified, the runtime enters `degraded`
  (`RECOVERY_FAILED`, `ACTIVE_CONFIGURATION_UNKNOWN`) and refuses
  `ForwardActive` until a periodic recovery evaluation confirms a
  configuration. Rollback is never assumed.
- Shutdown during probing or activation cancels the requests; an
  interrupted activation is never committed.

### DecisionRecord schema (`aafr.decision_record.v1`)

One record per evaluation, written once (`runtime/adaptive/decision_record.h`,
JSON via `ToJson`, optionally appended to `decision_log_path`):

| Group | Fields |
|---|---|
| Identification | `schema`, `decision_id`, `session_id`, `monotonic_ns`, `timestamp_unix_ms`, `evaluation_id` |
| Network context | `network_state_id` (0: none), `network_validity`, `measurement_quality`, `graph_id`, `graph_version`, `constraint_version` (hash), `trigger`, `coalesced_triggers`, `trigger_detail` |
| Recommendation | `provider_id`, `recommendation_status`, `recommended_algorithm`, `ranking` (algorithm, path, J(P), computation ms), `eligible_algorithms`, `overridden_checks`, `evidence_source`, `estimated_improvement(_relative)`, `recommendation_explanation` |
| Statistical evidence | `probing_attempted`, `probing_unavailable_reason`, `probe_scenario_id`, `incumbent_samples`, `challenger_samples`, `paired_samples`, `statistical_method`, `latency_ratio`, `ratio_ci_low/high`, `ci_level`, `p_holm`, `assumption_validation`, `evidence_validity`, failure rates, `simulated_delay`, `evidence_explanation` |
| Stability | `stability` (allowed, safety override, six gates with applicability, result, remaining time, detail; blocking reasons), `safety_override`, `safety_reason` |
| Decision | `previous_algorithm/path`, `candidate_algorithm/path`, `decision_type` (`initial_activation`, `retain`, `switch`, `switch_rejected`, `safety_switch`, `recovery`, `unavailable`, `discarded`), `reason_codes`, `explanation` |
| Execution | `activation_request_id`, `activation_status`, `confirmed_algorithm/path`, `config_version`, `recovery_status`, `execution_detail`, `activation_latency_ms` |
| Overheads | `algorithm_computation_ms`, `decision_ms`, `probe_overhead_ms`, `probe_requests` |
| Outcome | `outcome_window_ms`, `outcome_status` (`pending`; results arrive as revisions) |

Missing data stays empty (`null`, `""`, or an explicit reason), never zero.
The record also converts to the V0.3 `observability::DecisionRecord`, so
the existing Decisions table shows real records.

Reason codes (`runtime/adaptive/reason_codes.h`, stable text and meaning,
new codes only appended): `INITIAL_ACTIVATION`, `NETWORK_CHANGE_MEANINGFUL`,
`NETWORK_CHANGE_IGNORED`, `CONSTRAINT_CHANGED`, `TOPOLOGY_CHANGED`,
`CANDIDATE_INELIGIBLE`, `RECOMMENDATION_UNAVAILABLE`,
`CANDIDATE_SAME_AS_ACTIVE`, `PROBING_UNAVAILABLE`, `PROBE_TIMEOUT`,
`INSUFFICIENT_SAMPLES`, `STATISTICAL_EVIDENCE_INVALID`,
`STATISTICAL_EVIDENCE_INSUFFICIENT`, `IMPROVEMENT_CONFIRMED`,
`IMPROVEMENT_MARGIN_NOT_MET`, `MINIMUM_DWELL_NOT_MET`, `COOLDOWN_ACTIVE`,
`HYSTERESIS_NOT_SATISFIED`, `ACTIVE_PATH_FAILED`, `SAFETY_OVERRIDE`,
`SWITCH_REQUESTED`, `SWITCH_COMMITTED`, `SWITCH_FAILED`,
`RECOVERY_REQUESTED`, `RECOVERY_SUCCEEDED`, `RECOVERY_FAILED`,
`ACTIVE_CONFIGURATION_UNKNOWN`, `ACTIVE_ALGORITHM_INELIGIBLE`,
`ACTIVE_CONFIGURATION_INVALID`, `CHALLENGER_NOT_BETTER`,
`NO_ELIGIBLE_ALGORITHM`, `NETWORK_STATE_UNAVAILABLE`, `EVALUATION_STALE`,
`MANUAL_EVALUATION`, `CONSTRAINT_VIOLATED`, `IMPROVEMENT_ESTIMATE_ONLY`,
`PREVIOUS_CONFIGURATION_RETAINED`, `CANDIDATE_BACKOFF`, `ACTIVATION_TIMEOUT`.
Each record's `explanation` is built from the values that were compared.

### Outcome observation

`RuntimeOutcomeObserver` records every request the runtime itself sends
over a configuration (activation, periodic health checks every 3 s,
application requests through `AAFRRuntime::ForwardActive`) with decision id,
configuration version, algorithm and NetworkState id. `Outcome(decision_id)`
derives an `OutcomeRevision` (revision number = observations so far, 30 s
window, measured performance) on demand; DecisionRecords are never rewritten.
Counters keep activation failures, recovery results and switching latencies.
An outcome after a switch is correlation, not proof of causation; nothing
learns online or retrains.

### Developer Tools

- **Overview:** adaptive runtime lifecycle, confirmed configuration, latest
  evaluation, latest switching result, runtime health.
- **Runtime -> Adaptive runtime:** lifecycle, evidence mode, active
  algorithm and confirmed path (graph version, activation request, latency),
  candidate, provider, eligible algorithms, trigger, dwell and cooldown
  remaining, activation status, health, latest change detection, latest
  evaluation and switch, outcome of the active configuration, counters,
  errors, and the six gates of the latest assessment. Buttons: *Evaluate
  now* (a manual request; every gate still applies), *Congest relay_a /
  relay_b (+60 ms)* and *Restore configured topology*, which change only the
  controlled loopback topology (a new graph version, delays labelled
  simulated). Before the runtime starts every field says so; nothing is
  invented.
- **Runtime state / Decisions:** the V0.3 views now read the runtime. A new
  evidence table lists each decision's trigger, action, incumbent,
  challenger, evidence (method, latency ratio and interval, pairs),
  validity, reason codes, activation and outcome; selecting a row shows the
  full record and its latest outcome revision.
- **Events:** a sixth filter, *Adaptive runtime*, for `runtime.*` events
  (`initialized`, `lifecycle_changed`, `network_change_detected`,
  `evaluation_started`, `recommendation_generated`, `probing_started`,
  `probing_completed`, `statistical_evaluation_completed`,
  `algorithm_retained`, `switch_requested`, `switch_committed`,
  `switch_failed`, `recovery_started`, `recovery_completed`,
  `evaluation_discarded`). The NetworkState builder ignores them.

The panel refreshes on its timer and only copies status; the runtime works
on its own thread.

### Reference repository adaptation

Inspected: `oddproblem/Adaptive-runtime` at `6b6c008` (2026-11-15, "Audit
fixes ..."), the latest commit, read on 2026-11-16: `engine/src/` (anova,
decision, probing, engine, ml_engine, sdn, telemetry, live_engine,
netmap), `engine/include/`, `engine/tests/unit_tests.cpp`,
`tests/check_stats.py`, `engine/config/engine_v3.json`, `engine/README.md`,
`implementation.md`, `issues.md`. The current code uses XGBoost only as a
link-weight **regressor** (three inputs: delay, loss, inverse bandwidth);
there is no traffic classifier, whatever older documents or the
architecture image say.

**Licensing:** the repository has no license file, so no code was copied.
Everything in AAFR was implemented independently from the concepts below.

| Component in the reference | AAFR V0.6 | Why |
|---|---|---|
| Interleaved rounds with rotated arm order | adapted: seeded random order per round, rounds as pairing blocks | removes drift from the comparison; randomization avoids a fixed order effect |
| Log-transformed RTT | adapted: natural-log paired differences | multiplicative effects, right-skewed latency |
| RM-ANOVA on 2 arms | replaced by the equivalent paired t test plus assumption checks and a bootstrap fallback | identical for k = 2; the checks were missing |
| Holm-Bonferroni | adopted (independent implementation) | several challengers |
| 95 % CI of the paired difference | adopted, at the Bonferroni level when m > 1 | direction claim must hold for all challengers |
| Wilson interval and loss budget | adapted: separation of intervals instead of "within budget" | a bounded campaign cannot prove "within 5 %" |
| Composite confidence `C` and its 0.70 threshold | rejected | not a probability; clamping distorted it (the reference's own issue list) |
| 30 s dwell timer | adapted into dwell + cooldown + backoff + hysteresis | dwell alone cannot stop flapping or retry storms |
| Demand-aware Goodness | not adopted | AAFR keeps measured metrics separate and uses the V0.5 objective |
| XGBoost link weights | rejected for V0.6 (no ML) | V0.7 integrates AAFR's own selector model |
| OpenFlow / Ryu-compatible SDN, Mininet, tc | rejected | AAFR forwards through its own loopback relays |
| TTL-sweep live mapping, Npcap capture | deferred | outside the controlled forwarding layer |
| Dashboard control endpoints | rejected | unauthenticated process control (reference issue list) |

Reference defects and the AAFR tests that cover them:

| Reference issue | AAFR test |
|---|---|
| No return to the faster path after congestion clears | `bidirectional_recovery`, `algorithm_level_bidirectional`, `bidirectional_over_real_relays` |
| Equal latency hides a lossy incumbent | `equal_latency_different_loss`, `missing_data_and_unreliable_challenger` |
| Clamped Goodness term distorts the threshold | no composite score; margin tests in `improvement_margin` |
| Unsynchronized topology reads | immutable graph snapshots; `concurrent_updates_with_worker`, `stale_evaluation_discarded` |
| No rollback on rule-installation failure | confirm-before-commit; `failed_switch_keeps_previous`, `activation_timeout_and_unconfirmed` |
| Missing tests for equal-latency cases | `equal_latency_different_loss`, `no_significant_difference` |
| Missing transient-congestion scenario | `congestion` scenario of `aafr-runtime-experiment` |

### V0.6 tests

| Program | Covers |
|---|---|
| `runtime_change_detection` (9 cases) | sustained meaningful change, minor fluctuation, missing / stale / insufficient / old features, zero baseline, duplicate snapshots and rapid updates, constraint and interface changes, topology changes, scheduler coalescing and spacing, configuration validation |
| `runtime_statistics` (12) | numerics against published values (t 0.975 quantiles, Wilson, Holm), valid paired comparison with correct effect direction and CI coverage, CI against a hand calculation, insufficient samples, equal latency with different loss, different latency with equal loss, no significant difference, heavy tails (bootstrap), multiple comparisons (Holm, Bonferroni-level CI), invalid assumptions (blocked design, serial correlation, mixed campaigns), missing data, unreliable challenger, performance metrics and objective compatibility |
| `runtime_stability` (10) | dwell, cooldown, failed-activation cooldown and backoff (doubling, cap, expiry), margins (measured, interval bound, per-request ms, estimate-only, reliability), hysteresis streaks, safety override, disabled gates, symmetric gates, reason codes, lifecycle transitions |
| `runtime_recommendation` (6) | ranking, tie-break rules, ineligible / invalid / infeasible candidates, no usable candidate, estimated improvement against the active path, benchmark evidence (a real V0.5 benchmark aggregated, plus a demotion case) |
| `adaptive_runtime` (26) | lifecycle (initial activation, missing NetworkState, no eligible algorithm, invalid configuration, initial activation failure and later recovery, clean shutdown), minor fluctuation, switching with statistical evidence, dwell, bidirectional recovery (path and algorithm level), estimate-only policy, probing unavailable, candidate same as active, no better challenger, failed switch keeps the previous configuration, timeout and unconfirmed activation, active-path failure safety override, recovery failure then success, stale evaluation discarded, no overlapping switches (4 threads), concurrent telemetry / topology / readers with the worker, shutdown during probing and during switching, complete DecisionRecords, outcome association, telemetry events |
| `runtime_loopback` (3) | real TCP relays: A -> B -> A with probe evidence and relay-log proof of every activation; a relay process taken down for real and the detour recovery; the `ForwardingService` topology controls with the runtime wired as in the browser |
| `browser_observability::adaptiveRuntimeSwitchesInBrowser` | the browser runtime starts after verification, Developer Tools shows it, and the Congest/Restore buttons drive a switch away from relay_a and back |

All run without Internet access; the loopback ones open sockets on
127.0.0.1 only.

### Performance measurements (Debug build, 2026-11-16)

`aafr-runtime-experiment --seconds 20 --seeds 1,2,3` (writes
`experiments/results/v0.6/runs.csv` and `summary.json`, git-ignored). Real
TCP relays on 127.0.0.1; path A is configured 5 + 5 ms, path B 25 + 25 ms,
slept by the relays (simulated values, real forwarding). Application
requests every ~60 ms (seeded exponential gaps, 64-byte payload). Measured
end-to-end latency includes relay overhead (a thread per connection, Debug
build, Windows timer granularity): path A's median is about 80 ms, not 10.
Means over 3 seeds:

| Scenario | Configuration | Mean latency | p95 | Goodput | Switches | Failed | Evaluations | Probe requests | Adaptation | Recovery |
|---|---|---|---|---|---|---|---|---|---|---|
| congestion | fixed (Dijkstra) | 103.1 ms | 223.0 ms | 1241 B/s | 0 | 0 | 0 | 0 | - | - |
| congestion | adaptive, estimate only | 84.4 ms | 115.7 ms | 1517 B/s | 2 | 0 | 5 | 0 | 525 ms | 551 ms |
| congestion | adaptive + statistical arbitration | 103.1 ms | 226.5 ms | 1243 B/s | 1.3 | 0 | 4.7 | 53 | 3159 ms (2 of 3 runs) | 1855 ms (2 of 3) |
| congestion | no stability controls (isolated) | 82.5 ms | 115.2 ms | 1552 B/s | 2 | 0 | 3 | 0 | 124 ms | 36 ms |
| fluctuation | fixed (Dijkstra) | 96.2 ms | 145.7 ms | 1332 B/s | 0 | 0 | 0 | 0 | - | - |
| fluctuation | adaptive, estimate only | 94.4 ms | 145.3 ms | 1357 B/s | 0.3 | 0 | 41.3 | 0 | - | - |
| fluctuation | adaptive + statistical arbitration | 92.2 ms | 143.5 ms | 1389 B/s | 0 | 0 | 29 | 224 | - | - |
| fluctuation | no stability controls (isolated) | 96.0 ms | 138.6 ms | 1334 B/s | 28 | 0 | 29 | 0 | - | - |

Congestion: path A gets +150 ms from 25 % to 65 % of the run (8 s).
Fluctuation: path A alternates +0 / +60 ms every 700 ms. Every request
succeeded in every run (success rate 1.0). Overheads per evaluation: 140-152
ms of algorithm computation (all ten algorithms, Debug build) and 159-171 ms
of decision time including it; activation (switching) latency 64-82 ms; a
probe campaign (8 rounds x 2 arms = 16 requests) took about 0.6 s on
uncongested paths and up to 2.5 s with a congested arm.

What the numbers show, and only that:

- Under congestion that lasts several seconds, switching on the J(P)
  estimate with the stability gates cut mean latency from 103 to 84 ms and
  p95 from 223 to 116 ms, adapting about 0.5 s after onset and recovering
  about 0.55 s after it cleared.
- Statistical arbitration made every switch rest on measured evidence, but
  with 8-round campaigns and hysteresis it adapted only after about 3 s, and
  in one of three runs not within the 8 s congestion window, so it gained
  nothing in this scenario and cost 53 probe requests. Fewer rounds or no
  hysteresis on strong evidence would trade some of that certainty for speed.
- Under fluctuation faster than the gates, the runtime without stability
  controls switched 28 times in 20 s and its median latency rose from about
  83 to 97 ms (it kept landing on B just as A recovered); with the gates it
  switched 0-1 times and matched the fixed path.
- These are one topology, one workload and three seeds on one machine;
  they are not a general claim about adaptive forwarding.

### V0.6 verification record (2026-11-16)

Debug preset (`build\windows-msvc-qt`, MSVC 19.44, Qt 6.12.0, browser tests
on). Baseline before any V0.6 change: build succeeded; 10 of 10 non-browser
tests passed; `browser_observability` passed; `browser_interface` timed out
at its 90 s limit.

After V0.6, `ctest --test-dir build\windows-msvc-qt --output-on-failure`:

- 16 of 16 non-browser programs passed (the 10 existing and the 6 new):
  runtime_change_detection 76 checks, runtime_statistics 97,
  runtime_stability 218, runtime_recommendation 28, adaptive_runtime 274
  (19.5 s), runtime_loopback 55 (24.9 s); the V0.5 programs unchanged
  (forwarding_pipeline, loopback_forwarding, benchmark, path_algorithms,
  forwarding_graph all passed).
- `browser_observability`: 37 passed, 2 skipped (the opt-in live-capture and
  screenshot cases, as before), 73-77 s. The new
  `adaptiveRuntimeSwitchesInBrowser` passed in the full suite twice and
  alone once; in the first full run it hit Qt's 300 s per-function limit
  (before the layout fix below; not reproduced since).
- `browser_interface` (54 cases): 3 passes and 3 timeouts at the 90 s limit
  over six runs. The passes (71, 72 and 74 s) came on an idle machine; the
  timeouts came right after a relink or another heavy run, each with a
  different keyboard/focus case still waiting (`windowNavigationAndShortcuts`,
  `newTabFocusAndNavigation`). Without V0.6 it timed out once right after the
  baseline build here, and the unchanged commit built in a separate worktree
  passed twice (65 and 79 s). The suite runs close to its limit with or
  without V0.6.
- Layout regression found and fixed: the new Adaptive runtime pane raised
  the dock's minimum height (`devToolsDockingResizingAndContentLayout`); it
  is now in a scroll area.

Runtime verification: switching, statistical evaluation, bidirectional
recovery and forwarding activation were verified by the deterministic
integration tests and, over real TCP relays on 127.0.0.1, by
`runtime_loopback` (three consecutive runs: A -> B at a measured latency
ratio of 0.50-0.53, B -> A at 0.69-0.76, every activation confirmed by the
relays' own request logs, activation latency 62-126 ms) and by the browser
test. Not verified: a Release build, interactive use by a person, and any
browser page traffic through the runtime (not implemented).

### Known limitations

- Browser page loads are still not routed through the relays; the runtime
  adapts AAFR's own controlled forwarding layer.
- Edge delays on the relays are configured values slept by the relays
  (labelled simulated). The traffic, timing and confirmation are real, but
  Windows timer granularity compresses small differences: on path A 10 ms
  vs B 50 ms the measured latency ratio was about 0.7, not 0.2.
- On the browser's 4-node demo topology every exact algorithm finds the
  same optimum, so most switches change the path while the algorithm label
  stays the same; the label changes when eligibility changes (for example a
  negative operator preference) or when algorithms disagree.
- Recommendations come from J(P) on configured graph values; measurements
  confirm or veto them but do not originate them (no measured edge model).
- Probes share the relays with other requests; interference is bounded but
  not removed. Probe campaigns cost seconds of request time.
- One failure budget (5 %) and fixed thresholds; no per-application
  workload requirements yet.
- `ForwardActive` exists for AAFR-originated requests; nothing in the
  browser calls it for page traffic.
- Verified in the Debug build only; no Release-build measurements.
- `browser_interface` runs close to its 90 s limit before and after this
  change and times out under load (see the verification record).

### V0.7 readiness

Ready: the `IRecommendationProvider` seam, per-decision provider ids, the
eligibility, validation, probing, gates, confirmation and recovery that any
provider's output must pass, and the deterministic fallback. Not ready:
the out-of-process model worker, the feature builder with per-feature
provenance and refusal on unknown inputs, and per-path measured features.
The contract is in
[`expert_model/README.md`](../../expert_model/README.md#v07-model-integration-contract-prepared-in-v06).

## V0.7.3 real browser forwarding

V0.7.3 closes the gap V0.5–V0.7.2 left open: **Qt WebEngine page loads can
now traverse AAFR's relays.** In the browser's controlled forwarding mode,
every HTTP request and every HTTPS (`CONNECT`) connection goes through a
local AAFR forwarding gateway. The gateway pins the connection to the
runtime's confirmed active configuration and carries it as a byte stream along
that configuration's relay path, entry → relay(s) → destination node. The
destination node then opens the connection to the real server. The default is
still **direct mode**, which behaves exactly like V0.7.2.

### Architecture assessment (audit before implementation)

| Component | Finding |
|---|---|
| Qt WebEngine initialisation | `main.cpp` creates `QApplication`, then a named `QWebEngineProfile` (or the default one), then `BrowserWindow`. There was no proxy configuration and no request interceptor. |
| V0.5 relays (`loopback_forwarding.*`) | A line protocol (`AAFR1 REQ` / `AAFR1 RSP`). The payload is at most 1 KiB, newline-free, and one request gets one response. **They could not carry arbitrary TCP or TLS byte streams**: synthetic messages only. |
| V0.5 executor | `IForwardingExecutor::Execute()` sends one message and confirms the path from the relays' trace plus each node's log. |
| V0.6 runtime | Owns the confirmed `ActiveConfiguration` (`config_version`, algorithm, path, graph version, decision). `ForwardActive()` sends one message over it. There was no read-only accessor for other components. |
| Telemetry / NetworkState | Had no browser-forwarding source. |

So the existing executor was **not** usable for browser traffic as it stood.
V0.7.3 adds a byte-stream mode to the same relay processes rather than a
second, unrelated forwarder.

### Feasibility study and choice

Verified on this machine (Qt 6.12.0, Windows 11), first with a throwaway
prototype and then in `browser_forwarding_tests`:

- Chromium's `--proxy-server=http://127.0.0.1:<port>`, passed through
  `QTWEBENGINE_CHROMIUM_FLAGS` before `QApplication`, is honoured by Qt
  WebEngine. HTTP requests arrive at the proxy in absolute form, HTTPS as
  `CONNECT host:port`, and DNS resolution is left to the proxy.
- Chromium never proxies `localhost` or `127.0.0.0/8` (implicit bypass)
  unless `--proxy-bypass-list=<-loopback>` is given. With that switch, both
  are proxied.
- The proxy setting is **process-wide**; there is no per-profile isolation.
  It is fixed when the WebEngine context initialises, so changing mode
  requires a restart.
- With a fixed `--proxy-server`, Chromium does **not** fall back to direct
  when the proxy is down: the load fails. Tested.

| Option | Assessment |
|---|---|
| A. Local explicit HTTP/HTTPS (CONNECT) proxy | Supported, least invasive, HTTP + HTTPS + WebSocket (Chromium tunnels `ws`/`wss` through CONNECT), TLS stays end to end. **Chosen.** |
| B. Chromium/Qt proxy configuration alone | Only routes the traffic somewhere; something must still speak HTTP/CONNECT. Used as the *delivery mechanism* for A. `QNetworkProxy::setApplicationProxy` cannot express the loopback bypass rule, so the command-line switch is used. |
| C. Adapter over the existing message executor | Impossible as is (1 KiB newline-free messages). Implemented as a new tunnel mode of the same relays and executor (`IStreamForwardingExecutor`). |
| D. `QWebEngineUrlRequestInterceptor` | Can observe, block or redirect URLs, but cannot carry bytes, so it is not forwarding. Used only to *observe* bypassed requests. |

Rejected: modifying Chromium, system proxy / WinINet settings, TLS
interception, administrator rights.

### Components

| Component | File | Role |
|---|---|---|
| Socket layer | `routing/forwarding/socket_io.*` | Bounded loopback/egress sockets, line and head readers, opaque bidirectional `Pump`, abortive close. Shared by relays and gateway. |
| Destination policy | `routing/forwarding/egress_policy.*` | `EgressPolicy` (public/private/loopback/link-local classes, port allow/deny), host and target syntax. |
| Relay tunnels | `routing/forwarding/loopback_forwarding.*` | `AAFR1 TUN` / `AAFR1 TUNRSP`, the same next-hop checks, per-node `StreamTraversal` evidence, at most 64 tunnels per node, egress at the Destination node. |
| Stream executor | `LoopbackForwardingExecutor::OpenStream` | Opens a tunnel along a path and confirms it exactly as a request (trace == path and every node's log). Never returns an unconfirmed stream. |
| HTTP proxy rules | `routing/gateway/proxy_http.*` | Request parsing and validation, smuggling defences, origin-form rebuild, response framing, chunked tracker. Pure functions. |
| Gateway | `routing/gateway/browser_forwarding_gateway.*` | `AAFRBrowserForwardingGateway`: accept, validate, pin, open tunnel, relay, records, events, draining. |
| Runtime binding | `routing/gateway/runtime_gateway_binding.*` | Reads `AAFRRuntime::ConfirmedConfiguration()`, checks usability on the current graph, reports outcomes with `ObserveBrowserStream()`. |
| Browser setup | `browser/qt_webengine/browser_forwarding.*` | Modes, gateway bind and Chromium switches before Qt starts, bypass observer. |
| Experiments | `tools/browser_experiment/` | `aafr-browser-experiment` (section *V0.7.3 controlled experiments*). |

### Relay tunnel protocol

```
gateway -> entry   AAFR1 TUN <conn_id> <budget_ms> <route|-> <trace|-> <host:port>
each hop           checks link/neighbour/availability, appends itself to the trace,
                   logs conn_id, (sleeps the configured edge delay if injection is on),
                   forwards TUN to the next hop
destination node   EgressPolicy port check -> resolve -> per-address class check -> connect
all, back up       AAFR1 TUNRSP <conn_id> <status> <trace> <egress|->
then               opaque bytes both ways until both sides close, idle 120 s, or an abort
```

Statuses: `OK`, `NEXT_HOP_UNAVAILABLE`, `LINK_DOWN`, `NOT_A_NEIGHBOUR`,
`DNS_FAILED`, `DEST_FORBIDDEN`, `DEST_UNREACHABLE`, `DEST_TIMEOUT`, `TIMEOUT`,
`BUSY`, `LOOP_DETECTED`, `HOP_LIMIT`, `BAD_REQUEST`. A relay or link failure
is reported as `relay_failure`. Destination failures are not, because they
say nothing about the path. An aborted tunnel (relay shutdown, reset,
cancellation) is torn down with resets along the whole chain, not left
half-open.

### Connection lifecycle

1. Accept on `127.0.0.1` only. The peer must be loopback and, on Windows,
   the TCP connection must be owned by this process or one of its children
   (`GetExtendedTcpTable`). Other local processes are refused. Tested with a
   PowerShell client.
2. Read the request head (32 KiB, 15 s). Parse and validate. Malformed or
   forbidden requests get a short gateway-generated error with an
   `X-AAFR-Reason` code; nothing from the request is echoed.
3. Pre-check the destination policy on the requested host and port.
4. Obtain the runtime's confirmed configuration, waiting up to 15 s while
   AAFR starts. If there is none, answer **503**; the gateway never goes
   direct. Check that its path is still valid on the current graph (else
   503 `configuration_unusable`).
5. **Pin** the connection to that configuration's generation
   (`config_version`) for its lifetime.
6. `OpenStream` along the pinned path. On failure, answer 502, 503 or 504
   with the reason, and record whether it was a relay failure.
7. CONNECT: answer `200 Connection Established` **only now**, after the
   relays confirmed the path. Then relay opaque bytes.
   HTTP: send the rebuilt origin-form request, then relay the response head
   (re-sent with `Connection: close`) and exactly the framed body.
8. Finish: attach the relays' `StreamTraversal` records, tell the runtime
   (`ObserveBrowserStream`), publish the record and telemetry.

### Runtime configuration, pinning, switching and draining

- The gateway only reads `AAFRRuntime::ConfirmedConfiguration()`: a
  configuration that passed the runtime's eligibility, probing,
  statistical arbitration, stability gates and confirmed activation. Model
  recommendations, candidates and unconfirmed activations never reach it,
  and the gateway cannot change runtime state.
- **Connection-level pinning:** an existing connection keeps its path until
  it closes. New connections use the newest confirmed generation. Active
  TCP/TLS streams are never migrated.
- When the generation changes, the gateway emits `RelayPathChanged`.
  Generations that still have pinned connections emit
  `ConfigurationDraining` and are counted as *draining*; when the last
  connection closes they emit `ConfigurationRetired`. Bounded draining is
  optional (`drain_timeout_ms`, default 0 = never terminate a connection
  because the algorithm changed).
- If a new configuration fails activation, the runtime keeps the previous
  one confirmed, so the gateway keeps using it. Tested with a relay link
  down in reality but not in the graph.
- Browser-side relay failures on the current configuration count toward the
  runtime's safety override, like failed `ForwardActive` requests, so a
  dead relay triggers `FailureRecovery`. Tested: two failed page loads, then
  the runtime moves to the other path.

### Modes

| Mode | How | Effect |
|---|---|---|
| direct (default) | nothing | No gateway, no Chromium switches, V0.7.2 behaviour. |
| controlled | `--forwarding=controlled` or `AAFR_BROWSER_FORWARDING=controlled` | Gateway on `127.0.0.1:<ephemeral>`, `--proxy-server=… --disable-quic`. Relays, verification and runtime start immediately (not when Developer Tools opens). |
| diagnostic | `--forwarding=diagnostic` | As controlled. Telemetry records additionally carry the target host:port and per-relay byte counts. |
| + loopback | `--forward-loopback` or `AAFR_FORWARD_LOOPBACK=1` | Adds `--proxy-bypass-list=<-loopback>` and lets the egress reach loopback on any port (controlled experiments with local servers). |
| + private | `AAFR_GATEWAY_ALLOW_PRIVATE=1` | Lets the egress reach RFC 1918 / ULA addresses. |

The mode is fixed for the life of the process. If the gateway cannot bind,
`aafr-browser` exits with an error rather than browsing directly.

### Protocol support

| Protocol | Status |
|---|---|
| HTTP/1.1 (absolute-form) | **Verified** with a real browser: pages, navigation, redirects (passed through, followed by the browser), POST bodies (Content-Length), large (8 MiB) and chunked streaming responses, downloads. One request per proxy connection (`Connection: close`); plain-HTTP connection reuse is not offered. |
| HTTPS (CONNECT) | **Verified** with a real browser and with a Qt TLS client that verifies the server against the test CA. TLS is end to end; the gateway never decrypts. |
| HTTP/2 over TLS | Negotiated by ALPN *inside* the CONNECT tunnel, invisible to AAFR. **Verified** (closure): ALPN `h2` on both ends, requests decoded from HTTP/2 frames, 8 concurrent streams on one connection, relay traversal, reuse, RST_STREAM cancellation. |
| Connection reuse | Inside CONNECT tunnels: yes (Chromium keeps them alive; an HTTP/2 connection carried 15 page loads in one tunnel). Plain HTTP: no. |
| WebSocket (`ws`/`wss`) | Chromium tunnels both through CONNECT. **Verified** (closure): handshake, text/binary both ways up to 2 MiB, ping/pong, close codes, aborts, pinning across a switch, relay traversal. Plain-HTTP `Upgrade` is stripped (never used by Chromium through a proxy). WebSocket over HTTP/2 (RFC 8441) and compression not tested. |
| HTTP/3 / QUIC | **Excluded.** `--disable-quic` is set in controlled mode, and Chromium does not use QUIC through an HTTP proxy. UDP never traverses AAFR. |
| Chunked request bodies | Refused (501). Chromium uses them only for streaming uploads. |
| Service workers, cache | Go through the same profile network stack, so their network fetches use the proxy. Cache hits make no request at all (the experiments use `NoCache`). |
| Loopback hosts | Bypass the proxy unless `--forward-loopback` is given. Reported as `ProxyBypassDetected` by the request observer; never counted as forwarded. |
| Non-network schemes | `data:`, `blob:`, `qrc:`, `file:`, `chrome:` make no network request. Counted as non-network. |

### Security

- **Binding and clients:** the gateway binds `127.0.0.1` only (verified
  unreachable through the host's two non-loopback addresses), with
  `SO_EXCLUSIVEADDRUSE`. Non-loopback peers and foreign processes are
  refused.
- **No open proxy:** the default egress policy allows public addresses on
  ports 80 and 443 only. Loopback, private and link-local destinations
  need an explicit option. Unspecified, multicast and reserved addresses
  are never allowed. The policy is checked by the gateway on the request
  and by the destination node on every resolved address, so DNS rebinding
  fails (tested: `localhost:443` refused after resolution).
- **Request smuggling and parsing:** strict CRLF, no obsolete line folding,
  no whitespace before colons, no control characters, at most 100 headers,
  single Host. Rejected: Content-Length together with Transfer-Encoding,
  conflicting or list-valued Content-Length, request Transfer-Encoding,
  userinfo, fragments, origin-form requests to the gateway, `https://`
  absolute-form and TRACE. Hop-by-hop headers (Connection and everything it
  names, Proxy-Connection, Proxy-Authorization, Keep-Alive, TE, Trailer,
  Upgrade) are removed.
- **TLS:** no certificate, no decryption, no root installed, no
  verification disabled. The browser test proves Chromium saw the origin's
  own leaf certificate, and an unknown certificate still fails the load.
  The test certificate (`tests/fixtures/tls`, test-only, CA key discarded)
  is trusted only in that test, only for its exact fingerprint, and only
  when the sole error is the unknown test CA.
- **Bounds:** 64 clients, 32 KiB heads, 15 s head timeout, 10 s
  establishment, 120 s idle, 10 s send timeout. 64 tunnels per relay node.
  Bounded records (512) and events (1024).
- **Logging:** no header values, cookies, credentials, paths, query strings
  or payload bytes. Records keep `host:port` only, in memory. Telemetry
  carries the target only in diagnostic mode and is written to disk only
  when JSONL output is enabled.
- **Failure:** no confirmed configuration, gateway detached or stopped → 503
  or a failed load, **never a direct connection** (tested).
- No administrator rights, no system proxy, no firewall change, no Chromium
  change, sandbox untouched.

### Telemetry and measurement provenance

Each gateway record carries the connection id, generation, algorithm,
decision id, expected path, **observed** path (relay trace), confirmation
evidence, per-relay `StreamTraversal` records (open/close time, bytes each
way, how it ended), egress address, and the timings below.

| Quantity | Measured by | Method |
|---|---|---|
| `head_ms` | gateway | accept → complete request head |
| `establish_ms` | gateway (executor) | tunnel request → relays' confirmed `TUNRSP`, including destination connect and any injected (simulated) set-up delay |
| `first_byte_ms` | gateway | request forwarded → first response byte (HTTP) / first server byte (CONNECT) |
| `duration_ms` | gateway | accept → close |
| relay bytes / open–close | each relay node | `StreamTraversal` |
| page wall time, TTFB | browser | `loadFinished` timer; Navigation Timing `responseStart - requestStart` (experiments only) |
| probe latency | runtime | V0.6 controlled probes (synthetic messages, not browser traffic) |

Gateway events go to the telemetry pipeline as `gateway.*` with source
`browser_gateway` and `measured_by=browser_gateway`. They are **not yet
turned into NetworkState features** (V0.7.4). Browser streams are recorded
as `browser_stream` outcomes of their decision
(`OutcomeRevision.browser_streams`), kept apart from the relay-request
performance summary.

### Developer Tools

- **Runtime → Browser forwarding:** mode, gateway status, listening address,
  confirmed algorithm, configuration generation, the active relay path for
  new connections, its decision, active connections (by generation),
  draining connections, connection counters, bytes, bypassed requests,
  latest forwarding error, and the Chromium switches.
- **Network → Browser connections:** one row per connection: id, kind,
  state, target, generation/algorithm, selected path, observed relay
  traversal with per-relay bytes, establish / first-byte / duration ms,
  bytes, result and error.
- **Decisions:** the decision detail adds the browser forwarding evidence:
  connections pinned to that decision's configuration and their
  relay-confirmed paths.
- **Events:** a "Browser gateway" filter for `gateway.started`,
  `browser_proxy_configured`, `connection_accepted/rejected`,
  `connection_pinned`, `relay_path_established`,
  `forwarding_started/completed/failed`, `connection_cancelled`,
  `relay_path_changed`, `configuration_draining/retired`,
  `proxy_bypass_detected`.

### V0.7.3 tests

| Suite | What it proves |
|---|---|
| `gateway_http` (15 cases) | Parsing, smuggling and malformed input, hop-by-hop removal, response framing, chunked tracker, address classes, egress policy, config validation, loopback-only binding and shutdown, and (closure) `ConnectHostFallsBackQuickly`. |
| `gateway_forwarding` (27 cases, incl. closure `RapidConnectionStress`) | Real relays + local origin. HTTP via A and CONNECT via B, checked against relay records; redirect, POST, 8 MiB, chunked streaming, keep-alive server, 16 parallel; refusals never forwarded; head timeout; destination policy and DNS rebinding; DNS failure, unreachable, server disconnect; no or stale configuration; invalid path; relay failure (also mid-stream); relay timeout; capacity; cancellation, detach and shutdown; foreign process refused; pinning, draining, retirement, bounded drain. With the real `AAFRRuntime`: A→B switch with a pinned connection surviving on A, B→A recovery, failed activation keeps A, relay failure → `FailureRecovery`, outcome observation. |
| `gateway_model` (6 cases) | Deterministic, shadow, AI preferred, a model change alone, model unavailable (fallback), all ten algorithms evaluated. The gateway always follows the confirmed configuration. |
| `browser_protocol` (12 Qt WebEngine cases, closure) | HTTP/2: ALPN `h2` both ends, HPACK-decoded requests, concurrent streams, relay traversal and a 2 MiB response counted at every relay, reuse, RST_STREAM cancellation, unexpected closure, untrusted certificate, unreachable, DNS failure. WebSocket `ws` and `wss`: bidirectional text/binary up to 2 MiB, ping/pong, close codes, abort, navigation away, 20 rapid cycles, 3 tabs, **pinning across an A→B switch with retirement only after every reference closed**, relay failure, gateway shutdown. |
| `browser_forwarding` (12 Qt WebEngine cases) | Real page load over the confirmed path with relay logs, three tabs and navigation, redirect, **HTTPS navigation with validation intact** and rejection of an unknown certificate, a TLS client verifying through the tunnel, chunked page and 1 MiB download, **A→B switch with a held page pinned to A and new pages on B, then B→A**, stop and close mid-load, the real `BrowserWindow` (own `ForwardingService` runtime) plus Developer Tools fields, and gateway detach or stop never going direct. |

### V0.7.3 controlled experiments (Debug build, 2026-11-18)

`aafr-browser-experiment --all --trials=15` (base commit 3629de4 plus the
V0.7.3 changes, Windows 11, Qt 6.12.0 **Debug** WebEngine, this
workstation). Each row is 15 page loads in one long-lived view of a
`NoCache` off-the-record profile; every URL is unique. The origin is the
local test server. Debug Chromium is 2.5–4× slower than Release (see the
V0.5 note), so absolute numbers are high: **compare rows, not absolute
values**. Raw records are in `experiments/results/v0.7.3-debug/`
(`direct.jsonl`, `forwarded.jsonl`, `summary.md`; ignored by git).

| Phase | Page | OK | Navigation median / p90 (ms) | TTFB median / p90 (ms) | Gateway establish median (ms) | Observed relay path (relay records) |
|---|---|---|---|---|---|---|
| A direct baseline | small HTML | 15/15 | 219.5 / 260.5 | 1.0 / 1.1 | — | direct |
| A direct baseline | 1 MiB HTML | 15/15 | 2040.9 / 2117.1 | 12.6 / 17.6 | — | direct |
| B fixed path A, no injected delay | small | 15/15 | 258.0 / 279.6 | 44.2 / 72.5 | 41.8 | entry→relay_a→dest ×31 |
| B fixed path A, no injected delay | 1 MiB | 15/15 | 2105.6 / 2134.6 | 58.5 / 81.3 | 44.7 | entry→relay_a→dest ×29 |
| B fixed path A, configured delay (A 5+5, B 25+25 ms) | small | 15/15 | 281.2 / 302.7 | 83.3 / 107.7 | 80.1 | entry→relay_a→dest ×28 |
| C A degraded to 150+5 ms, before the switch | small | 15/15 | 434.2 / 454.6 | 238.1 / 257.7 | 235.3 | entry→relay_a→dest ×31 |
| C after the runtime's switch | small | 15/15 | 313.1 / 338.6 | 119.1 / 134.6 | 114.9 | entry→relay_b→dest ×29 |
| D A restored, after the runtime's return | small | 15/15 | 280.4 / 304.3 | 84.2 / 110.4 | 81.4 | entry→relay_a→dest ×29 |
| E relay_a offline, after recovery | small | 15/15 | 321.8 / 339.1 | 127.0 / 137.3 | 124.7 | entry→relay_b→dest ×30 |
| F shadow, no model inputs | small | 3/3 | 352.9 / 374.6 | 141.3 / 142.5 | 136.3 | entry→relay_b→dest ×5 |
| F shadow, configured profile | small | 3/3 | 326.5 / 335.4 | 111.9 / 128.0 | 108.4 | entry→relay_b→dest ×5 |

The observed-path counts include each page's favicon request. Every
forwarded connection in every phase was relay-confirmed on the path the
runtime had confirmed at that moment.

Events:

- **C, switch:** the runtime committed A→B after its usual two evaluations,
  probing 24 requests (3.76 s probe overhead), with latency ratio 0.49
  (`challenger_better`) and activation confirmed in 114 ms. It took
  **8.3 s of wall time** from the degradation to the committed switch,
  dominated by probing over the delayed path and the sustain requirement.
  Every later page used B.
- **D, recovery:** B→A after 5.0 s (ratio 0.71, 24 probes, activation
  74 ms).
- **E, failure:** with relay_a stopped, **2 page loads failed** (502
  `relay_unavailable`, never direct). Their relay-failure reports triggered
  `FailureRecovery`, the runtime moved to B, and the third load succeeded
  **1.97 s** after the failure. 4 relay failures were reported in total.
- **F, shadow mode, real model worker (ready):** with no source for the 23
  telemetry inputs, the model **abstained on every decision**
  (`23 missing`), and the deterministic runtime decided and forwarded as
  usual. With the configured (not measured) profile, the model answered
  Dijkstra in all 3 decisions (inference about 17 ms), agreeing with the
  deterministic choice 3/3. In shadow mode it controlled nothing.

Interpretation, with its limits:

- **Forwarding overhead (B vs A):** about **+40 ms TTFB** and **+39 ms**
  median navigation for a small page, and +65 ms on a 1 MiB page. This is
  almost entirely tunnel set-up (gateway establish ≈ 42 ms: three relay
  hops and thread start-up in a Debug build, plus the origin connect).
  Plain HTTP opens a new tunnel per request; reused CONNECT tunnels would
  amortise it. Throughput through three relays was about 74 MiB/s in the
  integration test (8 MiB).
- **Adaptive selection (C):** compared with staying on the degraded path,
  the switch cut median TTFB from 238 to 119 ms and navigation from 434 to
  313 ms. The degradation and both paths' delays are **configured
  (simulated) edge delays** slept by AAFR's own relays, so this shows the
  mechanism working on real browser traffic, not a benefit on a real
  network. Overhead and selection benefit are reported separately (B vs A,
  C-before vs C-after).
- **CPU and memory:** the browser process used about 9.1 s of CPU per
  small-page phase both direct and forwarded, with a working set of
  350–430 MB, so the gateway and relays did not measurably add to Debug
  Chromium's own cost. Renderer processes are not included. Probe overhead
  is the runtime's, reported above.
- One run of 15 trials per phase, so the p90 values are rough; there are no
  confidence intervals. Repeat on a Release build before citing numbers.
- **No AI performance claim:** the model never controlled a tested decision
  (shadow only), and with measured inputs only it always abstained.

### V0.7.3 verification closure (2026-11-18)

This closes the items the first V0.7.3 report left unverified: HTTP/2 and
WebSocket through the gateway, a Release build, Release regression and
Release performance. It started from commit `2611936` with a clean tree.
The architecture was not changed; one genuine gateway defect was found and
fixed (below).

**Status**

| Item | Status |
|---|---|
| HTTP/2 over HTTPS CONNECT (ALPN `h2`), real browser, relay traversal | **Verified** (Debug and Release) |
| Secure WebSocket (`wss://`) through CONNECT, bidirectional, relay traversal | **Verified** (Debug and Release) |
| Plain WebSocket (`ws://`): Chromium tunnels it through CONNECT too | **Verified** |
| WebSocket pinned across an authorised switch; new socket on the new path; draining and retirement | **Verified** |
| Release build of the complete project | **Verified** |
| All 33 suites (28 pre-V0.7.3 + 5 V0.7.3) pass in Release and in Debug | **Verified** |
| Release performance experiments (15 trials per phase, raw data kept) | **Done** |
| HTTP/2 server push, WebSocket over HTTP/2 (RFC 8441), WebSocket compression | **Not tested** |
| HTTP/3 / QUIC | **Unsupported** (excluded by design) |

#### Test infrastructure (local only)

- `tests/support/h2_test_server.h`: a controlled HTTP/2 server (Qt
  `QSslServer`, Schannel, the test-only certificate). ALPN offers `h2`
  **only**, so a client that cannot speak HTTP/2 fails instead of silently
  falling back. It implements the connection preface, SETTINGS, HEADERS and
  CONTINUATION with a full HPACK decoder (static and dynamic tables, size
  updates, every literal form), DATA with connection- and stream-level flow
  control, WINDOW_UPDATE, PING, RST_STREAM and GOAWAY.
- Huffman decoding: RFC 7541's code is canonical, so the decoder rebuilds
  the codes from the code lengths of NUL and printable ASCII. Codes longer
  than 19 bits are refused with a recorded HPACK error, never decoded
  wrongly. Qt's own HPACK classes are not exported (`Q_AUTOTEST_EXPORT`), and
  no HPACK library is installed.
- `tests/support/ws_test_server.h`: an RFC 6455 server for `ws://` and
  `wss://`. It validates the handshake and requires masked client frames,
  reassembles fragmented messages, handles 7/16/64-bit lengths (16 MiB
  limit), and does ping/pong and the close handshake. An echo and control
  protocol lets the test trigger pings, large messages, server closes and
  aborts.
- Schannel negotiating ALPN `h2` as a *server* was verified first with a
  throwaway probe.

#### HTTP/2 evidence (`browser_protocol`, real Qt WebEngine)

| Check | Evidence |
|---|---|
| CONNECT tunnel, TLS, validation | `200 Connection Established` only after the relays confirmed the path. The browser raised a certificate error for the unknown test CA and accepted only the exact test leaf. An untrusted certificate is refused, and that server decodes **zero** HTTP/2 requests. |
| ALPN `h2` | Server: `nextNegotiatedProtocol() == "h2"`. Browser: `nextHopProtocol == "h2"` for the navigation and for all 8 subresources. |
| Server processed HTTP/2 | All requests were decoded from HTTP/2 HEADERS frames and HPACK on one connection: 11 in the reference run (page, 8 async scripts, a 2 MiB fetch, a favicon). |
| Concurrent streams | **8** streams open at once on that connection (8 async scripts, 150 ms each). |
| Connection reuse | A second page load in the same profile used the same HTTP/2 connection and the same gateway tunnel. Release experiment: 15 page loads, 1 server connection, 3 tunnels in total. |
| Relay traversal | The carrying tunnel's id is in every path node's log and absent from the other relay. A 2 MiB HTTP/2 response raised every path node's client-bound byte counter by at least 2 MiB, and its server-bound counter grew too. |
| Spare connection | Chromium raced a second TLS connection and abandoned it after the server's first flight (1394 bytes). The server saw one connection, and that tunnel carried no HTTP/2 data. |
| Cancellation | `AbortController.abort()` sent RST_STREAM (counted by the server). The HTTP/2 connection and tunnel stayed up and kept serving. |
| Unexpected closure | The server aborts TCP after response HEADERS: the fetch fails, the tunnel and its relay chain close, and the next request reconnects on a new tunnel. |
| Negatives | Unreachable server → `DEST_UNREACHABLE`; DNS failure → `DNS_FAILED`; active relay down → the HTTP/2 tunnel ends and a new load fails as `relay_failure` (never direct). |

#### WebSocket evidence

| Check | Evidence |
|---|---|
| Transport | `ws://` and `wss://` both arrive as CONNECT tunnels (`kind=connect`), relay-confirmed on the active path. The browser-side observer counts the handshakes (`webSockets`). |
| Bidirectional | Text echo; 20 sequential messages in order; 4 KiB and **1 MiB** binary client→server echoed with matching checksums; **2 MiB** server→client binary with matching checksum. |
| Ping/pong | A server ping was answered by the browser's pong (the server counts it, then confirms with a message). |
| Close | Client close 1000 (the server records 1000); server close 1000 (the browser reports 1000, clean). The test server first answered a server-initiated close incorrectly (it echoed a second close frame) and the browser reported 1006; the test server was fixed. |
| Abnormal | Server abort → 1006; browser navigation away → the server sees the close and the tunnel and relays end; tabs closed with sockets open → all tunnels end. |
| Rapid repeated | 20 open/echo/close cycles: 20 relay-confirmed tunnels, none left open afterwards. |
| Multiple tabs | 3 tabs, 3 tunnels, all on the confirmed path. |
| Relay failure | The active relay stops: the socket closes 1006, and its tunnel and relays end. |
| Gateway shutdown | `Stop()` with a socket open: it closes 1006 promptly, nothing leaks (`ActiveStreams() == 0`), and a new socket cannot open (no direct fallback). |
| Relay traversal | Every relay of the path carried at least 3 MiB toward the browser and at least 1 MiB toward the server for one socket. |

**Pinning across a switch** (`wssPinnedAcrossSwitchAndDrains`, and Release
experiment C):

- A `wss` socket is opened on A, then the runtime switches A→B through all
  its gates. The socket stays open. Its messages keep flowing over A:
  relay_a's counters grow, relay_b never sees its id. Its record is marked
  `superseded` and counted as draining.
- A new socket opened after the switch uses B (relay_b logs it).
- After the old socket closes, generation A is **not** retired while any
  connection still references it. In the test, Chromium's pooled HTTP/2
  connection was still pinned to A; once it closed, `ConfigurationRetired`
  was emitted. No relay records were left open.
- Release experiment C: a socket opened before the switch exchanged 30
  messages after it, stayed open, and remained pinned to `entry → relay_a →
  dest` while the active path was `entry → relay_b → dest`. RTT median was
  31.7 ms before and 31.6 ms after the switch (unchanged; the delay is only
  injected at set-up).

#### Defect found and fixed: destination connect fallback

The Release measurements showed a `wss://localhost` socket taking **4.3 s**
to open through AAFR, against 0.7 s direct. Diagnosis from the gateway
records: each tunnel took about 2.1 s to establish. `localhost` resolves to
`::1` first, the local servers listen on 127.0.0.1 only, Windows retries a
refused loopback SYN for about 2 s, and the destination node tried resolved
addresses strictly one after another.

`net::ConnectHost` now connects Happy-Eyeballs style (RFC 8305):
- address families are interleaved;
- the next attempt starts 250 ms after the previous one, or immediately when
  it fails;
- the first completed connection wins.

Every resolved address is still checked against the egress policy first,
and the overall deadline is unchanged. Result: tunnel set-up about 300 ms,
`wss` open 0.78 s (direct 0.71–0.75 s), and `browser_protocol` runtime cut
from 208 s to 63 s. Regression test: `gateway_http` /
`ConnectHostFallsBackQuickly` (localhost → 127.0.0.1 in about 280 ms; a
policy that permits nothing still refuses; a closed port still fails within
the deadline).

#### Resource safety and lifecycle

- `gateway_forwarding` / `RapidConnectionStress`: 100 sequential requests,
  then 32 parallel tunnels abandoned before the CONNECT answer, mid-request,
  or reset mid-response. Afterwards no gateway connection, relay stream or
  relay worker remains. The gateway still serves, and `Stop()` takes about
  46 ms.
- Covered elsewhere: browser tabs closed with open sockets, gateway shutdown
  with an open socket, the relay dying mid-stream, configurations retained
  while referenced and retired after, and abortive teardown along the whole
  relay chain.
- No deadlock or hang was observed in any run: 8 complete runs of all
  suites, Debug and Release.

#### Release build

```text
cmake -S . -B build\windows-msvc-qt-release -G Ninja -DCMAKE_BUILD_TYPE=Release ^
      -DCMAKE_PREFIX_PATH=C:/Qt/6.12.0/msvc2022_64 -DAAFR_BUILD_BROWSER_TESTS=ON ^
      -DAAFR_MODEL_DIR=<expert-model release root>
cmake --build build\windows-msvc-qt-release --parallel 4
ctest --test-dir build\windows-msvc-qt-release --output-on-failure -j 2
```

- Toolchain: MSVC 14.44.35207 x64, Ninja, Release Qt 6.12.0 libraries (Qt
  WebEngine included). It was built in its own directory, so the Debug build
  was untouched.
- The model interpreter resolves to `.venv\Scripts\python.exe`, the same
  manifest-checked artifacts as Debug. `model_worker`, `model_e2e` and
  `model_worker_python` start the real worker and pass in Release.
- On this machine the MSVC environment must be set manually; see the build
  notes.

#### Regression results (33 suites)

| Build | Result |
|---|---|
| Debug, start of closure (commit 2611936) | 32/32 |
| Release, first run (existing suites) | 31/32: `browser_interface` failed (see flakiness) |
| Release, final, twice in a row (`-j 2`) | **33/33**, 33/33 |
| Debug, final (`-j 2`) | **33/33** |

**Flakiness found and its causes** (no assertion was weakened):

1. **`browser_interface` in Release under `ctest -j 2`.** Keyboard shortcuts
   and the tab-search popup failed in different cases on each run. The cause
   is a desktop-focus race: the V0.7.3 browser suites show real windows, and
   running concurrently they took focus from `browser_interface`.
   *Fix:* the four Qt browser suites now share a ctest `RESOURCE_LOCK
   aafr_desktop`, so they never run at the same time. Run alone, the suite
   can still fail if someone uses the desktop during the run, because
   Windows won't always give a test window foreground focus.
2. **"The last decision must be a Switch."** Under CPU load, or when the
   first of two evaluations already committed, the final decision is
   `Retain` or `SwitchRejected` although the switch is (or later becomes)
   correctly committed. The tests now check that exactly one more switch was
   committed (the runtime's switch counter) and that the confirmed path is
   the expected one. They allow up to four further *gated* evaluations, each
   through every stability gate and logged.
3. **Test assumptions exposed by faster connects.** A generation retires
   only when *all* its connections close, and Chromium pools connections.
   The tests now prove the generation is not retired while referenced, then
   close the pooled connection and require retirement. The relay-failure
   case uses a fresh profile, so its tunnels are known to be on the active
   path, and a guard always brings the relay back.

#### Release performance (15 trials per phase, 2026-11-18)

Command: `aafr-browser-experiment --all --trials=15` from the Release build.
Raw records: `experiments/results/v0.7.3-release/` (`direct.jsonl`,
`forwarded.jsonl`, `summary.md`; ignored by git).

Setup:
- Workloads: the local origin (HTTP/1.1), the HTTP/2 server and the
  WebSocket server, all on 127.0.0.1.
- One long-lived view per phase, a `NoCache` off-the-record profile, and a
  unique URL per trial.
- Delays are configured edge latencies slept by AAFR's relays at tunnel
  set-up. They are **not** network measurements.
- One run per phase on one workstation; no confidence intervals.

Time is in ms: median / mean / sd / min / max (p95 equals max at n = 15).

| Phase | Navigation | TTFB | Gateway establish median | Observed path |
|---|---|---|---|---|
| A direct, small page | 18.0 / 21.3 / 11.9 / 15.9 / 63.9 | 0.3 / 0.5 / 0.9 / 0.2 / 3.8 | — | direct |
| A direct, 1 MiB page | 31.3 / 33.2 / 4.5 / 28.7 / 43.2 | 1.8 / 2.0 / 0.5 / 1.5 / 3.4 | — | direct |
| B AAFR no delay, small | 80.4 / 74.3 / 21.9 / 33.2 / 105.9 | 49.4 / 44.3 / 14.4 / 15.6 / 71.7 | 42.8 | A ×31 |
| B AAFR no delay, 1 MiB | 86.7 / 83.6 / 21.9 / 39.8 / 136.4 | 45.6 / 44.8 / 13.5 / 8.5 / 59.6 | 42.2 | A ×31 |
| B AAFR, configured delay (A 5+5, B 25+25) | 104.5 / 102.9 / 16.2 / 67.8 / 129.8 | 71.3 / 68.8 / 13.6 / 37.0 / 92.8 | 66.7 | A ×78 |
| C A degraded (150+5), before the switch | 248.7 / 248.1 / 22.1 / 211.9 / 282.1 | 218.2 / 221.6 / 18.6 / 194.6 / 252.0 | 219.5 | A ×33 |
| C after the switch | 150.0 / 149.0 / 16.5 / 121.1 / 181.9 | 122.2 / 120.6 / 14.8 / 92.8 / 149.3 | 116.3 | B ×30 |
| D A restored, after the return | 102.2 / 100.8 / 12.1 / 81.2 / 120.0 | 72.3 / 69.9 / 10.3 / 47.9 / 82.7 | 66.8 | A ×31 |
| E relay_a down, after recovery | 140.5 / 146.4 / 19.4 / 119.2 / 175.4 | 115.5 / 117.5 / 16.6 / 93.4 / 140.7 | 109.4 | B ×31 |

Switching, recovery and failure:
- **C:** A→B committed after 8.3 s of wall time (24 probes, 3.9 s probe
  overhead, latency ratio 0.50, activation 96 ms).
- **D:** B→A after 4.9 s (ratio 0.69, activation 94 ms).
- **E:** relay_a stopped; 2 page loads failed with 502 (never direct), and
  the runtime recovered to B in 2.07 s.

Protocol workloads (same 8-subresource page, 150 ms per subresource; time
until all 8 have run):

| Phase | All subresources (ms): median / mean / sd / min / max | Protocol | Connections / tunnels |
|---|---|---|---|
| Direct, HTTP/1.1 | 432.7 / 425.8 / 12.6 / 401.9 / 445.4 | http/1.1 ×15 | — |
| AAFR, HTTP/1.1 | 560.5 / 561.4 / 50.5 / 486.3 / 649.6 | http/1.1 ×15 | 149 tunnels (one per request) |
| Direct, HTTP/2 | 245.3 / 321.4 / 303.2 / 212.7 / 1415.5 | h2 ×15 | 2 server connections |
| AAFR, HTTP/2 | 249.2 / 324.3 / 301.3 / 206.7 / 1412.2 | h2 ×15 | 1 server connection, 3 tunnels, 8 concurrent streams |

| Phase | Open (ms): median / mean / sd | Echo RTT (ms): median / mean / sd / min / max / p95, n | Disconnects | Path |
|---|---|---|---|---|
| Direct, wss | 748.6 / 734.1 / 27.9 | 31.6 / 27.1 / 8.8 / 2.1 / 40.3 / 33.0, n = 250 | 0 | direct |
| AAFR, wss | 696.5 / 690.0 / 22.1 | 31.6 / 26.5 / 8.9 / 1.5 / 37.8 / 33.0, n = 250 | 0 | A ×5 |
| AAFR, ws | 38.1 / 36.8 / 10.2 | 31.9 / 29.2 / 6.9 / 2.2 / 46.3 / 34.0, n = 250 | 0 | A ×5 |

Interpretation:

- **Forwarding overhead is a per-connection cost.** It is about +45–50 ms
  TTFB (tunnel set-up through 3 relays plus the origin connect) and is paid
  once per connection.
- **HTTP/2 amortises it.** With HTTP/2, one tunnel carries every request:
  median completion through AAFR (249 ms) is within noise of direct
  (245 ms). The mean and max of both are dominated by the first load, which
  pays the TLS handshake and the certificate decision (about 1.4 s).
- **Plain HTTP/1.1 pays it per request:** 149 tunnels for 120 requests'
  worth of pages (+128 ms median). This is the documented
  one-request-per-connection limitation.
- **WebSocket:** the open time through AAFR matches direct after the
  connect fix. Echo RTTs are identical. The ~31 ms RTT is set by the
  experiment process's Qt event loop (the server runs on it), so it bounds
  rather than measures AAFR's per-message cost; a WebSocket message adds no
  tunnel set-up.
- **Adaptive selection (C):** the switch cut median TTFB from 218 to
  122 ms on the configured degradation. This shows the mechanism working on
  real browser traffic. It is **not** evidence of a real-network benefit.
- **CPU and memory:** about 1.1–1.7 s of CPU per 15-load phase, both direct
  and forwarded; working set 194–330 MB (renderer processes not included).
  Debug Chromium had cost about 9 s per phase.
- The model (F, real worker in Release, shadow mode): it abstained on every
  decision without measured inputs, and agreed 3/3 with the deterministic
  choice on the configured profile. It never controlled forwarding, so no AI
  performance claim is made.

#### Security revalidation

These pass in Debug and Release:
- Loopback-only binding (unreachable through the host's two non-loopback
  addresses).
- Foreign-process refusal (a PowerShell client is refused and its request
  never reaches the origin).
- The default egress policy (public on 80/443 only), DNS-rebinding refusal
  (`localhost:443` refused after resolution, also with the new parallel
  connect), port restrictions, and CONNECT target validation.
- Smuggling and malformed-input rejection (none reaches the origin) and the
  capacity limit.
- TLS: no interception, and an unknown certificate is refused for HTTPS,
  HTTP/2 and `wss`.
- No header value, cookie or path in records or events.
- Fail-closed shutdown and detach.

No control was relaxed for the protocol tests: loopback destinations are
reachable only through the explicit `--forward-loopback` experimental
configuration.

### Known limitations

- HTTP/3/QUIC is excluded by design. Not tested: HTTP/2 server push,
  WebSocket over HTTP/2 (RFC 8441), WebSocket compression extensions.
- Measurements are from one workstation, one run per phase, with configured
  (simulated) relay delays; they are not real-network results.
- Connecting to a name whose first address is unreachable costs up to the
  250 ms attempt delay (RFC 8305 style) before the next family is tried.
- Plain HTTP uses one request per proxy connection (no reuse), and chunked
  request bodies are refused (501).
- The proxy is process-wide and fixed at start-up; there is no per-tab or
  per-profile routing, and changing mode needs a restart.
- Loopback destinations bypass the gateway unless `--forward-loopback` is
  given (reported as `ProxyBypassDetected`).
- The destination node resolves names with blocking `getaddrinfo`; a very
  slow resolver holds that tunnel's thread (bounded only by the resolver).
- The relays, the controlled graph and every injected delay are AAFR's own
  loopback environment; the destination is reached from this host
  directly. No multi-host relay network exists.
- The process-ownership check (`restrict_to_process`) is Windows-only;
  elsewhere the gateway relies on loopback binding alone.
- Gateway and relay measurements are not yet NetworkState features, and the
  model's 23 telemetry inputs still have no measured source (see
  [`expert_model/README.md`](../../expert_model/README.md#v073-feature-readiness-from-real-browser-forwarding)).
  The topology mismatch (4-node graph vs the model's training networks) is
  unchanged.
- Synthetic-only parts that remain: runtime probes and health checks are
  AAFR messages, not browser traffic; injected delays are configured.

### V0.7.4 readiness

Done in V0.7.4: gateway and relay records now reach NetworkState as windowed
`fwd.*` features (Developer Tools → Network → *Forwarding measurements*); see
[expert model README, V0.7.4](../../expert_model/README.md#v074-real-network-feature-acquisition-and-model-compatibility).
The text below is kept as written for V0.7.3.

Ready: real browser connections over relay-confirmed paths, with
per-connection gateway timings, per-relay byte and lifetime records,
relay-failure and path-change events, and browser-stream outcomes per
decision, all with provenance. Next (not started): turn those into windowed
per-path NetworkState features with freshness and validity, build the
estimators listed in the feature table where a direct measurement does not
exist, and only then feed a measured telemetry source to the model.
