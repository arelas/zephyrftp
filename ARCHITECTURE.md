# ZephyrFTP: Architecture & Verification Notes

This is the technical deep-dive — how the app is built internally, what's
actually been verified vs. just compiled, and the known gaps in the
current implementation. If you're looking for how to *use* ZephyrFTP, see
[README.md](README.md) instead. If you're looking for how to *build* or
*test* it, see [CONTRIBUTING.md](CONTRIBUTING.md) — this document assumes
you've already got it building and want to understand what's underneath.

## Verification status

Claims below are backed by something concrete — a test that actually
runs, a real screenshot analyzed pixel-by-pixel, a real SFTP server —
not just "the code compiles." Where something *hasn't* been verified
that way, it's flagged explicitly rather than left implied.

- CMake configures cleanly against Qt6 (Widgets, Network, Concurrent,
  Svg) and libssh2 via pkg-config. Full build compiles and links with
  zero errors on Ubuntu 24.04 (Qt 6.4.2, libssh2 1.11.0, GCC 13.3.0).
  Resulting binary correctly links `libQt6Widgets`, `libQt6Gui`,
  `libQt6Core`, `libssh2`. App starts and runs its event loop headlessly
  (`QT_QPA_PLATFORM=offscreen`) without crashing.

- **The actual threading contract was exercised, not just compiled.**
  `src/smoke_test.cpp` (built via the `smoke-test` CMake target,
  `EXCLUDE_FROM_ALL` so it's not part of a normal build) does the following
  for real: creates an `SftpBackend`, moves it to a `QThread`, invokes
  `connectToHost()` via a queued cross-thread call, lets the blocking
  libssh2 socket connect fail against a closed local port (fast,
  deterministic, no live server needed), confirms the resulting
  `connectionFailed` signal is delivered back on the *GUI* thread
  (`QThread::currentThread() == qApp->thread()` checked explicitly), then
  tears the worker thread down via `deleteLater()` + `quit()` + `wait()`
  and confirms `isFinished() == true`. Run it with:
  ```
  cmake --build build --target smoke-test
  QT_QPA_PLATFORM=offscreen ./build/smoke-test
  ```

- **The transfer queue actually moves files, verified byte-for-byte —
  and now covers cancel/retry too, not just the happy path.**
  `src/transfer_queue_test.cpp` (built via the `transfer-queue-test` CMake
  target) runs three phases against real `FilePaneWidget`s on real
  `LocalBackend`s: (1) a full transfer — destination file exists, MD5
  matches source exactly, `Queued -> InProgress -> Done` with plausible
  progress numbers; (2) cancelling a `Queued` (not-yet-started) item —
  exploits a real, deterministic property of `TransferManager` (two
  `enqueue()` calls back-to-back synchronously produce one `InProgress`
  and one still-`Queued` item, no timing race needed) and confirms the
  cancelled item is never actually processed; (3) retrying a `Failed`
  item — points at a nonexistent source so both the original attempt and
  the retry fail deterministically, proving `retryItem()` genuinely
  re-runs the transfer rather than just resetting a status field.
  This test is also what caught two real bugs during development, both
  fixed before this note was written: `LocalBackend`'s copy methods
  weren't emitting `transferProgress` at all (blank progress column for
  local transfers), and — found by phase 2/3's back-to-back transfers —
  `QFile::copy()` specifically refuses to overwrite an existing
  destination file (unlike `QFile::open(WriteOnly)`, which truncates),
  so any re-transfer of a file already present at the destination was
  silently failing. Run it with:
  ```
  cmake --build build --target transfer-queue-test
  mkdir -p /tmp/transfer_test/src_dir /tmp/transfer_test/dst_dir
  head -c 500000 /dev/urandom > /tmp/transfer_test/src_dir/testfile.bin
  QT_QPA_PLATFORM=offscreen ./build/transfer-queue-test
  ```

- **A real SFTP round-trip against a live server, confirmed by hand.**
  Password auth, the host-key trust-on-first-use flow (`SftpBackend::verifyHostKey()`
  against a persistent `known_hosts` file), and the home-directory default
  (`libssh2_sftp_realpath(sftp, ".", ...)` instead of hardcoding `/`) have
  all been exercised against a real local SFTP server and confirmed
  working as intended — connecting lands in the actual home directory,
  and the host-key prompt/persistence behaves correctly on repeat
  connections. This is real per-feature verification, not inferred from
  the code compiling.

