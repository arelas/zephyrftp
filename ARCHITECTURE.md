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

- **Site Manager persists correctly, and the security property that
  matters most about it (no password ever hits disk) is verified, not
  assumed.** `src/site_store_test.cpp` (built via the `site-store-test`
  CMake target, isolated from any real saved sites via `XDG_CONFIG_HOME`
  — see its own header comment) round-trips a password-auth and a
  key-auth site through `SiteStore::save()`/`load()` and confirms every
  field survives — including `useHomeDirectory`/`startingDirectory`,
  deliberately set to non-default values on the test sites so the
  round-trip check actually exercises them, not just the fields that
  happened to already be non-default; confirms
  `SavedSite::toCredentials()` never fabricates a password from thin
  air, and separately confirms it correctly carries the
  starting-directory choice through; confirms `load()` with no file yet
  returns an empty list rather than erroring; and parses the raw written
  JSON to confirm no object has a `password`- or `passphrase`-named key
  anywhere in it — checked via actual `QJsonDocument` key inspection,
  not a raw text search, after a first version of that check
  false-positived on the perfectly innocuous `"authMethod": "password"`
  label (which distinguishes auth type, carries no secret) and had to be
  corrected before it could misreport a real problem that wasn't there.
  Separately, a real screenshot of the new `SiteManagerDialog` (same
  `QWidget::grab()` + pixel-sampling method proven on the toolbar
  earlier) caught a genuine, pre-existing bug: `theme.qss` only ever
  styled `QMainWindow`, never `QDialog` — every dialog window, including
  the original `ConnectionDialog` (shipped several sessions earlier),
  had been silently rendering with Qt's default *light* background this
  whole time, since nobody had screenshotted a dialog specifically until
  now. Fixed with one added selector; re-verified both dialogs by pixel
  sampling after the fix (background sample went from `(239,239,239)`
  to `(20,23,28)`, matching `#14171c` exactly).

