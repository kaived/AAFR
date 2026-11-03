# V0.2 runtime verification and stability

Verified on 2026-11-03 with Windows 11, MSVC 19.44 x64, Qt 6.12.0 Debug,
CMake, and Ninja. Scope: diagnostics and object lifetime only. No browser
features, network monitoring, routing, or V0.3 work were added.

## Diagnostic isolation

Each control ran in a separate invocation of `browser-interface-tests.exe`,
using temporary local HTML files and the default WebEngine profile. The plain
controls instantiate `QWebEngineView` directly; they do not create AAFR's
`BrowserWindow`, `TabManager`, or `BrowserView` adapter.

| Isolated test | Result | Captured Chromium stderr |
| --- | --- | --- |
| `plainWebEngineLifecycle:loaded` | Passed | One `WebFrame LEAKED 1 TIMES` message at renderer shutdown |
| `plainWebEngineLifecycle:missing` | Passed | Three media URL-safety messages |
| `plainWebEngineLifecycle:missing-without-error-page` | Passed | Empty |
| `failedNavigationAndRecovery` | Passed | Three media URL-safety messages |

All controls assert the expected `loadFinished` result. Plain controls also
verify that their view, default page, and Back action are destroyed. The third
control disables `ErrorPageEnabled` only on its own view. Application behavior
and other tests retain Qt's error page; no security restrictions or log levels
were changed.

The installed runtime reproduces both diagnostics without AAFR ownership or
signal wiring. The missing-file comparison implicates the built-in error
document as the media-diagnostic path in this environment. It does not identify
the exact rejected media resource. The successful local-page control shows
that the frame message is not specific to failed navigation or AAFR teardown.

Chromium's [renderer shutdown source](https://chromium.googlesource.com/chromium/src/+/refs/heads/main/content/renderer/render_process_impl.cc)
logs remaining Blink frame instances under a Debug-build guard. This explains
the kind of diagnostic, not its underlying cause in the installed binary.
The linked upstream branch is contextual source, not a claim of an exact
source revision match to Qt's bundled Chromium.

Disposition: Qt/Chromium-level reproducers established; underlying renderer
shutdown cause remains unresolved. Do not suppress the diagnostics or infer
that they are harmless. These checks establish QObject destruction, not native
or renderer memory-leak freedom. Further renderer investigation can use the
plain-view control without modifying AAFR's architecture.

## Ownership review and regression checks

| Object/path | Ownership and evidence |
| --- | --- |
| Main window and controls | `main.cpp` owns the window on the stack. Qt parents own the central manager, toolbar, actions, shortcuts, and status widgets. |
| Tab view | QTabWidget reparents views into its internal stack; the manager remains an ancestor. `removeTab` does not delete or transfer ownership. Tests check this path and manager destruction. |
| Default page and actions | Default pages are children of their views; page actions disappear with page destruction. Tests retain `QPointer` guards and verify nullness after teardown. This agrees with Qt's [default page implementation](https://raw.githubusercontent.com/qt/qtwebengine/dev/src/webenginewidgets/api/qwebengineview.cpp). |
| Default profile | Qt's built-in profile is shared; the manager creates no custom profile and must not delete the global one per tab. See [Qt profile documentation](https://doc.qt.io/qt-6.8/qwebengineprofile.html). |
| Closing a tab | View/page/action connections to the manager are disconnected before removal; state is removed; loading stops; the parented view is scheduled with `deleteLater`. A synthetic-signal test confirms closed views cannot update state or emit manager completion events. |
| Pending deletion at manager shutdown | The parent destroys both live and detached views, including a view awaiting DeferredDelete. Tests verify view/page/action destruction and exactly one destroyed signal per view. |
| Shutdown during loading | Two tabs load loopback URLs whose server deliberately does not reply. Destroying the manager destroys both views and pages while loading. |
| External view destruction | State is removed and a context-bound timer schedules last-tab replacement. Tests destroy foreground/background views and then the manager before timer delivery, confirming no replacement after manager destruction. |
| Manager destructor | Incoming view/page/action callbacks are disconnected before the state hash is destroyed and QWidget later destroys children. Existing teardown behavior passed the added regression cases. |

No production ownership defect was reproduced by these checks, so this task
changes tests and documentation only. The renderer-termination signal in the
closed-view test is synthetic: it checks disconnection, not recovery from a
real Chromium crash. The suite does not test arbitrary external replacement
of a view's page or unsupported cross-thread widget operations.

## Commands and evidence

Initialize the matching Visual Studio x64 environment with
`VsDevCmd.bat -arch=x64 -host_arch=x64 -vcvars_ver=14.44`, then run:

```powershell
cmake --build build\windows-msvc-qt --parallel 4
ctest --test-dir build\windows-msvc-qt --output-on-failure
```

Build succeeded. CTest passed **1/1 suite in 19.55 seconds** (19.56 seconds
total). Qt Test reported **32 passed, 0 failed, 0 skipped**, including
initialization and cleanup. The full suite includes three plain-view data rows
and four additional lifecycle cases. Four separate diagnostic invocations also
passed, each reporting its selected case plus initialization and cleanup.

The agent's process sandbox denies temporary HTML writes, so execution used
the normal developer environment outside that sandbox. Chromium's sandbox and
Windows security settings remained enabled/unchanged. The standalone browser
executable was not launched.

Full-suite evidence lives under `build/windows-msvc-qt/`:

- `browser-interface-results.txt`: Qt assertions and totals.
- `Testing/Temporary/LastTest.log`: CTest execution and Chromium stderr.
- `diagnostic-*.qt.txt` and `diagnostic-*.stderr.txt`: isolated controls.

To reproduce a control independently from the repository root:

```powershell
$env:PATH = "C:\Qt\6.12.0\msvc2022_64\bin;$env:PATH"
.\build\windows-msvc-qt\browser-interface-tests.exe 'plainWebEngineLifecycle:loaded' -o 'build/windows-msvc-qt/control.qt.txt,txt' 2> build/windows-msvc-qt/control.stderr.txt
```

Other selectors are `plainWebEngineLifecycle:missing`,
`plainWebEngineLifecycle:missing-without-error-page`, and
`failedNavigationAndRecovery`. Use distinct output filenames to retain each
comparison. Generated reports remain build artifacts.

## Acceptance decision

**V0.2 is functionally implemented and automated-test verified, but not yet
fully accepted.** Standalone visual acceptance remains explicitly pending
until an execution environment permits the browser under application-control
policy. No attempt was made to bypass Smart App Control or modify Defender,
Code Integrity, registry security settings, or Chromium's sandbox. V0.3 has
not started.