- **The dark theme and icon set render correctly, confirmed with a real
  screenshot, not just "doesn't crash."** `IconTheme::tintedIcon()` (loads
  a vendored SVG, recolors it via `QPainter` compositing) was verified
  against all 18 vendored icons with a throwaway test that renders each
  one and counts opaque pixels — this caught a real bug before it shipped:
  the first version of `resources/icons.qrc` had the wrong resource path
  (`:/icons/icons/plug.svg` instead of `:/icons/plug.svg` — the qresource
  prefix and the file's own relative path were both contributing
  `icons/`). Separately, a full headless screenshot of the running app
  (`QWidget::grab()`, `QT_QPA_PLATFORM=offscreen`) was analyzed
  pixel-by-pixel: the dark background matches the design's `#14171c`
  token on 97.9% of sampled pixels, and all three toolbar icon colors
  (green/red/amber) appear specifically within the toolbar region, with
  zero blue there — correct, since blue is reserved for pane
  headers/selection and none of the three toolbar actions use it.
  **Not verified this way:** the transfer queue's colored progress bars
  and status icons — the screenshot was taken with an empty queue, so
  only their code paths and unit-level logic are covered (via
  `transfer_queue_test.cpp`), not their actual rendered appearance.

**Still not verified:** public-key authentication (implemented, but no
real key file has been tested against it yet — see Known gaps), the
transfer queue's progress-bar/status-icon rendering with a real active
transfer (see above), and real window rendering on a real physical
display (this development environment has no windowing system; only
headless/offscreen runs have been checked).

## Architecture

- `RemoteBackend` — abstract interface (`connectToHost`, `listDirectory`,
  `downloadFile`, `uploadFile` + signals for results/progress). Everything
  in the UI layer talks to this, never to a concrete backend type.
- `LocalBackend` — wraps `QDir`/`QFile`. Runs on the GUI thread; local
  listing/copy is fast enough that this hasn't been a problem, but it's a
  design decision worth revisiting if it ever needs to handle slow
  network-mounted paths.
