# AAFR Qt WebEngine browser V0.2

The browser UI runs independently of the adaptive forwarding runtime.

## Architecture

- `main.cpp`: application startup and top-level window lifetime.
- `browser_window.h/.cpp`: navigation toolbar, address bar, shortcuts, window
  title, loading progress, errors, and integration status.
- `tab_manager.h/.cpp`: tab ownership, per-page loading state, WebEngine signal
  wiring, new-window requests, and safe tab closure. An internal `BrowserView`
  adapter routes links requesting a new window into tabs.

## Features

Back, Forward, Refresh, Stop, and Home operate on the active tab. Home opens
`https://example.com`. Enter a URL or search terms and press Enter. Qt's
`QUrl::fromUserInput` handles hostnames and local file paths. Bare hostnames use
HTTPS; explicit HTTP and HTTPS URLs retain their scheme. Words and phrases
without a recognizable hostname use the configured search URL, with the query
percent-encoded. Invalid URLs display a message without navigating.

`BrowserWindow(parent, homeUrl, searchUrlTemplate)` configures the home page and
search endpoint. Defaults are `https://example.com` and
`https://www.google.com/search?q=%1`; search templates must use HTTP(S) and
contain `%1`. `BrowserWindow::urlFromInput` provides the same normalization
without starting WebEngine navigation. There is no settings persistence or
search-provider preferences dialog in V0.2.

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

Verification on 2026-11-03 using MSVC 19.44 and Qt 6.12.0 Debug:
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

V0.2 does not include adaptive routing, interception, inference, or V0.3
observability.

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
- `BrowserWindow::setIntegrationStatus(text)` lets an adapter display runtime
  connection status; the initial label is `AAFR: standalone`.

View pointers are owned by the tab manager; external adapters should use
`QPointer` or QObject lifetime connections and must not retain raw pointers
after a tab closes. Update widgets only on the GUI thread.

No forwarding policy, request interceptor, sensor, or AI model is installed by
this UI. Future adapters should use `runtime/interfaces/interfaces.h`, validate
support in `runtime/execution/action_space.h` and `configs/policy_registry.yaml`,
and keep policy selection and execution outside the browser UI.