- **Back/forward/up navigation is verified against real filesystem
  behavior, including the tricky cases, not just the happy path.**
  `src/navigation_test.cpp` (built via the `navigation-test` CMake
  target) creates a real `FilePaneWidget` on a real `LocalBackend`
  pointed at a real nested temp directory tree
  (`/tmp/nav_test/a/b/c`), sequences 14 checks through the actual async
  `navigateTo()`/`goBack()`/`goForward()`/`goUp()` path (same
  `QMetaObject::invokeMethod(..., Qt::QueuedConnection)` route real
  navigation uses, sequenced with `QTimer::singleShot` the same way
  `transfer_queue_test.cpp` does), and confirms: three-deep forward
  navigation lands correctly; two `goBack()` calls retrace it exactly;
  `goForward()` after a partial retrace lands back where expected;
  branching to a new directory *while sitting mid-history* (not at the
  newest entry) correctly truncates the abandoned forward entries, the
  same convention every browser uses — confirmed both that the branch
  landed correctly and that forward history is actually gone afterward,
  not just untested; `goUp()` at two different depths lands on the
  correct parent; and `goUp()` from the filesystem root is a safe
  no-op (stays at root) rather than erroring or producing a malformed
  path. `canGoBack()`/`canGoForward()` (added as public API specifically
  because they make this kind of precise, non-inferred verification
  possible, not only for the test's sake) are checked directly rather
  than inferred from navigation side effects.

  **A real bug reported after shipping, fixed and now covered directly:**
  on Windows, pressing Up from a top-level local folder (e.g.
  `C:/Users`) landed on the app's own working directory instead of the
  actual drive root `C:\`. Root cause: `parentOfPath("C:/Users")` was
  computing the bare string `"C:"` (no trailing slash) — which Windows
  interprets as "the process's current directory on drive C" (a legacy
  per-drive-working-directory quirk), not the drive's actual root. Fixed
  by special-casing a computed parent matching the `<letter>:` pattern
  and appending a trailing slash. Since this sandbox is Linux, a real
  `C:/Users` path can never resolve through the full
  `navigateTo()->LocalBackend->onDirectoryListed()` stack here — the only
  way to verify Windows drive-root behavior in this environment is
  calling the now-public `parentOfPath()` directly, which
  `navigation-test` now does explicitly (6 checks: the fix itself, the
  drive-root no-op case one level up, and three POSIX cases confirming
  the fix didn't disturb the already-working `/`-rooted behavior).

- **Pause/resume's orchestration logic is verified via a purpose-built
  fake backend, since the real byte-offset resume needs a live SFTP
  server this environment doesn't have.** `src/transfer_pause_test.cpp`
  (built via the `transfer-pause-test` CMake target) defines
  `FakePausableBackend` — a `RemoteBackend` implementation that simulates
  an interruptible async transfer via a `QTimer` ticking in fixed
  chunks, genuinely asynchronous (not one synchronous loop) so there's a
  real window between ticks for the test to call
  `pauseItem()`/`resumeItem()`, same as there'd be with a real backend
  on a real network. This is a legitimate test-double technique —
  `TransferManager` only ever talks to the `RemoteBackend` interface, so
  a fake honoring that interface's contract exercises the same
  orchestration code a real backend would. Confirms: pausing produces a
  genuine `Paused` status with real nonzero `bytesDone` (checked against
  the manager's own item list, not a local test variable); resuming
  produces a first `InProgress` update whose `bytesDone` is at or above
  the paused value — proving it continued rather than restarted from
  zero, the entire point of pause/resume over cancel/retry; and the
  transfer reaches `Done` with the full byte count afterward. Does
  **not** verify `SftpBackend`'s actual seek/clamp/no-truncate logic —
  see Known gaps.

- **Site Manager's new Group field renders in the right place,
  confirmed by pixel analysis, not assumed from the code alone.** A
  screenshot of `SiteManagerDialog` with two grouped sites was analyzed
  for the QLineEdit/QComboBox "surface" color (`#1a1d23`) — the first
  attempt used too loose a tolerance and matched the dialog's `#14171c`
  background as well (they differ by only ~6-7 per channel), giving a
  false single-block result; corrected to a tight tolerance and found
  exactly 6 distinct field-row bands — Site name, **Group**, Host, Port,
  Username, then a gap where the Authentication radios sit (no text
  field there), then Starting directory further down — matching the
  expected form structure exactly, with Group in the correct second
  position.

- **Pipelined SFTP I/O's mechanics are verified directly, and its
  real-world impact is now confirmed in two rounds.** Reported after
  use: ~4MB/s in this app versus ~40MB/s via FileZilla, Termius, and SMB
  on the same connection. Root-caused against independent external
  sources (libssh2's own issue tracker, the curl maintainer's own
  writeups on this exact problem) rather than guessed at — blocking-mode
  libssh2 sends one SFTP packet and waits for its ACK before sending the
  next, making round-trip time the throughput ceiling; real-world
  reports elsewhere of ~5-10x slowdowns from this exact cause line up
  with what was reported here. Fixed by pipelining reads/writes through
  a temporary non-blocking mode switch, modeled on libssh2's own
  canonical `sftp_write_nonblock.c` example rather than improvised.
  **Confirmed on a real connection: ~18MB/s after this fix — a real,
  measured 4.5x improvement.** Still short of the ~40MB/s comparison, so
  a second, more incremental round followed: buffer size aligned to an
  exact multiple of libssh2's actual 30000-byte internal packet cap
  (confirmed directly against libssh2's current source, not assumed to
  be 32KB), and `TCP_NODELAY` enabled on the connection. **This second
  round is also confirmed, not just theorized: ~22-25MB/s on a re-test —
  a further ~25-38% on top of the pipelining fix, ~5.5-6x improvement
  from the original ~4MB/s baseline overall.** What's directly verified
  about the pipelining mechanics themselves, separate from the
  throughput numbers above: a throwaway test (not committed — see its
  own reasoning below) confirmed the `ScopedNonBlocking` RAII guard
  actually flips `libssh2_session_get_blocking()` to non-blocking inside
  its scope and restores blocking mode afterward, through both a normal
  scope exit and — critically, since `uploadFile()`'s write-error path
  returns from inside the guard's scope — an early-return path too.
  **Still open:** the remaining gap to ~40MB/s (now roughly 1.6-1.8x
  rather than the original ~10x) — whether it reflects a deeper
  architectural difference between libssh2-based clients and more
  optimized/native SFTP implementations (cipher negotiation overhead,
  libssh2's own internal design) that further buffer/socket tuning can't
  close, or whether there's another concrete, fixable lever still to
  find, is unknown. No live SFTP server is available in this environment
  to investigate further — everything past this point would need to be
  chased with the same evidence-first approach used so far, not guessed
  at.

- **File management (delete/rename/create file/create folder) is
  verified against `LocalBackend`, real files, real temp directories —
  including the two cases that matter most for a destructive,
  no-undo feature.** `src/file_operations_test.cpp` (built via the
  `file-operations-test` CMake target) tests the backend directly rather
  than through `FilePaneWidget`'s right-click menu, since the menu's
  role is just prompting via `QInputDialog`/`QMessageBox` — dialogs that
  can't be driven headlessly — while the actual risk lives in the
  backend operations themselves. 15 checks confirm: creating a file
  produces a real empty file on disk; creating a file where one already
  exists is reported as a failure AND — checked explicitly, not assumed
  — the original content is untouched, not silently truncated; the same
  two checks for creating a folder; renaming a file and a folder both
  work and leave no trace of the old name; deleting a file and an empty
  folder both work; and **deleting a non-empty folder is reported as a
  failure, with the folder and its contents confirmed still present
  afterward** — proving the non-recursive-by-design behavior actually
  holds rather than silently degrading into a partial or accidental
  recursive delete. Also confirmed by a real screenshot (grabbed via
  `QApplication::activePopupWidget()` while the context menu's `exec()`
  call was still blocking, letting a `QTimer` fire during it) that the
  new menu items actually render, with a pixel-level check confirming
  the right number of distinct content rows. **Confirmed against a real
  server after this note was first written: all four operations work as
  intended.** That real test also surfaced a genuine bug, not a
  hypothetical one — creating a file that already existed reported the
  unhelpful "SFTP error 4" instead of a readable message, because the
  server returned `LIBSSH2_FX_FAILURE` (the protocol's generic
  "something went wrong" code) rather than the more specific
  `LIBSSH2_FX_FILE_ALREADY_EXISTS` that the original mapping handled.
  Fixed by giving `sftpErrorString()` a second, operation-specific
  `likelyReason` parameter for exactly this ambiguous case — see the
  `SftpBackend` architecture entry above for the full explanation. Not
  re-verified against a live server after that specific fix (the person
  who found the bug hasn't re-tested this exact scenario yet), but the
  fix compiles clean and the full existing regression suite still
  passes.

- **Whole-folder transfer is verified end-to-end against real nested
  directories, not just the individual pieces in isolation.**
  `src/folder_transfer_test.cpp` (built via the `folder-transfer-test`
  CMake target) builds a real multi-level directory tree — a root
  folder with a top-level file, a subdirectory with two files, a
  subdirectory containing a further-nested subdirectory with a file
  three levels deep, and a genuinely empty leaf directory (no files, no
  subdirectories inside it at all) — and calls
  `TransferManager::enqueueFolder()` against real `FilePaneWidget` +
  `LocalBackend` instances. 17 checks confirm: `folderTransferStarted`/
  `folderTransferFinished` fire with the right folder name at the right
  times; exactly 4 files (not 5 — directories correctly excluded from
  the count) get added to the visible transfer queue and all 4 reach
  `Done`; every directory gets created on the destination, including
  the three-levels-deep one (proving the walk doesn't stop after one
  level) and — the case most likely to be silently wrong in a buggy
  implementation — **the genuinely empty directory gets created despite
  contributing zero files anywhere in its own subtree**, rather than
  being skipped because nothing "needed" it; and every file's content
  is verified correct at every nesting depth, not just presence/absence.
  All 17 passed on the first real run. **Not verified:**
  `SftpBackend`'s side of the same walk (see the matching Known Gaps
  entry) — no live SFTP server is available in this environment.

- **Destination conflict resolution — Overwrite/Skip for files, Write
  Into/Skip for folders, and the "apply to all" remembered-decision
  mechanism — is verified by actually driving the real dialog, not a
  mock of one.** `src/conflict_resolution_test.cpp` (built via the
  `conflict-resolution-test` CMake target) schedules a `QTimer` to fire
  *during* `askConflict()`'s still-blocking `QMessageBox::exec()` call
  (`exec()` pumps the event loop internally, which is what makes this
  possible at all — the same technique already proven for capturing an
  open context menu earlier in this project's development), finds the
  live dialog via `QApplication::activeModalWidget()`, toggles its real
  checkbox, and clicks its real button by matching text. Four phases,
  all passing reliably (confirmed 5/5 clean runs, not just once): file
  conflict resolved Overwrite-with-apply-to-all (confirms the *second*
  conflicting file gets overwritten automatically with no second
  prompt, and that both files' content on disk actually changed, not
  just that their status reached Done); file conflict resolved
  Skip-with-apply-to-all in a **separate** batch (confirms both that the
  skip mechanism itself works and — a real, deliberately-checked
  claim, not assumed — that the remembered decision from the *previous*
  phase did NOT leak into this one once the queue had drained between
  them); folder conflict resolved Skip (confirms enumeration never even
  starts — `folderTransferStarted` never fires — since paying for a
  recursive walk of the source before knowing whether it'll be used at
  all would be wasteful); folder conflict resolved Write Into (confirms
  a genuine merge: the new file arrives, and a pre-existing, unrelated
  file already in that destination folder is left completely alone,
  not replaced).
  **Two real, non-hypothetical bugs surfaced and fixed while building
  this test, neither in the feature itself:** an *existing* test
  (`transfer-queue-test`) legitimately triggered a real conflict it
  wasn't written to expect — it deliberately re-transfers the same file
  to the same destination to test cancel-while-queued behavior, which
  now correctly produces a conflict prompt that a headless test can't
  click through, hanging the whole run. Fixed by removing the
  destination file first, keeping that test focused on what it's
  actually about (cancellation) rather than entangling it with conflict
  resolution, which has its own dedicated coverage. Separately, this
  new test itself initially failed intermittently in a way that looked
  like a logic bug but wasn't: `QApplication`'s default
  `quitOnLastWindowClosed` behavior was ending the test's event loop the
  moment the *first* dialog closed (it was effectively the only visible
  window, since the test's panes are never shown), and a later phase's
  timing was tight enough to occasionally miss — both root-caused by
  actually investigating (adding diagnostic output, confirming the
  failure was timing-sensitive rather than logical) rather than
  papering over with a longer sleep and hoping.
  **Not verified:** `SftpBackend`'s `checkExists()` against a real
  server — no live SFTP server is available in this environment, the
  same limitation already flagged for every other SFTP-specific path in
  this project.

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
  Also declares `deleteEntry`/`renameEntry`/`createDirectory`/`createFile`
  — file management, all "fire and refresh": on success each backend
  re-lists the current directory itself (reusing the existing
  `directoryListed` signal `FilePaneWidget` already handles), and on
  failure emits `fileOperationFailed(operation, path, reason)` rather
  than overloading `connectionFailed`/`transferFailed` for a
  conceptually different kind of error. `deleteEntry()`'s directory case
  is deliberately **not recursive** — both backends only remove an empty
  directory (plain POSIX `rmdir`/SFTP `RMDIR` semantics) and report a
  clear "not empty" failure rather than either silently no-op'ing or
  wiping out a whole tree; recursive delete is a meaningfully bigger,
  more dangerous feature that wasn't asked for.
  Also declares `listDirectoryForEnumeration(path, requestId)` +
  `directoryEnumerated`/`enumerationFailed` — deliberately separate from
  `listDirectory()`/`directoryListed`, which have the side effect of
  updating the pane's own `currentPath()` and driving its visible
  listing. Reusing that for a recursive folder-transfer walk would
  hijack the pane's display and corrupt its navigation history mid-walk
  as it stepped into each subdirectory — `requestId` lets a caller
  managing several outstanding enumeration requests (see
  `FolderEnumerator`) match responses back to requests.
  Also declares `checkExists(path, requestId)` + `existsChecked(path,
  exists, isDir, requestId)` — a lightweight existence check
  `TransferManager` uses to detect a destination conflict before
  starting a transfer or creating a directory, rather than the previous
  behavior of silently overwriting. `SftpBackend`'s implementation uses
  `libssh2_sftp_stat()` (signature confirmed against the installed
  header before use, same discipline as everywhere else in this class);
  a non-zero return is treated as "doesn't exist" rather than trying to
  distinguish a genuine not-found from other stat failures, since
  libssh2 doesn't give a reliable way to do that without walking the
  same ambiguous-error-code territory `sftpErrorString()` already has to
  navigate (see that helper's own comment on `LIBSSH2_FX_FAILURE`) —
  not worth the complexity for an existence check specifically.
- `LocalBackend` — wraps `QDir`/`QFile`. Runs on the GUI thread; local
  listing/copy is fast enough that this hasn't been a problem, but it's a
  design decision worth revisiting if it ever needs to handle slow
  network-mounted paths. File management uses `QDir::rename()` (not
  `QFile::rename()`, which isn't documented to reliably rename
  directories across platforms) for both files and folders, and checks
  `QFileInfo::exists()` explicitly before create/rename so "the name is
  already taken" is reported as a specific error rather than silently
  overwriting or falling through to a generic OS failure message.
- `SftpBackend` — wraps libssh2's synchronous API directly. Runs on a
  dedicated `QThread`, confirmed by the smoke test above. Its four
  operations (`connectToHost`, `listDirectory`, `downloadFile`,
  `uploadFile`) are declared `public slots:` in the `RemoteBackend` base
  class specifically so `QMetaObject::invokeMethod(..., Qt::QueuedConnection)`
  can reach them by name across the thread boundary — the base class's
  moc-generated slot thunk dispatches through the vtable, so the derived
  override still runs (Qt's "virtual slot" pattern).
  `downloadFile`/`uploadFile` take a `resumeOffset` parameter (0 for a
  fresh transfer) implementing real byte-offset resume, not just a
  cosmetic "paused" label: on resume, the local file is opened
  read/write and trimmed or clamped to match what's actually on disk
  (not blindly trusted — the local file could have changed between
  pause and resume) rather than reopened fresh, `libssh2_sftp_seek64()`
  positions the remote side, and — for uploads specifically —
  `LIBSSH2_FXF_TRUNC` is deliberately omitted on a resumed open, since
  truncating would destroy the bytes already uploaded before the pause.
  `requestPause()` follows the exact same thread-safe-flag pattern as
  `requestCancel()` (`QAtomicInteger<bool>`, polled inside the same
  read/write loop, both checked every iteration) — see `RemoteBackend`'s
  doc comment on why a queued signal can't do this job instead.
  The read/write loop itself is pipelined, not blocking-and-serial: a
  `ScopedNonBlocking` RAII guard (file-local to `SftpBackend.cpp`)
  temporarily switches the session to non-blocking mode for just that
  loop, restoring blocking mode on scope exit regardless of which return
  path was taken (verified directly — see Verification status). This
  fixes a well-documented libssh2 characteristic: in blocking mode,
  libssh2 sends one SFTP packet and waits for its ACK before sending the
  next, so round-trip time rather than bandwidth becomes the throughput
  ceiling — reported in this app as ~4MB/s versus ~40MB/s via
  FileZilla/Termius/SMB on the same connection, in line with real-world
  reports elsewhere of roughly 5-10x slowdowns from this exact cause.
  Confirmed on a real connection after this fix: ~18MB/s, a real 4.5x
  improvement, though still short of the ~40MB/s comparison. `EAGAIN`
  retries wait via `libssh2_poll()` (not raw `select()`/`fd_set`)
  specifically for portability — `select()` needs different headers on
  POSIX vs. Windows (`sys/select.h` vs. `winsock2.h`), the exact category
  of mistake that broke the first real Windows build of this app;
  `libssh2_poll()` handles that gap internally.
  Two further, more incremental tuning changes followed once the
  remaining gap was reported: the read/write buffer is now exactly
  480000 bytes (16 x 30000) rather than an arbitrary 256KB — libssh2's
  actual internal SFTP packet size is hardcoded to exactly 30000 bytes
  (confirmed directly against libssh2's own current source,
  `MAX_SFTP_READ_SIZE`/`MAX_SFTP_OUTGOING_SIZE` in `sftp.h` — not 32KB,
  and not something callers can change), and a buffer that isn't an
  exact multiple leaves a small "leftover" packet under that cap every
  cycle, which doesn't fully use the pipeline — independently reported
  elsewhere as costing roughly 20% from this exact misalignment alone.
  Separately, `QAbstractSocket::LowDelayOption` (Qt's portable
  `TCP_NODELAY` equivalent) is now set on the connection — Qt doesn't
  disable Nagle's algorithm by default, and Nagle's small-write batching
  behavior works directly against a protocol now issuing many pipelined
  small reads/writes, each one waiting on a response. **Confirmed on a
  re-test: ~22-25MB/s** — a further ~25-38% on top of the pipelining
  fix's ~18MB/s, roughly 5.5-6x improvement from the original ~4MB/s
  baseline overall.
  **Honest framing on the remaining gap:** every lever applied so far has
  been evidence-based and each has moved the number, confirmed on a real
  connection both times — but there may still be a gap between
  libssh2-based clients and OpenSSH-derived or other native SFTP
  implementations that further buffer/socket tuning can't fully close
  (cipher negotiation overhead and libssh2's own internal architecture
  relative to more optimized implementations both came up in the
  research behind this). The remaining gap to ~40MB/s is now roughly
  1.6-1.8x rather than the original ~10x; whether it's closeable at all
  from here, and if so how, is unknown — would need to be chased with
  the same evidence-first approach used so far, not guessed at.
  File management (`deleteEntry`/`renameEntry`/`createDirectory`/
  `createFile`) is built on `libssh2_sftp_unlink`/`rmdir`/`rename`/
  `mkdir`/`open` — every signature confirmed directly against the
  installed `libssh2_sftp.h` before use, same discipline as everywhere
  else in this class. `createFile()` uses `LIBSSH2_FXF_EXCL` specifically
  so it fails rather than silently truncating something already at that
  path. Error reporting for these four uses a `sftpErrorString()` helper
  — `libssh2_sftp_last_error()` only returns a numeric `LIBSSH2_FX_*`
  status code, with no built-in string conversion, so this maps the
  codes actually plausible here (not-found, permission denied,
  already-exists, not-empty, etc. — confirmed against the full
  `LIBSSH2_FX_*` list in the header) to something a person reads
  directly. **Real-world finding, not theoretical:** after testing
  against a live server, creating a file that already existed surfaced
  as the unhelpful "SFTP error 4" instead of a readable message — the
  server had returned `LIBSSH2_FX_FAILURE` (4), the protocol's own
  generic "something went wrong, no detail given" code, rather than the
  more specific `LIBSSH2_FX_FILE_ALREADY_EXISTS` (11) that would have
  made this case self-explanatory through the existing mapping. Real
  SFTP servers commonly do this — return the generic code instead of a
  specific one. Since a generic code genuinely doesn't reveal the actual
  cause, `sftpErrorString()` now takes a second `likelyReason` argument
  that each of the four call sites supplies with an operation-specific,
  plainly-hedged guess ("may mean X, or you may not have permission to Y")
  used only for that ambiguous case — not asserted as certain, since it
  isn't. Any other, genuinely unmapped code still falls back to the raw
  number rather than guessing at those too.
- `ConnectionDialog` — host/port/username form plus a password/private-key
  auth toggle (`QStackedWidget` swaps the relevant fields). Returns a
  single `SftpCredentials` struct (`src/backends/SftpCredentials.h`) that
  `SftpBackend` consumes directly — no more unpacking/repacking individual
  fields at the call site. Still the "one-off connection" path — kept
  deliberately unchanged when Site Manager was added (see below) rather
  than risking regressing an already-verified flow; `MainWindow::startConnection()`
  is the shared code both paths funnel through afterward.
- `SavedSite` / `SiteStore` (`src/backends/SavedSite.h/.cpp`) — a saved
  connection profile (host/port/username/auth method/key path, optional
  starting directory, optionally grouped into a folder) and its JSON
  persistence (`QStandardPaths::AppConfigLocation/sites.json`).
  **Deliberately has no password field, full stop** — not "encrypted,"
  not "obfuscated," simply never collected for storage. This is
  stricter than the source design mockup, which showed a "Password"
  logon type implying stored credentials; overridden on purpose, since
  shipping plaintext credential storage without being explicitly asked
  to would cut against the security hygiene the rest of this app has
  been built with (host-key TOFU, no silent trust). The same
  no-storage rule extends to a private key's passphrase, for
  consistency, even though a passphrase's risk profile (protects a key
  file already under OS permissions) differs from a bare password's.
  `useHomeDirectory`/`startingDirectory` (both mirrored onto
  `SftpCredentials`, consumed by `SftpBackend::ensureSession()`) let a
  site skip the default home-directory resolution and land somewhere
  specific instead — not validated at save time; an invalid path
  surfaces through the same `listDirectory()`/`connectionFailed` error
  path as typing a bad path into the pane's own path bar.
- `SiteManagerDialog` — the saved-sites UI: a grouped tree on the left,
  a details form on the right, matching the design package's
  site-manager.html mockup, plus a starting-directory radio choice
  (Home / Specific) the mockup didn't have. Persists via `SiteStore` on
  every field edit (`QLineEdit::editingFinished`, not per-keystroke) and
  every structural change (new/duplicate/delete), so there's no separate
  "Save" step to forget. Its Connect button prompts for the password or
  key passphrase fresh every time, regardless of what's saved — see
  `SavedSite` above. Groups are organized via an editable `QComboBox`
  (`m_groupCombo`) next to the site name — pick an existing group from
  the dropdown or type a new one to create it on the spot; there's no
  separate "groups" collection to manage, a group exists precisely when
  at least one site references it. Changing a site's group triggers a
  full `rebuildTree()` (hierarchy actually changed) rather than the
  simple in-place item-text update every other field edit uses.
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
  Selected" (multi-select, via `filesActivated` — carries full
  `RemoteEntry`, not bare names, specifically so `isDir` survives to
  `MainWindow`'s routing between `TransferManager::enqueue()` (files) and
  `enqueueFolder()` (directories); a mixed selection of both is valid
  and each entry is routed individually), on top of the original
  double-click-one-file behavior (`fileActivated`, files only — double-
  click on a directory means "navigate into it", a different action
  entirely). The same context menu
  also has New File/New Folder (prompt for a name via `QInputDialog`,
  always enabled), Rename (prompts pre-filled with the current name,
  enabled only for a single selection — renaming several items to one
  name doesn't mean anything), Delete (works on any mix of selected files
  and folders, confirms via `QMessageBox` first, dispatches one
  `deleteEntry()` call per item — Qt's queued-connection ordering means
  each one's own internal refresh completes before the next deletion
  starts, so there's no race between them), and Refresh. All four new
  operations funnel through `onFileOperationFailed()`, connected to the
  backend's `fileOperationFailed` signal in `setBackend()`, which shows
  the failure as a message box — there's no persistent queue/log for
  these one-shot operations the way there is for transfers.
  Back/forward/up navigation lives here too: a per-pane `QStringList`
  history plus an index, updated only from `onDirectoryListed()` — i.e.
  only once a navigation is *confirmed successful* — rather than eagerly
  when `navigateTo()` is called, so a failed navigation (bad path) never
  becomes a history entry. A `m_navigatingHistory` guard distinguishes a
  `goBack()`/`goForward()`-triggered listing (just moves the index) from
  any other navigation (pushes a new entry, truncating whatever "forward"
  entries existed past the current position — the same convention every
  browser uses). `goUp()` computes the parent via `parentOfPath()` (a
  public static helper — deliberately public so its Windows drive-root
  special case can be tested directly, since a real `C:/...` path can't
  resolve through this Linux sandbox's actual filesystem) using `/` as
  the separator — correct for SFTP paths always (the protocol mandates
  forward slashes regardless of the server's OS) and for local paths too
  under Qt's own convention (`QDir`/`QFileInfo` normalize to `/` even on
  Windows). A computed parent matching the bare `<letter>:` pattern
  (e.g. `"C:"`) gets an explicit trailing slash appended — Windows
  interprets `"C:"` without one as "the process's current directory on
  drive C" rather than the drive's actual root, which caused a real
  reported bug (Up from a top-level folder landing on the app's own
  working directory instead of `C:\`) before this was added. Otherwise a
  safe no-op at any kind of root rather than erroring or producing a
  wrong path.
  `resetHistory()` clears all of this on every `setBackend()` call — a
  new backend (Connect/Disconnect) is a fresh navigation context, not a
  continuation of the old one's history.
- `FileTreeView` — thin `QTreeView` subclass adding cross-pane
  drag-and-drop. Qt's built-in item-view DnD pulls its `QMimeData` from
  the *model* (`QAbstractItemView::startDrag()` calls
  `model()->mimeData(...)`), which is built for reordering within one
  model — it doesn't fit "drag from one pane's tree onto a different
  pane's tree", so drag start (`startDrag()`) and drop handling
  (`dragEnterEvent`/`dropEvent`) are both overridden here instead. The
  dragged `QMimeData` carries the source `FilePaneWidget*` as a raw
  `quintptr` — same-process-only, never meant to leave this app, a
  legitimate technique for internal Qt drag-and-drop — alongside the
  selected entries encoded as `"<0 or 1>\t<name>"` per line, the leading
  flag being `isDir`, needed since whole folders can be dragged now, not
  just files.
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
  `pauseItem()`/`resumeItem()` follow the same `requestPause()` pattern as
  cancel, but need no equivalent disambiguating flag — `transferPaused` is
  its own signal, unambiguous the moment it arrives, unlike
  `transferFailed` which both cancellation and genuine errors produce.
  `resumeItem()` deliberately does NOT reset `bytesDone` the way
  `retryItem()` resets it to zero — that value is exactly the resume
  offset `startNext()` passes through to the backend on the next run.
  Live speed (`TransferItem::speedBytesPerSec`) is sampled roughly every
  250ms in `onBackendProgress()` (via `QElapsedTimer`) rather than on
  every single progress signal, which for SFTP's 32KB-chunk read/write
  loop would be far too frequent to read as a stable "live" number.
  `enqueueFolder()` handles whole-folder transfer: enumerates the source
  folder via `FolderEnumerator` (see below), creates the mirrored
  directory structure on the destination, then hands every discovered
  file to the ordinary `enqueue()` above — a genuinely useful design
  outcome discovered while building this, not planned in advance:
  `enqueue()`'s existing `joinPath(pane->currentDirectory(), fileName)`
  logic already produces the correct nested path when `fileName` is
  actually a relative path like `"photos/subdir/photo.jpg"`, so folder
  transfer needed no separate file-transfer mechanism at all — every
  file discovered by the walk rides through the exact same queue,
  pause/resume/cancel, and speed tracking as any other transfer, for
  free. Directory creation calls are fired in `FolderEnumerator`'s
  guaranteed parent-before-child order via
  `QMetaObject::invokeMethod(..., Qt::QueuedConnection)` without waiting
  for each one's individual completion — Qt's per-target-object FIFO
  ordering on queued connections is what guarantees the destination
  backend processes them in that exact sequence regardless of how long
  each individual creation takes, so there's nothing to wait on.
  **Conflict resolution** happens in two places, deliberately different
  in scope. Files: `startNext()` calls `checkExists()` on the
  destination right before an item would actually start (no visible
  status change yet), and only calls the new `dispatchActiveItem()` —
  the actual backend dispatch, split out from what used to be
  `startNext()`'s own body — once that comes back clean or the person
  chooses Overwrite. Folders: `enqueueFolder()` checks the root folder's
  own existence *before* paying for a full recursive enumeration of the
  source at all — if it's going to be skipped, there's no reason to walk
  the source tree first. Choosing Write Into does **not** check every
  nested subdirectory individually; it means exactly what it says — merge
  into whatever's already there, the same way most file managers handle
  copying a folder onto an existing one — while files found inside still
  get their own individual conflict check as their turn comes up in the
  ordinary queue. Both flows share one `askConflict()` dialog (a
  `QMessageBox` with a custom checkbox via `setCheckBox()`) and one
  `ConflictResolution` enum (`Ask`/`AlwaysOverwrite`/`AlwaysSkip`), but
  file and directory decisions are tracked as two **independent**
  members (`m_fileConflictResolution`/`m_directoryConflictResolution`),
  matching the two separate checkboxes/decisions a person actually gets
  asked about — checking "apply to all" for files doesn't silently also
  answer a folder conflict. Both reset back to `Ask` whenever the queue
  fully drains (the existing "nothing left to run" path in `startNext()`),
  so a fresh batch of transfers gets fresh decisions rather than quietly
  inheriting a choice from an unrelated earlier transfer.
  A subtlety worth knowing if this code is touched again: the file- and
  folder-conflict checks use two *separate* pending-request-id trackers
  (`m_pendingFileConflictCheckId`/`m_pendingFolderConflictCheckId`)
  rather than one shared one, because they can genuinely overlap —
  `askConflict()`'s `QMessageBox::exec()` is modal but still pumps the
  event loop internally (that's what makes a modal dialog work at all),
  so a brand-new drag-and-drop can trigger `enqueueFolder()` while an
  earlier conflict dialog from a different check is still on screen. The
  shared `onDestinationExistsChecked()` slot routes each response by
  checking which of the two ids it matches, so the two flows can't
  corrupt each other's state even when interleaved. Relatedly,
  `ensureExistsCheckConnected()` connects a backend's `existsChecked`
  signal via `Qt::UniqueConnection` and is **never explicitly
  disconnected** — unlike `connectToBackend()`'s progress/finished/
  failed wiring, which does need teardown between transfers to avoid
  double-delivery, `existsChecked`'s `requestId` already disambiguates
  responses on its own, so there's no double-delivery risk to avoid, and
  tearing the connection down here would risk losing a response if two
  different backends both have checks in flight at once.
- `FolderEnumerator` (`src/transfer/FolderEnumerator.h/.cpp`) —
  recursively walks a folder via a backend's
  `listDirectoryForEnumeration()`, one directory at a time (not several
  concurrent requests: a real `SftpBackend` has exactly one session, so
  concurrent calls would just serialize through libssh2 anyway, and
  `LocalBackend` gains nothing from parallel `QDir` reads either — simple
  and strictly ordered beats a concurrency scheme that wouldn't actually
  buy anything). Emits a flat `QList<EnumeratedItem>` manifest on
  `finished()`, each entry's `relativePath` already reading as "the path
  this item should have under wherever the caller intends to recreate
  the folder" (seeded with the root folder's own name, so
  `TransferManager::enqueueFolder()` doesn't need to do any path
  rewriting itself). Any enumeration failure aborts the whole walk
  rather than skipping the failed subdirectory and continuing — silently
  completing with missing content would look successful while actually
  being wrong, which is worse than clearly failing.
- `TransferQueueWidget` — table view mirroring `TransferManager`'s
  `itemAdded`/`itemUpdated` signals, plus a right-click context menu
  (Cancel/Pause/Resume/Retry, each enabled based on the item's current
  status — Pause additionally checks the item's direction, since
  `LocalBackend`'s `requestPause()` is a documented no-op and offering
  Pause for a local-to-local transfer would just silently do nothing)
  that calls straight back into the manager. `TransferStatus::Skipped`
  (a conflict resolved as "skip") shares `Cancelled`'s icon and muted
  gray color — both mean "didn't happen, not an error" — distinguished
  only by the status text next to it, rather than inventing a fifth
  accent color the design's four-color system doesn't really have room
  for. Retry is enabled for Skipped the same as Failed/Cancelled — a
  skip applied automatically via "apply to all" is still worth being
  able to reverse for one specific item. A Speed column shows
  `TransferManager`'s sampled rate, formatted B/s -> KB/s -> MB/s. All
  state still lives in the manager — this is a view, not a second source
  of truth.
- `MainWindow` — two `FilePaneWidget`s in a `QSplitter`, plus a
  `QDockWidget` at the bottom holding the transfer queue. Left pane is
  always `LocalBackend`. Right pane starts on `LocalBackend`; the
  toolbar's "Connect..." action (via `ConnectionDialog`) and "Sites..."
  action (via `SiteManagerDialog`) both funnel into a single
  `startConnection(const SftpCredentials &)` — spins up an `SftpBackend`
  + worker `QThread` and hands it to the right pane — rather than
  duplicating that setup in two places. "Disconnect" swaps back to
  `LocalBackend`. Double-clicking a file in either pane calls
  `TransferManager::enqueue()` with that pane as source and the other as
  destination; `TransferManager::transferSucceeded` triggers a refresh of
  both panes' listings.

## Design system

The dark theme and icon set come from a design package (not included in
this repo's history prior to this pass) built around Tabler Icons (MIT)
and a fixed four-color semantic system — full rationale in that package's
own README and `ICON-MAP.md`. The first pass porting this theme was
scoped to a **visual re-skin of existing features only**; Site Manager
(saved connections, including its group organization), pause/resume for
transfers, and live transfer speed — all shown in the original mockups —
were added in later passes and are covered in the Architecture section
above.

- `resources/icons/*.svg` — 28 vendored Tabler Icons SVGs (27 in-app UI
  icons — 18 from the original theming pass, `server-cog`/`folder-plus`/
  `copy`/`trash` added for Site Manager, `arrow-left`/`arrow-right`/
  `corner-left-up` added for pane navigation, `player-pause`/`player-play`
  added for transfer pause/resume — plus `wind.svg`, used only as the
  app icon's source glyph — see below), fetched directly from
  `github.com/tabler/tabler-icons` (MIT
  license included as `resources/icons/LICENSE-tabler-icons.txt`). Each
  uses `stroke="currentColor"`, deliberately not pre-colored — recolored
  at render time instead (see `IconTheme` below), so there's one source
  file per icon regardless of how many accent colors it's used with.
- **Application icon** (`resources/icons/app-icon-*.png`,
  `resources/icons/app-icon.ico`) — a composed badge (a deep
  indigo-to-blue-violet "night sky" gradient — `#1e1b4b` to `#433884`,
  deliberately distinct from `IconTheme::Blue`'s bright daytime blue,
  which is a separate in-app semantic color with its own reason for
  staying legible against the app's dark UI — with the Tabler "wind"
  glyph in white, centered), not a bare Tabler icon, since a
  single-color line-art glyph on a transparent background doesn't read
  as an app icon at any size. Generated by `tools/generate_app_icon.cpp`
  (source + regeneration instructions in `tools/README.md`), which
  renders **each target size independently** with a tuned stroke width
  and glyph-to-badge ratio, rather than downsampling one large master —
  a shared 512px master downsampled to 16px lost the glyph almost
  entirely (confirmed by direct pixel count: 2 legible white pixels out
  of 256) before this was corrected; per-size rendering brought that to
  33 (31 after the badge color was later changed to the night-sky
  gradient — re-verified at the time, not assumed to still hold). The
  `.ico`'s per-size frames were verified pixel-identical to the
  independently-rendered PNGs, confirming Pillow's ICO writer used the
  supplied frames rather than silently re-downsampling internally.
  Wired in twice, since these are two separate things on Windows: the
  `.ico` is embedded as the compiled `.exe`'s native icon via
  `resources/app-icon.rc` (what File Explorer and the taskbar show
  *before* the app runs — `WIN32`-guarded in `CMakeLists.txt`, a clean
  no-op on Linux), and the PNGs are loaded into a multi-resolution
  `QIcon` via `QApplication::setWindowIcon()` in `main.cpp` (the runtime
  window/taskbar icon on all platforms, Qt picks whichever size fits the
  context rather than scaling one image).
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

- **`SftpBackend::checkExists()` — the primitive destination-conflict
  detection depends on — is unverified against a real server.**
  `LocalBackend`'s implementation is thoroughly tested (see
  `conflict-resolution-test` in Verification status), but no live SFTP
  server is available in this environment. It's built on
  `libssh2_sftp_stat()`, the same underlying call other already-working
  SFTP paths in this codebase rely on for similar purposes, just routed
  differently — so the protocol operation itself is proven, but "does a
  conflict check actually work against a live server, including the
  ambiguous-stat-failure fallback treating any error as 'doesn't
  exist'" hasn't been tried.
- **Directory deletion is never recursive, on either backend, by
  design.** Deleting a non-empty folder fails with a clear error rather
  than removing its contents first. This wasn't an oversight or a
  missing feature so much as a deliberate scope decision — recursive
  delete is a meaningfully bigger, more dangerous feature (a bug there
  could delete far more than intended, with no undo) than what was
  actually asked for. Worth revisiting explicitly if "delete this folder
  and everything in it" turns out to be something people actually want.
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
- **`SftpBackend`'s `listDirectoryForEnumeration()` — the primitive whole-
  folder transfer's recursive walk depends on — is unverified against a
  real server.** `LocalBackend`'s implementation is thoroughly tested
  (see the folder-transfer-test entry in Verification status), including
  the trickiest cases (multi-level nesting, a genuinely empty leaf
  directory), but no live SFTP server is available in this environment.
  The libssh2 calls it's built on (`libssh2_sftp_opendir`/`readdir`) are
  the same ones `listDirectory()` already uses successfully against a
  real server, just routed to a different signal — so the underlying
  protocol operations are proven, but "does a recursive walk over SFTP
  actually work end-to-end" hasn't been tried.
- **Cancel and pause/resume are implemented but only automated-tested
  against a fake backend and against `LocalBackend`, where both are a
  documented no-op** (`QFile::copy()` can't be interrupted mid-call).
  `SftpBackend`'s actual mid-transfer interruption — the cancel and pause
  flags checked inside the libssh2 read/write loops — and its real
  byte-offset resume logic (the `libssh2_sftp_seek64()` calls, the
  local-file clamping/trimming against what's actually on disk) compile
  and follow patterns proven correct elsewhere in this codebase, but
  none of it has a real-server test confirming it actually works —
  `transfer-pause-test` verifies `TransferManager`'s orchestration
  (status transitions, offset preservation, resume-not-restart) using a
  fake backend built for exactly that purpose, which is a real and
  useful thing to have verified, but is a different claim than "resuming
  a paused SFTP upload actually picks up where it left off on a live
  server." Needs a slow-enough real transfer, paused and resumed by
  hand, to verify that specific claim.
- **The remaining ~1.6-1.8x gap to the ~40MB/s comparison
  (FileZilla/Termius/SMB) is unexplored, not just unverified.** Three
  rounds of evidence-based tuning (pipelining, buffer alignment to
  libssh2's exact 30000-byte packet size, `TCP_NODELAY`) each confirmed
  a real improvement on a real connection — ~4MB/s to ~18MB/s to
  ~22-25MB/s — but none of the sources consulted so far point to a
  specific next lever with the same confidence as the first three. Two
  candidates surfaced in the research without being investigated
  further: cipher/MAC negotiation overhead (libssh2's default algorithm
  preferences vs. what other tools negotiate — `libssh2_session_method_pref()`
  exists to influence this but hasn't been touched), and the possibility
  that libssh2's own internal architecture has a ceiling below more
  optimized/native SFTP implementations that no amount of buffer/socket
  tuning reaches. No live SFTP server is available in this environment
  to investigate either — whatever's tried next should be chased with
  the same evidence-first approach used so far, not guessed at.
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