- `SftpBackend` — wraps libssh2's synchronous API directly. Runs on a
  dedicated `QThread`, confirmed by the smoke test above. Its four
  operations (`connectToHost`, `listDirectory`, `downloadFile`,
  `uploadFile`) are declared `public slots:` in the `RemoteBackend` base
  class specifically so `QMetaObject::invokeMethod(..., Qt::QueuedConnection)`
  can reach them by name across the thread boundary — the base class's
  moc-generated slot thunk dispatches through the vtable, so the derived
  override still runs (Qt's "virtual slot" pattern).
- `ConnectionDialog` — host/port/username form plus a password/private-key
  auth toggle (`QStackedWidget` swaps the relevant fields). Returns a
  single `SftpCredentials` struct (`src/backends/SftpCredentials.h`) that
  `SftpBackend` consumes directly — no more unpacking/repacking individual
  fields at the call site. No saved-site list yet — a reasonable next
  addition.
- `HostKeyVerifier` — lives on the GUI thread for the app's lifetime.
  `SftpBackend`'s worker thread calls into it via
  `QMetaObject::invokeMethod(..., Qt::BlockingQueuedConnection)` to get a
  synchronous host-key trust decision from a real person — the standard
  Qt pattern for a background thread needing a blocking answer from the
  UI, since popping a `QMessageBox` off the GUI thread isn't safe.
- `FilePaneWidget` — one side of the dual-pane view. Holds a
  `QStandardItemModel`, doesn't know or care which backend it's attached
  to. `setBackend(backend, thread)` swaps backends at runtime: if a
  `QThread` is passed, the pane does NOT parent the backend to itself
  (Qt disallows reparenting across thread boundaries) and instead manages
  its lifetime manually — `deleteLater()` on the backend (runs on its own
  thread's queue) followed by `thread->quit()` + `thread->wait()`. Passing
  `thread=nullptr` (e.g. for `LocalBackend`) falls back to normal
  parent-child ownership. Right-click on selected rows offers "Transfer
  Selected" (multi-select, via `filesActivated`), on top of the original
  double-click-one-file behavior (`fileActivated`).
- `FileTreeView` — thin `QTreeView` subclass adding cross-pane
  drag-and-drop. Qt's built-in item-view DnD pulls its `QMimeData` from
  the *model* (`QAbstractItemView::startDrag()` calls
  `model()->mimeData(...)`), which is built for reordering within one
  model — it doesn't fit "drag from one pane's tree onto a different
  pane's tree", so drag start (`startDrag()`) and drop handling
  (`dragEnterEvent`/`dropEvent`) are both overridden here instead. The
  dragged `QMimeData` carries the source `FilePaneWidget*` as a raw
  `quintptr` — same-process-only, never meant to leave this app, a
  legitimate technique for internal Qt drag-and-drop.
- `TransferManager` — owns the transfer queue, processes it **serially**
  (one item at a time — SftpBackend holds a single libssh2 session, and
  concurrent transfers on the same session aren't safe without more
  synchronization than this app has yet). `enqueue()` figures out
  direction (local->remote / remote->local / local-copy / unsupported
  remote-to-remote) from each pane's `isLocalFilesystem()`, then dispatches
  to whichever backend actually owns the "remote" side of the operation.
  Reconnects its progress/finished/failed signal listeners to whichever
  backend is executing the current item — `RemoteBackend` objects persist
  across multiple transfers, so `connectToBackend()` explicitly disconnects
  the previous backend before wiring up the next to avoid stacking
  duplicate connections. `cancelItem()`/`retryItem()` (right-click in
  `TransferQueueWidget`) round out the queue: cancelling a not-yet-started
  item just marks it `Cancelled` directly; cancelling the active one calls
  `RemoteBackend::requestCancel()` — a plain thread-safe method (not a
  queued slot, since a queued signal wouldn't be processed until the
  blocking transfer loop it's meant to interrupt already returned on its
  own) — and a flag (`m_activeItemCancelled`) distinguishes the resulting
  `transferFailed` as "cancelled" vs. "genuinely failed" once it arrives.
- `TransferQueueWidget` — table view mirroring `TransferManager`'s
  `itemAdded`/`itemUpdated` signals, plus a right-click context menu
  (Cancel/Retry, enabled based on the item's current status) that calls
  straight back into the manager. All state still lives in the manager —
  this is a view, not a second source of truth.
- `MainWindow` — two `FilePaneWidget`s in a `QSplitter`, plus a
  `QDockWidget` at the bottom holding the transfer queue. Left pane is
  always `LocalBackend`. Right pane starts on `LocalBackend` and the
  toolbar's "Connect..." action swaps it for a live `SftpBackend` + worker
  `QThread` via `ConnectionDialog`; "Disconnect" swaps back to
  `LocalBackend`. Double-clicking a file in either pane calls
  `TransferManager::enqueue()` with that pane as source and the other as
  destination; `TransferManager::transferSucceeded` triggers a refresh of
  both panes' listings.

## Design system

The dark theme and icon set come from a design package (not included in
this repo's history prior to this pass) built around Tabler Icons (MIT)
and a fixed four-color semantic system — full rationale in that package's
own README and `ICON-MAP.md`. Scope of this pass was a **visual re-skin
of existing features only** — the mockups also cover a Site Manager
(saved connections), pause/resume, and live transfer speed, none of
which exist in this app yet; only the icon/color/theme layer was ported.

- `resources/icons/*.svg` — 18 vendored Tabler Icons SVGs, fetched
  directly from `github.com/tabler/tabler-icons` (MIT license included
  as `resources/icons/LICENSE-tabler-icons.txt`). Each uses
  `stroke="currentColor"`, deliberately not pre-colored — recolored at
  render time instead (see `IconTheme` below), so there's one source
  file per icon regardless of how many accent colors it's used with.
- `resources/icons.qrc` / `resources/theme.qrc` — compiled into the
  binary via Qt's resource system (`CMAKE_AUTORCC`), so there's no
  runtime dependency on the icons existing as loose files or a CDN.
- `IconTheme` (`src/ui/IconTheme.h/.cpp`) — loads a vendored SVG via
  `QSvgRenderer`, renders it to a transparent `QPixmap`, then recolors
  every non-transparent pixel via `QPainter::CompositionMode_SourceIn` —
  standard Qt technique for tinting monochrome icons. Also generates a
  `@2x` pixmap (`QPixmap::setDevicePixelRatio(2.0)`) for HiDPI displays.
  Exposes the four semantic accent colors (`IconTheme::Blue/Green/Red/Amber`)
  plus two neutral grays as named constants, matching
  `assets/zephyr-theme.css`'s `--zf-*` tokens from the design package.
- `resources/theme.qss` — the design package's CSS theme ported to Qt
  Style Sheets token-by-token (QSS has no equivalent of CSS custom
  properties, so each `--zf-*` value is hardcoded here with its source
  token name in a comment — **if the design package's palette changes,
  this file needs updating too; there's no shared source between them**).
  Loaded and applied via `qApp->setStyleSheet()` in `main.cpp` at startup.
- Icon/color choices in the actual UI (which file extension gets which
  icon, which toolbar action gets which color) follow `ICON-MAP.md`
  directly — see `FilePaneWidget::iconForEntry()` and
  `TransferQueueWidget::statusIcon()` for where those choices live in code.

## Windows builds (CI)

`.github/workflows/windows-build.yml` builds this on GitHub's
`windows-latest` runner: MSVC + Qt6 (via `jurplel/install-qt-action`) +
libssh2 (via vcpkg, with `actions/cache` on the `installed/` output so
only the first run pays the ~8.5 minute openssl/zlib/libssh2 build) +
Ninja, then `windeployqt` plus a wildcard copy of vcpkg's own DLLs to
bundle everything the exe needs. Runs on every push to `main` and on
`v*` tags; tag pushes also attach the build as a zipped GitHub Release
asset (untested — no tag has been pushed yet).

**Confirmed working end-to-end**, not just "builds without error": the
resulting `.exe` has actually been run on real Windows, launches as a
proper GUI app (no trailing console window), and connects to a real SFTP
server successfully. Getting here surfaced and fixed several real,
non-obvious bugs along the way — worth knowing about if this pipeline
ever needs touching again:
- Qt version/arch mismatch (`win64_msvc2022_64` requires Qt >= 6.8)
- `run-vcpkg` needs a full 40-character commit SHA, not a tag name
- A stale vcpkg commit pin 404'd on a pruned MSYS2 mirror artifact
- Two POSIX-only portability bugs in `SftpBackend.cpp` (raw BSD socket
  headers; `mode_t`/`S_IRUSR`-style permission macros) — this code had
  only ever been compiled on Linux until the first real Windows build
- Missing runtime DLLs (`libcrypto-3-x64.dll`, `z.dll`) — libssh2's own
  transitive dependencies, which `windeployqt` doesn't know about
- `qt_add_executable()` doesn't set `WIN32_EXECUTABLE` automatically —
  without it, every launch opened a blank console window alongside the UI
- vcpkg's `x-gha` binary-cache backend was fully removed by Microsoft
  (not just deprecated) — `actions/cache` on the `installed/` directory
  replaced it
- Qt's SVG module (`qtsvg`) is a base install archive, not a separate
  opt-in module like `qtcharts`/`qtwebengine` — confirmed via
  aqtinstall's own docs before assuming a CI change was needed when
  `Qt6::Svg` was added as a build dependency; it wasn't.

## Known gaps (flagged, not fixed)

- **Public-key authentication is implemented but unverified.**
  `ConnectionDialog` has a password/private-key toggle and
  `SftpBackend::ensureSession()` branches accordingly, but no real key
  file has been tested against it yet — only password auth has been
  confirmed against a real server. It also assumes the conventional
  `<privatekey>.pub` sibling file exists rather than deriving the public
  key from the private key directly; untested if that assumption doesn't
  hold for a given key.
- **Remote-to-remote transfers are unsupported**, not silently dropped —
  `TransferManager::enqueue()` marks them `Failed` immediately with an
  explanatory message. Would need a stage-through-a-local-temp-file
  fallback (download then upload) to support.
- **Recursive directory transfer isn't implemented.** Multi-select and
  drag-and-drop both skip directories entirely (files only) — dragging or
  selecting a folder does nothing, silently, rather than erroring. Would
  need real recursive traversal + a nested queue structure to support.
- **Cancel is implemented but only automated-tested against `LocalBackend`,
  where it's a documented no-op** (`QFile::copy()` can't be interrupted
  mid-call). `SftpBackend`'s actual mid-transfer interruption — the cancel
  flag checked inside the libssh2 read/write loops — compiles and follows
  the same pattern proven correct elsewhere in this codebase, but has no
  real-server test confirming it actually stops a transfer partway
  through. Needs a slow enough real transfer to verify by hand.
- **Queue item execution is bound to whatever backend is on the pane when
  its turn comes up**, not when it was enqueued. If you queue a transfer,
  then hit Disconnect before it starts, it'll run against whatever backend
  is on the pane at that point instead. Only matters if items are queued
  faster than they complete, which won't happen with the current
  one-file-at-a-time UI — worth revisiting if batch queueing is added.
- **`libssh2_init(0)` is called once per connection attempt** inside
  `ensureSession()` rather than once globally at app startup — harmless
  today (libssh2 tolerates repeat init calls) but worth moving to
  `main()` if multiple simultaneous SFTP connections are ever supported.
