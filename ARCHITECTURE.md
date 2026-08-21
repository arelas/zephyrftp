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
  **The transfer queue's colored progress bars and status icons are now
  confirmed too, with a real active transfer, not an empty queue.** A
  throwaway screenshot harness (same `QWidget::grab()` technique, not
  kept in the repo — see the project's established pattern of one-off
  visual checks) drove a real `TransferManager`/`TransferQueueWidget`
  through a real upload against the local SFTP test server
  (`tools/local-test-servers/`), grabbing the widget mid-transfer and
  again on completion. Confirmed genuinely correct: the green progress
  bar chunk fills proportionally to real progress (~35% at the point
  captured, matching the actual `bytesDone`/`bytesTotal` at that
  moment), the upload direction icon and blue "Transferring" status
  text render correctly, the Speed column shows a real live-sampled
  rate ("152.0 MB/s"), and on completion the direction icon switches to
  a green checkmark, the status text turns "Done," the bar fills fully,
  and the Speed column correctly goes blank (not meaningful once
  finished — matches `TransferItem`'s own doc comment on that field).
  Getting a clean single-row screenshot at all surfaced two real bugs
  in the throwaway harness itself, not the app: `FilePaneWidget::setBackend()`
  fires its own automatic `connectToHost()` and an initial
  home-directory listing, which raced ahead of the harness's own
  sequencing and had to be gated on each pane's own `directoryListed`
  for the specific path expected, not a guessed delay; and a stale
  partial file left on the server by an earlier interrupted run
  produced a real, correctly-detected destination conflict — a good
  sign for `TransferManager`'s conflict detection, not a harness flaw,
  once understood, but the immediate cause of an early confusing
  result.

- **Site Manager persists correctly, and the security property that
  matters most about it (no password ever hits disk) is verified, not
  assumed.** `src/site_store_test.cpp` (built via the `site-store-test`
  CMake target, isolated from any real saved sites via
  `QStandardPaths::setTestModeEnabled(true)` — see its own header
  comment for the real cross-platform isolation bug this fixed)
  round-trips a password-auth and a key-auth site through
  `SiteStore::save()`/`load()` and confirms every
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

- **The opt-in "Save password" checkbox is verified end to end against
  the real OS credential store on Linux, and the fact that `sites.json`
  still carries zero secrets even when it's used is re-confirmed
  directly, not just assumed to still hold.** A throwaway harness (not
  committed — matches this project's established pattern for one-off
  visual/functional checks) drove a real `SiteManagerDialog` through
  its actual UI (tree selection, the real checkbox, the real Connect
  button, the real password `QInputDialog` — driven the same way
  `conflict_resolution_test.cpp` drives a live `QMessageBox`, via a
  `QTimer` firing during the dialog's still-blocking `exec()`) across
  three phases: checking the box and connecting saves the typed
  password for real (confirmed via `CredentialStore::load()` returning
  the exact value); reopening a fresh dialog instance shows the
  checkbox auto-checked (reflecting real stored state, not a flag in
  `sites.json` — there isn't one) and the prompt pre-filled with the
  stored password; editing that pre-filled value and reconnecting
  genuinely overwrites the stored secret (the update path, not a
  separate one); and unchecking the box removes the stored secret
  immediately, no Connect click needed. Separately confirmed by
  inspecting the raw `sites.json` content directly after the full
  save/update flow (isolated via `QStandardPaths::setTestModeEnabled()`,
  same as `site-store-test`): still zero `password`/`passphrase` keys,
  exactly as before this feature existed. **The Windows side is now confirmed
  too, on real hardware, not just compiled/linked under `wine`.**
  `CredentialStore`'s `wincred.h` backend (`CredWriteW`/`CredReadW`/
  `CredDeleteW`) has been manually confirmed saving and reloading a real
  password through the real Windows Credential Manager on an actual
  Windows machine — not just running without crashing under `wine`,
  which only ever proved the code path executes, not that Windows'
  own credential store actually round-trips the secret correctly.

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
  fake backend.** (The real byte-offset resume against `SftpBackend`
  itself is now separately confirmed against a live server too — see
  the cancel/pause/resume entry in Known gaps below; this test predates
  that and still earns its keep as the orchestration-level check,
  independent of any real network.) `src/transfer_pause_test.cpp`
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
  **not** itself verify `SftpBackend`'s actual seek/clamp/no-truncate
  logic against a real server — see Known gaps for where that's
  covered instead.

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
  **Update: the remaining ~1.6-1.8x gap does NOT reproduce against a
  real, non-loopback server, and is now confirmed closed rather than
  left open.** `src/verify_sftp_throughput.cpp` (new; run manually via
  `verify-sftp-throughput`, env-var-configured host/user/password since
  it needs a real externally-provided server this project can't spin up
  a container for) measured this app's own `SftpBackend` against a real
  remote server (~6-7ms round-trip time, not loopback) alongside an
  independent baseline: OpenSSH's own `scp` client, same server, same
  file, timed the same way — not a years-old number from a different
  network, but a same-link, same-moment comparison. Result, confirmed
  across two runs at two file sizes (100MB and 300MB): ZephyrFTP's
  SftpBackend reached ~29-31MB/s upload and ~37-39MB/s download, against
  `scp`'s own ~29-36MB/s upload and ~35-37MB/s download on the identical
  link — a ratio of ~0.93-1.13x (`scp` sometimes slightly faster, this
  app sometimes slightly faster), i.e. real parity, not the previously-
  reported 1.6-1.8x gap. Since `scp`/`sftp` are themselves libssh2-free,
  natively-optimized reference implementations of this exact protocol,
  matching them directly rather than falling meaningfully short is strong
  evidence there's no further fixable client-side bottleneck left in this
  app's own code. The likeliest explanation for the originally-reported
  gap: it was measured against FileZilla/Termius/SMB on a different
  network at a different time — SMB in particular isn't even the same
  transport/protocol family as SFTP, so it was never a fully apples-to-
  apples comparison to begin with, and the other network's real-world
  RTT/congestion characteristics were never controlled for. No further
  code change made here — the honest result is "parity confirmed," not
  "bug found."

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
  All 17 passed on the first real run. This test only ever exercised
  `LocalBackend`; `SftpBackend`'s side of the same walk is now also
  confirmed against a real server — see the `FolderEnumerator`/
  `listDirectoryForEnumeration()` entry in Known gaps below.

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
  This test only ever exercised `LocalBackend`'s side of conflict
  detection; `SftpBackend`'s `checkExists()` is now separately confirmed
  against a real server — see the matching Known gaps entry below.
  **A third bug, this time in the test's own hygiene, surfaced later
  (v0.7.21) by repeated local runs rather than CI**: `/tmp/conflict_test`
  was never wiped before each run, unlike its sibling tests
  (`transfer_queue_test.cpp`/`folder_transfer_test.cpp`, both of which
  do wipe their own `/tmp` scratch dirs). After enough repeated local
  runs left a stale `mergedir/f.txt` behind from an earlier run, Phase
  D's own per-file `checkExists()` correctly detected a genuine
  conflict for it (the file really did already exist) that Phase D's
  own test logic never accounts for (it only clicks the folder-level
  "Write Into" dialog) — popping a second, never-clicked dialog that
  Phase E's later flood-and-click logic then found as the "active
  modal" and clicked instead of `reentrancy.txt`'s own dialog, leaving
  it blocked until the 20-second safety net. Always passed on CI (a
  fresh container has no leftover state) but failed consistently
  locally once state had accumulated — root-caused by adding temporary
  instrumentation (per-item id/status/timestamp logging, both in the
  test and directly in `TransferManager::onDestinationExistsChecked()`)
  rather than guessing from the symptom alone, then reverted once the
  cause was confirmed. Fixed with the same `QDir(base).removeRecursively()`
  its siblings already use; confirmed 5/5 clean runs afterward.
  **A fourth, real, live-reported bug — a genuine dialog-stacking
  reentrancy, fixed (Phase F):** "Write Into/Overwrite is asked for
  every top-level dir. The first choice made should be honored for the
  rest of the transfer." `MainWindow::enqueueEntries()` calls
  `enqueueFolder()` for every dropped top-level folder in one
  synchronous loop, queuing all their `checkExists()` calls together
  before any reply arrives. `askConflict()`'s `QMessageBox::exec()`
  pumps the WHOLE app's event queue while its own dialog is still
  open — including a SECOND folder's own already-queued `checkExists()`
  reply. Since that reply's own `onDestinationExistsChecked()` call ran
  with `m_directoryConflictResolution` still at `Ask` (the first
  dialog hadn't been answered yet), it opened its OWN independent
  dialog, stacked on top of the first, with no way for the person's
  first answer — even "apply to all" — to reach a dialog that had
  already opened before that answer was given. Fixed with a new
  `m_conflictDialogInProgress` guard set for the duration of every
  `askConflict()` call: any `onDestinationExistsChecked()` invocation
  that arrives while it's set is stashed (`m_deferredExistsChecks`)
  instead of processed immediately, then replayed in original order via
  `drainDeferredExistsChecks()` — called from each of the three call
  sites (folder root, Move root, ordinary file) AFTER that branch's own
  resolution state is updated from `applyToAll`, not from inside
  `askConflict()` itself (an earlier draft of this fix drained too
  early, before the caller had applied the checkbox's answer, and the
  replayed check saw the stale `Ask` state and popped its own dialog
  anyway — caught by the test below, not assumed fixed). At most one
  dialog is ever open at a time now, sequential rather than stacked.
  New Phase F (`src/conflict_resolution_test.cpp`) reproduces this
  deterministically — not a timing gamble: it fires `enqueueFolder()`
  for two top-level folders back-to-back, synchronously, matching
  `enqueueEntries()`'s own real loop exactly, then confirms no second
  dialog ever appears for the second folder and that its file still
  transfers correctly (the remembered "apply to all, Write Into"
  choice genuinely applied, not just silently skipped). Proved
  non-vacuous via `git stash` on just the `TransferManager` fix,
  rebuild, rerun: the pre-fix code genuinely fails (a second dialog
  opens, and the first folder's own file never transfers because the
  wrong dialog gets clicked); restoring the fix passes cleanly again.
  A separate, narrower related bug fixed alongside this one:
  `startNext()`'s "nothing left to run" reset of
  `m_fileConflictResolution`/`m_directoryConflictResolution` only ever
  checked `m_active.isEmpty()`, not whether a folder/file root-conflict
  check was still in flight (`m_pendingFolderConflictChecks`/
  `m_pendingFileConflictChecks`) — a SEPARATE, sequential (not
  reentrant) scenario where one dropped folder's own transfer finishes
  fast enough to fully drain `m_active` while a LATER dropped folder's
  own root-conflict check is still outstanding would have silently
  discarded the remembered choice before that later folder's own prompt
  could benefit from it. Same class of premature-reset bug Move already
  hit and fixed for its own separate resolution state — see
  `maybeResetMoveConflictResolution()`'s own comment.
  **A fifth, real, live-reported bug — a genuine Windows crash, fixed
  (Phase G/H):** a real user testing the Phase F fix above hit a
  `STATUS_STACK_OVERFLOW` (confirmed from an actual Windows Application
  Error / WER event log, not a hypothetical) after choosing Write Into —
  the app spawned "a large number of 'Create folder' dialogs" and then
  crashed. Root cause, in two parts:
  1. **The actual trigger**: `TransferManager::startFolderFileTransfers()`
     dispatches one `createDirectory()` call per directory in the
     enumerated tree, unconditionally — its own doc comment already
     claimed "already exists" for a nested directory (an entirely
     normal, expected case: Write Into means "merge into whatever's
     already there") "isn't treated as an error for this bulk path...
     createDirectory()'s fileOperationFailed signal for that case simply
     isn't listened to HERE at all." **That claim was false in
     practice** — `FilePaneWidget`'s OWN, separate, unconditional
     connection to that same signal (shared with the right-click "New
     Folder" action) reacts regardless of who triggered it, popping a
     real `QMessageBox::warning()`. A Write Into merge onto a
     destination that already has many nested subdirectories (which is
     the ordinary case, not an edge case) burst-dispatched one real
     failure per already-existing directory.
  2. **What turned those failures into a crash, not just annoying
     dialogs**: `QMessageBox::warning()`'s own `exec()` pumps the whole
     app's event queue while its dialog is open, same mechanism as
     Phase F's own bug — a SECOND, already-queued failure reply could be
     delivered and opened its OWN dialog nested inside the first, and so
     on for every failing directory, an unbounded, genuinely nested (not
     sequential) call stack of `QMessageBox::exec()` calls that
     genuinely overflowed the stack for real.
  Fixed with two independent, complementary changes:
  - **The semantic root cause**: `RemoteBackend::createDirectory()`
    gained a new `bool ignoreAlreadyExists = false` parameter (all three
    backends implement it: `LocalBackend` checks `QFileInfo::exists()`+
    `isDir()` directly; `SftpBackend` does a synchronous
    `libssh2_sftp_stat()`, matching its own `checkExists()`'s pattern
    and fitting its already-blocking-call worker-thread style;
    `FtpBackend` reuses `checkExists()`'s own listing-based existence/
    type check, since FTP has no single command that cleanly reports
    both). `TransferManager`'s own directory-creation dispatch now
    passes `true` for every call. A genuine TYPE conflict (a FILE
    already at that path, not a directory) still fails normally either
    way — silently proceeding as if a same-named file were the wanted
    directory would be wrong. `QMetaObject::invokeMethod`-by-name
    correctly resolves BOTH the old 1-arg calls (every other existing
    caller — the right-click "New Folder" action, `ScriptRunner`'s
    `mkdir`, `CompareSyncExecutor`'s own directory creation — all
    unaffected, still get a real error if the target's already taken)
    and the new 2-arg call, confirmed directly via a disposable probe
    calling both shapes against a real `LocalBackend`, not assumed from
    reading MOC's own default-argument-overload documentation alone.
  - **General defense-in-depth**: `FilePaneWidget::onFileOperationFailed()`
    gained the exact same `QMessageBox::exec()` reentrancy guard Phase
    F's own `TransferManager::askConflict()` fix already established —
    a `m_warningDialogInProgress` flag plus a
    `m_deferredFailureWarnings` queue, drained in a LOOP (not
    recursively — a burst of many failures must not become unbounded
    C++ call-stack depth either, even the much lighter kind a plain
    function-call loop would produce instead of nested `exec()` calls).
    This closes the same crash risk for ANY OTHER bulk-failure scenario
    (a permission error, a full disk, a dropped connection partway
    through a large tree) — not just the one confirmed trigger above.
  New Phase G (`src/conflict_resolution_test.cpp`) reproduces the actual
  trigger: Write Into onto a destination with several pre-existing
  nested subdirectories must show ZERO "Create folder failed" dialogs.
  New Phase H directly exercises the reentrancy guard itself: six
  genuine, unavoidable type-conflict failures (a file already at each
  target path — deliberately NOT suppressed by `ignoreAlreadyExists`)
  fired back-to-back must all be shown and dismissed one at a time
  without hanging or crashing. Proved non-vacuous the same way as
  always: `git stash` on just the fix files (both backends and
  `FilePaneWidget`) reproduced Phase G's own failure against the pre-fix
  code (the expected "Write Into" dialog never appeared in time, and the
  new file never landed — consistent with the transfer getting stuck
  behind unclicked dialogs); Phase H's own 2-arg `invokeMethod` call
  necessarily has no pre-fix equivalent to compare against (the API
  itself is what's new), so it's validated on its own logical parity
  with Phase F's already-proven identical guard pattern instead. Seven
  existing fake/mock `RemoteBackend` subclasses across the required
  suite (`queue_persistence_test.cpp`, `checksum_verification_test.cpp`,
  `remote_to_remote_test.cpp`, `sort_and_commands_test.cpp`,
  `move_entry_test.cpp`, `transfer_pause_test.cpp`,
  `transfer_concurrency_test.cpp`) plus `smoke_test.cpp` needed their own
  `createDirectory()` override signature updated to match — caught
  immediately as real compile failures (an abstract-class instantiation
  error, not a silent runtime gap) the first time the full required
  suite was rebuilt after this change, not discovered later.

- **FTP's directory-listing parsers are verified directly, in isolation
  from any network I/O.** `src/ftp_parsing_test.cpp` (built via the
  `ftp-parsing-test` CMake target) runs 36 assertions against
  `FtpBackend::parseMlsdLine()` and `parseListLine()` using sample lines
  modeled on real server output formats rather than invented syntax. It
  covers MLSD's standardized fields (type, size, `Modify` timestamps
  parsed to the right `QDateTime`), MLSD's `cdir`/`pdir` entries being
  filtered out rather than returned as real files, and the
  best-effort `LIST` fallback's various shapes. These two parsers are
  pure functions, deliberately factored out and made `public`
  specifically so they could be tested this way — the same reasoning
  that made `FilePaneWidget::parentOfPath()` public. This is the
  highest-risk part of the FTP feature isolated and proven, which is
  worth having, but it is a narrow claim.
  This test only ever exercised the two parsers as pure functions; the
  control connection's command/reply cycling, PASV response parsing, the
  `AUTH TLS` handshake, and actual file transfer all needed a real
  server to verify — see the bullet immediately below, which closes that
  gap.

- **The protocol-selection wiring is verified, including the two ways it
  could silently do the wrong thing.** `src/protocol_selection_test.cpp`
  (built via the `protocol-selection-test` target) drives a real
  `ConnectionDialog` through all three protocols and runs 36 assertions
  over the result: the port follows the protocol (22/21/21) but a
  user-typed port survives a protocol switch untouched; the request is
  tagged correctly and populates the matching credential struct while
  leaving the other empty; plain FTP sets `FtpsMode::None` so it can't
  accidentally claim TLS; selecting private-key auth and then switching
  to FTP clears the selection and drops the key path rather than
  carrying it into a protocol with no such concept. It also covers
  `SavedSite`'s side: protocol survives a real `SiteStore` JSON
  round-trip, a hand-written legacy `sites.json` with no `protocol` key
  at all reads back as SFTP, and a saved FTP site still writes no
  password/passphrase/secret key to disk — the no-secrets-on-disk rule
  restated for the one protocol that has no key-based alternative.
  **Two assertions in this test's first draft passed vacuously and were
  caught before it shipped**, which is worth recording because both
  failure modes are easy to reproduce elsewhere: one had a stray
  `|| true` making it unconditionally green, and the other asserted a
  widget was hidden using `isVisible()` — which returns false for
  *every* widget on a dialog that is never shown, so it would have
  passed had the code hidden nothing at all. Probing both branches and
  finding identical values is what exposed it; `isVisibleTo(&dialog)`
  discriminates properly, and both the positive and negative case are
  now asserted, since a one-sided check can't tell "correctly hidden"
  from "always hidden."
- **The three dialog states were verified visually, not just
  structurally.** `ConnectionDialog` was rendered offscreen under the
  real stylesheet in each protocol state and the PNGs actually looked
  at. The auth row genuinely collapses for FTP/FTPS rather than leaving
  a stranded "Authentication:" label or dead space — confirmed by the
  dialog's own height dropping from 292px to 266px, which is the kind of
  claim a structural assertion alone doesn't establish.

- **Public-key SFTP auth and FTP/FTPS both now confirmed against real
  local servers — closing two gaps this section used to list as
  "still not verified."** `tools/local-test-servers/` spins up
  throwaway local `sshd` (pubkey-only), FTP, and FTPS servers, and
  `verify-sftp-pubkey`/`verify-ftp-live` (`EXCLUDE_FROM_ALL` CMake
  targets, not part of the required self-contained suite since they need
  those external servers already running) drive real
  `SftpBackend`/`FtpBackend`
  instances against them — real connect, list, download, upload, with
  content confirmed both from the client side and by reading files back
  directly off the server's own disk. The same local SFTP server also
  now backs a real, deterministic cancel/pause/resume verification
  (`verify-sftp-pause-cancel`) — a genuinely interrupted, genuinely
  resumed, byte-for-byte-correct transfer in both directions against a
  live server, not just `TransferManager`'s orchestration around a fake
  backend. Full detail, including exactly what remains unproven even
  after this, is in the "Known gaps" entries for FTP/FTPS, public-key
  authentication, and cancel/pause/resume below — this bullet is the
  short version.

**Real window rendering on a real physical display is now confirmed
too, not just headless/offscreen runs.** A real KDE Plasma (Wayland)
desktop has since become available in this environment, and the app has
actually been launched and clicked through on it for real: the local
pane's permissions column, a real FTP server's directory listing (a
vsftpd container, not pyftpdlib), and a genuine download+upload round
trip, all driven through the real GUI (not a headless harness) and
visually confirmed via real screenshots — see the FTP/FTPS and SFTP
Known gaps entries below for what those sessions specifically found and
fixed. The same real-display capability (screenshot-driven, AT-SPI- and
XTest-automated) was used again investigating the SFTP throughput gap,
to drive FileZilla itself for a same-desktop comparison.

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
  Also declares `setPermissions(path, mode)` — same "fire and refresh"
  contract as `deleteEntry()`/`renameEntry()` above, `mode` is the raw
  POSIX permission bits (e.g. `0644`), not a `QFileDevice::Permissions`
  bitmask (different bit positions entirely — each implementation
  converts internally). `LocalBackend` maps the bits onto
  `QFile::setPermissions()`. `SftpBackend` is the first `libssh2_sftp_
  setstat()` call site in the codebase — `libssh2_sftp_stat()`
  (read-only, used by `checkExists()` above) is the closest existing
  analog for how `LIBSSH2_SFTP_ATTRIBUTES` gets populated; only
  `LIBSSH2_SFTP_ATTR_PERMISSIONS` is set in `flags`, so nothing else
  about the file's stat (size/time/ownership) is touched. `FtpBackend`
  issues `SITE CHMOD` — a widely-supported (vsftpd, proftpd) but
  non-standard FTP extension with no equivalent in RFC 959; a server
  that doesn't implement it typically replies 502, already `>= 400`
  here, so it surfaces as an ordinary `fileOperationFailed` like any
  other rejected command, not a special "unsupported" case. Verified
  against real vsftpd and proftpd containers
  (`verify_ftp_vendors.cpp`) — `SITE CHMOD` is accepted and genuinely
  applied by both, confirmed via `podman exec`/`stat` against the
  container's own filesystem directly, independent of `FtpBackend`'s
  own listing code: `FtpBackend`'s `LIST`/`MLSD` parsers deliberately
  never translate real permission bits back into
  `RemoteEntry::permissions` (every FTP entry gets a `"-"` placeholder,
  a pre-existing, disclosed display-only limitation — display, not the
  actual mode, was never wired up, and fixing that is a separate,
  bigger parsing project this feature didn't need), so re-listing
  through the client itself can never observe the real effect of a
  successful `SITE CHMOD` — the test had to reach around it. UI side:
  `PermissionsDialog` (`src/ui/PermissionsDialog.h/.cpp`) — a 3×3
  read/write/execute-by-owner/group/other checkbox grid plus a live
  octal readout, offered from `FilePaneWidget`'s context menu
  (Permissions..., single-entry only, every backend including Local)
  via the free functions `permissionsStringToMode()`/
  `modeToPermissionsString()`, which parse `RemoteEntry::permissions`
  (`"rwxr-xr-x"`) back into bits to pre-populate the dialog — the first
  place in the codebase that string is ever parsed rather than just
  displayed. A malformed or placeholder string (including FTP's own
  `"-"` above) parses to `0` (all unchecked) rather than guessing,
  which is the right default anyway: chmod sets an absolute mode, not
  an incremental change, so the user is expected to set every bit
  explicitly. Deliberately just the standard 9 `rwxrwxrwx` bits — no
  setuid/setgid/sticky, and single-entry only, no multi-select or
  recursive apply — matching the restraint `deleteEntry()` (no
  recursive delete) and `renameEntry()` (`FilePaneWidget` gates it to
  one selected entry) already established.
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
  Also declares `connectionIdentity()` — an opaque, comparable string for
  "which underlying filesystem/server this backend actually talks to",
  used by `TransferManager` to decide whether a cross-pane Move can go
  through a single server-side `moveEntry()` (rename) instead of the much
  slower stage-through-local-disk transfer path. `LocalBackend` returns a
  fixed constant (any two local panes are trivially the same real
  filesystem); `SftpBackend`/`FtpBackend` build one from
  protocol+host+port+username — the protocol prefix is deliberate, since
  an SFTP session and an FTP session to the same hostname can never be
  renamed between via one call regardless of whether they're "really" the
  same box, so the two must never compare equal even when host/port/
  username match.
  Also declares `moveEntry(oldPath, newPath, requestId)` +
  `entryMoved(requestId)`/`entryMoveFailed(reason, requestId)` —
  deliberately a separate primitive from `renameEntry()` above, even
  though both ultimately issue the identical underlying rename call
  (`libssh2_sftp_rename()`/`RNFR`+`RNTO`/`QDir::rename()`; each backend
  factors the shared core into a small private `performRename()` helper
  the two public methods wrap differently). Two real differences drove
  keeping it separate rather than reusing `renameEntry()`:
  `TransferManager` needs an explicit, request-id-correlated success
  signal to drive a `TransferItem`'s status off of —
  `renameEntry()`'s "fire and refresh, implicit success" contract has no
  success signal at all, only the *absence* of `fileOperationFailed`, too
  indirect to build UI state on — and `moveEntry()` must NOT self-refresh
  its directory the way `renameEntry()` does, since a cross-pane move
  needs BOTH panes refreshed, which `TransferManager` handles itself via
  the existing `transferSucceeded` signal once the move succeeds. Only
  ever called once the source and destination backends'
  `connectionIdentity()` already compared equal — implementations don't
  re-check that themselves; see `TransferManager::moveEligible()` below.
- `ProxyConfig` (`src/backends/ProxyConfig.h`) — a global SOCKS5/HTTP
  CONNECT proxy config (type/host/port/username/password), applied to
  every SFTP/FTP/FTPS connection. `SftpCredentials`/`FtpCredentials`
  each carry a `proxy` member (default `ProxyType::None`, a no-op —
  every existing construction site, tests included, is unaffected);
  `MainWindow::startConnection()` is the one place that fills it in,
  from `AppSettings::resolvedProxyConfig()`, right before constructing
  the backend — `ConnectionRequest`/`ConnectionDialog`/`SavedSite`/
  `SiteStore` are otherwise untouched, since this is a global setting,
  not a per-site one (no precedent for a per-site override exists
  anywhere else in `AppSettings` either — see that entry below).
  `LocalBackend` never touches this; a local filesystem has nothing to
  proxy.

  **How the proxy is actually applied — `connectThroughProxy()`
  (`src/backends/ProxyConnect.h/.cpp`) — is the one genuinely
  surprising part of this feature.** The obvious approach —
  `QAbstractSocket::setProxy(QNetworkProxy(...))` before
  `connectToHost()` — does work, but only through Qt's own
  `read()`/`write()` API (`QSocks5SocketEngine` internally). A
  standalone probe built while implementing this confirmed that
  `socketDescriptor()` afterward returns an unusable value (observed: a
  small, obviously-bogus fd number, not the real underlying socket) —
  a hard blocker for `SftpBackend::ensureSession()` (hands the fd to
  `libssh2_session_handshake()`) and `FtpTlsSocket` (hands it to
  OpenSSL's `SSL_set_fd()`, plus its own raw `poll()`/`recv()`/`send()`
  loops) — both need the *real* fd, not a Qt-internal one. First
  attempts at both used `setProxy()` and passed every check except
  actually connecting through the real proxy — `verify-socks5-proxy`
  (below) is what caught it: the plain-FTP phase passed immediately,
  but SFTP failed with a raw libssh2 "SSH handshake failed" and FTPS
  failed with "Server did not send a valid welcome banner" — both
  symptoms of reading garbage off a bad descriptor, not a proxy
  problem. `connectThroughProxy()` fixes this by doing the SOCKS5 (RFC
  1928/1929, including username/password subnegotiation) or HTTP
  CONNECT handshake **manually**, blocking, directly on an
  ordinary, not-yet-`setProxy()`'d `QTcpSocket` — after the handshake,
  `socketDescriptor()` returns the real fd, now a transparent
  byte-for-byte pipe to the target, exactly as if it were a direct
  connection. `SftpBackend::ensureSession()` and
  `FtpTlsSocket::connectToHost()` both use this. `FtpBackend`'s
  plain-FTP control/data connections (`QtSocketAdapter` over
  `QSslSocket`-as-TCP) are the one path that still uses
  `QAbstractSocket::setProxy()` directly — correct and simpler there,
  since that path never extracts a raw descriptor at all, only ever
  reading/writing through Qt's own socket API from connect through
  teardown.

  Verified against real proxies, not just this project's own code
  talking to itself: `verify-socks5-proxy` (`src/
  verify_socks5_proxy.cpp`) drives SFTP, plain FTP, and FTPS through a
  genuine SOCKS5 proxy — OpenSSH's own `ssh -D` dynamic port
  forwarding, tunneled through the same local `sshd`
  `start-sftp-pubkey.sh` already provides (`tools/local-test-servers/
  start-socks5-proxy.sh`) — plus a negative control (a deliberately
  wrong, nothing-listening proxy port must make the whole connection
  attempt fail, proving `setProxy()`/`connectThroughProxy()` is
  genuinely in the path rather than silently ignored, which would
  otherwise make every "success" above just as green via an accidental
  direct connection). `verify-http-connect-proxy` (`src/
  verify_http_connect_proxy.cpp`) does the same against a real
  tinyproxy instance (`tools/local-test-servers/
  start-http-connect-proxy.sh`, a throwaway podman container —
  `containers/Containerfile.tinyproxy`/`tinyproxy.conf`), with a real
  BasicAuth username/password tinyproxy actually enforces, exercising
  `connectThroughProxy()`'s `Proxy-Authorization` header path for real;
  its negative control is wrong credentials rather than a wrong port,
  proving that header is genuinely checked. **One real containerization
  gotcha found and fixed getting that second harness working:**
  tinyproxy's container initially used a normal port-mapped network
  (`-p 8888:8888`), under which `127.0.0.1` inside the container is the
  *container's own* loopback, not the host's — every CONNECT to the
  host-loopback-bound test servers failed with tinyproxy's own "500
  Unable to connect", nothing to do with the proxy protocol itself.
  Fixed by running the container with `--network host` instead,
  matching `start-socks5-proxy.sh`'s native (non-containerized) `ssh -D`
  process, which never had this problem because it already shares the
  host's network namespace.

  `AppSettings` (see its own entry below) stores the non-secret fields
  (`proxyType`/`proxyHost`/`proxyPort`/`proxyUsername`) as plain
  `settings.json` keys; the password routes through `CredentialStore`
  instead, keyed by a fixed sentinel string
  (`__zephyrftp_global_proxy__`) rather than a `SavedSite::id`, since
  there's exactly one global proxy config to store a secret for, not
  one per site — a deliberate, disclosed reuse of the existing
  per-site secret store rather than a second storage mechanism.
  `app-settings-test` (required suite) confirms the password never
  lands in `settings.json`; `verify-credential-store` (opt-in, touches
  the real OS keyring) confirms the password genuinely round-trips
  through `CredentialStore` for real — deliberately NOT the other way
  around, since a routinely, automatically run required-suite test
  writing a real secret into the developer's actual OS keyring on every
  run would violate the exact principle `CredentialStore.h`'s own doc
  comment already establishes (a real bug this session's own first
  draft of `app-settings-test`'s coverage had, caught by
  `verify-credential-store` failing on its second run — fixed by moving
  the real-secret-touching check there instead).

  Deliberately NOT proxied: `FtpBackend::openActiveDataChannel()`'s
  `SslAcceptingTcpServer` is a *listening* socket waiting for the FTP
  server to connect back — proxying only affects outbound connects, so
  active-mode data channels remain unproxied. Not a new limitation:
  active mode is already NAT-hostile and generally unusable from behind
  any restrictive network, proxy or not.
- `LocalBackend` — wraps `QDir`/`QFile`. Runs on the GUI thread; local
  listing/copy is fast enough that this hasn't been a problem, but it's a
  design decision worth revisiting if it ever needs to handle slow
  network-mounted paths. File management uses `QDir::rename()` (not
  `QFile::rename()`, which isn't documented to reliably rename
  directories across platforms) for both files and folders, and checks
  `QFileInfo::exists()` explicitly before create/rename so "the name is
  already taken" is reported as a specific error rather than silently
  overwriting or falling through to a generic OS failure message.
  `moveEntry()` differs from `renameEntry()` in one respect: it moves a
  pre-existing FILE destination aside before renaming onto it (mirroring
  `downloadFile()`/`uploadFile()`'s established Overwrite convention —
  see below), where `renameEntry()` rejects outright if the destination
  already exists — the two callers have genuinely different
  pre-conditions, so this isn't shared with `renameEntry()`'s own logic
  the way SFTP/FTP's `performRename()` is.
  **Three real bugs found by code review, all fixed, all now covered by
  regression scenarios in `file-operations-test`:**
  1. **`downloadFile()`/`uploadFile()`/`moveEntry()` deleted any
     pre-existing destination FIRST, then attempted the actual
     copy/rename** — if that then failed (disk full, permission denied,
     the source vanishing mid-copy), the original destination content
     was already gone, permanently, with only a generic failure message.
     Fixed with a shared `prepareOverwrite()` helper: an existing
     destination is moved aside to a same-directory sibling (a cheap,
     same-filesystem rename, not a delete) instead, and every failure
     path rolls it back into place — confirmed directly with a
     regression scenario that forces a real, deterministic copy/move
     failure (the source made to simply not exist, or — for the copy
     case — the source itself made to be a directory, so
     `QFile::copy()` refuses to open it) against a real pre-existing
     destination, and checks the original content survived intact.
     **The copy-failure scenario originally simulated "unreadable" via
     `chmod 000` on a regular file — a bug in the test itself, caught by
     this project's own container-based CI jobs
     (`build-linux-appimage`/`build-linux-rpm`/`build-windows`, all of
     which run their test suite as root inside a Docker container):
     root bypasses Unix permission bits entirely, so the "unreadable"
     source was actually still readable there, and the regression
     silently stopped testing anything on exactly the CI jobs meant to
     catch a real regression — while passing everywhere non-root**,
     including this project's own local dev sandbox and the bare
     (non-container) `build-linux` CI job, which is why it went
     unnoticed until a real release run failed. Confirmed with the same
     `git stash`-based before/after methodology used throughout this
     project, run inside a real `fedora:44` container as `root` (via
     `podman`) to match CI exactly: the `chmod 000` version reproducibly
     failed as root, the directory-as-source version reproducibly
     passed as both root and a normal user.
  2. **`renameEntry()` used a plain `QFileInfo::exists(newPath)` check to
     detect a naming conflict, which false-positives on a case-ONLY
     rename** (e.g. `"readme.txt"` -> `"README.txt"`) **on a
     case-insensitive filesystem** — the default on Windows/NTFS and
     macOS/APFS, both real release targets — since `newPath` resolves to
     the very file being renamed. Fixed by additionally checking
     `QFileInfo(oldPath) != QFileInfo(newPath)`, Qt's own "do these refer
     to the same file" comparison rather than a byte-identical string
     check — confirmed directly (via a standalone probe, not just
     assumed from documentation) that this comparison correctly
     recognizes two different path strings pointing at the same
     underlying file as equal (proven with a symlink, since this
     project's own Linux dev/CI environment is case-sensitive and can't
     reproduce the actual Windows/macOS collision directly), while still
     correctly rejecting a genuine conflict against a real, different
     file — also covered by its own regression check, specifically to
     confirm the fix didn't loosen this into a no-op.
  3. **`deleteEntry()` could never actually delete a directory
     symlink.** `isDirectory` comes from `QFileInfo::isDir()`
     (`RemoteEntry::isDir`, set in `listDirectory()`), which FOLLOWS
     symlinks — so a directory symlink also reports `isDirectory=true`
     here — but `QDir::rmdir()` (POSIX `rmdir(2)`) deliberately does NOT
     follow a symlink in its final path component, so it always failed
     with the same misleading "may not be empty" message regardless of
     how genuinely removable the symlink was. Confirmed directly with a
     standalone probe before fixing: `rmdir()` fails on a real directory
     symlink every time, while `QFile::remove()` (POSIX `unlink(2)`
     semantics) correctly removes just the symlink entry and leaves its
     target directory completely untouched. Fixed by routing a directory
     symlink through the same `QFile::remove()` path a plain file
     already uses, gated on `!QFileInfo(path).isSymLink()`.
     **The regression test itself is Unix-only** — confirmed empirically
     (a standalone probe run under `wine`, the same way this project's
     own CI exercises the Windows build): `QFile::link()` does not
     create a real filesystem symlink on Windows the way it does on
     Unix; it writes a small Shell-Shortcut-style file instead, which
     `QFileInfo` correctly reports as neither a directory nor a symlink.
     The test's hardcoded `isDirectory=true` (mirroring what a REAL
     directory symlink's `QFileInfo::isDir()` reports on Unix) would
     then make `deleteEntry()` wrongly attempt `QDir::rmdir()` on that
     plain file and fail — not an app bug, just this phase's simulation
     technique having no Windows equivalent — so `file-operations-test`
     guards this phase with `#ifndef Q_OS_WIN` rather than asserting
     something false. This was the actual cause of `build-windows`'s CI
     failure after the chmod/root fix above was applied on its own;
     `deleteEntry()`'s own `isSymLink()` guard needed no change — real
     Windows NTFS symlinks/junctions are still correctly detected by
     Qt's `isSymLink()` there, only `QFile::link()`'s own simulation
     technique doesn't produce one.
  **`file-operations-test` itself had a flaky-timing bug, unrelated to
  any of the three fixes above, that only surfaced under real CI load:**
  three of its check phases (the rename-conflict-rejection check, the
  download-rollback check, and the move-rollback check) each assumed a
  flat 200ms window was always enough time for a `Qt::QueuedConnection`
  dispatch plus real disk I/O to complete before reading the result —
  the same class of assumption this project's own `navigation-test`
  header comment already flags as unsafe under real load. Confirmed via
  wall-clock timing analysis that this — not the ConnectionDialog change
  actually being released at the time — was the true cause of a real
  `build-linux-rpm` CI failure blocking v0.6.12: local podman-container
  reproduction of the exact same `fedora:44` image passed cleanly, but
  20-plus-iteration local stress testing (`for i in $(seq 1 20); do rm
  -rf /tmp/file_ops_test; ./file-operations-test; done`) reproduced
  intermittent failures on these same three phases. Fixed by replacing
  the fixed delay with a polling `waitUntil()` helper (matching
  `navigation-test`'s and `move_entry_test`'s own established pattern)
  that waits on the actual signal-driven state change instead of a
  guessed wall-clock window; confirmed with 30/30 clean stress-test runs
  afterward.
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
  **Update: the remaining gap is now confirmed closed, not just
  theorized.** `verify-sftp-throughput` (`src/verify_sftp_throughput.cpp`)
  measured this app against a real, non-loopback server (~6-7ms RTT)
  alongside OpenSSH's own `scp` as an in-the-moment baseline on the same
  link: ~29-31MB/s upload / ~37-39MB/s download here vs. `scp`'s
  ~29-36MB/s / ~35-37MB/s, a ~0.93-1.13x ratio — real parity with a
  native reference SFTP client, not the previously-reported 1.6-1.8x gap.
  See the Known Gaps entry on this same subject for the full writeup;
  kept brief here to avoid duplicating it.
  **Bandwidth throttling (`BandwidthThrottle`, `src/backends/BandwidthThrottle.h/.cpp`)
  is the deliberate inverse of all the throughput work above** — a
  `BandwidthThrottle` is constructed fresh at the top of each
  `downloadFile()`/`uploadFile()` call from `m_credentials.bandwidthLimitKBps`
  (0 = unlimited, the default — a true no-op, no `QElapsedTimer`/sleep
  overhead at all) and its `pace(bytesSoFar, shouldStop)` is called right
  after each chunk's `transferProgress` emission, sleeping in short
  (~150ms) increments — re-checking `shouldStop` between each — until the
  average rate since construction is back down to the configured limit.
  **Deliberately PER-TRANSFER, not one shared/global cap** — confirmed
  with the user before implementing (the simpler of two real options: a
  global aggregate would need a thread-safe rate limiter shared across
  backend instances/worker threads, meaningfully more complexity for a
  feature this project didn't have any version of yet). With at most
  ~2 panes able to run concurrently (see `TransferManager`'s own entry),
  worst-case combined bandwidth usage is ~2x the configured number — an
  accepted, understood tradeoff, not an oversight. Wired in exactly like
  `ProxyConfig`: `SftpCredentials`/`FtpCredentials::bandwidthLimitKBps`
  populated once from `AppSettings::bandwidthLimitKBps()` by
  `MainWindow::startConnection()`, fixed for that connection's whole
  lifetime (changing the Preferences value needs a reconnect to take
  effect, same as proxy). `LocalBackend` is NOT throttled — same
  "`QFile::copy()` is one atomic OS-level call with no loop of ours to
  interrupt" reasoning `RemoteBackend::requestPause()`'s own doc comment
  already establishes for why Local doesn't support pause either; a
  one-line note there extends it to cover throttling too, a documented
  scope boundary rather than a gap. Verified two ways, mirroring the
  concurrent-transfers precedent: `bandwidth-throttle-test` (required
  suite) proves `BandwidthThrottle`'s own pacing math directly against
  real wall-clock time (no server) — a 100 KB/s limit paced two 50KB
  calls to almost exactly 500ms/1000ms on a real run, and a `shouldStop`
  callback reliably cuts what would otherwise be a ~50-second sleep short
  within ~150ms of becoming true; `verify-bandwidth-throttle-live`
  (external precondition, one real local `sshd`, not part of CI) proves
  a real `SftpBackend` upload actually achieves close to a configured
  real-world KB/s over a real network, not just correct pacing logic in
  isolation — confirmed on a real run: 400000 KB/s unthrottled vs.
  exactly 300.0 KB/s throttled against a 300 KB/s configured limit.
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
  **Five real bugs found by a dedicated code review of this class's
  auth/host-key code, all five fixed:**
  1. **`ensureSession()` leaked the `QTcpSocket` and `LIBSSH2_SESSION` on
     every failure path after the socket connected** — a wrong password,
     a rejected host key, a handshake failure, or an SFTP subsystem init
     failure all just `return false` without calling `teardown()` (only
     ever invoked from the destructor before this fix) or resetting
     `m_socket`/`m_session` to `nullptr`. Worse than a one-time leak: on
     retry, `ensureSession()`'s own `if (m_session && m_sftp) return true;`
     guard doesn't catch this (`m_sftp` is still null after an auth
     failure), so it allocates a BRAND NEW socket and session, silently
     overwriting the old pointers — a real, repeatable leak of one socket
     fd and one libssh2 session per failed connect+retry cycle (a wrong
     password typed once, corrected, and retried, is a completely normal
     interactive flow). Fixed by calling `teardown()` — already
     null-safe on each of `m_sftp`/`m_session`/`m_socket` — before every
     `return false` from this point in the function onward.
  2. **Host keys were looked up with `libssh2_knownhost_checkp()` (which
     takes a port, "to do a better check" per its own header comment) but
     stored with `libssh2_knownhost_addc()` (which has no port parameter
     at all)** — both calls used the bare hostname, with no port
     information going into the stored entry either way. Confirmed via a
     standalone reproduction against the actual installed libssh2 (not
     assumed from the header alone): two different fake keys added/
     checked against "127.0.0.1" on two different ports came back
     `CHECK_MISMATCH` for the second one — a real "host key changed —
     possible MITM!" warning for what should have been a first-time
     sighting of a genuinely different service. This is exactly the
     shape of this project's own `tools/local-test-servers/` setups
     (multiple fixture servers, same "127.0.0.1", different ports) and
     of Move/`RemoteToRemote`'s own two-connections-to-possibly-different-
     ports pattern. Fixed by storing (not looking up — `checkp()` already
     matches this form correctly when given a port, confirmed in the same
     reproduction) entries as `"[host]:port"`, OpenSSH's own bracket
     convention for a non-default port — confirmed working for the
     default port too, so no port-based special-casing was needed.
  3. **`libssh2_knownhost_writefile()`'s return value was ignored on both
     the accept-new-host and accept-changed-host paths** — the
     confirmation dialog promises "this will be remembered for future
     connections," but a failed persist (a briefly read-only config
     directory, a full disk) left nothing telling the user that promise
     didn't hold; every later connection would silently re-prompt from
     scratch with no indication why. Fixed by checking the return value
     and surfacing a warning via `commandLogged` (the Commands pane's
     existing informational-message channel) — the connection itself
     still succeeds either way, only the on-disk persistence failed.
  4. **A blank fingerprint if `libssh2_hostkey_hash()` returns null**
     (SHA256 hashing unsupported by whatever crypto backend libssh2 was
     linked against — rare, but real) **rendered as an empty
     "Fingerprint:" line in the trust dialog**, asking the person to
     verify a host key against nothing. Fixed with an explicit,
     honest placeholder explaining why it's unavailable instead of
     silently blank.
  5. **Public-key auth always built the public-key path as
     `<privatekey>.pub` and passed it unconditionally**, even when that
     file didn't actually exist on disk (a private key exported, copied,
     or renamed without its `.pub` sibling — not an unusual thing to
     happen) — auth would then fail with a generic "Public-key
     authentication failed" error even though the private key itself was
     entirely valid and sufficient. The code's own comment already
     flagged this convention as UNVERIFIED. Fixed by checking whether the
     `.pub` sibling actually exists first, falling back to `nullptr` (letting
     libssh2 derive the public key directly from the private key file)
     when it doesn't — confirmed working against a real server: a real
     ed25519 keypair authenticated successfully with its `.pub` sibling
     temporarily removed.
  **Not fixed, documented instead — a real, latent trap, not currently
  reachable in practice:** `askUserToTrustHostKey()`'s
  `Qt::BlockingQueuedConnection` call into `HostKeyVerifier` silently
  returns `false` (rather than deadlocking or asserting) if it's ever
  invoked from the same thread as `m_hostKeyVerifier` — nothing in
  `SftpBackend` enforces the "always runs on its own worker thread"
  invariant at runtime. Every real call site (`MainWindow::startConnection()`,
  every `verify-*` harness) already respects this, so it isn't reachable
  today; a future caller that didn't (a new test harness, a refactor that
  calls `connectToHost()` synchronously from the GUI thread for a quick
  validation path) would see every host key silently rejected with a
  misleading "not trusted" error rather than a clear signal that the
  threading precondition was violated. Not fixed here — enforcing it
  would mean adding real cross-thread runtime assertions this class
  doesn't have anywhere else either, a larger change than this pass
  attempts.
  **A sixth, related issue found independently while re-verifying these
  fixes against real servers (not from the original review) — initially
  left as a known gap, now fixed too:** two `SftpBackend` instances
  connecting around the same time (exactly what Move and `RemoteToRemote`
  both do) could race on `known_hosts`, since each instance independently
  reads-modifies-writes the entire shared file with no coordination — the
  second to finish could silently overwrite the first's newly-trusted
  entry. Not a one-off: an 8-iteration stress test (two brand-new host
  keys, two `SftpBackend` instances connecting concurrently via
  `verify-remote-to-remote-live`, `known_hosts` wiped and both throwaway
  servers restarted with fresh host keys between each run) lost one of
  the two entries on **8 out of 8** pre-fix runs — a reliably reproducible
  race, not a rare one. Fixed with a `QLockFile`
  (`<known_hosts path>.lock`) guarding the entire read-modify-write span
  (`libssh2_knownhost_readfile()` through `libssh2_knownhost_writefile()`)
  — a `QLockFile` rather than a plain in-process `QMutex` specifically
  because the race is across separate app instances sharing the same
  config directory, not just separate threads within one process, which
  a mutex alone wouldn't cover. `lock()` (blocking, not `tryLock()`) is
  used deliberately: this always runs on a worker thread, never the GUI
  thread, so blocking here can't freeze the UI, and the operation being
  waited on is a quick file read+write, not something worth giving up on.
  Re-ran the identical 8-iteration stress test after the fix: **0 dropped
  entries across 13 total runs** (8 fresh + 5 more on a later
  re-verification pass), with a real pre/post control — the same build
  with the fix reverted reproduced the loss on 8/8 runs again, confirming
  the fix (not some other change) is what closes the race.
  Re-verified against real servers after all six fixes:
  `verify-sftp-pubkey` (including a real run with the `.pub` sibling
  temporarily removed, directly exercising fix #5), `verify-sftp-pause-cancel`,
  `verify-sftp-move`, and `verify-remote-to-remote-live` (two real
  servers, exercising both the port-qualification fix #2's actual
  scenario and, repeatedly, fix #6's) all still pass.
- `FtpBackend` (`src/backends/FtpBackend.h/.cpp`) — FTP and both FTPS
  variants (explicit and implicit — see the dedicated entry below),
  hand-rolled directly on `QTcpSocket`/`QSslSocket`. Implements the full
  `RemoteBackend` interface and runs on a dedicated worker thread under
  the same threading contract as `SftpBackend`. Also uses the same
  `BandwidthThrottle` in its own `downloadFile()`/`uploadFile()` loops,
  same per-transfer scope and `FtpCredentials::bandwidthLimitKBps`
  wiring — see `SftpBackend`'s own entry above for the full design,
  kept brief here to avoid duplicating it. **Reachable from the UI**
  — `MainWindow::startConnection()` constructs one for `Protocol::Ftp`/
  `Protocol::Ftps`, same as `SftpBackend` for `Protocol::Sftp` (see the
  `MainWindow` entry below); confirmed against a real server too, not
  just wired — see Known gaps. The
  no-bundled-library decision mirrors `SftpBackend`'s direct-on-libssh2
  approach, and for FTP specifically it was checked rather than assumed:
  libcurl's own documentation confirms it doesn't parse `LIST` output
  either, leaving that to callers — so the one big thing a library might
  have bought here didn't actually apply. Directory listings try `MLSD`
  (RFC 3659, standardized and machine-parseable) first and fall back to
  best-effort `LIST` parsing on a 500/502 reply; `LIST`'s output format
  is not standardized across server implementations, which makes it the
  single largest real-world fragility source for any FTP client. **PASV
  is tried first always**; only if the server outright refuses the PASV
  command does this fall back to active/PORT (`openDataChannel()`/
  `finalizeDataChannel()`/`openActiveDataChannel()`) — a local
  `QTcpServer` (via a small `SslAcceptingTcpServer` subclass so an
  active-mode data connection can still act as the TLS client once PROT
  P is active, regardless of which side dialed the TCP connection) is
  opened and the server told to connect back to it via `PORT`. This is a
  fallback, not a mode toggle — PASV's NAT-friendly default behavior is
  unchanged for every server that accepts it. Both explicit FTPS
  (`AUTH TLS` upgrading an existing plaintext control connection) and
  implicit FTPS (TLS from the connection's first byte) are supported —
  see the dedicated entry below for the handshake-timing split and a
  real bug this addition found and fixed.
  **Certificate verification is a real trust-on-first-use (TOFU) model**
  — the SSH-host-key shape (see `SftpBackend`'s entry above and
  `HostKeyVerifier`), not the old fail-closed-only behavior: an
  unverifiable certificate (self-signed, unknown CA, ...) is routed to
  `CertificateVerifier` (`src/ui/CertificateVerifier.h/.cpp`, a GUI-thread
  object exactly mirroring `HostKeyVerifier`) via the same
  blocking-cross-thread-call pattern `SftpBackend` uses for host keys.
  The accepted certificate's SHA-256 fingerprint persists to
  `AppConfigLocation/known_certs.json` (plain JSON — this project's own
  convention for its own non-secret state, same as `SiteStore`; a
  certificate fingerprint is public information, the same status an SSH
  host-key fingerprint already has in plaintext `known_hosts`) and is
  re-checked on every future connection to that host:port; a certificate
  that later CHANGES gets a strong mismatch warning defaulting to no,
  same as a changed SSH host key. Data connections reuse the control
  connection's already-trusted fingerprint instead of prompting again — a
  data connection presenting a DIFFERENT certificate fails closed with no
  prompt, treated as suspicious rather than something to ask about.
  FTPS's control AND data connections are handled by `FtpTlsSocket` (raw
  OpenSSL, forced to exactly TLS 1.2), not `QSslSocket` — genuine,
  confirmed TLS session reuse, not a best-effort attempt: RFC 4217
  permits a server to require the data connection's TLS session to
  demonstrably reuse the control connection's, and `QSslSocket`'s public
  API has no way to drive that for real (see the TLS-session-reuse entry
  below for the full story of why, and how this was actually confirmed
  working end to end against a real strict server, not just a
  self-hosted one).
  **Four real bugs found by a dedicated code review of this file's
  parsing/data-channel/TLS logic, all fixed:**
  1. **The data-connection certificate-pinning guarantee this entry
     itself describes above ("a DIFFERENT certificate fails closed with
     no prompt") had a real hole**: the fingerprint comparison lived
     entirely inside the `sslErrors` handler in `verifyPeerCertificate()`
     — if the data connection's certificate happened to validate cleanly
     against the system trust store on its own (no `sslErrors` at all,
     e.g. a certificate that legitimately chains to a public CA but
     still isn't the SAME certificate the control connection already
     trusted), the fingerprint check never ran and the connection
     silently succeeded. "Independently valid" was never the actual bar
     for a data connection — matching the control connection's own
     already-trusted certificate is. Fixed with an explicit check for
     exactly this case (encrypted, is a data connection, no `sslErrors`
     fired) that still compares fingerprints and fails closed on a
     mismatch.
  2. **`verifyPeerCertificate()`'s `sslErrors` connection was never
     explicitly disconnected**, and its context object was `socket`
     (which, for the control connection, outlives the function call for
     the whole session) rather than the function's own scope — the
     lambda captures `sawErrors`/`fingerprint`/`details`/`isDataConnection`
     BY REFERENCE, all stack-local to `verifyPeerCertificate()`. If
     `sslErrors` were ever emitted again on the same socket after the
     function returned, the lambda would read/write already-destroyed
     stack memory — real undefined behavior, not just a style issue.
     Fixed by explicitly disconnecting the connection before the
     function returns, on every path.
  3. **`openActiveDataChannel()`'s active-mode fallback rejected any
     local address whose `protocol()` wasn't reported as exactly
     `IPv4Protocol`** — on a dual-stack host, the control connection's
     local address can be surfaced as an IPv4-MAPPED IPv6 address
     (`protocol()` reports `IPv6Protocol`) even though it's genuinely
     IPv4 underneath, something `QHostAddress::toIPv4Address()`'s own
     success flag still correctly recognizes. The old check would refuse
     the active/PORT fallback outright in that case — no real attempt at
     all — even though a genuine active-mode session over that same
     address would have worked. Fixed by checking `toIPv4Address()`'s own
     `ok` output parameter instead of `protocol()`.
  4. **PASV reply octets were parsed with `toInt()` but never range-checked
     to [0, 255]** — a malformed or actively malicious PASV reply (a
     compromised/rogue server, or a MITM on the control connection) could
     produce an invalid dotted-quad host string (a confusing DNS-lookup
     failure instead of the "Could not parse PASV reply" error this code
     already intends for malformed replies) or, worse, a negative octet
     feeding a left shift of a negative signed value — undefined behavior
     in C++. Fixed with an explicit per-octet range check.
  Re-verified against real servers after all four fixes:
  `verify-ftp-live` (plain FTP, FTPS with an untrusted self-signed cert
  correctly still rejected, legacy-LIST fallback, **active-mode
  fallback** — directly exercising fix #3's code path — and a real
  CA-trusted FTPS round trip with a clean handshake and no `sslErrors` at
  all, directly exercising fix #1's new branch) and `verify-ftps-trust`
  (first-sighting trust prompt, silent match on an unchanged certificate,
  and a real mismatch warning on a genuinely changed one) all still pass.
- **Implicit FTPS** — `Protocol::FtpsImplicit` (a fourth peer value, not
  a mode flag on `Ftps` — see the `Protocol`/`ConnectionRequest` entry
  below for why) and `FtpsMode::Implicit`. The real difference from
  explicit FTPS is handshake timing, isolated entirely in
  `FtpBackend::ensureConnected()`: explicit connects on the plain port,
  reads a plaintext welcome banner, sends `AUTH TLS`, then handshakes;
  implicit handshakes **immediately** after the raw TCP connect — no
  plaintext phase at all, no `AUTH TLS` command — and only then reads
  the welcome banner, now over TLS. `FtpTlsSocket` itself needed zero
  changes: its `connectToHost()` was already just a raw TCP connect with
  no assumption of prior plaintext traffic. Everything after login
  (`PBSZ 0`/`PROT P`, `TYPE I`, starting-directory resolution) is
  unchanged and shared between both modes.
  **A real, security-relevant bug found by reading the code directly
  during planning, not from initial exploration, and fixed before this
  ever shipped**: four separate call sites in this file (`ensureConnected()`,
  `openDataChannel()`, `openActiveDataChannel()`, `finalizeDataChannel()`)
  each independently re-checked `m_credentials.ftpsMode ==
  FtpsMode::Explicit` to decide whether a socket should be TLS-capable.
  Left as-is, implicit mode's control connection would have encrypted
  correctly while every DATA connection (list/upload/download) got a
  plain, non-TLS-capable socket — `finalizeDataChannel()`'s own
  `dynamic_cast<FtpTlsSocket*>` would then fail closed on every transfer,
  not silently downgrade, but still a real correctness gap for a mode
  whose entire point is "encrypted from the first byte." Fixed by
  extracting `bool FtpBackend::usesTls() const { return
  m_credentials.ftpsMode != FtpsMode::None; }` and replacing all four
  occurrences — makes correctness automatic at every call site instead
  of four independent edits that could silently drift apart later.
  **Test-server research, confirmed hands-on rather than assumed**: the
  project's existing FTP/FTPS test server (`ftp_server.py`, pyftpdlib)
  cannot do implicit mode at all — its `TLS_FTPHandler` only supports
  explicit `AUTH TLS`. Confirmed via a throwaway `podman` container
  running Fedora that real vsftpd (a separate, independently-implemented
  server) supports implicit FTPS through its own documented
  `implicit_ssl=YES`/`listen_port` directives (read directly from
  vsftpd's own man page inside the container, not assumed from
  secondhand knowledge), and that a single vsftpd process cannot serve
  both explicit and implicit simultaneously — confirmed the project
  needed, and now has, a second dedicated container
  (`Containerfile.vsftpd-implicit`, `start-vsftpd-implicit.sh`,
  `127.0.0.1:2128`) alongside the existing vsftpd one used for explicit
  FTPS vendor testing. New permanent live target `verify-ftps-implicit`
  (handshake, login, `listDirectory()`, and a real upload/download
  round-trip with content verification) proved non-vacuous via a
  `git stash`/rebuild/rerun/`git stash pop` cycle: reverting just the
  `usesTls()` fix causes it to fail (the control connection hangs,
  attempting a plaintext read against a server speaking TLS from byte
  one); restoring the fix passes all assertions again.
  **A pre-existing, unrelated UI bug found by this addition's own
  mandated visual check, not caused by it**: `SiteManagerDialog`'s port
  field never auto-followed the protocol combo (`ConnectionDialog`, a
  fresh one-shot dialog every time, already had this via
  `onProtocolChanged()`'s own `defaultPortFor()` call gated on a
  `m_portManuallyEdited` dirty flag; `SiteManagerDialog` never called
  `defaultPortFor()` at all, for any protocol — confirmed via a
  `git stash` before/after check that this was true before implicit FTPS
  existed too, just far less likely to bite anyone when every protocol's
  default port was 21 or 22). Implicit FTPS's 990 made the gap
  materially worse (a stale port silently pointed at the wrong service
  instead of a usually-harmless off-by-one). Fixed with the same
  `m_portManuallyEdited` pattern, but as a THIRD, separate `connect()` on
  `m_protocolCombo`'s `currentIndexChanged` signal rather than folded
  into `onProtocolChanged()` itself: `SiteManagerDialog` is a single
  long-lived instance reused across every site in the tree, and
  `loadSiteIntoForm()` calls `onProtocolChanged()` directly (bypassing
  its own `QSignalBlocker`-protected combo update) purely to refresh
  field visibility — folding the auto-follow logic into that method
  would have overwritten a freshly-loaded existing site's own stored
  port the instant it was clicked. The flag is also reset to `false`
  inside `loadSiteIntoForm()` on every load, so a manual edit made while
  viewing one site can't suppress auto-follow for a different site
  selected next.
- `Protocol` / `ConnectionRequest` (`src/backends/Protocol.h`,
  `ConnectionRequest.h`) — the seam between "the user picked a protocol"
  and "construct the matching backend." `Protocol` is a four-value enum
  (Sftp/Ftp/Ftps/FtpsImplicit) with small helpers hanging off it: the
  conventional port, the combo-box label, whether key auth applies, and
  stable string keys for JSON. `ConnectionRequest` pairs a `Protocol` tag
  with both credential structs, only the relevant one populated.
  Deliberately NOT one merged credentials struct: `SftpCredentials` and
  `FtpCredentials` genuinely diverge (key path, passphrase and auth
  method are meaningless for FTP; `FtpsMode` is meaningless for SFTP),
  and merging them would produce a type where half the fields are dead
  depending on a value stored elsewhere in the same object — exactly the
  shape that invites someone to read `privateKeyPath` on an FTP
  connection and get an empty string that means nothing. The three
  fields both structs really do share (`host`, and the
  `useHomeDirectory`/`startingDirectory` pair) get accessors on
  `ConnectionRequest` so validation code doesn't re-branch on the tag to
  read something protocol-independent.
  `MainWindow::startConnection()` is the ONLY place in the UI layer that
  names a concrete backend type: it switches on the tag, produces a
  `RemoteBackend *`, and everything downstream (pane, transfer manager,
  queue widget) stays protocol-agnostic — which is what `RemoteBackend`
  was for in the first place.
- `ConnectionDialog` — host/port/username form plus a password/private-key
  auth toggle (`QStackedWidget` swaps the relevant fields). Returns a
  single `SftpCredentials` struct (`src/backends/SftpCredentials.h`) that
  `SftpBackend` consumes directly — no more unpacking/repacking individual
  fields at the call site. Still the "one-off connection" path — kept
  deliberately unchanged when Site Manager was added (see below) rather
  than risking regressing an already-verified flow; `MainWindow::startConnection()`
  is the shared code both paths funnel through afterward.
  **A real, found-not-reported layout bug, fixed during a systematic
  dialog-consistency pass across every custom dialog:** switching the
  Protocol combo away from SFTP on an already-open dialog left dead
  space where the Authentication row used to be, instead of the window
  shrinking to match — while a dialog constructed directly in FTP/FTPS
  mode from the start (e.g. via `setProtocol()` before the first `show()`,
  which is exactly what `protocol-selection-test` does) already looked
  correctly compact. This split — same hidden row, different outcome
  depending on *when* the hide happens — is why the existing test suite
  never caught it: it only ever drives the "fresh, already in the target
  state" path, never the "user opens Connect, then changes their mind on
  the dropdown" path, which is the one real people actually take, since
  the dialog always opens on SFTP. Root cause: `QFormLayout` doesn't
  shrink a row's reserved layout space just because both its widgets are
  hidden — `onProtocolChanged()`'s `setVisible(false)` calls only
  invalidate the layout and post a deferred `LayoutRequest` event to
  actually recompute it. Fixed with `layout()->activate()` (forces the
  recompute immediately) followed by `adjustSize()`, both guarded on
  `isVisible()` — calling `adjustSize()` on a not-yet-shown dialog (the
  `protocol-selection-test`/pre-configured-caller case) reads sizeHint
  from not-yet-fully-resolved style metrics and can stick a wrong
  (larger) size that then blocks Qt's own correct auto-size-on-first-show
  behavior, so the fix deliberately leaves that path alone. Getting the
  fix right took two iterations: the first version called `adjustSize()`
  without `layout()->activate()` first, which read a stale (pre-hide)
  size hint — confirmed by testing all three transitions in sequence,
  not just one: the *first* live switch away from SFTP silently kept the
  old size, and only the *next* switch (now reading a hint stale by one
  step) resized, which would have shipped as "fixed" against a test that
  only checked a single before/after pair instead of a full sequence.
  **Later simplified, by a dedicated code review of this file, to
  `QFormLayout::setRowVisible()`** (Qt 5.15+, and this project requires
  Qt6) in place of the manual `setVisible()` + `layout()->activate()` +
  `adjustSize()` dance above — confirmed with a standalone probe before
  relying on it that `setRowVisible()` alone already updates the row's
  contribution to the layout's size hint synchronously, so `adjustSize()`
  right after it (still guarded on `isVisible()`, for the exact reason
  above — that part of the original fix's reasoning is unaffected by
  this) resizes correctly on the very first live switch, same as the
  two-iteration fix above needed `layout()->activate()` for. Re-verified
  directly with the same three-transition sequence this bug was
  originally caught with, now automated in `protocol-selection-test`
  rather than only ever checked by screenshot.
  **Confirmed on a real Windows build, not just Linux — the actual point
  of this pass, not an afterthought:** cross-compiled via MinGW and run
  under `wine` against a real KDE desktop already available in this
  environment (no `Xvfb` needed — see CONTRIBUTING.md's note that a real
  graphical session makes it unnecessary), the same live-switch sequence
  produces the same correct shrink/grow on the actual Windows binary,
  not just the Linux one. The same pass also turned up two more
  Windows-specific renders worth recording honestly rather than either
  fixing blindly or ignoring: (1) the Site Manager's Starting-directory
  radio pair visually *appeared* to show "Home directory" checked instead
  of "Specific directory" for a site saved with `useHomeDirectory=false`
  — investigated directly rather than trusted, by reading
  `isChecked()` on both radios through `findChildren<QRadioButton*>()`
  rather than eyeballing the screenshot, which confirmed the actual
  widget state is correct (`homeDirRadio=false`, `specificDirRadio=true`)
  on *both* platforms; `theme.qss` has no radio-button styling at all
  (confirmed by grep), so this is Qt's native-style indicator painting
  under Wine specifically, not an app bug — but also not confirmed
  innocent on genuine Windows hardware, since Wine has already shown
  real, documented gaps from true Windows fidelity elsewhere in this
  project (the `qDebug()`-doesn't-reach-terminal quirk in CONTRIBUTING.md).
  (2) `QMessageBox`'s standard Warning/Question icons rendered as a
  blank placeholder glyph under `wine` instead of the expected
  triangle/question-mark — plausibly Wine's emulation of the native
  Windows system-icon lookup `QMessageBox` uses under the "windows"
  style, entirely inside Qt itself with nothing in this app's own code
  to fix either way, but again not something a real Windows machine has
  been available to confirm or rule out. Both flagged here rather than
  silently accepted or silently "fixed" against a guess, per this
  project's own rule: say so explicitly when something genuinely can't
  be verified in the environment at hand, rather than letting silence
  read as a claim of correctness.
  **Four more real issues found by a dedicated code review of this
  file, all fixed:**
  1. **`portIsUntouchedDefault()` could silently discard a deliberately-
     typed port, a real data-loss bug.** It inferred "the user hasn't
     typed their own port" by checking the current value against ALL
     THREE protocols' defaults (22 for SFTP, 21 for both FTP and FTPS) —
     but FTP and FTPS share 21 with a value a user might genuinely type
     under SFTP, and the heuristic can't tell the two apart. Concretely:
     SFTP (22) -> type 21 (a real, deliberate choice) -> switch to FTP
     (still 21, no visible change, "confirmed" untouched) -> switch back
     to SFTP silently reset it to 22. Fixed by replacing the heuristic
     entirely with a real dirty flag (`m_portManuallyEdited`) set only by
     a genuine `QSpinBox::valueChanged` from user interaction — every one
     of `onProtocolChanged()`'s own programmatic `setValue()` calls is
     wrapped in a `QSignalBlocker` specifically so it can never be
     mistaken for one. Covered by `protocol-selection-test`'s own
     SFTP -> FTP -> SFTP round trip with a deliberately-typed 21.
  2. `browseForPrivateKey()` duplicated the identical
     `QFileDialog::getOpenFileName()` "Browse..." logic already in
     `SiteManagerDialog.cpp`. Extracted to
     `FileDialogs::pickPrivateKeyFile()` (`src/ui/FileDialogs.h`) — a
     small header-only shared helper, returning the picked path rather
     than taking a target `QLineEdit*` directly, since the two callers'
     follow-up behavior had already drifted (`SiteManagerDialog` also
     calls its own `onFieldEdited()` afterward, which `ConnectionDialog`
     has no equivalent of) — this unifies only the part that was
     genuinely identical.
  3. `onProtocolChanged()`'s forced-Password `setChecked(true)` (for
     FTP/FTPS, which have no key auth) had no `QSignalBlocker`, unlike
     `SiteManagerDialog`'s identical reset. Harmless today
     (`updateAuthFieldsVisibility()` is idempotent), but guarded to match
     — a silent double-fire waiting to matter the moment either slot
     stops being idempotent.
  4. Username was read via plain `.text()` in both `ConnectionDialog`
     and `SiteManagerDialog`, while host is always `.trimmed()` — pasting
     a username with invisible whitespace (common when copying from a
     credentials email or spreadsheet) connected fine but failed auth
     with a generic wrong-username/password error, no hint the
     whitespace was the actual cause. Trimmed in both files' credential-
     building code paths (four call sites total) to match host's
     existing convention.
- `SavedSite` / `SiteStore` (`src/backends/SavedSite.h/.cpp`) — a saved
  connection profile (host/port/username/auth method/key path, optional
  starting directory, optionally grouped into a folder) and its JSON
  persistence (`QStandardPaths::AppConfigLocation/sites.json`).
  **Still has no password field, and never will** — `sites.json` stays
  permanently secret-free, verified directly by `site-store-test.cpp`'s
  raw-JSON key inspection. What changed: a password/passphrase CAN now
  be remembered, opt-in via `SiteManagerDialog`'s "Save password"
  checkbox, but it's written to `CredentialStore` (the OS's own
  credential store) instead, keyed by this struct's `id` — never to this
  file. See `CredentialStore` below for the full reasoning, and
  `SavedSite.h`'s own doc comment for why this is a narrower, more
  deliberate promise than "no passwords, full stop" used to be, not an
  abandonment of it. `useHomeDirectory`/`startingDirectory` (both
  mirrored onto `SftpCredentials`, consumed by
  `SftpBackend::ensureSession()`) let a site skip the default
  home-directory resolution and land somewhere specific instead — not
  validated at save time; an invalid path surfaces through the same
  `listDirectory()`/`connectionFailed` error path as typing a bad path
  into the pane's own path bar.
  **A real bug found by code review: `SiteStore::load()` backfilled a
  missing `id` for legacy entries, but never checked for (or deduped) an
  `id` that was already a duplicate of another entry's.** Every id-based
  lookup elsewhere does a linear first-match search
  (`SiteManagerDialog::selectedSite()`, in particular), so a hand-edited
  or otherwise corrupted `sites.json` with two entries sharing an id
  would silently route an action on the SECOND colliding row to the
  FIRST site instead of the one actually selected. Fixed with a
  dedup pass after the existing backfill loop: the first occurrence of a
  given id keeps it, any later entry sharing that id gets a fresh one,
  so every site `load()` returns is guaranteed genuinely unique.
  Confirmed with a real regression test in `site-store-test`: a
  hand-written `sites.json` with two entries both using `"id":
  "duplicate-id"` comes back with two distinct ids (the first keeping
  the original, confirmed via a real pre-fix control that the old code
  returned both entries still sharing the same id).
  `load()`/`save()` are now thin wrappers (`load() { return
  loadFromFile(filePath()); }`) around two new path-parameterized
  primitives, `loadFromFile(path)`/`saveToFile(sites, path)` — what
  `SiteManagerDialog`'s Export.../Import... buttons actually call, on
  an arbitrary user-chosen path rather than the fixed `sites.json`
  location. Every existing call site keeps its exact original
  zero-arg signature and behavior unchanged; the refactor's only
  effect is that all of `load()`'s defensive parsing (missing-field
  defaults, the duplicate-id dedup pass above, failing soft to an
  empty list on a corrupt/unreadable file) now also applies for free
  to an imported file — exactly the tolerance a hand-edited or
  foreign/older-version export wants, with no separate import-parsing
  code to write or get wrong. The export format is deliberately
  `SavedSite`'s own existing JSON schema, not a new interchange
  format — already documented, and already secret-free **by
  construction**, not by a stripping step this feature has to get
  right: `SavedSite` has no password/passphrase field to accidentally
  serialize in the first place (see this entry's own opening
  paragraph), a materially stronger guarantee than FileZilla's own XML
  site export, which does store passwords and has to be told not to.
  The one real design question import raises is what happens to an
  imported site's `id`, since it's also the `CredentialStore` lookup
  key: `SiteManagerDialog::onImportSites()` always regenerates it
  (`QUuid::createUuid().toString(QUuid::WithoutBraces)`, the exact same
  idiom `onNewSite()`/`onDuplicateSite()` already use for the same
  reason) — never reused from the file, both because it could collide
  with a real local site's id and because any `CredentialStore` secret
  named by the original id was stored on the *exporting* machine's own
  OS credential store and could never resolve on this one anyway.
  Imported sites are appended to the existing list, never replacing it
  — importing must never destroy what's already saved locally.
- `CredentialStore` (`src/backends/CredentialStore.h/.cpp`) — the OS
  credential store, opt-in, and the only place a secret from this app
  is ever written to disk. `save`/`load`/`remove`/`hasSecret`, keyed by
  a `SavedSite`'s `id`. Three platform backends behind `#ifdef _WIN32`
  / `#elif defined(__APPLE__)` / `#else` (one file, not separate ones —
  the amount of platform-specific code is small enough that CMake
  source-list conditionals would be more ceremony than the split is
  worth): libsecret on Linux (the freedesktop Secret Service — GNOME
  Keyring, KWallet's compatibility layer, whichever the desktop
  provides), the real Win32 Credential Manager API (`wincred.h`,
  `CredWriteW`/`CredReadW`/`CredDeleteW`) on Windows, and Keychain
  Services (`Security.framework`) on macOS. The macOS backend keys a
  `kSecClassGenericPassword` item on a fixed `kSecAttrService`
  (`"ZephyrFTP"`) plus `kSecAttrAccount` = the site id — the direct
  analog of Windows' namespaced `targetName()` and Linux's `site_id`
  schema attribute. `SecItemAdd()` alone doesn't overwrite an existing
  item the way `CredWriteW()`/`secret_password_store_sync()` both do
  unconditionally, so `save()` falls back to a `SecItemUpdate()` call
  on `errSecDuplicateItem` to satisfy that same "overwrites any
  existing secret" contract. **Confirmed working for real** on a
  genuine macOS GitHub-hosted runner (this sandbox has no macOS
  hardware, so this is the only verification available, unlike the
  Linux/Windows backends below which have also been checked directly
  in this environment): `verify-credential-store`'s full
  save/load/hasSecret/remove round trip, including non-ASCII content,
  passed inside the `build-macos` CI job. It's the last target in that
  job's required-suite run, so `set -e` meant it didn't actually get a
  chance to execute at all until the three unrelated, pre-existing
  test-timing bugs earlier in the list were fixed (see "Windows,
  macOS, and Linux builds (CI)" above) — those bugs blocked this
  target from running, they weren't bugs in it. Deliberately NOT a
  bundled cross-platform wrapper library — Fedora ships
  `qtkeychain-qt6` for native Linux, but only a Qt5 build for the
  mingw64/Windows cross-target, a real ABI mismatch with this project's
  Qt6 Windows build — so this follows the same
  direct-on-the-platform-API approach already used elsewhere
  (`SftpBackend` on libssh2 directly, `FtpBackend` hand-rolled instead
  of libcurl) rather than fighting a packaging gap. Deliberately NOT the
  weaker pattern FileZilla (`sitemanager.xml`, Base64 — obfuscation, not
  encryption) and WinSCP (without its optional master password) both
  use by default, and have both been publicly criticized for — real
  secret storage lives entirely in the OS's own protected store, not in
  a file this app controls. One real header-collision bug caught
  building this: libsecret transitively pulls in glib/gio headers
  declaring a struct member literally named `signals`, which collides
  with Qt's `signals:` macro the instant any Qt header has already been
  parsed — fixed by including `<libsecret/secret.h>` before
  `"CredentialStore.h"` in the `.cpp`, not by fighting the macro with
  `QT_NO_KEYWORDS`. **Confirmed working for real, not just compiling**,
  on Linux: a full save/load/overwrite/remove round trip against the
  real local D-Bus secret service (this development environment
  actually has one running — KDE's `ksecretd`), independently
  cross-checked with the `secret-tool` CLI (not just this app's own
  code self-reporting success) — real entry, correct schema, correct
  secret value. **The Windows `wincred.h` path is now confirmed too, on
  real Windows hardware, not just compiling/linking cleanly (including a
  full mingw cross-build) and running without crashing under `wine`.**
  The `CredWriteW`/`CredReadW`/`CredDeleteW` round trip has been manually
  confirmed saving and reloading a real password through the actual
  Windows Credential Manager, closing what had been the same category of
  gap already flagged for other Windows-specific code in this project.
  **Three issues found by a dedicated code review of this file — its
  first, despite being the one file this project's own CLAUDE.md singles
  out as security-critical:**
  1. **`SiteManagerDialog::onConnectClicked()` discarded `save()`'s bool
     return value entirely.** A failed save (a locked/unreachable Secret
     Service on Linux, or Windows's documented 2560-byte
     `CredentialBlob` size cap) went silently unnoticed — the checkbox
     stayed checked and the user believed the secret was saved, only
     discovering otherwise on next launch when `hasSecret()` correctly,
     but unexplainedly, unchecked it. Fixed with a `QMessageBox::warning`
     on a failed save, explaining what happened rather than staying
     silent.
  2. **The Linux implementation encoded `site_id` (the libsecret schema
     attribute used to find a secret again) via `qPrintable()`
     (`QString::toLocal8Bit()`) instead of explicit `toUtf8()`,
     inconsistently with `secret` (already `toUtf8()`, from an earlier
     review round).** The concern raised: a hand-edited or migrated
     `sites.json` supplying a non-ASCII id (`SavedSite`'s JSON loader
     does no format validation beyond empty/duplicate checks) could
     encode differently between a `save()` and a later `load()`/
     `remove()`, breaking the match. **Investigated and found NOT
     actually reachable, verified directly rather than assumed**: a
     standalone probe confirmed `QString::toLocal8Bit()` and `toUtf8()`
     produce byte-identical output under Qt6 regardless of the process's
     C-library locale (`LC_ALL=C`, `en_US.ISO-8859-1`, and
     `en_US.UTF-8` all tested) — Qt6 dropped the old locale-dependent
     "local 8-bit" behavior entirely, so `qPrintable()` and `toUtf8()`
     are provably identical on this codebase's Qt6 baseline. The switch
     to explicit `toUtf8()` for `site_id` was kept anyway (a
     clarity/robustness improvement that doesn't depend on that Qt6
     behavior staying true forever) but is NOT a live-bug fix, unlike
     item 1 above — corrected here rather than left as an overclaimed
     "real bug," per this project's own "verify rather than assume"
     rule (see CONTRIBUTING.md).
  3. `targetName()` (Windows-only) was defined unconditionally in the
     anonymous namespace, triggering a `-Wunused-function` warning on
     every Linux/non-Windows build. Fixed by moving its definition
     inside the existing `#ifdef _WIN32` block.
  **First-ever test coverage for this file**, added alongside: a new
  `verify-credential-store` target (in the `verify-*` live-service
  family, not one of the required self-contained `EXCLUDE_FROM_ALL`
  targets —
  its "external precondition" is a real, already-in-use OS credential
  store rather than a disposable local server) exercises a full
  save/load/hasSecret/remove round trip, including non-ASCII id and
  secret content, against the real Secret Service. Writes and
  unconditionally removes its own clearly-namespaced test entries, safe
  to run against a real, in-use keyring.
- `SiteManagerDialog` — the saved-sites UI: a grouped tree on the left,
  a details form on the right, matching the design package's
  site-manager.html mockup, plus a starting-directory radio choice
  (Home / Specific) the mockup didn't have. Persists via `SiteStore` on
  every field edit (`QLineEdit::editingFinished`, not per-keystroke) and
  every structural change (new/duplicate/delete), so there's no separate
  "Save" step to forget. Groups are organized via an editable `QComboBox`
  (`m_groupCombo`) next to the site name — pick an existing group from
  the dropdown or type a new one to create it on the spot; there's no
  separate "groups" collection to manage, a group exists precisely when
  at least one site references it. Changing a site's group triggers a
  full `rebuildTree()` (hierarchy actually changed) rather than the
  simple in-place item-text update every other field edit uses.
  Its Connect button still always prompts for the password or key
  passphrase, same as before — what's new is `m_savePasswordCheck`
  (unchecked by default, one checkbox shared across both auth pages
  since a site only ever has one relevant secret at a time), which does
  three things depending on what's actually true at Connect time: pre-fills
  that prompt from `CredentialStore` when this site has a saved secret
  (so accepting it is one click, not a retype), saves whatever was
  actually entered/edited there when the box is checked (which is also
  how an already-saved secret gets updated — there's no separate "edit
  the saved password" field, the connect-time prompt does double duty),
  and removes anything stored the instant the box is unchecked, without
  waiting for a Connect click. The prompt itself is never skipped
  outright even when a secret is saved — deliberately: this stays a
  real, visible, one-click confirmation rather than a silent
  auto-connect, consistent with this project's refusal to do anything
  credential-related invisibly.
  **A real bug found by code review: `commitFormToSelectedSite()`
  persisted to `sites.json` with no equivalent of `onConnectClicked()`'s
  own validation.** Two real, if narrow, ways to trigger it: clearing
  the Host field (e.g. select-all then retype) and losing focus before
  finishing — `m_hostEdit` commits on `editingFinished`, not
  per-keystroke, but clicking away mid-retype still fires it with an
  empty value — or switching straight to the "Specific directory" radio,
  which (unlike the text fields) commits immediately on toggle, before
  any path has been typed. Either hit disk instantly with an unusable
  value, silently overwriting whatever was there before with nothing to
  recover it if the person then navigated away mid-edit. `name` already
  had an equivalent safeguard for this exact class of problem (falls
  back to "Untitled Site" instead of persisting empty) — fixed by giving
  `host`/`startingDirectory` the same protection, just by skipping the
  disk write itself rather than substituting a placeholder value (no
  sensible placeholder host exists the way "Untitled Site" does for
  name). The in-memory `SavedSite` this function edits still reflects
  the true current form state either way, so `onConnectClicked()`'s own
  validation (which calls this function first) still correctly sees and
  rejects an empty host/starting-directory rather than silently
  connecting with a stale one — only the disk write is deferred, not the
  in-memory model. **Not covered by an automated test** — the dialog's
  form fields are private with no test-oriented accessor, and adding one
  purely for this would mean changing production code beyond the fix
  itself; verified by direct reasoning through the exact
  `onConnectClicked()` interaction instead (confirmed a naive
  "just skip updating `site->host` too" version would have silently
  reconnected using a stale host instead of showing "Host cannot be
  empty," before landing on deferring only the disk write).
  **A real, reported layout bug, fixed:** the checkbox row added real
  height the dialog's original fixed `resize(700, 440)` (set before
  this feature existed) didn't account for — confirmed directly by
  measuring actual rendered geometry, not eyeballing: Qt's layout
  engine compressed the starting-directory field to 20px against its
  own 33px `sizeHint()` to make everything fit, while every sibling
  `QLineEdit` still got its full height. Fixed by growing the dialog to
  `resize(700, 520)`, the exact height (found by testing, not guessed)
  where the field stops being squeezed. Separately, `m_portSpin` itself
  rendered 3px taller than every `QLineEdit` beside it (36px vs. 33px)
  under the identical QSS padding/border rules — `QAbstractSpinBox`
  reserves sizeHint space for its spin-button area even with
  `NoButtons` hiding it visually. Fixed with
  `m_portSpin->setFixedHeight(m_hostEdit->sizeHint().height())` —
  matched to a sibling field's actual height rather than a hardcoded
  pixel count, so it stays correct if the theme's font/padding changes.
  **The same squeeze bug recurred (live-reported, v0.7.26)**: the
  "Simultaneous connections" row added in v0.7.24 pushed the dialog's
  own real `sizeHint()` past the fixed `resize(700, 520)` again (to
  694x527, confirmed directly) — the starting-directory field was the
  one Qt's layout engine reached for again to absorb the shortfall
  (26px against its own 33px `sizeHint()` at height 520, full height
  once actually resized larger, matching the user's own report almost
  exactly: "I can resize the site manager dialog, and it will be
  correct"). Root cause, confirmed empirically, not just inferred from
  the symptom repeating: `dirFieldColumn` (the starting-directory
  radio-buttons-plus-lineedit field) is the one row in this form that's
  a `QVBoxLayout` passed to `QFormLayout::addRow()`, not a plain
  widget — every other row's `QLineEdit` has a `Fixed` vertical size
  policy `QFormLayout` won't shrink below `sizeHint()` even when the
  dialog is a few pixels short overall, but a nested layout used as a
  row's "field" doesn't get that same hard floor, so it's what silently
  absorbs the deficit instead. Fixed by growing the dialog again, this
  time to `resize(700, 540)` — deliberately past the bare 527px
  `sizeHint()` with real headroom, specifically so the *next* row this
  dialog gains doesn't reopen the identical bug a third time before
  someone happens to resize the window and notice. Confirmed via the
  same disposable, non-committed `QWidget::grab()` probe technique as
  the v0.7.25 spinbox-height fix above: measured 26px before, 33px
  (matching `sizeHint()` exactly) after, at the dialog's own default
  construction-time size.
  **Five real bugs found by a dedicated code review of this file and
  `CredentialStore`, all fixed:**
  1. **Deleting a saved site never removed its stored secret.**
     `onDeleteSite()` removed the site from `sites.json` but never called
     `CredentialStore::remove()` — a saved password/passphrase orphaned
     permanently in the OS credential store with no id left in
     `sites.json` to ever look it up or remove it again. Fixed by calling
     `CredentialStore::remove()` unconditionally in `onDeleteSite()`,
     same as the existing "checkbox unchecked" path already does
     (`remove()` is a harmless no-op when nothing was actually stored).
  2. **Switching a saved site's auth method (Password <-> Private key —
     including indirectly, via protocol changes that force Password for
     a protocol with no key auth) left an already-stored secret
     untouched under the checkbox's OLD meaning.** A password and a
     passphrase are different kinds of secrets; `onConnectClicked()`
     would pre-fill the new prompt with the old one as if it were the
     right kind, and accepting that pre-fill as-is would silently
     overwrite the stored secret with the wrong value. Fixed in
     `updateAuthFieldsVisibility()`: compares the radio buttons' current
     state against the *selected site's own currently persisted*
     `authMethod` (not just "did the radio fire," which happens
     constantly, including harmlessly while a different site's data is
     being loaded into the form) and unchecks `m_savePasswordCheck` — its
     own existing `toggled` handler removes the stale secret — only on a
     genuine divergence.
  3. **`CredentialStore::save()`'s libsecret backend encoded the secret
     with `qPrintable()` (the local 8-bit encoding) while `load()`
     decoded with `QString::fromUtf8()`.** Identical bytes on a UTF-8
     locale (the common case, and why this went unnoticed), but a real,
     silent corruption of any non-ASCII password/passphrase on a system
     whose locale isn't UTF-8. Fixed by encoding the secret specifically
     (not the site id or label, both always this app's own ASCII text)
     with `.toUtf8()` explicitly, matching `load()`'s decode.
  4. **`onConnectClicked()` called `CredentialStore::hasSecret()` and
     then `CredentialStore::load()` separately** — since `hasSecret()` is
     itself implemented on top of `load()` (per `CredentialStore.h`'s own
     documented contract), this performed the exact same OS
     credential-store lookup twice for one logical operation. Harmless
     with an already-unlocked keyring, but a real, avoidable cost: a
     locked keyring can prompt the user to unlock it on each lookup, so
     this could have shown that prompt twice in a row for a single
     Connect click. Fixed by calling `load()` once and using its own
     return value, dropping the redundant `hasSecret()` call.
  5. **`CredentialStore`'s Windows `hasSecret()` reimplemented its own
     `CredReadW`/`CredFree` pair instead of delegating to `load()`**,
     contradicting `CredentialStore.h`'s own documented contract
     ("implemented on top of `load()` on both platforms") and diverging
     from the Linux implementation, which already delegates correctly.
     Not a runtime bug on its own, but a real maintenance trap: a future
     fix to `load()` (error handling, encoding) wouldn't automatically
     have applied here too. Fixed to delegate, matching Linux.
  6. **`onTreeSelectionChanged()`/`loadSiteIntoForm()` called
     `CredentialStore::hasSecret()` fresh on every single tree-item
     click, directly on the UI thread** — an efficiency finding from a
     later code review, not a correctness bug: a locked/slow OS keyring
     can turn every click while browsing the site tree into a real,
     synchronous stall (an OS unlock prompt, or just a slow libsecret
     D-Bus round trip / `SecItemCopyMatching` / `CredReadW` call), worse
     than a one-time cost since nothing was cached across visits to the
     same site within one dialog session. Fixed with `m_hasSecretCache`
     (`QHash<QString, bool>`, keyed by site id) — populated lazily the
     first time each site is selected, and kept in sync (not just
     invalidated) at every one of the three points this dialog itself
     changes a site's stored secret (the checkbox's own `toggled`
     handler, `onConnectClicked()`'s save/remove, `onDeleteSite()`'s
     cleanup), so a later revisit never reads a stale value. Verified
     with a disposable, non-committed harness (same "build a throwaway
     target, run it, then fully revert" technique used elsewhere in this
     project for something needing a real environment dependency) — a
     real site with a real stored secret (via the real local Secret
     Service, confirmed available and used, then cleaned up) correctly
     stayed checked across repeated re-selection, and correctly flipped
     to unchecked — not stale-cached true — immediately after the
     checkbox was unchecked and the site re-visited.
  **Not covered by a PERMANENT automated test**: none of the six fixes
  above have regression coverage that ships with this codebase —
  `CredentialStore` writes to the real OS credential store with no
  test-friendly override the way `SiteStore` has
  `QStandardPaths::setTestModeEnabled()`, so a permanent automated test
  would leave real test secrets in whoever's keyring runs it, the same
  category of risk this project avoids elsewhere (e.g. `site-store-test`'s own config
  isolation). Fixes 1-5 were verified by direct code reading and
  reasoning through each call path instead, consistent with this
  project's "say so explicitly rather than letting it read as verified"
  standard where a real test isn't a safe option; fix 6 above was the
  first of the six to actually get a real (if disposable, not committed)
  run against the genuine local Secret Service.
  Gained **Export.../Import...** buttons later, below Duplicate/Delete
  (`arrow-up.svg`/`arrow-down.svg`, reused from `TransferQueueWidget`'s
  own upload/download-direction icons — same "data leaving/entering the
  app" metaphor, a different UI surface that never appears alongside
  it). `onExportSites()` writes **every** saved site (not the current
  selection/folder — the primary use case is a full backup/migrate, and
  scoping to a selection is real UI surface not needed for that) via
  `SiteStore::saveToFile()` to a user-chosen path
  (`FileDialogs::pickSitesExportFile()`, the first `getSaveFileName()`
  call site in this codebase). `onImportSites()` reads a file back via
  `SiteStore::loadFromFile()`, regenerates every imported site's `id`
  (see the `SiteStore`/`SavedSite` entry above for why that's not
  optional), and appends to `m_sites` — merges with the existing list,
  never replaces it — followed by the same `SiteStore::save(m_sites)` +
  `rebuildTree()` pair every other mutation here already ends with.
  Neither button has direct test coverage of its own click handler —
  matching this dialog's own existing, established precedent (New/
  Duplicate/Delete aren't click-tested either, only their underlying
  `SiteStore` behavior is); the actual algorithm (`loadFromFile()` +
  id regeneration + merge) is covered directly in `site-store-test`
  instead, without constructing a `SiteManagerDialog` — the first
  direct test coverage of any kind for that specific class remains
  none, unchanged by this addition.
- `AppSettings` (`src/AppSettings.h/.cpp`) — app-wide preferences, the
  first general settings mechanism this project has had (there was
  nothing to persist before this). Follows `SiteStore`'s own convention
  exactly: a hand-written JSON file (`settings.json`) in
  `QStandardPaths::AppConfigLocation`, not `QSettings` — one persistence
  mechanism across the app, not two. Unlike `SiteStore` (stateless static
  `load()`/`save()` functions), this is a `QObject` with a real
  lifetime, owned by `MainWindow` for the app's whole run: `showHiddenFiles`
  needs to propagate live to both `FilePaneWidget`s the instant it's
  toggled in `PreferencesDialog`, via `showHiddenFilesChanged`, without
  either pane re-fetching its listing from the backend.
  `windowGeometry`/`windowState` are opaque `QMainWindow::saveGeometry()`/
  `saveState()` blobs, base64-encoded into the same JSON file, read once
  at startup and written once from `MainWindow::closeEvent()` — no live
  propagation needed for either, unlike `showHiddenFiles`.
  Transfers/Commands dock visibility has no dedicated preference field
  at all — each dock's own `toggleViewAction()` (the View menu's
  "Transfers"/"Commands" entries, kept in sync by Qt with however the
  dock was actually shown/hidden, whether via that menu entry or the
  dock's own titlebar close button) is the sole source of truth,
  persisted for free as part of the `windowState` blob `saveState()`
  captures in `closeEvent()`. This replaced an earlier
  `showTransfersOnStart`/`showCommandsOnStart` pair of preference
  fields, settable only from `PreferencesDialog`, applied as an
  explicit override right after `restoreState()` ran in `MainWindow`'s
  constructor — which meant the live View-menu toggle never actually
  controlled what reopened on the next launch, only that separate,
  easy-to-forget-about checkbox did. Removed once it became clear
  `restoreState()` alone already reproduces whatever dock-visibility
  state existed at the moment `saveState()` ran — confirmed directly
  with a disposable probe (two `QDockWidget`s, one hidden before
  `saveState()`, a second window's fresh `restoreState()` call
  reproducing exactly that visibility with no manual override needed)
  before deleting the redundant mechanism.
  **A real, verified bug, not a hypothetical one:** the first working
  version filtered `showHiddenFiles` only in `FilePaneWidget`, on the
  theory that every backend returns dotfiles raw and the UI layer
  decides visibility centrally. A throwaway verification harness (not
  committed — this project's established pattern for one-off functional
  checks) constructed a real `FilePaneWidget` over a real `LocalBackend`
  pointed at a real temp directory containing a dotfile, toggled
  `showHiddenFiles` live, and found the toggle did nothing for the local
  pane specifically: `LocalBackend::listDirectory()`/
  `listDirectoryForEnumeration()` both called `QDir::entryInfoList()`
  without `QDir::Hidden`, so a dotfile never reached `FilePaneWidget` in
  the first place, filtered or not — while `SftpBackend`/`FtpBackend`
  never excluded dotfiles from their own listings to begin with. Fixed
  by adding `QDir::Hidden` to both `LocalBackend` call sites, making
  "return everything, filter for display centrally" actually true across
  all three backends, not just two of them. This also surfaced (and
  fixed, in the same change) a related latent inconsistency: local
  whole-folder transfers previously skipped a source folder's dotfiles
  silently, while SFTP/FTP transfers already included them — enumeration
  now includes dotfiles unconditionally on all three backends regardless
  of the display setting, since "hidden from the browsing view" and
  "excluded from what actually gets transferred" were never meant to be
  the same thing. Re-run after the fix: all three toggle assertions
  passed, including the live re-render with no fresh `listDirectory()`
  round-trip.
  `FilePaneWidget::rebuildModel()` is the one place that now applies this
  filter, keeping `m_currentEntries` — which every row-indexed method
  (`onRowDoubleClicked`, `selectedFileNames()`, `selectedEntries()`, the
  context menu) relies on matching the view row-for-row — as the
  *filtered* set, while a separate `m_lastRawEntries` holds everything
  the last `listDirectory()` actually returned, so a live toggle can
  re-filter and redraw instantly without a pointless backend round-trip.
  A per-pane filename filter (`m_filterEdit`, a `QLineEdit` below the
  path bar, object name `"filterEdit"` so tests can find it unambiguously
  now that the pane has two `QLineEdit`s) composes in the exact SAME
  predicate as `showHiddenFiles` above, not a separate pass — case-
  insensitive `QString::contains()`, deliberately not wildcard/regex.
  There is no `QSortFilterProxyModel` anywhere in this codebase — column
  sorting is done directly on `m_model` via `QStandardItem::operator<`
  overrides (see below) — so the filter follows the same "filter the raw
  cached entries, then rebuild the model" shape `showHiddenFiles`
  already established, rather than introducing a proxy-model layer.
  Live, as-you-type (`textChanged` → `rebuildModel()`), the same
  mechanism `showHiddenFilesChanged` already triggers; confirmed a
  non-issue performance-wise for this app's realistic directory sizes,
  since `rebuildModel()` already does a full clear-and-rebuild of the
  model on every hidden-files toggle today, same order of work.
  `setFilterFieldVisible(bool)` (public) is called by `MainWindow`'s
  View-menu toggle on *each* pane independently — hiding also clears
  the filter text (firing `textChanged` → `rebuildModel()` on its own),
  since a hidden field must not leave a stale, invisible filter
  silently narrowing the list with no visible control left to clear
  it. Initial visibility is read directly from `AppSettings::
  filenameFilterVisible()` in the pane's own constructor — unlike
  `MainWindow`'s quick-connect field (which had to be built inside
  `buildMenuBar()` specifically so the View menu's own toggle could
  reference the one widget that needed to exist by that point), each
  pane already owns its `AppSettings*` and is constructed well before
  `buildMenuBar()`'s toggle is ever actually triggered by a real user
  action, so there's no equivalent ordering hazard here.
  **A real bug found by a dedicated code review of this file — its
  first, and its first-ever test coverage of any kind:** `save()` wrote
  `settings.json` in place (`QFile::open(WriteOnly | Truncate)`), so a
  crash, power loss, or a full disk mid-write could leave a truncated
  file behind. That mattered more than it might for an ordinary
  settings file: `load()`'s own "corrupt or unexpected content — fail
  soft to defaults" policy treats ANY parse failure as total corruption,
  so a truncated file from an interrupted write to just ONE preference
  (say, window geometry on close) would silently reset every OTHER
  already-saved preference back to hardcoded defaults on the next
  launch too — not a hypothetical, since every setter in this class
  calls `save()` on every single change, including live window
  resizing. Fixed with `QSaveFile` (writes to a temporary file first,
  only atomically replaces the target on a successful `commit()`) in
  place of `QFile` — confirmed directly with a standalone probe, not
  just assumed from documentation, that an abandoned `QSaveFile` (never
  committed, simulating a crash) leaves the original file completely
  untouched, while a committed one replaces it correctly. That specific
  crash-mid-write recovery isn't exercised by an automated test —
  `save()` is private and always commits on success from inside
  `AppSettings`' own public API, so there's no seam to interrupt it
  through short of adding a test-only hook that doesn't otherwise need
  to exist — documented honestly as verified-by-probe rather than
  test-covered, per this project's own "say so explicitly" rule (see
  CONTRIBUTING.md). What the new `app-settings-test` DOES cover for
  real: the full setter/save/load round trip for every field (the
  actual regression risk of swapping write primitives), the documented
  fresh-start and corrupt-file fallback-to-defaults behavior, and that a
  same-value `set()` call is a genuine no-op rather than silently
  re-writing defaults over a real saved value. Also fixed in the same
  pass: `save()` was calling `QStandardPaths::writableLocation()` twice
  per invocation (once directly, once again inside `filePath()`) —
  minor, but redundant on every single preference change.
  Also gained `proxyType`/`proxyHost`/`proxyPort`/`proxyUsername`
  (plain fields, same `settings.json` treatment as everything else
  here) and `proxyPassword()`/`setProxyPassword()`/
  `resolvedProxyConfig()` — see the `ProxyConfig` entry above for the
  full story, including why the password specifically does NOT follow
  this class's own `settings.json` pattern.
  Also gained `quickConnectFieldVisible` (bool, default `true` —
  see `MainWindow`'s entry above for the full story of why this field
  exists at all: `QMainWindow::saveState()` doesn't capture an
  arbitrary toolbar widget's visibility the way it does a dock's).
  Also gained `filenameFilterVisible` (bool, default `true`, same
  shape) — controls BOTH panes' filter-field visibility at once (see
  `FilePaneWidget`'s entry above), though each pane's own filter text
  is never persisted here — only whether the row is shown at all.
- `PreferencesDialog` (`src/ui/PreferencesDialog.h/.cpp`) — a "Show
  hidden files" checkbox, a default-protocol combo box, and two more
  checkboxes added later ("Show Transfers pane on start", "Show Commands
  pane on start", both mirroring `AppSettings`' matching fields above) —
  the only real settings that exist to show, and nothing invented just to
  fill the dialog out. Every field persists to
  `AppSettings` the instant it changes (same immediate-persist
  convention `SiteManagerDialog` already uses — "no separate Save step to
  forget"), so the only button is Close. Verified with the same
  screenshot-and-pixel-sample technique proven on `ConnectionDialog`/
  `SiteManagerDialog` earlier (a throwaway harness, not committed): the
  dialog's background sampled `rgb(20,23,28)`, matching the `#14171c`
  dark-theme token, confirming it doesn't repeat the earlier
  dialogs-ignoring-the-stylesheet bug now that a third dialog exists to
  potentially get it wrong again.
  **A dedicated code review of this file found no correctness bugs — the
  construction order (every `setChecked()`/`setCurrentIndex()` runs
  before its `connect()`, so no spurious settings writes fire during
  construction) and every `AppSettings` setter's own `if (old == new)
  return;` guard were both already correct — but did find a real,
  three-way duplication worth fixing:** `ConnectionDialog`,
  `SiteManagerDialog`, and this dialog each hand-wrote the identical
  three-`addItem()` protocol-combo population pattern. A fourth
  `Protocol` value would have needed the same line added by hand at all
  three call sites, with a silently-out-of-sync combo the cost of
  missing one. Extracted to `ProtocolCombo::populate()`
  (`src/ui/ProtocolCombo.h`, new) — deliberately NOT folded into
  `Protocol.h` itself, since that header is included from
  `AppSettings.h`, which is linked into Core-only targets like
  `app-settings-test` (no `Qt6::Widgets`); a `QComboBox` dependency
  there would have broken that build. Confirmed non-regressing via all
  three dialogs' existing tests (`protocol-selection-test` especially,
  which drives `ConnectionDialog`'s combo directly) plus a full local
  suite run, all still passing. Also removed an unused `#include
  <QLabel>` (this dialog never constructs one directly —
  `QFormLayout::addRow(QString, QWidget*)` builds its own labels).
  Also gained a Proxy section — type combo (None/SOCKS5/HTTP), host,
  port, username, password — same immediate-persist convention as
  every other field here, routed to `AppSettings`' new proxy
  getters/setters (see that entry and the `ProxyConfig` entry above).
  The four detail fields disable themselves when type is None
  (`updateProxyFieldsEnabled()`), verified visually via the same
  disposable offscreen-probe screenshot technique used elsewhere in
  this file's history — confirmed both states (a populated SOCKS5
  config with all fields enabled; the None default with all four
  detail fields correctly greyed out).
- `HostKeyVerifier` — lives on the GUI thread for the app's lifetime.
  `SftpBackend`'s worker thread calls into it via
  `QMetaObject::invokeMethod(..., Qt::BlockingQueuedConnection)` to get a
  synchronous host-key trust decision from a real person — the standard
  Qt pattern for a background thread needing a blocking answer from the
  UI, since popping a `QMessageBox` off the GUI thread isn't safe.
  **Two real bugs found by a dedicated code review of this file, fixed
  together with the identical pattern in its sibling `CertificateVerifier`:**
  1. **The prompt showed only the host, never the port** — even though
     the known-hosts store this decision feeds into is keyed on
     `"[host]:port"` together (see `SftpBackend::verifyHostKey()`'s own
     comment on why: two different SSH services can share a hostname on
     different ports). Two saved sites on the same hostname but
     different ports got an identical, indistinguishable "unknown/changed
     host key" prompt, with no way to tell which service was actually
     being verified — undermining the informed MITM-vs-legitimate-change
     judgment the dialog exists to support. `CertificateVerifier`/
     `FtpBackend`'s trusted-fingerprint store has the identical host+port
     key, so the same bug applied there too. Fixed by threading `port`
     through both `confirmHostKey()`/`confirmCertificate()`'s signatures
     (a real, compile-time-enforced fix — the old 3/4-argument signatures
     simply don't compile against the new 4/5-argument call sites, which
     is exactly how the regression test below proves this was a genuine
     API gap, not just a wording nit) and showing `"host:port"` in the
     dialog text, matching each store's own key format exactly.
  2. **Both dialogs were shown with a `nullptr` parent instead of the
     main window.** A parentless `QMessageBox` has no guaranteed
     stacking/focus relationship to the app — on some window managers or
     multi-monitor setups it could open unfocused, behind the main
     window, or on a different virtual desktop, making the app LOOK hung
     (the worker thread really is blocked, waiting on this dialog) while
     the actual security-critical prompt is invisible. Fixed by using
     `qobject_cast<QWidget *>(parent())` — both classes' one real
     construction site (`MainWindow.cpp`) already passes the main window
     as the `QObject` parent (`new HostKeyVerifier(this)`), so no new
     member/parameter was needed to get a real widget reference.
  Also extracted `TrustPromptDialog::confirm()`
  (`src/ui/TrustPromptDialog.h`, new, header-only) — a real duplication
  found in the same review: both classes hand-wrote an identical
  warning-vs-question dispatch, Yes/No buttons, and fail-safe "No"
  default, differing only in the title/body text each already builds
  separately. A third, narrower finding — a host/fingerprint value
  containing HTML-like characters could in theory be misinterpreted as
  rich text by `QMessageBox`'s default `Qt::AutoText` format — was
  investigated and left undone: the host-key/certificate trust step only
  runs after a successful TCP connection, and no real DNS-resolvable
  hostname can contain HTML-special characters (RFC 1123), so reaching
  this would require a user-crafted `/etc/hosts` alias — narrow enough
  that fixing it now would mean solving a problem nothing currently
  triggers.
  **Now covered by an automated regression test**, `trust-prompt-test`
  (`src/trust_prompt_test.cpp`) — drives the REAL `QMessageBox` each
  class pops (not a mock), using `conflict-resolution-test`'s own proven
  live-dialog technique (`QApplication::activeModalWidget()` while
  `exec()` is still blocking). Confirms the dialog text includes the
  port, the dialog's parent is the main window, and the return value
  matches which button was actually clicked, for both classes.
- `FilePaneWidget` — one side of the dual-pane view. Holds a
  `QStandardItemModel`, doesn't know or care which backend it's attached
  to. `setBackend(backend, thread)` swaps backends at runtime: if a
  `QThread` is passed, the pane does NOT parent the backend to itself
  (Qt disallows reparenting across thread boundaries) and instead manages
  its lifetime manually — `deleteLater()` on the backend (runs on its own
  thread's queue) followed by `thread->quit()` + `thread->wait()`. Passing
  `thread=nullptr` (e.g. for `LocalBackend`) falls back to normal
  parent-child ownership.
  **A real bug found by testing: `thread->wait()` above can freeze the
  entire GUI if called while the OLD backend is still stuck inside a
  blocking `connect()`/SSH-handshake syscall on its worker thread** —
  `thread->quit()` only stops the thread's event loop after whatever's
  currently running returns, and it can't interrupt a blocking syscall
  already in progress. Calling `setBackend()` again on a pane whose
  previous connection attempt hasn't resolved yet (a second Connect
  click, or Disconnect, against a slow or packet-dropping host) would
  hit this teardown path and hang the whole app until that syscall times
  out on its own. `isConnecting()` (true from the moment `setBackend()`
  queues a thread-owning backend's `connectToHost()` until `connected`
  or `connectionFailed` fires) lets a caller check before ever calling
  `setBackend()` again on the same pane, rather than attempting to
  interrupt the blocking I/O itself — a much larger change to
  `SftpBackend`/`FtpBackend`'s connection handling. See `MainWindow`'s
  entry below for where this is actually checked
  (`startConnection()`/`disconnectPane()`).
  Right-click on selected rows offers "Transfer
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
  **A real bug found by testing: overlapping navigation requests could
  corrupt `m_history`/`m_historyIndex`.** `onDirectoryListed()` decides
  whether to push a new history entry using the single shared
  `m_navigatingHistory` flag above, set by whichever `navigateTo()` call
  most recently ran — but `listDirectory()`/`directoryListed()` carry no
  per-request id to correlate a response back to the specific call that
  triggered it. Clicking Back twice quickly (or Back then typing a new
  path before the first response lands) let a second, overlapping
  `navigateTo()` call's setup silently stomp the first's still-pending
  state, or vice versa. Confirmed directly: two synchronous `goBack()`
  calls landed on the wrong directory (skipping past the intended one)
  with the real forward-history entry silently lost. Since there's no
  way to correlate responses to requests without changing
  `RemoteBackend`'s interface, `navigateTo()` instead refuses to issue a
  second request while one is still outstanding (`m_navigationInFlight`,
  set right before dispatching, cleared by `onDirectoryListed()` on
  success, the `connectionFailed` handler on a bad path, or
  `resetHistory()` on a backend swap) — `goBack()`/`goForward()` check
  this BEFORE mutating `m_historyIndex`, so a refused request never
  desyncs the index from what's actually displayed.
  Clicking a column header sorts by that column (`setSortingEnabled(true)`
  on the `QTreeView`; Qt's own header handling gives ascending/descending
  toggle on a second click for free), with a real `SortDataRole` numeric
  key on Size specifically — its display text is an unpadded byte count,
  which sorts wrong lexicographically ("10" before "9"). Before any header
  is clicked, `rebuildModel()` now applies one consistent default order
  across all three backends (folders first, then name ascending A-Z, by
  the entry's real name rather than the later `"[folder]"`-wrapped
  display text) — previously only `LocalBackend` sorted its own results
  at all, making the "default" order silently backend-dependent.
  **A real, live-reported inconsistency, found after this had already
  shipped**: the unifying comparator was originally written descending
  (`> 0`), the opposite of both `NameItem`'s own ascending-by-default
  header-click comparator (right below) and `LocalBackend`'s
  pre-unification behavior this same entry already claimed to match — a
  fresh listing showed Z-A with no sort indicator on the Name header at
  all, then clicking that header once flipped everything to A-Z, a
  jarring reversal on the very first click. Fixed to ascending, matching
  both the header-click comparator and this entry's own original
  intent; `sort-and-commands-test`'s Part 3 fixture (`zulu`/`alpha`
  dirs, `bravo.txt`/`delta.txt`/`Aaa.txt` files) had its expected
  default-order assertion corrected to match (it had been asserting the
  wrong, descending order as if it were correct) — a genuine case of a
  test faithfully encoding a real bug rather than catching it, since it
  only ever checked internal self-consistency (comparator output vs.
  itself), never that ascending was the actually-intended direction.
  Sorting
  surfaced a real latent bug: every row-lookup method
  (`onRowDoubleClicked`, `selectedEntryName()`, `selectedFileNames()`,
  `selectedEntries()`) used to index `m_currentEntries` by row position, an
  invariant sorting breaks outright once rows move. Fixed by tagging each
  row's Name item with the entry's real name (`SortDataRole`) and adding
  `entryForRow()` to look entries up by that instead of position.
  **A real bug: `rebuildModel()` cleared and rebuilt the model
  unconditionally with no attempt to preserve the current selection.**
  `removeRows()` destroys the old `QStandardItem`s outright, and Qt's
  `QItemSelectionModel` can't follow a selection across that — so
  toggling "Show hidden files" or hitting Refresh mid-selection (both
  re-enter `rebuildModel()` for the SAME directory via
  `onDirectoryListed()`, not just a genuine navigation to a different
  one) silently dropped whatever was selected, losing the target set of
  a pending Transfer/Move with no indication why. Fixed by capturing the
  selected entries' NAMES before rebuilding and re-selecting whichever
  rows match those names afterward — naturally correct for a genuine
  navigation to a DIFFERENT directory too (the old selection's names
  essentially never match anything in an unrelated listing, so nothing
  gets re-selected there, exactly as it should), no separate "is this
  the same directory" check needed.
  **Either pane can now connect independently**, not just the right one —
  `updatePathBarIcon()`'s existing leading icon on the path bar (already
  there, previously just a static local/remote indicator) is now
  clickable: `onPathBarIconClicked()` opens a small menu (Connect.../
  Sites.../Disconnect, Disconnect disabled while already local) and emits
  one of three new signals (`connectRequested`/`siteManagerRequested`/
  `disconnectRequested`, each carrying `this`) rather than doing anything
  backend-related itself — same shape as the file-management prompts
  above (prompt/menu locally, dispatch structurally), preserving the rule
  that `MainWindow::startConnection()` is the only place in the UI layer
  that names a concrete backend type. Deliberately reuses this
  already-present icon instead of adding a new toolbar button or menu
  item — the global toolbar's own Connect/Sites/Disconnect remain a
  fixed shortcut to the right pane (see `MainWindow`'s entry below for
  why), so the path-bar icon is the only way to connect the *left* pane,
  or to reconnect/disconnect either pane individually without reaching
  for the toolbar. No explicit signal disconnect is needed when the
  backend changes — `updatePathBarIcon()` already deletes and recreates
  the leading `QAction` on every backend swap, which tears down its
  connections along with it.
  **Nine real bugs found by a dedicated code review of this file — the
  largest source file in the project never previously reviewed on its
  own — all fixed, each now covered by a dedicated regression scenario
  in `navigation-test` or `sort-and-commands-test`:**
  1. **`onDirectoryListed()` couldn't tell a file operation's own "fire
     and refresh" `directoryListed` apart from a genuine `navigateTo()`
     response.** `deleteEntry()`/`renameEntry()`/`createFile()`/
     `createDirectory()` all reuse the same signal on success (see
     `RemoteBackend`'s own doc comment); with no per-request id, a file
     op's refresh arriving while a real navigation was still in flight
     (queued and processed first, since the backend handles queued calls
     strictly in order) got misread as that navigation's own response —
     clearing `m_navigationInFlight` early and opening a window where a
     second action (Back, another navigation) could fire, corrupting
     `m_history`/`m_historyIndex`. Fixed with `m_pendingFileOpRefreshes`,
     a count (not a bool — several deletes can be dispatched at once)
     incremented right before each of the four dispatches and decremented
     when consumed by its own refresh or by the matching
     `fileOperationFailed`; `onDirectoryListed()` only treats an arrival
     as a real navigation response once that count is back to zero.
  2. **`currentPath()` on `SftpBackend`/`FtpBackend` was a genuine
     unsynchronized cross-thread `QString` race.** `FilePaneWidget::
     currentDirectory()` reads it from the GUI thread while
     `ensureSession()`/`listDirectory()` write it from the backend's own
     worker thread, with no equivalent to `m_cancelRequested`/
     `m_pauseRequested`'s `QAtomicInteger` protection — `QString` has no
     atomic form, so a `QMutex` (`m_currentPathMutex`) now guards every
     write site and the read in `currentPath()`; same-thread reads
     elsewhere in each backend don't need it.
  3. **A failed `goBack()`/`goForward()` left `m_navigatingHistory` stuck
     true.** Only `onDirectoryListed()` (a confirmed successful listing)
     ever reset it; a failed navigation reports via `connectionFailed`
     instead (same reason `m_navigationInFlight` is reset there too — see
     above), which never reached it. The next successful fresh navigation
     would then wrongly take the "this was Back/Forward" branch and skip
     pushing itself onto history at all. Fixed by resetting
     `m_navigatingHistory` alongside `m_navigationInFlight` in the same
     `connectionFailed` handler.
  4. **The selection-restore-by-name in `rebuildModel()` (see its own
     entry above) fired across a genuine navigation to a DIFFERENT
     directory, not just a same-directory refresh** — its own comment's
     assumption that an old name "essentially never" matches an unrelated
     directory's listing is false for common names (`README.md`,
     `.gitignore`, `index.js`, `__init__.py`), silently auto-selecting a
     same-named file with no user action, dangerous if the next action is
     Delete or Move. Fixed with `m_entriesDirectory`, tracking which
     directory `m_lastRawEntries` actually represents, compared against
     `currentDirectory()` on each call — restore now requires the
     directory to be unchanged since the previous rebuild.
  5. **`promptAndRename()`/`confirmAndDelete()`/`promptAndCreateFile()`/
     `promptAndCreateFolder()` all read `currentDirectory()` themselves,
     AFTER their own modal `QInputDialog`/`QMessageBox` had already
     closed** — but `showContextMenu()`'s `menu.exec()` just before them
     is ALSO a nested event loop, and either one still pumps a genuinely
     in-flight navigation's queued, cross-thread `directoryListed`
     response while open. A navigation completing before Rename was even
     clicked could silently retarget the eventual rename to the NEW
     directory instead of the one the entry was actually selected from.
     Fixed by having `showContextMenu()` capture `directory =
     currentDirectory()` once, at the same moment as `selected`, before
     `menu.exec()` — threaded through to all four as a parameter instead
     of each calling `currentDirectory()` again later. Refresh is the one
     deliberate exception, since it wants whatever's current, not a
     snapshot.
  6. **`onPathBarReturnPressed()` had no `m_navigationInFlight` guard or
     feedback, unlike `goBack()`/`goForward()`.** `navigateTo()`'s own
     internal guard already made this safe (never corrupted state), but
     pressing Enter on a freshly typed path while a prior navigation was
     still resolving got silently dropped, and looked actively broken
     once that earlier navigation's response arrived —
     `onDirectoryListed()` overwrites the path bar with ITS path,
     erasing what had just been typed with nothing to explain why. Fixed
     by checking the flag first and showing a status message when
     refused.
  7. **Clicking the Name column header sorted on `QStandardItem`'s
     default comparator — the bracketed `"[Folder]"` DISPLAY text via
     plain ASCII code-point order, not the real name, and with no
     folders-first grouping or locale awareness** — `'['` sits between
     uppercase and lowercase ASCII, so folders interleaved inconsistently
     with files instead of staying grouped, in an order that also ignored
     locale rules entirely. Fixed with a `NameItem` comparator (same
     `SortDataRole`/folders-first-as-part-of-the-key convention `SizeItem`
     already uses, so a descending click flips folders to the end exactly
     like Size's dirs already do) using the real name and
     `QString::localeAwareCompare()`.
  8. A stale doc comment on `selectedFileNames()` claiming
     `FileTreeView::startDrag()` and the context menu's Transfer action
     call it — both were switched to `selectedEntries()` once folder
     drag/transfer needed `isDir`, and it's had zero production callers
     since (kept only because test code still exercises it directly).
     Corrected so a future maintainer doesn't rely on the stale claim.
  9. **`iconForEntry()` re-rendered every icon from scratch — SVG parse
     plus `QPainter` composite, twice for the @2x HiDPI variant — via
     `IconTheme::tintedIcon()`, which had no caching, on every row of
     every `rebuildModel()` call.** A directory listing of a few hundred
     entries re-rendered the same handful of distinct (path, color, size)
     icons hundreds of times over on every navigation and every "Show
     hidden files" toggle. Fixed with a `(resourcePath, color, size) ->
     QIcon` cache in `IconTheme::tintedIcon()` itself (GUI-thread only,
     so a plain `QHash` needs no locking) — measured ~144x faster for
     repeated cache hits vs. forced misses in `sort-and-commands-test`.
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
  **A real, security-relevant bug found by code review: "same-process
  only" was an assumption never actually enforced, and cross-process
  delivery is genuinely reachable, not just theoretical.** `dropEvent()`
  used to reconstruct the `FilePaneWidget*` from the raw bytes with only
  a byte-COUNT check — no check the pointer was live or belonged to this
  process — and the reconstructed pointer IS genuinely dereferenced
  downstream (`MainWindow::onFilesDropped()` -> `enqueueEntries()` ->
  `TransferManager::enqueue()`, which calls
  `sourcePane->currentDirectory()`/`backend()` directly). The underlying
  OS drag-and-drop transport (XDND on X11, the Wayland data-device
  protocol, OLE DnD on Windows) is inherently cross-PROCESS by design on
  every platform this app targets, and Qt adds no same-process
  restriction for a custom MIME type — dragging a file from one running
  ZephyrFTP instance onto a SECOND instance's window really did deliver
  the first instance's raw pane pointer to the second, which would then
  dereference it: a wild-pointer read into memory that process never
  allocated. Fixed with `kProcessDragToken`, a random 64-bit value
  generated fresh once per process at static-initialization time and
  embedded alongside the pointer — checked BEFORE the pointer bytes are
  ever even reconstructed, let alone dereferenced, so a cross-process
  drop (carrying a token this process never generated) is rejected
  outright. Not hardened against a hostile process on the same machine
  deliberately guessing/brute-forcing this exact value — a much larger
  threat model a malicious local binary could attack this process
  through a dozen other ways regardless — this closes the ordinary,
  non-adversarial case of two genuine ZephyrFTP windows (or a second
  instance) sharing one desktop. Confirmed with a real regression test
  in `sort-and-commands-test`: forging a drop with a valid-looking but
  wrong token, and separately an old-format (pre-fix) token-less
  payload, against a real `FileTreeView` — the old-format payload is
  confirmed (via a real pre-fix control) to have been genuinely accepted
  and dereferenced before this fix, safely rejected after it. Delivering
  the forged `QDropEvent` needed sending it to the view's `viewport()`
  child widget specifically, not the `FileTreeView` itself — confirmed
  directly (a send to the outer widget never reached `dropEvent()` at
  all) rather than assumed, since `QAbstractScrollArea` (which
  `QTreeView` derives from) routes mouse/drag-and-drop events through
  its internal viewport, not the outer widget.
  **A second real bug in the same drag payload, found by code review:
  the selected-entries list was encoded as `"<0 or 1>\t<name>"` lines
  joined by `'\n'`, which a name containing a literal newline (legal in
  a POSIX filename, e.g. on ext4) would corrupt** — `dropEvent()`'s
  `split('\n')` silently truncated the name at the embedded newline and
  discarded the leftover fragment (no tab prefix to parse), confirmed
  directly with a standalone probe reproducing the exact corruption
  (`"weird\nname"` decoded back as bare `"weird"`). Fixed by joining
  entries with a NUL byte instead: unlike `'\n'` or `'\t'` (both legal,
  if rare, in a real filename), a literal NUL byte can never appear in
  one on any filesystem this app targets — the OS path APIs themselves
  treat it as a string terminator — making it a genuinely unambiguous
  record separator. Verified with the same standalone technique: the new
  encoding round-trips a name with an embedded newline (and, separately,
  one with an embedded tab, confirming the field separator stays safe
  too) byte-for-byte. Not wired into the automated suite as a live
  `QDropEvent` scenario like the token check above — doing so would need
  the real per-process `kProcessDragToken` to get a forged drop past the
  check that fix added, which isn't exposed outside this file and
  shouldn't be just for testability — so this one is confirmed by direct
  encode/decode verification instead, not a live event-delivery test.
- `TransferManager` — owns the transfer queue. Concurrency is per
  *backend instance*, not per item: at most one item may ever be
  dispatched against a given backend at a time (SftpBackend holds a
  single libssh2 session, FtpBackend a single control connection, and
  concurrent transfers on the same one aren't safe without more
  synchronization than this app does), but two items that need
  *different* backend instances — e.g. a left-pane upload and a
  simultaneous right-pane download, each pane's remote backend already
  pinned to its own worker `QThread` — now run genuinely concurrently.
  `m_active` (`QList<ActiveTransfer>`) tracks one entry per item
  currently claiming a backend; `startNext()` scans the whole queue on
  each call rather than stopping at the first `Queued` item, skipping
  any item whose required backend(s) are already claimed and claiming
  them immediately (before that item's own conflict check even goes
  out) for whichever it does dispatch. This replaced an earlier design
  that served exactly one active item globally, via a single
  `m_activeIndex`/`m_currentBackend`; see `TransferManager.h`'s own
  class-level doc comment for the current design in full. `enqueue()`
  figures out direction (local->remote / remote->local / local-copy /
  remote-to-remote) from each pane's `isLocalFilesystem()`, then
  dispatches to whichever backend actually owns the "remote" side of the
  operation.
  Connects each backend's progress/finished/failed/paused signals once,
  the first time an item is dispatched to it (`ensureTransferSignalsConnected()`,
  `Qt::UniqueConnection`, same never-torn-down pattern as
  `ensureExistsCheckConnected()`/`ensureMoveConnected()`) — routing a
  received signal back to the right item is done by matching `sender()`
  against `m_active`'s `currentExecutor` fields, not by "whichever
  backend is currently *the* one" (there can be several at once now).
  **The concurrency itself is verified two ways, not just one.**
  `transfer-concurrency-test` (fake `QTimer`-tick backends, part of the
  required `EXCLUDE_FROM_ALL` suite) proves the *scheduling* — two items
  on different fake backends both reach `InProgress` at once, two on the
  same one still serialize, cancelling one doesn't affect the other —
  and was itself confirmed via a before/after `git stash` control (fails
  against the pre-concurrency code, passes against this one), not just
  trusted on inspection. `verify-concurrent-transfers-live` (two real,
  independent local `sshd` instances, `EXCLUDE_FROM_ALL` but not part of
  that required suite — external precondition) proves the network-level
  claim the fake-backend test structurally can't: a real serial baseline
  (one upload to server A alone, one to server B alone, each timed) is
  compared against a real concurrent 3-item batch (two uploads to A, one
  to B) — against the pre-concurrency `TransferManager` the batch takes
  as long as the full serial sum (ratio ~0.99); against this one it lands
  at ~0.66-0.68, matching the ~0.67 theoretically expected for a 2-vs-1
  backend split, confirmed directly via the same `git stash` control.
  Content is checked byte-for-byte on every concurrently-uploaded file
  too, catching any cross-thread corruption a timing-only check would
  miss. See `src/verify_concurrent_transfers_live.cpp`'s own header
  comment and CONTRIBUTING.md's matching entry for exact run commands.
  **Simultaneous connections per site (v0.7.24)** — found while
  investigating why a bulk transfer of many small files felt slow: every
  file to/from a given site reused that pane's *single* backend
  connection, so a batch always serialized one file at a time through
  it, no matter how many were queued — the exact gap FileZilla/WinSCP's
  own "N simultaneous connections" setting exists to close, confirmed
  via `transfer_concurrency_test.cpp`'s own header comment ("two items
  that share the SAME backend instance still serialize strictly").
  `SavedSite::simultaneousConnections` (default 1, so every existing
  site's behavior is unchanged unless explicitly raised, 1-10 via a
  `SiteManagerDialog` spinbox) now lets a site open extra connections on
  demand. `FilePaneWidget` owns the pool (`configureTransferPool()`/
  `pickIdleTransferBackend()`) — built lazily, one at a time, only once
  `TransferManager` actually needs one, via the exact same
  construction recipe `MainWindow::startConnection()` already uses for
  the primary (same credentials, no new password/host-key prompts for
  pool members: the primary's own TOFU accept already covers the shared
  `known_hosts`/`known_certs.json` file every pool member reads).
  `TransferManager`'s own claim/lock model (`isBackendClaimed()`,
  `ActiveTransfer::claimedBackends`) needed **zero changes** — it
  already treats N independent backend instances correctly, which is
  what made cross-pane concurrency (above) work in the first place. The
  one real gap: `requiredBackendsForDispatch()` always resolved to
  exactly `pane->backend()`, so a new `TransferItem::capturedTransferBackend`
  (set at most once, at the moment a pool member is actually claimed,
  mirroring `capturedDestBackend`'s own existing pattern below) makes
  that resolution stable across the method's several call sites instead
  of risking a second, different pick.
  **Three real bugs found only by actually running a pooled scenario
  end-to-end, not by inspection** — `transfer-concurrency-test`'s new
  Scenario 5 (a real `FilePaneWidget` pool, not just two independently-
  constructed fake panes) catches all three again now:
  1. `startNextIfLikelyToDispatch()`'s enqueue-time "is a real scan even
     worth it" heuristic predates pooling; its documented safety
     invariant ("skipping is safe because whatever would make an item
     dispatchable already re-triggers a scan on its own") silently broke
     for a pooled pane, since growing the pool is a new event type only
     reachable from *inside* an actual `startNext()` scan — items sat
     Queued, never scanned, until something unrelated happened to
     trigger one. Fixed with a new `FilePaneWidget::hasTransferPool()`
     escape hatch (deliberately imprecise — "is pooling enabled," not
     "is the pool actually not full right now," since the precise
     answer needs `pickIdleTransferBackend()`'s own side effects).
  2. An item re-scanned while still nominally `Queued` (its own async
     `checkExists()` response not back yet, so `item.status` hadn't
     flipped to `InProgress` even though it already held a real claim)
     read as a fresh, unclaimed candidate for a pool pick — spuriously
     handing it a *second*, different backend and creating a second
     `ActiveTransfer` entry for the same item, while its real, already-
     in-flight `checkExists()` call stayed orphaned on the first one.
     One of three items dispatched to a freshly-grown pool got stuck at
     `InProgress`/0 bytes forever before this was found and fixed with
     an `activeIndexForItem(i) < 0` guard.
  3. A null-pointer crash tearing down a pool member built with no
     `QThread` (legitimate — a pool member never needs one for e.g. a
     `LocalBackend`-style backend) during Disconnect, since the
     teardown loop assumed every entry had one, unlike the primary's own
     already-guarded `if (m_backendThread)` teardown right next to it.
  Also confirmed against a real SFTP server (`tools/local-test-servers/
  start-sftp-pubkey.sh`, a disposable, non-committed probe — this
  project's established technique for a change like this that doesn't
  warrant a new permanent live-server target on its own, since
  `transfer-concurrency-test`'s Scenario 5 already covers the
  correctness side): real `SftpBackend` instances coexist with no
  hidden global lock (`ensureSession()`'s own session/socket state is
  entirely per-instance), no repeated host-key prompts once the shared
  `known_hosts` file has the entry, and clean teardown on Disconnect.
  The realistic warm-pool throughput comparison (a cold pool's first
  batch pays for establishing new SSH sessions — real handshake/auth
  crypto — as part of its own measured window, so that comparison alone
  is misleading) showed a genuine ~2x speedup for pool size 4 vs. 1 on
  loopback, small because loopback latency is near-zero — the same
  mechanism `transfer-concurrency-test`'s fake backends already showed
  scaling dramatically once realistic per-item latency is involved.
  A companion fix shipped alongside this: `FtpBackend.cpp`'s
  `known_certs.json` read-modify-write got the same `QLockFile`
  protection `SftpBackend.cpp`'s `known_hosts` already has (a real,
  previously-confirmed live bug for concurrent instances — see
  `verifyHostKey()`'s own doc comment) — needed because pooling makes
  "several concurrent first-time connects to the same host" the common
  case instead of a rare, incidental one.
  Deliberately out of scope for this pass: `RemoteToRemote` (still
  exactly one source + one dest connection, its own two-phase
  `capturedDestBackend` logic below untouched) and `Move` (never reaches
  `startNext()`, a metadata-only operation with no data-transfer
  component pooling could speed up); pooling is per-*pane*, not a
  global per-site connection registry shared across both panes; and no
  new resilience for a pool member's connection dropping mid-transfer
  beyond what a primary-connection loss already gets.
  **A small follow-up visual bug (v0.7.25), caught only by actually
  screenshotting `SiteManagerDialog`** (a disposable, non-committed
  `QWidget::grab()` probe, same established technique as elsewhere in
  this file) rather than trusting the layout code alone: the new
  "Simultaneous connections" spinbox rendered visibly taller than every
  other field in the form — confirmed numerically (33px vs. its own
  36px `sizeHint()`), the exact same spin-button-reserved-height
  mismatch `m_portSpin`'s own comment right above it already documents
  and works around, just not yet applied to this newer sibling field.
  Fixed with the same `setFixedHeight()` treatment.
  **Transfer integrity checksums** — right-click a `Done`, non-
  `RemoteToRemote` item in the transfer queue and choose "Verify
  Checksum..." to confirm it matches on both ends (SHA-256). The pivotal
  constraint driving the whole design, confirmed by reading the
  installed `libssh2_sftp.h` directly rather than assumed: no
  check-file/hash extension binding exists, and no generic "send an
  arbitrary SFTP extended request" escape hatch either, so server-side
  SFTP hash verification is unreachable without bypassing libssh2's
  public API entirely; `FtpBackend` and this project's own test servers
  send no hash command either. That means verifying anything but a
  `LocalToLocal` transfer can only mean re-reading the remote file a
  second time over the network — there's no cheaper path — which is why
  this is a manual, per-file action rather than automatic: doubling
  transfer cost silently in the background would be a bad default.
  `LocalToLocal` hashes both files directly (genuinely free, no network
  involved). Everything else hashes its local side directly and re-reads
  its remote side via a new `TransferManager::startVerificationDownload()`
  — deliberately NOT built on `enqueue()`, which derives its paths from
  `pane->currentDirectory() + a bare filename` (wrong here: the pane may
  have navigated elsewhere by the time someone clicks Verify on an old
  completed item) — instead mirroring `startEditDownload()`'s exact
  shape (explicit remote path, no `destPane`, conflict-check skipped,
  fresh temp path) and reusing `TransferDirection::EditDownload` rather
  than adding a new direction, since every dispatch/pool/claim switch
  already handles that shape correctly. The new, hidden item is
  disambiguated from a real edit-in-place download by a new
  `TransferItem::isVerificationTask` flag (set before `itemAdded` fires)
  — `TransferQueueTable`'s category filter and `TransferQueueWidget`'s
  `onItemAdded()`/`onItemUpdated()` both gained a one-line guard on it,
  the only two places that needed to change to keep it out of the
  visible queue entirely. Going through the ordinary claim/pool
  machinery this way means the hidden re-download automatically gets
  busy-backend queuing (verifying an item whose remote side is already
  claimed by another in-flight transfer correctly stays `Queued` rather
  than corrupting it) and cancellation for free — confirmed by a new
  `checksum-verification-test` scenario, not just asserted. The new
  `ChecksumVerifier` class (`src/transfer/ChecksumVerifier.h/.cpp`) is
  the orchestrator — constructed with a `TransferManager*`, used only
  through its public surface, matching this project's established
  "extract a focused subsystem" pattern (`DirectoryComparer`/
  `CompareSyncExecutor`); local hashing runs via `QtConcurrent::run()` +
  `QFutureWatcher` (never synchronous — hashing a large local file
  inline would reintroduce the exact GUI-thread-blocking bug class the
  v0.7.20 local-copy fix exists to prevent). `TransferQueueWidget` (not
  `MainWindow`) owns the `ChecksumVerifier`, connects each
  `TransferQueueTable`'s new `verifyChecksumRequested` signal to it, and
  shows the result `QMessageBox`/in-flight `QProgressDialog` itself —
  kept there, not routed further up, once checking `TransferQueueTable::
  showContextMenu()`'s actual existing pattern found the other four
  context-menu actions (Cancel/Pause/Resume/Retry) call `TransferManager`
  directly with no signal-forwarding chain to `MainWindow` at all, so
  "Verify Checksum" only needed to reach as far as the widget that
  already constructs every table, not the whole way up. Verified three
  ways: a new required-suite `checksum-verification-test`
  (`FakeVerifiableBackend`, matching `remote-to-remote-test`'s/
  `transfer-concurrency-test`'s own real-bytes-on-disk fake-backend
  technique, with one addition — a single mutable `remoteContent` field
  the test corrupts directly between an original transfer and a later
  `verify()` call to simulate real server-side drift) covering match,
  mismatch, busy-backend queuing, cancellation, and hidden-from-UI, all
  passing; and a disposable, non-committed live probe against a real
  `SftpBackend`/local `sshd` (this project's established throwaway-
  verification technique) that uploaded a real file, verified a match,
  then corrupted the server's own copy out-of-band (direct filesystem
  write, bypassing SFTP entirely) and confirmed the mismatch was
  correctly caught — proving this against real libssh2 I/O, not just the
  fake backend.
  **A follow-up visual bug, caught only by actually screenshotting the
  new UI** — same discipline as the Site Manager spinbox check above,
  applied to this feature immediately after it shipped: the in-flight
  `QProgressDialog`'s label text (`Verifying "filename"...`) ran
  straight off the dialog's right edge with zero margin, for an
  ordinary, not-especially-long filename — `QProgressDialog` sizes
  itself from its progress bar/button row, not its label, so a short
  label produces a dialog too narrow for a longer one. Neither
  `checksum-verification-test`'s headless assertions nor the live-SFTP
  probe's `fprintf` output could have caught this — both only checked
  that the signals/hashes were correct, never that the dialog rendered
  legibly. Fixed with an explicit `setMinimumWidth(420)`, confirmed by
  re-running the same disposable screenshot probe before and after. The
  two `QMessageBox` result dialogs (match and mismatch) and the
  context-menu item itself were screenshotted the same way and rendered
  correctly the first time — no fix needed there.
  **Connection history** — each pane's own path-bar Connect/Sites/
  Disconnect menu (`FilePaneWidget::onPathBarIconClicked()`) gained a
  "Recent Connections" submenu, listing up to the 10 most recent AD-HOC
  connections (Quick Connect or the plain Connect... dialog) so one can
  be picked to reconnect without retyping host/port/username/auth
  method. Deliberately excludes anything already saved in Site
  Manager (`ConnectionRequest::sourceSiteId` non-empty) — that's
  already one click away there, so duplicating it into a second list
  would be noise, not new value. New `src/backends/ConnectionHistory.h`/
  `.cpp` (`ConnectionHistoryEntry` + `ConnectionHistoryStore`) mirrors
  `SavedSite.h`/`SiteStore`'s own combined struct-plus-store-class
  convention, in its own `connection_history.json` file rather than a
  second array folded into `sites.json` — matching
  `TransferQueueStore`'s own established "one persistence convention,
  one file per concern" precedent. **No secret is ever stored, with no
  opt-in exception** — stricter than `SavedSite`'s own opt-in
  `CredentialStore` password saving, since a history entry has no
  stable, user-chosen identity worth keying a stored secret on;
  reconnecting always re-prompts for exactly one secret, adapting
  `SiteManagerDialog::onConnectClicked()`'s own existing passphrase-vs-
  password branch inline (no `CredentialStore` save-back step). Recorded
  from inside `MainWindow::startConnection()`'s existing `connected`
  signal lambda — deliberately NOT the nearby `ConnectionDescriptor`-
  building code a few lines below it, which runs synchronously right
  after `setBackend()` regardless of whether the connection actually
  succeeds (confirmed by reading the code before assuming); recording
  there would have captured typo'd hosts that never actually connected.
  `recordConnection()` dedupes on (protocol, host, port, username),
  moving an existing match to the front instead of duplicating it, and
  caps the list at 10. Verified three ways, same pattern as the
  checksums feature above: a new required-suite `connection-history-
  test` (16 assertions — round-trip for an SFTP-key and an FTPS entry,
  `toConnectionRequest()`'s secrets-always-empty guarantee, the same
  raw-JSON-key-inspection technique `site-store-test` established
  confirming no `password`/`passphrase` key ever appears, dedup/move-
  to-front, cap-at-10, `clear()`, and path-parameterized `loadFromFile()`/
  `saveToFile()`); a disposable screenshot probe of the real submenu
  (empty state, and a populated one with an SFTP and an FTPS entry) —
  rendered correctly on the first try, no bug found this time, unlike
  the checksums progress dialog above; and a disposable live-SFTP probe
  connecting a real `SftpBackend`, recording a real entry, reconstructing
  a `ConnectionRequest` from it, and reconnecting with a fresh backend —
  proving the record -> reconstruct -> reconnect round trip against
  real libssh2 I/O, not just in-memory structs.
  **`RemoteToRemote` (server-to-server) is staged through a local temp
  file** — neither backend has a direct way to move a file straight to
  another server, so `dispatchActiveItem()` runs it in two phases:
  `TransferPhase::Downloading` (source -> a temp file under
  `<TempLocation>/zephyrftp-staging/`, named by the item's own unique id
  so concurrent items can't collide) then `TransferPhase::Uploading` (that
  same temp file -> destination). The phase transition is the interesting
  part: `onBackendFinished()` checks for `RemoteToRemote` + `Downloading`
  *before* its normal "mark Done" logic, and if that's what just finished,
  resets `bytesDone`/`bytesTotal` to 0 (a fresh 0-100% for the upload half,
  not a continuation of the download half's numbers — `TransferQueueWidget`
  reflects this with "Downloading (1/2)"/"Uploading (2/2)" status text, and
  a phase-aware progress-bar chunk color (Blue/Green, matching the status
  icon), so the reset doesn't read as a bug), then re-enters
  `dispatchActiveItem()` directly rather than going through `startNext()`/a
  fresh conflict check (already done once for this item).
  **The destination backend for phase 2 is captured once, at phase 1's own
  first dispatch (`TransferItem::capturedDestBackend`, a `QPointer`), NOT
  re-fetched from `item.destPane->backend()` at phase-2 dispatch time —
  fixed after a code review found the live re-fetch as a real bug.**
  Before this fix, both `onBackendFinished()`'s `connectToBackend()` call
  and `dispatchActiveItem()`'s own `dstBackend` lookup read
  `item.destPane->backend()` fresh every time — harmless when nothing
  about a pane's connection can change mid-transfer, but "either pane can
  connect independently" (this same feature) made that untrue: swapping
  the destination pane's backend (via its own path-bar Connect/Disconnect
  menu) while phase 1 was still downloading would silently redirect the
  eventual upload to whatever backend happened to be attached once phase 2
  started — e.g. a swap to `LocalBackend` would write the file to the
  *original remote absolute path* taken literally as a local path. Now:
  if the captured backend has gone null (the pane's old backend was
  torn down), `dispatchActiveItem()`'s existing "no backend to execute
  this transfer" fallback catches it, with a specific, honest message
  for this exact case rather than a generic internal-error string, and
  `cleanupTempFile()` still runs so the already-downloaded temp file
  doesn't leak. This also incidentally fixed the same live-re-fetch risk
  for **resuming** a `Paused` upload-phase item, which goes through this
  same `dispatchActiveItem()` path.
  `cleanupTempFile()` deletes the staging file (a no-op for every other
  direction) from three call sites: `onBackendFinished()`'s success path
  (only after the *upload* half completes), `onBackendFailed()` (covers a
  phase-1 failure, a phase-2 failure — including the already-downloaded
  temp file — and cancellation during either phase, since cancellation
  surfaces through this same path), and the capturedDestBackend-null
  failure path above. `retryItem()` additionally resets `phase` back to
  `Downloading` and clears `tempFilePath` for this direction specifically
  — without that, retrying an item that failed during the upload half
  would stay at `phase == Uploading` and try to re-upload a temp file
  `cleanupTempFile()` already deleted, instead of correctly restarting
  from the download half.
  **Pause/resume now work for this direction too, in both phases** — a
  capability gap closed the same day it was found. `TransferQueueWidget`'s
  `pauseCapableDirection` used to exclude `RemoteToRemote`; an earlier
  version of that exclusion's own comment claimed resuming would need to
  preserve which phase was active, the resume offset, and the temp file
  across the pause. Code review found that reasoning had never actually
  been verified: neither `pauseItem()` nor `resumeItem()` touches `phase`
  or `tempFilePath` at all, and `resumeItem()` already deliberately
  preserves `bytesDone` (the resume offset) the same way it does for every
  other direction, so all three already survived a pause/resume cycle with
  zero `RemoteToRemote`-specific handling needed — including correctly
  resuming against the captured destination backend for the upload-phase
  case. The only real gap was test coverage, not model-layer behavior —
  closed by `remote-to-remote-test`'s scenario E (pausing and resuming
  during BOTH phases, confirming the resume offset, the same temp file,
  and the phase all survive correctly in each half), after which
  `pauseCapableDirection` was updated to include `RemoteToRemote`.
  `statusText()`/the progress-bar chunk color are phase-aware for a
  `Paused` `RemoteToRemote` item too now, matching the existing
  `InProgress` treatment — "Paused - Downloading (1/2)"/"Paused -
  Uploading (2/2)", not just "Paused" with no indication of which half.
  **Two more real bugs found by a later code review of the concurrency
  rewrite, both in the same neighborhood as `capturedDestBackend`'s fix
  above — a stale backend snapshot outliving the moment it's actually
  correct — fixed together, each with its own regression test:**
  1. **A resumed `Paused` item at phase `Uploading` wrongly re-claimed
     the source backend it no longer needed.** `requiredBackendsForDispatch()`
     (used by `startNext()`'s pre-dispatch busy-check, added by this
     rewrite) claimed BOTH backends for every `RemoteToRemote` item
     unconditionally — correct for a fresh dispatch (`phase ==
     Downloading`, matching its own doc comment's stated invariant), but
     `resumeItem()` routes back through `startNext()` too, and
     deliberately does NOT reset `phase` (a resume continues from
     wherever it was paused, unlike `retryItem()`'s full restart) — so a
     `RemoteToRemote` item paused mid-`Uploading` reached this helper
     with `phase == Uploading`, silently violating its own "phase is
     never Uploading here" comment. `dispatchActiveItem()`'s own
     Uploading branch never touches the source backend at all (only
     `capturedDestBackend`), so demanding it as a dispatch precondition
     could make an otherwise-ready resume silently stay `Queued`
     indefinitely if some unrelated item happened to be using the source
     backend for anything at all, even something with zero real relation
     to this item anymore. Fixed by making the helper phase-aware: claim
     both only at `phase == Downloading`, claim only
     `capturedDestBackend` at `phase == Uploading`.
  2. **`ActiveTransfer::claimedBackends` — captured once, at claim time
     in `startNext()`, from `item.sourcePane->backend()`/
     `item.destPane->backend()` — could desync from `currentExecutor`,
     captured separately (and later) in `dispatchActiveItem()` via a
     FRESH re-fetch of the same two calls.** The two reads are separated
     by `startNext()`'s own async `checkExists()` round trip; if either
     pane's backend is swapped (Disconnect/Connect — nothing guards
     against this mid-conflict-check, `FilePaneWidget::setBackend()`'s
     callers only check `stillConnecting()`, unrelated) during that
     window, `claimedBackends` keeps pointing at the OLD backend while
     `currentExecutor` correctly points at the new one —
     `isBackendClaimed()` (checks `claimedBackends`) then wrongly reports
     the new backend as free, letting a second, unrelated item dispatch
     to it concurrently: two `uploadFile()`/`downloadFile()` calls racing
     on the same libssh2 session or FTP control connection, exactly the
     invariant this whole rewrite exists to enforce. Fixed by refreshing
     `claimedBackends` inside `dispatchActiveItem()` itself, right where
     `currentExecutor` is set, from the same fresh backend pointers —
     never left holding a stale claim-time snapshot past the point
     reality is actually known. As a direct consequence, this also fully
     closes bug #1 above at the model level, not just at the dispatch
     gate: once a `RemoteToRemote` item's phase-2 dispatch actually runs
     (resumed or not), `claimedBackends` correctly drops down to just
     `capturedDestBackend` — the source backend is genuinely free for
     other items from that exact point on, not just exempted from a
     resume's own claim check.
  Both confirmed via dedicated regression scenarios, not just reasoned
  about — neither was a "trust the fix by inspection" case:
  `remote-to-remote-test`'s new scenario F pauses an item mid-`Uploading`,
  lets a completely unrelated item claim the source backend, then
  resumes — before the fix, the resumed item only reaches `InProgress`
  once the unrelated blocker item has already finished (the blocker is
  `Done`, not `InProgress`, at the moment of resume); confirmed via a
  before/after `git stash` control. Bug #2's window (a live pane reconnect
  racing a specific in-flight `checkExists()` response) needed a new test
  hook to land deterministically rather than racing real timing —
  `transfer-concurrency-test`'s `FakeAsyncBackend` gained `holdCheckExists`/
  `releaseHeldCheckExists()`, letting its new scenario 3 stash a
  `checkExists()` request, swap the pane's backend out from under the
  already-claimed item, then release the OLD backend's held response and
  confirm dispatch correctly lands on the NEW one. A second, unrelated
  item is then enqueued against that same new backend while the first is
  still running on it — pre-fix, both are observed `InProgress` on the
  identical backend instance AT THE SAME TIME (a genuine double-dispatch,
  confirmed via the same before/after `git stash` control); post-fix, the
  second correctly stays `Queued` until the first is done with it.
  **A third, structurally different gap in the same neighborhood, found
  by the same review round but left open at the time (a real design
  decision, not a small patch like the two above) and closed here:**
  `isBackendClaimed()` only ever serializes per-backend-*instance* — it
  has no way to know that two DIFFERENT instances (e.g. both panes
  connected to the same real server, routine) are talking to the SAME
  underlying server, so two items targeting the identical destination
  path via two different instances could both see a destination
  `checkExists()==false` and dispatch concurrently, silently clobbering
  each other — structurally impossible before this rewrite (the old
  design was globally serial), a real gap the per-instance concurrency
  model opened. Fixed with a second, independent reservation table,
  `m_reservedDestinationKeys` (`QSet<QString>`), keyed by
  `connectionIdentity() + '\n' + destPath` — deliberately a SEPARATE
  mechanism from `claimedBackends`/`isBackendClaimed()`, not a merge
  into it, since the two answer genuinely different questions ("is this
  backend object busy" vs. "is this exact remote path on this exact
  server already being written to by something else") and conflating
  them would make either harder to reason about on its own. Checked
  alongside the existing per-backend busy check in `startNext()`'s
  claim gate, and released (like `claimedBackends`) for an
  `ActiveTransfer` entry's WHOLE lifetime by a new
  `releaseActiveTransfer()` helper — replacing all six of the file's
  own `m_active.removeAt()` call sites (finished/failed/cancelled/
  paused/no-backend-to-dispatch-to/skipped), the single point every
  "this entry is no longer running" path already funneled through, so a
  reservation can never accidentally outlive the entry that acquired
  it. `LocalBackend::connectionIdentity()` returning a fixed constant
  ("local") means this protection extends to `LocalToLocal` transfers
  too, for free — two different `LocalBackend` instances writing to the
  same local destination path is the exact same race, just without a
  remote server involved.
  **Deliberately scoped to the ordinary per-file `enqueue()`/`startNext()`
  pipeline only** — `moveEntry()`/`moveFolder()` and a folder transfer's
  own root-level conflict check both already bypass `m_active`/
  `isBackendClaimed()` entirely by design (see `moveEntry()`'s own doc
  comment), so neither participates here either, a deliberate boundary
  matching that existing carve-out rather than a new asymmetry; a
  folder transfer's individual FILES still get full protection, since
  those ride through this exact same `enqueue()`/`startNext()` pipeline
  like any other file (see `startFolderFileTransfers()`'s own doc
  comment on why folder transfer needed no separate mechanism from
  ordinary files in the first place).
  Confirmed via a new scenario 4 in `transfer-concurrency-test`: two
  `FakeAsyncBackend` instances that report the identical
  `connectionIdentity()`/`currentPath()` (matching what two real panes
  connected to the same real server look like from this class's own
  perspective), both targeting the same destination filename — the
  second item is asserted to stay EXACTLY `Queued` (not just "never
  simultaneously `InProgress`", a stronger, fully deterministic check
  possible here since the reservation is inserted and checked
  synchronously within the very same `startNext()` scan, no timing
  window to poll for at all) for as long as the first is running, then
  starts and completes normally once the first releases the shared
  destination. Confirmed as a genuine, not just theoretical, race via
  the same before/after `git stash` control this whole neighborhood's
  other fixes used: the pre-fix code fails this exact assertion.
  **Not verified against a real live server** — unlike the original
  concurrency rewrite's own `verify-concurrent-transfers-live`, this fix
  is pure scheduling/bookkeeping logic (whether `startNext()` dispatches
  a `checkExists()` call at all), touching none of the actual
  read/write I/O loops a live-server harness would exercise
  differently than the fake-backend test already does — the fake-backend
  test's deterministic, synchronous-within-one-scan proof was judged
  sufficient on its own, consistent with how bug #2 above (also pure
  scheduling/bookkeeping) was verified the same way.
  **Known, accepted gap, not attempted here:** the destination conflict
  check (`checkExists()`) only ever runs once, before phase 1 begins —
  phase 2's upload is never re-checked. A file created at the destination
  during a long-running phase-1 download would be silently overwritten by
  phase 2 instead of triggering the usual Overwrite/Skip prompt. Re-adding
  a second conflict check mid-flight would mean handling a live
  `askConflict()` dialog appearing partway through an already-InProgress
  item, real added complexity for a narrow TOCTOU window; not attempted
  here.
  Verified by `remote-to-remote-test` (`src/remote_to_remote_test.cpp`)
  against two independent fake backends — direction/phase assignment, the
  phase transition, temp-file cleanup on success and on cancellation
  during either phase, the `retryItem()` phase-reset, and (scenario E)
  pause/resume during either phase, all covered, each with its own
  individually-named assertion (31 total — 21 restored/confirmed by one
  code review pass after the event-driven rewrite that fixed this test's
  real CI flakiness had collapsed several explicit, immediately-diagnostic
  assertions into implicit stage-transition gates backed only by a single
  generic 20-second timeout [reverting that specific trade cost nothing in
  robustness, since every restored `check()` just re-states a condition
  the surrounding `if` already required true, purely for diagnostic
  completeness in a PASS/FAIL log], plus 10 new ones for scenario E from a
  follow-up pass) — plus a manual run against two real local SFTP servers,
  and (since closed for good) `verify-remote-to-remote-live` (see Known
  gaps for exactly what each did and didn't confirm).
  `cancelItem()`/`retryItem()` (right-click in
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
  **A real, general bug found via live-server verification (not specific
  to any one direction), fixed:** `startNext()` unconditionally re-runs
  the destination `checkExists()` conflict check for the next `Queued`
  item it finds — including one that just came from `resumeItem()`. For
  ANY paused-then-resumed transfer, the destination already legitimately
  has `bytesDone` bytes in it (this item's own partial content from
  before the pause) — a real backend's `checkExists()` would truthfully
  report "yes, something's there," triggering a real Overwrite/Skip
  conflict prompt for what was never actually a conflict, for exactly
  the item the person just asked to continue. Clicking "Skip" there
  would abandon a transfer the person explicitly tried to resume; even
  "Overwrite" only happens to work by accident (a resumed upload's write
  at a nonzero offset doesn't truncate regardless of what the app-level
  choice nominally means). This is not a `RemoteToRemote`-specific bug —
  it affects `LocalToRemote`/`RemoteToLocal` resume too, and predates
  this session's `RemoteToRemote` pause/resume work entirely; it went
  undetected because every fake backend's `checkExists()` stub in this
  test suite (`FakePausableBackend`, `FakeRemoteBackend`) unconditionally
  reports "doesn't exist" regardless of real state, and the one harness
  that DOES drive a real backend's pause/resume
  (`verify_sftp_pause_cancel.cpp`) calls `SftpBackend::uploadFile()`/
  `downloadFile()` directly, bypassing `TransferManager`'s conflict-check
  machinery entirely. Only surfaced once `verify-remote-to-remote-live`
  drove a real pause/resume through the FULL `TransferManager`
  orchestration against a real server with real destination state.
  Fixed with `TransferItem::skipConflictCheckOnDispatch` (set by
  `resumeItem()`, consumed and cleared by `startNext()` at the moment
  that specific item is actually dispatched) — `retryItem()` does NOT
  set it, since a retry genuinely restarts from byte 0 (and, for
  `RemoteToRemote`, resets `phase`/`tempFilePath` too), so whatever's at
  the destination in that case really is a fresh, worth-asking-about
  conflict, unlike a resume. Regression-tested two ways: `checkExists()`
  call-counting in `transfer-pause-test` (asserts exactly one call across
  a full pause/resume/complete cycle — no real dialog needed to prove
  the fix, since with it working there's nothing left to trigger one),
  and `verify-remote-to-remote-live`'s own pause/resume scenario against
  two real servers, which is what found the bug in the first place (it
  hung indefinitely on a real, undismissable `askConflict()` dialog
  before this fix).
  Live speed (`TransferItem::speedBytesPerSec`) is sampled roughly every
  250ms in `onBackendProgress()` (via `QElapsedTimer`) rather than on
  every single progress signal, which for SFTP's 32KB-chunk read/write
  loop would be far too frequent to read as a stable "live" number. Each
  raw 250ms sample is then run through an exponential moving average
  (alpha = 0.3, reset whenever a transfer (re)starts) before being shown —
  added after directly comparing against other SFTP clients' live speed
  readouts, which turned out to be smoothing their own numbers rather
  than being more accurate: this app's raw per-window sample was already
  a real, unlagged measurement, it just had nothing carried over between
  windows, so ordinary transfer burstiness (TCP window dynamics, disk
  flush stalls, scheduler jitter) showed up directly as visible jumpiness
  with nothing to damp it.
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
  **A real bug found by a later code review**: `FolderEnumerator`
  always emits the folder's own root as its first result (needed so a
  *fresh* transfer creates the root before anything nests under it), so
  the directory-creation loop above always tried to `createDirectory()`
  the root too — including on a "Write Into" merge, where the root is
  by definition already there (that's the whole premise of the
  conflict just resolved). `TransferManager` itself doesn't listen for
  that specific failure (deliberately — see the loop's own comment),
  but `FilePaneWidget`'s `fileOperationFailed` handling is unconditional
  (shared with the single right-click "New Folder" action, which very
  much does want to see a real failure) and popped a real, confusing
  "Create folder failed" error dialog on **every** Write Into merge,
  for something that was never actually an error. Fixed by threading a
  `rootAlreadyExists` bool from `onDestinationExistsChecked()`'s two
  `startFolderEnumeration()` call sites (false for a fresh transfer,
  true for Write Into) through to `startFolderFileTransfers()`, which
  now skips the root's own `createDirectory()` call specifically when
  it's already known to exist — every other directory in the walk is
  unaffected. Found while stabilizing `conflict-resolution-test`'s own
  Phase E (a Write Into merge sits earlier in that same file's timeline)
  — the stray dialog could become the active modal widget at an
  unpredictable moment (`createDirectory()` is a queued call) and get
  mistaken for a real conflict dialog by anything watching
  `QApplication::activeModalWidget()`, a real, if narrow, source of test
  flakiness in its own right, not just a confusing UX bug for a real
  user doing a Write Into merge.
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
  **A real, AddressSanitizer-confirmed heap-use-after-free found by a
  later code review, made reachable by the per-backend-instance
  concurrency rewrite above**: `onDestinationExistsChecked()`'s
  file-conflict branch used to take `TransferItem &item = m_items[itemIndex]`
  BEFORE calling `askConflict()`, then use that same reference after it
  returned. The paragraph above already establishes that `askConflict()`'s
  nested event loop lets other, unrelated `checkExists()` responses run
  reentrantly while a dialog is up — and with real transfer concurrency,
  more than one item can have its own conflict check in flight at once,
  so one of those reentrant responses can legitimately append a brand-new
  item to `m_items` (the Move-into-a-non-empty-folder branch above does
  exactly this, with no dialog of its own once its own resolution is
  already "always overwrite"). `QList<TransferItem>` reallocates its
  backing store on append, which dangles any reference taken before it —
  confirmed directly with AddressSanitizer (not just reasoned about): a
  disposable `podman`+`fedora:44` container build with
  `-fsanitize=address` reproduced a genuine heap-use-after-free at the
  exact `item.status = TransferStatus::Skipped;` line, 10/10 clean after
  the fix, this sandbox itself having no ASAN runtime available to test
  locally. Fixed by never holding a `TransferItem &` across the
  `askConflict()` call — only a plain `QString` copy of the filename is
  read beforehand, and `m_items[itemIndex]` is re-fetched fresh
  afterward, the same "re-fetch by stable index, don't hold a reference
  across the call" pattern `activeIndexForItem(itemIndex)` right below it
  already used for the same reason. Regression-covered by a new phase in
  `conflict-resolution-test`: floods `m_items` with 500 appends (via
  `enqueue()` calls that stay harmlessly `Queued`, all sharing the
  already-busy destination backend so none of them dispatch or open a
  competing dialog) while the target item's own real conflict dialog is
  open, then confirms that item's own resolution still lands on the
  correct id/fileName/destPath — a functional check that passes either
  way in this environment (undefined behavior doesn't reliably
  manifest without a sanitizer), so the actual proof lives in the ASAN
  run above, not in this test's pass/fail alone.
  `moveEntry(sourcePane, destPane, fileName)`/`moveFolder(sourcePane,
  destPane, folderName)` implement server-side Move — relocating a file or
  whole folder between two panes on the SAME connection
  (`TransferManager::moveEligible()`, both backends' `connectionIdentity()`
  equal and non-empty) via one backend `moveEntry()` (rename) call instead
  of `enqueue()`'s download+upload. Deliberately **not** routed through the
  ordinary `Queued`/`m_activeIndex` pipeline `startNext()`/
  `dispatchActiveItem()` drive: a rename is a single control-connection
  round trip, not a data transfer with meaningful progress/pause/cancel,
  so there's nothing to gain by serializing it behind whatever transfer
  happens to be running — it's dispatched immediately (still visible in
  the queue via the ordinary `itemAdded`/`itemUpdated` signals, appended
  straight in as `InProgress` rather than `Queued`), and since it targets
  the same backend object as any transfer already running against it, Qt's
  per-thread FIFO event queue alone guarantees it still only actually
  executes after that transfer's own already-queued call returns — no
  explicit coordination needed. Request-id correlation
  (`m_pendingMoveItemId`, mapping a `moveEntry()` call's request id to the
  `TransferItem::id` it belongs to) is a separate mechanism from
  `m_activeIndex`, since — unlike the ordinary queue, which only ever runs
  one item at a time — several Move requests could plausibly have backend
  calls in flight simultaneously. `moveFolder()` reuses the same
  root-level `checkExists()`/`askConflict()` machinery `enqueueFolder()`
  uses for a consistent prompt, but with a real semantic limit
  `enqueueFolder()` doesn't have: a single rename cannot **merge** into a
  non-empty existing destination directory (POSIX `rename(2)` fails with
  `ENOTEMPTY` in that case — the same underlying call both SFTP's `RENAME`
  and FTP's `RNFR`/`RNTO` map onto). So while `enqueueFolder()`'s "Write
  Into" genuinely merges (transferring every file individually into
  whatever's already there), `moveFolder()`'s "Write Into" instead fails
  the move outright with a clear explanation — building real merge
  machinery (copy the tree, then delete the source) would be significant
  new scope this project's existing "no recursive delete" precedent argues
  against, not a small addition. Cancel and Retry are both disabled in
  `TransferQueueWidget`'s context menu for a Move item specifically because
  of the "never the active item" design above: `cancelItem()` has nothing
  to interrupt (no `m_activeIndex` entry to act on), and `retryItem()`
  would be actively wrong — it re-queues through
  `startNext()`/`dispatchActiveItem()`, which has no case for
  `TransferDirection::Move` and would fail with a misleading generic "no
  backend to execute this transfer" error; redoing the "Move Selected"
  action is the retry path instead. `retryItem()` itself also guards
  against `TransferDirection::Move` directly now (not just
  `TransferQueueWidget` disabling the action) — see below for why that
  guard was added after shipping without it.
  **Three real bugs found by code review after this feature had already
  shipped (v0.5.0/v0.5.1), all fixed, all now covered by regression tests
  in `move-entry-test`:**
  1. **Multi-select Move silently dropped every entry but the last one.**
     `moveEntry()`/`moveFolder()`'s own conflict-check stage stashed the
     in-flight request's source/dest pane + name + `isFolder` in single
     shared scalar members (`m_pendingMoveConflictCheckId` and friends)
     rather than a per-request map. `MainWindow::moveEntries()` calls
     `moveEntry()`/`moveFolder()` in a plain synchronous loop for a
     multi-select "Move Selected," so each call in that loop silently
     clobbered the previous call's stashed state before its own async
     `checkExists()` response ever arrived — by the time any response
     came back, only the LAST iteration's state survived, so every
     earlier item's response matched nothing and was dropped with no
     error shown at all. Fixed by replacing the scalars with
     `QHash<int, PendingMoveConflictCheck> m_pendingMoveConflictChecks`,
     keyed by request id — the same per-request pattern
     `m_pendingMoveItemId` above already used for the *later*
     post-dispatch stage, just not applied to this *earlier*
     conflict-check stage until this fix.
  2. **A Move's "apply to all" conflict-resolution choice leaked into a
     completely unrelated later transfer.** `m_fileConflictResolution`/
     `m_directoryConflictResolution` were shared between the ordinary
     `enqueue()`/`enqueueFolder()` pipeline and Move, but are only ever
     reset back to `Ask` in `startNext()`'s "queue fully drained"
     branch — which Move never calls (by design, per this entry's own
     "never routed through the ordinary pipeline" reasoning above). A
     Move batch's "apply to all, Write Into" choice would persist
     indefinitely; any later, completely unrelated ordinary folder
     conflict (or another Move) before the ordinary queue happened to
     drain would silently reuse it with no prompt at all. Fixed by
     giving Move its own separate `m_moveFileConflictResolution`/
     `m_moveDirectoryConflictResolution`, reset via
     `maybeResetMoveConflictResolution()` once no Move activity remains
     outstanding (both the conflict-check hash above and
     `m_pendingMoveItemId` empty) — the closest Move equivalent to
     `startNext()`'s "queue drained" reset, since Move has no single
     queue to drain.
  3. **`retryItem()` had no guard for `TransferDirection::Move`** —
     mentioned above, listed here for completeness: currently
     unreachable via the shipped UI (`TransferQueueWidget` already
     disables Retry for Move items), but the invariant wasn't enforced
     at the model layer that actually owns it, only the UI layer. A
     future caller (a bulk "retry all failed" action, a test, a UI
     regression re-enabling the action) would have silently hit the
     misdispatch described above. Fixed with a direct early-return guard
     in `retryItem()` itself.
  **Five more real bugs found by a later code review of TransferManager
  broadly (not Move-specific this time), all fixed, each now also
  covered by a regression scenario in `folder-transfer-test`,
  `transfer-pause-test`, or `transfer-queue-test`:**
  1. **`enqueueFolder()` had the exact same shared-scalar bug Move's fix
     #1 above already closed once — just never applied to this earlier,
     separate call site.** `m_pendingFolderConflictCheckId`/
     `m_pendingFolderSourcePane`/`m_pendingFolderDestPane`/
     `m_pendingFolderName` stashed a folder's root-conflict check in
     single shared scalars, not a per-request map. Dragging two folders
     onto a pane at once (`MainWindow`'s drop handling calls
     `enqueueFolder()` once per folder in a plain synchronous loop, same
     shape as Move's multi-select loop) let the second call silently
     clobber the first's stashed state before its own async
     `checkExists()` response ever arrived, dropping the first folder's
     transfer entirely with no error shown. Fixed the same way: replaced
     with `QHash<int, PendingFolderConflictCheck> m_pendingFolderConflictChecks`,
     keyed by request id. Fixing this surfaced a second, previously-
     unreachable bug in `FolderEnumerator` — see that entry below.
  2. **`retryItem()` reset `bytesDone`/`bytesTotal`/`errorMessage` but
     never cleared `skipConflictCheckOnDispatch`.** `resumeItem()` sets
     that flag so a resumed transfer doesn't get a spurious conflict
     prompt against its own partial content (see `TransferItem.h`'s own
     comment on the flag) — but if that same item is then cancelled
     while still `Queued` (never dispatched, so the flag is never
     consumed) and retried instead of resumed, `retryItem()` genuinely
     restarts from byte 0, exactly the fresh case the flag is supposed
     to skip only for a resume. A stale flag from an earlier pause/
     resume could silently bypass a retry's destination conflict check
     and overwrite whatever now exists there. Fixed by explicitly
     resetting the flag to `false` in `retryItem()`.
  3. **`connectToBackend()`'s teardown was a wildcard
     `disconnect(m_currentBackend, nullptr, this, nullptr)`** — removing
     *every* connection the old backend had to `this`, not just the four
     transfer signals this method itself re-adds below. That included
     `entryMoved`/`entryMoveFailed` from `ensureMoveConnected()` for an
     in-flight Move still running against that same backend object: if a
     Move's destination backend was also (or became) `m_currentBackend`
     for an ordinary transfer, and a different queued transfer dispatched
     against a different backend before the Move's response arrived,
     `connectToBackend()`'s wildcard disconnect would sever the Move's
     signal wiring — its eventual `entryMoved` would arrive with nothing
     listening, leaving that `TransferItem` stuck `InProgress` forever.
     Fixed by disconnecting only the four specific signals this method
     connects (`transferProgress`/`transferFinished`/`transferFailed`/
     `transferPaused`); `existsChecked`/`entryMoved`/`entryMoveFailed`
     are `Qt::UniqueConnection` and request-id-correlated, so leaving
     them connected across backends was always safe.
  4. **`cancelItem()` on the active item silently no-op'd if
     `m_currentBackend` (a `QPointer`) had already gone null** —
     reachable if the active item's backend is destroyed/swapped
     mid-transfer (e.g. Disconnect on a pane with a transfer running
     against it). With nothing to call `requestCancel()` on and no
     `transferFailed`/`transferPaused` ever coming to resolve it, the
     item AND `m_activeIndex` stayed stuck `InProgress` forever,
     permanently blocking every later `Queued` item via `startNext()`'s
     `if (m_activeIndex != -1) return;` guard. Fixed by resolving the
     item directly to `Cancelled` (including `cleanupTempFile()` and
     resetting `m_activeIndex`) when there's no backend left to ask.
  5. **`onBackendPaused()` never reset `m_activeItemCancelled`, unlike
     `onBackendFailed()`, which does.** A cancel racing a near-
     simultaneous pause — `cancelItem()` sets the flag and calls
     `requestCancel()`, but the backend resolves to `transferPaused`
     instead of `transferFailed` before the cancel takes effect — left
     the flag stale. The next item's genuine, completely unrelated
     failure would then be misreported as `Cancelled` via
     `onBackendFailed()`, with its real `errorMessage` blanked to an
     empty string. Fixed by clearing the flag in `onBackendPaused()`
     too.
- `TransferQueueStore` (`src/transfer/TransferQueueStore.h/.cpp`) —
  queue persistence: whatever's still `Queued`/`Paused`/`InProgress` when
  the app closes cleanly is written to `queue.json`
  (`QStandardPaths::AppConfigLocation`, alongside `sites.json`/
  `settings.json`) and restored on the next launch. Same
  `SiteStore`-shaped stateless `load()`/`save()` pair, `QSaveFile`
  atomic-write pattern `AppSettings::save()` already established (not
  `SiteStore::save()`'s older plain-`QFile`+`Truncate` one — a truncated
  `queue.json` mid-write would otherwise make `load()`'s "corrupt, fail
  soft to empty" path silently discard a real, resumable queue instead
  of failing loudly). Operates on `PersistedTransferItem`
  (`TransferQueueStore.h`), a small struct distinct from `TransferItem`
  itself — most of `TransferItem`'s fields (`sourcePane`/`destPane`,
  `capturedDestBackend`, `tempFilePath`, `skipConflictCheckOnDispatch`)
  are live-only and meaningless across a restart.

  **Deliberate scope boundary — persisted only at clean shutdown
  (`MainWindow::closeEvent()`), never continuously.** A crash still
  loses the queue, same guarantee window geometry/dock state already
  have — real continuous durability (a write on every progress tick)
  is meaningfully more complex for a benefit nobody asked for.
  **`RemoteToRemote`/`Move`/`EditDownload`/`EditUpload` items are never
  persisted, regardless of status** — `TransferManager::
  saveQueueForShutdown()`'s direction filter excludes them
  unconditionally before even checking status. `RemoteToRemote`
  specifically because preserving one mid-transfer would also mean
  preserving its partially-downloaded local staging file and excluding
  it from the constructor's existing `zephyrftp-staging/`
  `removeRecursively()` sweep — real, avoidable complexity for the
  newest, least battle-tested direction in the app; a `RemoteToRemote`
  item still running at shutdown is simply dropped, same as a terminal
  one. `Move` structurally never needs this: it already bypasses the
  `Queued`/`m_activeIndex` pipeline entirely and resolves in one round
  trip, so it's never sitting in a resumable state at shutdown to begin
  with. Terminal items (`Done`/`Failed`/`Cancelled`/`Skipped`) aren't
  persisted either — this is queue persistence, not a transfer history
  feature.

  **`FilePaneWidget::ConnectionDescriptor`** (`src/backends/
  ConnectionDescriptor.h`) is the new piece this needed that didn't
  exist before: protocol/host/port/username (and a `SavedSite.id` when
  the connection came from one — `ConnectionRequest` gained a matching
  `sourceSiteId` field, populated by `SiteManagerDialog::
  connectionRequestToConnect()`), set on a pane by `MainWindow::
  startConnection()` right after it builds the concrete backend, and
  reset to empty by `FilePaneWidget::setBackend()` itself on every
  swap (not left to each caller to remember). Never a password/
  passphrase — same non-negotiable `SavedSite`/`sites.json` already
  keep. Nothing tracked this before: `SavedSite::toConnectionRequest()`
  already discarded which site a connection came from the moment a
  backend was built, and `RemoteBackend::connectionIdentity()`'s own
  `scheme://username@host:port` string has no `savedSiteId` component
  worth parsing back apart even if it were reused for this.

  **Restore/reclaim mechanism**, in `TransferManager`:
  `restorePersistedQueue(localExecutorPane)` (called once from
  `MainWindow`'s constructor, right after `buildLayout()` creates both
  panes) dispatches a restored `LocalToLocal` item immediately — both
  panes always start on `LocalBackend`, and `sourcePath`/`destPath` are
  already-resolved absolute paths, so which `FilePaneWidget` object
  stands in as executor genuinely doesn't matter. A restored
  `LocalToRemote`/`RemoteToLocal` item's local side gets
  `localExecutorPane` immediately too; its remote side stays `nullptr`
  and the item's status becomes the new `TransferStatus::
  PendingReconnect` — deliberately distinct from `Paused` (which means
  a live connection exists, just not running right now;
  `PendingReconnect` means no connection exists at all yet) — carrying
  the connection it's waiting for in a new `TransferItem::
  pendingConnection` field. `tryReclaimPendingItems(pane)`, called from
  the same `RemoteBackend::connected` handler `MainWindow::
  startConnection()` already had, matches every `PendingReconnect`
  item's `pendingConnection` against the newly-connected pane's own
  `connectionDescriptor()` — on protocol+host+port+username, NOT
  `savedSiteId` (a reconnect via the plain `ConnectionDialog`, with no
  site involved, to the exact same server an item's `pendingConnection`
  does carry a `savedSiteId` for would otherwise never match, even
  though it's genuinely the same server; `savedSiteId` is carried only
  for `TransferQueueWidget::statusText()`'s friendly-name display, never
  for matching — a refinement made during implementation after the
  original savedSiteId-first design turned out to have exactly this
  gap). A match assigns the waiting side (inferred from `direction`
  alone: `RemoteToLocal`'s `sourcePane`, `LocalToRemote`'s `destPane` —
  unambiguous, since only these two directions ever reach
  `PendingReconnect`), flips status to `Queued`, sets
  `skipConflictCheckOnDispatch` when `bytesDone > 0` (same flag/reasoning
  `resumeItem()` already established — a nonzero `bytesDone` means the
  destination already legitimately has this item's own earlier partial
  content, not a real conflict), and calls `startNext()` — which passes
  `bytesDone` through to the backend as the resume offset exactly the
  way it already does for every other `Queued`/`Paused` item, no new
  dispatch-layer logic needed. One reconnect can claim several pending
  items at once if they share a connection.

  **`MainWindow::startConnection()`/`disconnectPane()` both call
  `EditSessionManager::endSessionsForPane()`-style proactive teardown**
  — here, `saveQueueForShutdown()` specifically must run BEFORE
  `disconnectPane()`'s own `setBackend(new LocalBackend())` calls in
  `closeEvent()`, since that reset each pane's `connectionDescriptor()`
  to empty; ordered correctly in `closeEvent()` (save first, then the
  existing per-pane teardown).

  **A real gap found and fixed during implementation, not by design
  from the start:** `TransferManager::cancelItem()` had TWO separate
  status gates, not one — the active-item branch (never applies to
  `PendingReconnect`, which is never the active item) and a second,
  independent `if (status == Queued || status == Paused)` gate on the
  non-active path, which `PendingReconnect` didn't originally satisfy
  either, silently falling through to the "nothing to cancel" case
  reserved for terminal states. Caught by directly reading
  `cancelItem()`'s actual code rather than assuming the active-index
  check alone was sufficient — added to that second gate too.

  Verified by `queue-persistence-test` (`src/queue_persistence_test.cpp`)
  — see CONTRIBUTING.md's own subsection for the full detail, including
  why it needs a small custom fake backend (not `LocalBackend`, which
  ignores `resumeOffset` entirely) to prove the persisted `bytesDone`
  genuinely reaches `downloadFile()` as a real resume offset, not just
  that it round-trips through JSON.
- `EditSessionManager` (`src/ui/EditSessionManager.h/.cpp`) —
  edit-in-place: right-click a remote file → Edit downloads it to a
  local temp file, opens it in an external editor, and re-uploads it on
  every save. Added two new `TransferDirection` values for this,
  `EditDownload`/`EditUpload` — the first directions where only one of
  `sourcePane`/`destPane` is ever set (a download has no `destPane`,
  the destination is a fixed local temp path, not another pane's
  current directory; an upload has no `sourcePane`, for the mirror
  reason), which required guarding `dispatchActiveItem()`'s previously
  unconditional `item.sourcePane->backend()`/`item.destPane->backend()`
  derefs. Two new `TransferManager` entry points,
  `startEditDownload()`/`startEditUpload()`, build these items directly
  rather than going through `enqueue()` — that method always requires
  two real `FilePaneWidget`s and always runs a destination
  `checkExists()` conflict check, neither of which fits here (an edit
  download's destination is a fresh, guaranteed-unique temp path under
  `<TempLocation>/zephyrftp-staging/`, `edit_`-prefixed to stay visually
  distinct from a `RemoteToRemote` staging file in the same directory;
  an edit upload's "conflict" is the file the user was just editing,
  not a real one). Both set the existing
  `TransferItem::skipConflictCheckOnDispatch` flag (previously only
  used by `resumeItem()`) to skip that check, reused as-is rather than
  reimplemented. Unlike `RemoteToRemote`, an edit download's temp file
  deliberately survives its own item reaching `Done` — `cleanupTempFile()`
  stays hard-gated to `RemoteToRemote` unchanged, and `EditSessionManager`
  itself owns deleting the file once the edit session actually ends.

  **Design decision: routes every download/upload through
  `TransferManager`, never calls `RemoteBackend::downloadFile()`/
  `uploadFile()` directly.** A wholly separate component doing that was
  considered and rejected. `RemoteBackend`'s `transferProgress`/
  `transferFinished`/`transferFailed`/`transferPaused` signals are
  backend-instance-wide, not scoped per call, and `TransferManager` is
  already the sole thing connected to them
  (`TransferManager::ensureTransferSignalsConnected()`), serializing
  exactly one active item at a time per backend (see `TransferManager`'s
  own entry above for how it now allows several backends' worth of
  concurrency without weakening that per-backend guarantee). A second,
  independent listener on those same signals would race whatever
  `TransferManager` is
  legitimately doing with the same backend at the same moment — the
  same "two mechanisms claiming authority over one piece of state" bug
  class this project has already hit and fixed once (`MainWindow`'s
  Preferences dock-visibility override fighting `restoreState()`, fixed
  the same week this feature was built). `TransferManager` is the
  established single arbiter of backend I/O in this app; edit-in-place's
  downloads/uploads are items *in* that same queue, not a second queue
  running in parallel.

  Session lifecycle (`EditSessionManager::Session`, keyed by
  `(FilePaneWidget*, remote path)`): `startEditing()` re-launches the
  editor on an already-open file's existing temp copy rather than
  downloading it again. On a real download completion (observed via
  `TransferManager::itemUpdated`, filtered by the item id this class
  itself issued — every other item in the app, including ordinary
  transfers, is simply ignored), it launches the configured editor
  (`AppSettings::externalEditorCommand()`, empty by default — falls
  back to `QDesktopServices::openUrl()` for the OS's own file
  association; a non-empty command is run via
  `QProcess::startDetached(command, {tempPath})`, the path appended as
  the command's sole argument, no `{file}`-style template substitution)
  and starts a `QFileSystemWatcher` on the temp file. **Known Qt
  gotcha, defended against explicitly:** many editors save via
  write-to-temp-then-rename, which silently drops the underlying OS
  watch after the first change — `onFileChanged()` unconditionally
  re-`addPath()`s the file on every fire regardless of whether it looks
  like it's still being watched, the standard workaround. Debounced
  ~400ms (a single-shot `QTimer` per session, restarted on each
  `fileChanged`) before dispatching the re-upload, since some
  editors/OSes fire more than one change event per save. A session can
  have more than one upload in flight/queued at once (a second save
  landing before the first save's upload finished is a real case, not
  hypothetical — `TransferManager` just queues it behind the first, same
  as any other pair of items would be), so item-id-to-session-key
  tracking uses two `QHash<int, QString>` reverse lookups (download and
  upload separately) rather than a single scalar per session.

  A failed download surfaces as a `QMessageBox::warning` — the click did
  nothing useful, this must be visible, not a silent Commands-pane log
  line. **A failed upload is the more important case**: also a
  `QMessageBox::warning`, stating the local temp path explicitly (the
  edit isn't lost — it's still on disk) with a Retry button, since a
  save silently failing to reach the server would otherwise look like
  data loss the user has no way to recover from inside the app. The
  watcher keeps running afterward regardless, so a further save still
  attempts another upload on its own.

  `MainWindow::startConnection()`/`disconnectPane()` both call
  `EditSessionManager::endSessionsForPane()` immediately before their
  own `targetPane->setBackend()` call — this, not a passive check, is
  what actually prevents a pending edit session's re-upload from
  silently redirecting to a NEW connection that pane ends up with
  (reachable via Connect on an already-connected pane, not just
  Disconnect). `startEditUpload()`'s own dispatch re-fetches
  `destPane->backend()` live at the moment it actually runs — same as
  every other direction — so without this proactive teardown, a save's
  debounce timer firing in the narrow window before a reconnect
  completes could otherwise upload someone's edited file to an entirely
  unrelated server. `Session::backend` (a `QPointer<RemoteBackend>`,
  same pattern `TransferItem::capturedDestBackend` already uses) is a
  cheap secondary guard on top of that, not the primary defense.
  `MainWindow::closeEvent()` also calls `endAllSessions()` before its
  own per-pane teardown — logically redundant with what
  `endSessionsForPane()` is about to do for both panes anyway, but
  cheap and explicit rather than relying solely on that inference.

  Verified by `edit-session-test` (`src/edit_session_test.cpp`) against
  a real `LocalBackend` standing in for "the remote" (the same
  "real, simple implementation over a mock" approach `navigation-test`/
  `transfer-pause-test` already use for their own `LocalBackend`-backed
  panes — `TransferManager`'s dispatch calls `RemoteBackend::downloadFile()`/
  `uploadFile()` identically regardless of which concrete backend is
  behind the interface) — a real download producing byte-exact temp
  file content, a real `QFileSystemWatcher`-detected save triggering a
  real debounced re-upload that lands the new content back at the
  original path, re-editing an already-open file NOT triggering a
  second download, and session teardown actually deleting the temp
  file from disk. Editor-launching itself
  (`QProcess::startDetached()`/`QDesktopServices::openUrl()`) isn't
  something a headless test can verify beyond "the right command was
  invoked" — the test points `externalEditorCommand` at `/bin/true`
  specifically to exercise that code path without spawning anything
  that could hang or need a display, not to prove real editor
  integration; see CONTRIBUTING.md's own `edit-session-test` subsection
  for what's verified manually instead.
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
  **A real bug, previously unreachable, found by testing the
  `enqueueFolder()` fix above: two instances walking concurrently
  against the same backend could each accept the OTHER's response as
  their own.** Each `FolderEnumerator` numbered its own
  `listDirectoryForEnumeration()` requests starting from 1, but connects
  directly to the shared backend's `directoryEnumerated` signal in its
  constructor — Qt fans a signal out to every connected slot, not just
  the instance that issued the matching request. Two enumerators walking
  concurrently (only reachable at all once the `enqueueFolder()` fix
  above stopped silently dropping the second of two folders enqueued
  together) could both be waiting on request id 1 at the same moment, so
  each instance's `requestId == m_activeRequestId` filter matched for
  the wrong reason, letting one instance consume the other's response
  and corrupting both enumerations' results together — confirmed
  directly: a regression test enqueueing two folders onto the same
  source pane at once briefly showed one folder's file appearing inside
  the other's result set. Fixed by making the request-id counter
  (`s_nextRequestId`) `static` — shared across every `FolderEnumerator`
  instance ever created, not per-instance — so two concurrent instances
  can never collide regardless of how many requests either has issued.
  **A dedicated code review of this file (its first) found no new
  functional bugs — genuinely a clean, small file — but two real
  documentation-accuracy issues in the comment above, both fixed:** the
  `s_nextRequestId` comment justified skipping an atomic with "instances
  are only ever constructed on the GUI thread (see this class's own
  header comment)" — a circular citation pointing at itself rather than
  an independent reason. Corrected to cite the real reason (both actual
  construction sites — `TransferManager`, always GUI-thread per its own
  header comment, and `verify-sftp-pubkey`'s manual harness — are
  GUI-thread-only today; nothing enforces this for a hypothetical future
  caller). Separately, the fix's own comment claimed the static counter
  makes id collisions "structurally impossible" — true only between two
  concurrently-*active* enumerators; a just-finished one stays connected
  to the backend's signals until its `deleteLater()` actually runs on
  the next event-loop pass, so a backend that ever re-emitted
  `directoryEnumerated` for that enumerator's last-consumed request id
  during that window would be silently reprocessed. Not guarded against
  in code — no real backend (`LocalBackend`/`SftpBackend`/`FtpBackend`)
  does this, so there's nothing to defend against yet — but the
  overclaiming wording is fixed rather than left to mislead a future
  reader, per this project's own "verify rather than assume" rule (see
  CONTRIBUTING.md).
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
  Both Name (file panes) and File (here) started out in `Stretch` resize
  mode, which meant neither had a working drag handle of its own — its
  width was purely a side effect of dragging some OTHER column's handle.
  Both are now `Interactive` with a sensible starting width; here that
  needed an explicit `setStretchLastSection(true)` alongside it, since
  unlike `QTreeView` (the file panes), `QTableWidget` doesn't default that
  to true — without it, Speed stayed its own narrow width and the
  remaining header space rendered as a raw, unstyled gap, caught by
  actually screenshotting the running app rather than assumed. Cell
  alignment needed its own pass too: File is left-aligned, Direction/
  Status/Speed are centered, and the inline progress bars are vertically
  centered in their row instead of pinned to the top. Direction needed a
  different technique than the other columns — `Qt::TextAlignmentRole`
  only affects an item's text, not its decoration, so an icon-only item
  ignores it entirely; that column uses a `QLabel` cell widget instead,
  which actually centers.
  Clicking a header sorts the queue the same way the file panes do, but
  by a different mechanism: `QTableWidget::sortItems()` doesn't move cell
  widgets (`setCellWidget()`) along with a sort, and Direction/Progress
  are both cell widgets here, so it would silently pair each one with the
  wrong row. Sorting is instead done manually — a header click sorts a
  copy of `TransferManager::items()` by the clicked column, then fully
  rebuilds the table via the existing `onItemAdded()`+`onItemUpdated()`
  pair, reusing `percentFor()` (extracted from `onItemUpdated()` so the
  Progress column's sort key and its rendered value can't drift apart).
  **A real bug found by testing: a newly-added item ignored whatever
  sort was currently active.** `onItemAdded()` always appended the new
  row at the bottom via `insertRow(rowCount())` regardless of
  `m_sortColumn`/`m_sortOrder` — sort the queue by a column, then
  drag-drop a new batch of files, and the new rows landed out of order
  at the bottom and stayed there until the next header click silently
  broke the sort the person had just set. Fixed by splitting the raw
  row-construction out into its own `appendRow()` helper: `onItemAdded()`
  now checks for an active sort and calls `resortAndRebuild()` instead
  when one is set (which reads live from `TransferManager::items()`,
  already including the new item by the time `itemAdded` fires);
  `resortAndRebuild()`'s own rebuild loop calls `appendRow()` directly
  rather than `onItemAdded()`, specifically to avoid recursing back into
  itself — a real, caught-immediately bug the first version of this fix
  introduced.
  **Five more real issues found by a dedicated code review of this
  file, all fixed, each now covered by `transfer-queue-test`'s Phase 5:**
  1. **The Direction header's sort-indicator arrow showed (ascending,
     File column) from construction, even though nothing was actually
     sorted yet** — `setSortIndicatorShown(true)` was set unconditionally
     in the constructor, but `setSortIndicator()` is only ever called
     later, from `onHeaderSectionClicked()`; until a header is actually
     clicked, `QHeaderView`'s own default indicator state just showed on
     screen regardless, falsely implying the queue was already
     alphabetized when `m_sortColumn` was still -1 (plain insertion
     order). Fixed by not calling `setSortIndicatorShown()` at all until
     `onHeaderSectionClicked()`, at the exact moment a real sort begins.
  2. **`statusIcon()`'s InProgress color switch had no case for
     `TransferDirection::LocalToLocal`**, unlike `directionText()` and
     the icon-path switch just above it, which both already handle it —
     it silently fell into `default: color = IconTheme::Gray`, the exact
     same color `Queued` uses, making an actively-copying local file
     indistinguishable from one that hadn't started. Fixed with an
     explicit case (Green, matching the "active copy" reading
     `LocalToRemote`'s upload already uses).
  3. **`resortAndRebuild()`'s Direction-column sort compared raw
     `TransferDirection` enum ordinals (declaration order) instead of
     anything visible on screen** — unlike the adjacent Status-column
     sort, which correctly sorts by its own displayed `statusText()`.
     The Direction cell shows only an icon and a tooltip, no sortable
     text, so the resulting order had nothing on screen to explain it.
     Fixed to sort by `directionText()` — the same tooltip text the
     column already carries — via `localeAwareCompare()`, matching
     Status's own approach. `directionText()` made public (previously
     private) specifically so this fix's comparator basis can be
     verified directly.
  4. `onItemUpdated()`'s progress-bar chunk-color logic gave an
     in-flight `Move` item Green (the same fallback `RemoteToLocal`'s
     sibling branch uses), while `statusIcon()` colors that exact same
     row's direction icon Blue for `Move` — two indicators on one row
     visibly disagreeing. Fixed to match. **Traced while writing the
     regression test, this drift turns out to currently be unreachable
     in practice**: `dispatchMoveEntry()` emits exactly one `itemAdded`
     (already `InProgress`, rendered by `appendRow()`, which never
     touches chunk color at all) and later exactly one `itemUpdated`
     when the move resolves, flipping straight to `Done`/`Failed` —
     nothing ever calls `onItemUpdated()` while a Move item is still
     genuinely `InProgress`. Kept anyway: a real inconsistency, and cheap
     insurance if Move ever gains progress reporting or pause/resume.
  5. `onItemUpdated()` read `m_table->item(row, ColStatus)` without a
     null check, unlike the guarded `ColSpeed`/`ColProgress` lookups
     right next to it — a latent null-pointer deref if any future code
     path ever delivered `itemUpdated` for a row before `ColStatus` was
     populated. Guarded to match.
  **Split into three tabs — Active, Completed, Failed — a direct user
  request**: watching a batch finish, or finding a failed item to
  retry, used to mean scrolling or sorting through everything else
  still queued or already done to find it. The single flat table's own
  implementation (everything described above — sorting,
  `resortAndRebuild()`'s coalesced-rebuild fix, the `m_rowById` id->row
  hash) moved essentially unchanged into a new `TransferQueueTable`
  (`src/ui/TransferQueueTable.h/.cpp`), one instance per category;
  `TransferQueueWidget` itself shrank to a thin ~90-line router owning
  three `TransferQueueTable`s inside a `QTabWidget`, plus a
  `QHash<int, QueueCategory> m_categoryById` tracking which tab each
  item currently lives in. A new free function `categoryFor(TransferStatus)`
  maps `Queued`/`InProgress`/`Paused`/`PendingReconnect` -> Active,
  `Done` -> Completed, and `Failed`/`Cancelled`/`Skipped` -> Failed —
  that exact three-status grouping for Failed was chosen to match
  precisely what `TransferManager::retryItem()` already accepts (its
  own guard clause checks for exactly those three statuses), so every
  item visible in the Failed tab is guaranteed retry-eligible. Items
  migrate between tabs live as their status changes — most visibly,
  retrying a Failed item moves it straight back to the Active tab the
  instant `retryItem()` sets it back to `Queued` — via
  `TransferQueueWidget::onItemUpdated()` comparing an item's previously
  tracked category against its newly computed one and, on a mismatch,
  calling `removeItem()` on the old tab's table followed by `addItem()`
  on the new one. Each table gets its own `setObjectName()`
  (`activeQueueTable`/`completedQueueTable`/`failedQueueTable`) so
  `transfer-queue-test` can reliably find a specific tab's table via
  `findChild<TransferQueueTable*>(name)` now that there are three
  sibling instances of the same type instead of one.
  **A real bug caught during design, before it ever shipped**: a
  migrated item that already landed in a terminal state (e.g. reaches
  Done and gets moved into the Completed tab) would have shown 0%
  progress forever, because `addItem()`'s row-construction path
  (`appendRow()`) always initializes a fresh row's progress bar to 0
  and nothing would have called `updateItem()` again for it afterward.
  Fixed by having `addItem()` always follow `appendRow()` with an
  `updateItem()` call, so a freshly-migrated row picks up its item's
  actual current progress/status immediately instead of only the
  blank just-added default.
  **A real visual bug, reported by David right after v0.7.21 shipped:
  the new tab bar showed what looked like a doubled border, in both
  themes.** Root cause, confirmed by dumping the live widget hierarchy
  (geometry of every child, not guessed from the QSS alone) rather than
  iterating on the stylesheet blind: `QTabWidget`/`QTabBar` were the
  only tab widgets ever added to this app, so nothing styled them at
  all — the tab bar rendered in the platform style's default light
  appearance, sitting directly above the dark `QTableWidget` below it.
  Fixed with theme-matched `QTabWidget::pane`/`QTabBar`/`QTabBar::tab`
  rules in both `theme.qss` and `theme-light.qss` (selected/hover
  treatment mirrors `QToolButton:checked`'s existing blue-accent
  language elsewhere in these files, using an underline instead of a
  filled background since a filled tab reads more like a button than a
  page selector). A second, related gap only surfaced while fixing the
  first: a plain `QTabWidget { background-color: ... }` rule had zero
  visible effect on the strip of the tab-bar row past the last tab
  (confirmed directly — a debug `red` value there painted nothing) —
  the widget dump showed why: `QTabBar` sizes itself to its own tabs,
  not the tab widget's full width, and that unclaimed strip was never
  covered by `QTabWidget::pane` either (the pane starts below the tab
  row). It was actually showing through to `QDockWidget`'s own
  background, which had never been styled at all (only its `::title`
  sub-control had) — invisible before now only because the queue's old
  single flat table always painted edge-to-edge, leaving nothing of the
  dock's own background ever exposed. Fixed by giving `QDockWidget`
  itself an explicit background color in both theme files, which is
  the actually-correct place this gap needed covering regardless of
  what widget structure sits inside a dock. Verified visually before
  and after in both themes via the same disposable, non-committed
  `QWidget::grab()`-screenshot technique this project already uses for
  visual/config-value changes that don't warrant a new permanent test
  target (see the Light Theme entry above for the precedent).
  **A real, severe performance bug, live-reported and reproduced
  directly: "clicked sort on the queue during a transfer and the app
  froze."** Root cause was in `resortAndRebuild()`, not anything
  transfer-specific — every existing measurement of that method's cost
  (`transfer-queue-test`'s own Phase 6 included) was on a table that's
  never actually been shown, which turns out to matter enormously for
  this specific operation: `resortAndRebuild()`'s old shape called
  `insertRow()` once per item in a loop (via `appendRow()`), which is
  cheap on a hidden table but pays for real, synchronous layout/paint
  work on a real, visible one — measured directly, a 1500-row rebuild
  took 89ms hidden vs. **1107ms once actually shown** (the Transfers
  dock is always visible in the real app), scaling worse than linearly
  as the queue grows (100 rows: 16ms; 500: 169ms; 1500: 1064ms — none
  of those ratios are anywhere near proportional to the row-count
  ratios). Fixed by presizing with a single `setRowCount(items.size())`
  instead of N incremental ones (`appendRow()` split into itself, for
  the single-item append path, plus a new `fillRow(row, item)` the bulk
  path calls directly against a pre-sized table), plus
  `setUpdatesEnabled(false)`/`(true)` around the whole rebuild. Measured
  after the fix: the same 1500-row shown rebuild dropped to 112-150ms,
  with the fixed version scaling close to linearly (500 rows: 32ms;
  3000 rows: 268ms). A separate hypothesis — that SFTP/FTP's unthrottled
  per-chunk `transferProgress` emission (no throttling on the signal
  itself, only on the *speed number* recomputed from it, capped to
  every 250ms in `TransferManager::onBackendProgress()`) was flooding
  the GUI thread — was tested directly and ruled out as the dominant
  cause: even 2000 rapid progress ticks measured at ~30ms total,
  negligible next to the resort cost above. Found and fixed along the
  way, not the dominant cost but real waste: `updateItem()` was
  recomputing and reapplying the progress bar's chunk-color stylesheet
  on *every* progress tick, even though the color only actually changes
  on a status/direction/phase transition — a `zfChunkColor` dynamic
  property now remembers the last-applied color so an unchanged tick
  skips the `setStyleSheet()` call entirely. All of this reproduced and
  measured via a disposable, non-committed probe (a fake `RemoteBackend`
  emitting rapid progress ticks against a real `TransferManager`/
  `TransferQueueWidget`, modeled on `transfer_pause_test.cpp`'s own
  `FakePausableBackend`) — same established technique this project uses
  for a change that doesn't warrant a new permanent target on its own.
  This one did also get a small addition to the existing required
  suite, since it's directly load-bearing for a real, reproduced
  regression: `transfer-queue-test`'s Phase 6 gained a Phase 6b that
  actually calls `show()` before re-sorting an already-populated 1500-
  row table, with a generous (800ms) bound — confirmed to fail (1115ms)
  against the pre-fix code and pass (148ms) against the fix, so it's a
  genuine regression test, not a tautology.
  **Competitive context, not just this project's own history**: this
  exact class of bug — transfer-queue/progress UI performance under a
  large batch or fast link — is a known, recurring problem across this
  whole app category, not a sign of anything uniquely wrong here.
  FileZilla has a long-standing open ticket for hangs with 700k-file
  queues; WinSCP's own support forums have repeated, long-running
  freeze/hang reports during transfers. FileZilla's own source
  (`CStatusLineCtrl::SetTransferStatus()` in `statuslinectrl.cpp`, the
  per-row live-progress widget analogous to this project's progress
  bar) shows the general pattern worth keeping in mind for any FUTURE
  work in this area even though it wasn't needed for this specific
  fix: it stores the latest status cheaply on every raw transfer-engine
  callback, but only actually redraws on a fixed 100ms (10Hz) repeating
  timer — decoupling raw I/O event rate from UI refresh rate entirely,
  a coarser throttle than anything this project currently does. Not
  implemented here since the flood hypothesis was directly measured and
  ruled out as not yet a real bottleneck — noted as the natural next
  step if a much faster link or many concurrent transfers ever makes it
  one.
- `CommandsPaneWidget` (`src/ui/CommandsPaneWidget.h/.cpp`) — a live,
  read-only log of protocol traffic, modeled on FileZilla's own message
  log, docked between the toolbar and the file panes by default (View >
  Commands to toggle, undockable/floatable exactly like the Transfers
  dock). Deliberately dumb and shared: it doesn't know which pane or
  backend a line came from, it just appends whatever text `MainWindow`
  feeds it via `appendLine()`, wired to both panes' current
  `RemoteBackend::commandLogged(QString)` — `FilePaneWidget` forwards
  whichever backend is currently attached under the same signal name, so
  `MainWindow` only wires each pane up once regardless of later
  Connect/Disconnect swaps. Deliberately no raw-command input — letting
  someone inject arbitrary commands into a live control connection risks
  leaving this app's own state (current directory, an in-flight transfer)
  out of sync with what the server actually did. `FtpBackend` emits
  genuine raw command/reply lines straight off the control connection,
  with `PASS`'s argument masked before logging — verified live against a
  real vsftpd container that the actual password never reaches the log.
  `SftpBackend` has no textual wire protocol to show, so it emits
  human-readable descriptions of each high-level operation instead
  (`Status: Connecting`, `Command: LIST`/`GET`/`PUT`/`RENAME`/`MKDIR`/...),
  matching FileZilla's own approach for SFTP. `LocalBackend` never emits
  this at all — nothing protocol-like happens for a purely local pane. A
  one-line welcome message fills the log before any real traffic exists,
  so the pane never opens looking blank/broken.
  **A real bug found by a dedicated code review of this file:**
  `appendLine()` used to force-scroll to the bottom unconditionally on
  every call, regardless of where the view actually was. Confirmed
  directly (a standalone probe, not assumed from documentation) that
  `QPlainTextEdit::appendPlainText()` already preserves scroll position
  correctly on its own — the override was pure regression. Scrolling up
  to re-read an earlier line during an active transfer
  (`SftpBackend`/`FtpBackend` emit `commandLogged` on nearly every
  control-channel operation) got yanked straight back down the instant
  the next line arrived, making it functionally impossible to review
  scrollback while the log was still live — undermining the pane's
  whole stated purpose. Fixed by checking the scroll position BEFORE
  appending and only re-pinning to the bottom if the view was already
  there, matching how every real terminal/log viewer/chat app actually
  implements "tail like a live console." Also noted, not fixed: the
  synthetic welcome line goes through the same `appendLine()` path as
  genuine traffic with nothing in the data marking it as synthetic —
  only a code comment asserts it's never real traffic. Not acted on
  since no existing feature (export, filtering, counting) needs to tell
  them apart yet; inventing that distinction now would be solving a
  problem nothing currently has.
  **Now covered by an automated regression test**, `sort-and-commands-test`
  (`src/sort_and_commands_test.cpp`) — a headless, self-contained
  `EXCLUDE_FROM_ALL` target added after this component and the
  column-sort/default-order changes above had only ever been checked by
  screenshotting the running app and the vsftpd-password-masking check
  described above. Confirms: `appendLine()`'s welcome line and append
  order; a real `FilePaneWidget` forwards the CURRENTLY attached
  backend's `commandLogged` and re-targets (not adds to) that forwarding
  across a `setBackend()` swap, via a minimal fake `RemoteBackend` (same
  legitimate test-double technique `transfer-pause-test`'s
  `FakePausableBackend` uses); and, against a real `LocalBackend` and
  real temp directory, the default folders-first/name-descending order,
  a real numeric Size sort triggered via `QTreeView::sortByColumn()`
  (the exact call Qt's own header-click handling invokes, not a
  shortcut around it), and — the specific regression this pins down —
  that `entryForRow()` still maps a view row to the correct entry once a
  sort has physically moved rows, rather than the pre-fix row-position
  indexing that broke the instant rows moved. Just as required as every
  other self-contained target, but documented in its own subsection in
  CONTRIBUTING.md rather than folded into a renumbered list — see that
  file's "Running the test suites" section for how to build and run it,
  and for the current full count.
- `MainWindow` — two `FilePaneWidget`s in a `QSplitter`, plus a
  `QDockWidget` at the bottom holding the transfer queue. Both panes start
  on `LocalBackend`; **either can now become remote independently**, not
  just the right one. The toolbar's "Connect..." action (via
  `ConnectionDialog`) and "Sites..." action (via `SiteManagerDialog`) both
  funnel into a single `startConnection(const ConnectionRequest &request,
  FilePaneWidget *targetPane)` — the only place in the UI layer that
  switches on `Protocol` to pick a concrete backend type (`SftpBackend`
  for `Protocol::Sftp`, `FtpBackend` for `Protocol::Ftp`/`Protocol::Ftps`),
  moves it to a worker `QThread`, and hands it to `targetPane` — rather
  than duplicating that setup in two places or in two protocol-specific
  paths. The toolbar/menu-bar path always passes `m_rightPane` (thin
  wrappers, `connectViaDialog()`/`siteManagerViaDialog()`, own the actual
  dialog construction so both the toolbar and the per-pane path below can
  share it) — kept as a fixed shortcut to the common case rather than
  needing a "which pane is active" concept, which doesn't exist anywhere
  else in this codebase and would be more machinery than the benefit (a
  slightly more consistent toolbar) is worth. Each pane's OWN path-bar
  icon (see `FilePaneWidget`'s entry above) is the general, symmetric way
  to target either pane specifically — `onPaneConnectRequested`/
  `onPaneSiteManagerRequested`/`onPaneDisconnectRequested` take the
  requesting pane straight from the signal and call the same
  `connectViaDialog()`/`siteManagerViaDialog()`/`disconnectPane()` helpers
  with it. "Disconnect" (toolbar, or a pane's own menu) swaps that pane
  back to `LocalBackend` via `disconnectPane()`.
  A third path into `startConnection()`, added later: the toolbar's
  quick-connect `QLineEdit` (`m_quickConnectEdit`) — type
  `[protocol://][user@]host[:port]`, press Enter,
  `onQuickConnectReturnPressed()` parses it (`QuickConnectParser.h/.cpp`,
  a pure free function — `parseQuickConnectString(input, defaultProtocol)`
  — deliberately kept separate from `ConnectionDialog`/`SiteManagerDialog`
  rather than teaching either widget a text-parsing mode), pops a
  synchronous `QInputDialog::getText(QLineEdit::Password)` for the
  password (mirroring `SiteManagerDialog::onConnectClicked()`'s own
  established "click Connect, prompt for the password right then"
  pattern — no `CredentialStore` prefill, since a quick-connect isn't a
  saved site), builds a `ConnectionRequest` directly (no
  `ConnectionDialog` involved at all — it has no public setters besides
  `setProtocol()`, so pre-filling it for a "confirm and go" flow would
  have needed new API surface this simpler direct-build avoids), and
  calls the same `startConnection()` every other path already uses.
  Deliberately narrow scope, matching the fixed-`m_rightPane` shortcut
  above: password auth only (no way to fit a private-key path into a
  one-line string — `Connect...` still exists for that), no IPv6
  literal support (a bare `lastIndexOf(':')` port split is ambiguous
  with `[::1]:port`, and quick-connect isn't the place to solve that).
  `protocol://` is optional — `sftp`/`ftp`/`ftps`, case-insensitive; if
  omitted, `AppSettings::defaultProtocol()` is used, the same accessor
  `connectViaDialog()` already reads to preselect `ConnectionDialog`'s
  own combo. The field itself is constructed in `buildMenuBar()`, not
  `buildToolbar()` where it's actually shown — `buildMenuBar()` runs
  first in the constructor (docks must exist before it reads their
  `toggleViewAction()`s) and the View menu's own quick-connect toggle
  (below) needs the field to already exist; `buildToolbar()` just
  `addWidget()`s it in, which reparents it into the toolbar without
  caring that its original parent was `this`.
  The View menu's **Quick Connect Field** toggle is deliberately NOT a
  `toggleViewAction()` the way Transfers/Commands are — there's no
  `QDockWidget` here to own one, confirmed nothing like this (a plain
  toolbar-embedded widget, or a manually-managed checkable `QAction`
  controlling a non-dock widget's visibility) existed anywhere in this
  codebase before. A new `AppSettings::quickConnectFieldVisible` bool
  (default `true`) persists it — `QMainWindow::saveState()` only
  captures `QToolBar`/`QDockWidget` layout, not an arbitrary child
  widget's own visibility, so reusing the existing `windowState` blob
  the way Transfers/Commands do wasn't an option here. Not duplicated
  onto the toolbar itself (unlike Transfers/Commands' intentional
  toolbar+menu pairing) — a toolbar icon toggling a toolbar *field*'s
  own visibility read as more confusing than useful, and every
  plausible icon choice would visually collide with the existing green
  `plug.svg` **Connect...** action right next to the field.
  The View menu's **Filename Filter** toggle, added later, is the same
  shape — a plain checkable `QAction`, no `toggleViewAction()` — but
  controls BOTH panes' filter-field visibility at once
  (`m_leftPane->setFilterFieldVisible(visible)`/
  `m_rightPane->setFilterFieldVisible(visible)`) rather than one
  `MainWindow`-owned widget. No construction-order hazard here despite
  `m_leftPane`/`m_rightPane` not existing yet when `buildMenuBar()` runs
  (`buildLayout()` creates them afterward): the toggle's lambda only
  dereferences them at toggle time — a later, real user action — never
  at construction time, and each pane already sets its OWN initial
  filter-field visibility from `AppSettings` directly in its own
  constructor rather than waiting for this toggle to do it. See
  `FilePaneWidget`'s entry above for the filter field itself.
  `startConnection()` and `disconnectPane()` both check
  `targetPane->isConnecting()` first (via a shared `stillConnecting()`
  helper — see below) and refuse (a status-bar message instead) if it's
  true — see `FilePaneWidget`'s entry above for the real GUI-freeze bug
  this guards against (calling `setBackend()` again on a pane whose
  previous connection attempt is still blocked in a worker thread's
  synchronous handshake). `closeEvent()` now checks the SAME thing
  itself, directly, for both panes, before ever calling
  `disconnectPane()` — see this entry's own bug list below for why that
  used to be wrong.
  The menu bar's
  "Connection" menu (`buildMenuBar()`, next to Help) mirrors the toolbar's
  three (right-pane) actions — same `QAction`-triggered slots, no separate
  logic — for keyboard/menu access alongside the toolbar buttons rather
  than instead of them.
  Double-clicking a file in either pane calls
  `TransferManager::enqueue()` with that pane as source and the other as
  destination; `TransferManager::transferSucceeded` triggers a refresh of
  both panes' listings. A minimal `QMenuBar` (`buildMenuBar()`) holds a
  single Help → About action — `QMessageBox::about()`, not a hand-rolled
  dialog, since it's exactly the simple/rare use case that's built for.
  Both the window title and the About dialog show the actual build's
  version, via an `APP_VERSION` preprocessor define wired through
  `target_compile_definitions()` in `CMakeLists.txt` from this project's
  own `project(VERSION ...)` declaration — `MainWindow.cpp` defends
  against a future test target that compiles this file without
  remembering to set that define (`#ifndef APP_VERSION` falls back to
  `"dev"`) rather than letting that be a silent hard build failure the
  way it was the first time this was added, before the fallback existed
  (two existing test targets — `smoke-test`, `transfer-queue-test` — also
  construct a real `MainWindow` and needed the same define added
  explicitly once this was caught).
  **Real, reported crash, fixed:** closing the app while connected to a
  server used to abort with SIGABRT every time — confirmed from an
  actual coredump (`journalctl`/`coredumpctl`), not a hypothetical.
  `startConnection()` parents the worker `QThread` to `this` (the
  window itself is a perfectly normal, safe parent for the *controller*
  object — see that method's own comment on why the backend can't be
  parented the same way), and closing/quitting destroys the window
  through Qt's ordinary child-object cleanup, which reaches that
  `QThread` while its thread is still running — `QThread`'s own
  destructor calls `qFatal()` in exactly that case ("QThread: Destroyed
  while thread is still running"). Root-caused by reproducing it
  directly (a throwaway harness built the identical
  `FilePaneWidget`+`SftpBackend`+`QThread`-parented-to-a-window shape
  against a real local test server and crashed with the exact same
  stack trace as the real coredump), then fixed with a `closeEvent()`
  override that swaps the right pane back to a plain `LocalBackend`
  before the window is allowed to close — reusing
  `FilePaneWidget::setBackend()`'s already-correct teardown
  (`deleteLater()` + `thread->quit()` + `thread->wait()`) rather than
  duplicating that logic. Re-verified with the same harness, modified
  to apply the fix: no crash, confirmed across multiple runs. Same
  tradeoff Disconnect already accepted mid-transfer (a blocking wait if
  the worker thread is stuck in a non-interruptible call) now also
  applies at close time — not a new risk, just the same one closing a
  gap where it hadn't been applied yet.
  **Since either pane can now hold a thread-owning backend, `closeEvent()`
  calls `disconnectPane()` on BOTH `m_leftPane` and `m_rightPane`** — the
  original fix above only ever covered the right pane, since that was the
  only one that could be connected at the time. Manually re-confirmed
  against two real local SFTP servers (both panes genuinely connected,
  real worker threads, real sockets): closing the window doesn't crash —
  see the `TransferManager`/Known gaps entries for the rest of what that
  same manual pass did and didn't cover.
  **Four more real bugs found by a dedicated code review of this file —
  the second-largest source file in the project, never previously
  reviewed on its own — all fixed:**
  1. **`closeEvent()` had silently reintroduced the exact crash described
     above, specifically for a pane still mid-connect.** The fix above
     covers BOTH panes being torn down, but never accounted for
     `disconnectPane()`'s own `isConnecting()` guard (added for a
     genuinely different reason — see `startConnection()`'s entry) simply
     no-op'ing when a pane is still mid-connect, rather than tearing down
     its worker thread. `closeEvent()` never checked for that outcome and
     always fell through to `QMainWindow::closeEvent(event)`, accepting
     the close regardless — an earlier version of this very entry even
     documented that as deliberate and safe ("the window closes
     immediately either way... rather than freezing the whole app"),
     which was wrong: since `main.cpp` relies on the default
     `quitOnLastWindowClosed`, that let the whole app exit with a worker
     `QThread` still running and still parented to the now-destroyed
     window — the identical `qFatal("QThread: Destroyed while thread is
     still running")` this whole mechanism exists to prevent. Fixed by
     having `closeEvent()` check `isConnecting()` on both panes itself,
     first, and call `event->ignore()` if either is true — refusing the
     close outright rather than accepting it and hoping for the best,
     same "wait for it to finish or fail first" tradeoff
     `startConnection()`/`disconnectPane()` already make for the
     identical hazard. Covered by `smoke-test`, using a fake backend
     whose `connectToHost()` deliberately never resolves (avoids needing
     a real hung connection or live server): confirms `window.close()`
     returns `false` and the window stays open while a pane reports
     `isConnecting()`, and returns `true` again once it doesn't.
  2. The `isConnecting()`-guard-and-status-message pattern was
     copy-pasted between `startConnection()` and `disconnectPane()` —
     structurally part of why bug #1 above was possible at all, since
     nothing enforced that a third call site (`closeEvent()`) couldn't
     independently forget it. Extracted into a shared `stillConnecting()`
     helper both now call, and `closeEvent()` uses too.
  3. `connectViaDialog()`'s SFTP-public-key validation (empty
     `privateKeyPath`) duplicated the identical three-part condition
     already in `SiteManagerDialog::onConnectClicked()`, with no shared
     helper — risking the two silently drifting if the rule ever
     changed. Extracted to `ConnectionRequest::missingRequiredPrivateKeyPath()`,
     next to that struct's existing `host()`/`useHomeDirectory()`-style
     convenience accessors; both call sites now share it. Covered
     directly in `protocol-selection-test`.
  4. The two-line "re-list whatever both panes are showing" sequence was
     duplicated verbatim in `onTransferSucceeded()` and
     `onRefreshTriggered()`. Extracted to a private `refreshBothPanes()`.
  **Synchronized browsing** (`AppSettings::synchronizedBrowsingEnabled()`,
  a View-menu toggle default OFF — unlike Filename Filter/Quick Connect
  Field, this changes navigation *behavior*, not just a UI element's
  visibility, so opt-in is the safer default) — navigating one pane
  drives the other to the corresponding relative path. First half of the
  v2 competitive-roadmap "synchronized/mirrored browsing, directory
  compare-and-sync" item; the second half (directory compare-and-sync)
  is scoped separately, later — it has a real, deliberately-unresolved
  tension with this project's "no recursive delete" precedent that this
  feature doesn't. Built entirely on existing machinery, confirmed by
  reading the code rather than assumed: every navigation entry point
  (`goBack()`/`goForward()`/`goUp()`/`goHome()`, the path bar, double-click)
  already funnels through `FilePaneWidget::navigateTo()`, and a bad
  destination path already fails gracefully — `SftpBackend`/`LocalBackend`'s
  `listDirectory()` both emit `connectionFailed` (not `directoryListed`)
  on an open failure, which `FilePaneWidget` already turns into a real
  status-label error message. So "the mirrored path doesn't exist on the
  other side" needed no new error handling at all — driving a
  `navigateTo()` with a bad path just surfaces the pane's existing error
  UI. `FilePaneWidget` gained one new signal, `directoryChanged(path)`,
  emitted from `onDirectoryListed()`'s genuine-navigation branch only
  (not a delete/rename/create's own same-directory refresh); `MainWindow`
  connects both panes' copies to one handler via `sender()`, the same
  dual-pane pattern `onFilesDropped()` already established.
  **Reentrancy guard is path-based, not a plain boolean, for a real
  reason found while designing it, not by testing**: a bare "suppress
  the next signal from the other pane" flag looks right but would get
  stuck permanently the first time a driven navigation failed (a failed
  `navigateTo()` never fires `directoryChanged` to clear it). Fixed with
  `m_pendingSyncDrivenPath` (`QHash<FilePaneWidget*, QString>`) — the
  path a pane was just told to navigate to as a driven update, consumed
  only when that exact path echoes back; a failed drive just leaves a
  harmless, never-consumed entry instead of wedging anything.
  `sync-browsing-test` confirmed this negatively as well as positively —
  deliberately disabling the consumption `return` reproduced a real
  infinite navigation loop (the test hangs to its own timeout) before
  the fix, not just a theoretical concern. Two panes' anchors (each
  pane's directory at the moment the toggle switches on) are reset
  automatically — turning synchronized browsing off, not attempting to
  re-anchor — at both places a pane's backend actually changes
  (`startConnection()`, `disconnectPane()`), since a stale anchor
  pointing at a now-gone connection would otherwise produce nonsense
  joins the next time either pane navigates.
  **A real bug found by a later code review** (`onPaneDirectoryChanged()`):
  the anchored-subtree check was `path.startsWith(anchor)`, a raw text
  prefix test with no path-separator boundary — a sibling directory that
  merely shares the anchor as a TEXT prefix (anchor `/data` matching
  `/data2/photos`, say) was wrongly treated as "inside" the anchored
  subtree, computing a bogus relative path (`"2/photos"`, since the
  character right after the prefix is `'2'` not `'/'`, so the existing
  leading-slash strip was a no-op) and driving the other pane to a
  nonsense target. Fixed by requiring the anchor to be followed by a
  separator (or matched exactly) to count as genuinely "inside" it.
  Regression-covered by a new phase in `sync-browsing-test`: a sibling
  directory sharing the anchor as a text prefix, alongside a real,
  pre-existing decoy directory at the bogus computed target that only
  the old buggy prefix match would ever navigate to — confirmed via a
  before/after `git stash` control that the old code actually lands on
  the decoy (2 failing checks) while the fix leaves the other pane
  untouched.

  **Directory Compare-and-Sync** — the second half of the "synchronized/
  mirrored browsing, directory compare-and-sync" v2 item, deliberately
  scoped separately from synchronized browsing above because of a real
  tension with this project's "no recursive delete" precedent
  (`RemoteBackend::deleteEntry()` only ever removes an empty directory —
  see its own doc comment). Resolved by NOT adding a recursive-delete
  primitive to the backend layer at all: a new orchestrator,
  `CompareSyncExecutor` (`src/compare/CompareSyncExecutor.h/.cpp`),
  builds deletion entirely out of many individual, existing,
  non-recursive `deleteEntry()` calls — every selected file first, then
  every selected directory in a second, separate batch sorted deepest-
  first by relative-path depth, guaranteeing each directory is already
  empty (its own children, and any deeper subdirectories, already
  deleted earlier in the same pass) by the time its own non-recursive
  call runs. Gated behind an explicit, off-by-default "Also allow
  deleting files not present at source" checkbox in the new
  `CompareDialog` (`src/ui/CompareDialog.h/.cpp`, a non-modal `QDialog`
  reached via the View menu's "Compare Directories...", independent of
  whether synchronized browsing is on), and a second, separate itemized
  confirmation dialog (`CompareDeleteConfirmDialog`) right before
  anything is actually deleted — the tree's own checkboxes are the
  *selection* step, that dialog is the *commit* step, one more explicit
  gate than `FilePaneWidget::confirmAndDelete()`'s existing single
  "Delete N items?" prompt, justified because a compare-driven delete
  can affect far more items at once via bulk tree selection.

  The diff engine, `DirectoryComparer` (`src/compare/DirectoryComparer.h/.cpp`),
  reuses `FolderEnumerator` (see its own entry above) rather than
  building a second recursive-walk mechanism — one instance per side,
  joined on both `finished()` signals, classified by relative path into
  Only-in-Left/Only-in-Right/Differs/Identical. Classification is size
  AND modified time, deliberately not either alone (a same-size-
  different-content edit and a byte-identical re-upload with only a
  fresh timestamp are both real false-negative risks a single-field rule
  would miss) and deliberately not content hashing (matches this
  project's existing, still-open "transfer integrity checksums" v2 item,
  flagged there as possibly protocol-limited and not assumed easy).
  `EnumeratedItem` needed one small additive change first — it never
  carried `modified` at all (only `relativePath`/`isDir`/`size`), even
  though the `RemoteEntry` it's built from already had it; a one-line
  fix in `FolderEnumerator::onDirectoryEnumerated()`.
  **A real bug caught while writing `DirectoryComparer`, before it ever
  ran**: passing an empty `rootName` to `FolderEnumerator` (so both
  sides' paths are comparable as "relative to each side's own root,"
  unlike folder-transfer's own use of `rootName` to seed the manifest
  with the folder's own name) means `PathUtils.h`'s `joinPath("", name)`
  — which only special-cases a *trailing* slash on its `dir` argument,
  not an *empty* one — silently produces a leading `/` on every result.
  `DirectoryComparer::classify()` strips it and drops the synthetic
  root item (`relativePath == ""`) before ever building a `CompareEntry`.
  **A second real bug, this one caught by `compare-sync-test` actually
  failing during verification**: the test's own fixture helper called
  `QFile::setFileTime()` on a still-open handle, right before `close()`
  — and `close()`'s own buffered-write flush silently reset the
  timestamp straight back to "now" afterward, since it's itself a
  further write-affecting syscall. Not a product bug — a test-fixture
  bug — but a real one that would have made the size+modified
  classification's own regression coverage falsely pass by accident
  (both sides landing on nearly the same real "now" timestamp) rather
  than genuinely exercising the different-modified-time case. Fixed by
  closing fully first, then reopening fresh (with nothing left to flush)
  to set the timestamp as a completely separate operation.
  `CompareSyncExecutor::copySelected()` reuses `TransferManager::enqueue()`
  exactly as-is (including its existing folder-transfer precedent of a
  multi-segment relative path already joining correctly) with one small
  additive `TransferManager` change: a new `assumeOverwrite` parameter on
  `enqueue()`, which just sets the same `TransferItem::
  skipConflictCheckOnDispatch` flag `resumeItem()` already uses — reused,
  not duplicated — so a bulk copy of many already-known-conflicting
  `Differs` rows doesn't pop one `askConflict()` modal per file.
  A required `FilePaneWidget` refactor: `confirmAndDelete()`'s per-entry
  delete loop was extracted into a new public `deleteEntriesAt()` so
  `CompareSyncExecutor`'s deletes (which can span many different
  directories in one batch, unlike any existing single-directory delete
  caller) still route through the pane's own `m_pendingFileOpRefreshes`
  bookkeeping — calling `deleteEntry()` directly on the backend instead
  would have each delete's own resulting re-list misread as genuine
  navigation, corrupting the path bar/history and potentially re-driving
  the other pane if synchronized browsing happened to be on at the same
  time. Covered by a new 21st required test target, `compare-sync-test`
  — see CONTRIBUTING.md's own subsection for its three scenarios. Not
  verified against a real live server (deterministic `LocalBackend`
  coverage judged sufficient, same call made for a prior `TransferManager`
  concurrency fix); explicit non-goals kept out of this pass: content
  hashing, auto-repeat/scheduled sync, and any new symlink-specific
  handling beyond whatever the existing `RemoteEntry` pipeline already
  reports.

  **Recursive delete, with a warning** — David asked directly for this
  project's own "worth revisiting explicitly if 'delete this folder and
  everything in it' turns out to be something people actually want"
  note (see `RemoteBackend::deleteEntry()`'s own doc comment) to
  actually be revisited. Researched how the three established
  competitors (FileZilla, Cyberduck, WinSCP) handle this before
  designing, rather than assuming: all three recurse unconditionally
  once a plain "Delete this?" is confirmed, with no distinct, content-
  aware warning — and WinSCP users have a long-standing, still-
  unimplemented feature request in WinSCP's own tracker asking for
  exactly that distinction ("this directory is NOT EMPTY, remove dir &
  all its contents?") instead of the same generic message every time,
  confirming this really is a still-open gap worth doing better on, not
  a solved problem to copy verbatim. Discussed two shapes with David —
  proactive (scan every selected folder's contents up front, one
  combined warning) vs. reactive (keep the plain confirmation
  unchanged, add a second, more specific warning only when a folder
  turns out to have contents) — and he chose reactive: simpler, lower-
  risk, and matching this exact codebase's own established precedent
  (`CompareSyncExecutor`'s own delete flow, immediately above, already
  uses a deliberate two-gate confirmation specifically because a bulk
  delete can affect more than a single click implies — same reasoning,
  reused here).

  The backend primitive itself is UNCHANGED — `RemoteBackend::
  deleteEntry()` still fails cleanly on a non-empty directory, across
  all three backends, exactly as before. Recursive delete is built
  entirely as a higher-level `FilePaneWidget` orchestration out of that
  same unchanged primitive, reusing `CompareSyncExecutor::
  deleteSelected()`'s own bottom-up pattern (files first, then
  directories sorted deepest-first by relative-path `/` count) rather
  than inventing a second one: `deleteEntriesAt()` gained one new opt-in
  parameter, `offerRecursiveDeleteOnFailure` (default `false` — every
  existing caller, right now only `CompareSyncExecutor`, is completely
  unaffected; `confirmAndDelete()` is the one caller that opts in, kept
  deliberately separate so a Compare-driven delete can never be offered
  a recursive escape hatch for a directory that turns out to still
  contain something identical on both sides, which would silently
  delete more than the diff actually determined was "extra"). When set,
  every dispatched directory path is tracked in a new
  `m_recursiveDeleteCandidates` set; a `"Delete"` failure for a tracked
  path routes to a new `offerRecursiveDelete()` instead of the plain
  failure warning — which walks the whole tree via `FolderEnumerator`
  (same construction/lifecycle `TransferManager::
  startFolderEnumeration()` already established), then shows a content-
  aware warning ("isn't empty — it contains 12 files and 3 folders...")
  built from the real enumerated count, and — only on Yes — deletes
  everything via two `deleteEntriesAt(..., true)` calls (files, then
  deepest-first directories) built from the already-enumerated list, no
  re-walk needed. Passing `true` again on that second dispatch is
  deliberate, not an oversight: a nested directory that unexpectedly
  fails its own turn (a genuine race, not the common case) gets the
  identical recursive offer instead of just failing inconsistently with
  its siblings.

  **The reentrancy guard from the Write Into crash fix (see the
  `RemoteBackend::createDirectory()`/`ignoreAlreadyExists` entry
  earlier) was generalized, not duplicated, for this feature.** That
  guard (`m_warningDialogInProgress`/`m_deferredFailureWarnings`) only
  ever protected `onFileOperationFailed()`'s own plain warning against
  `QMessageBox::exec()`'s event-queue-pumping reentrancy hazard. This
  feature introduces a SECOND, distinct kind of modal (the recursive-
  delete confirmation) with the identical hazard — and, with two kinds
  live, they could reenter EACH OTHER, which two separate, uncoordinated
  guards would not have caught. Widened instead:
  `m_modalDialogInProgress` plus a generic `m_deferredDialogActions`
  (`QList<std::function<void()>>`, replacing the old fixed 3-string
  struct) and a shared `runOrDeferModalAction()` helper, so at most one
  modal dialog from this class is ever open at a time regardless of
  which kind — the exact same "future hand-wired dialog reentrancy needs
  the same guard" lesson the earlier fix's own doc comment already
  anticipated, now actually applied a second time.

  New required-suite target `recursive-delete-test` (25th) drives the
  real `QMessageBox` via `deleteEntriesAt()` directly — the same public
  entry point `CompareSyncExecutor` already uses — rather than
  simulating the context menu, matching how `file-operations-test`
  already tests `deleteEntry()` at the lower, unmodified backend-
  primitive level instead. 18 assertions: empty folder and single file
  both unaffected (no new dialog); declining leaves a non-empty
  multi-level folder completely untouched; accepting deletes every file
  at every depth and every subfolder, root last; a mixed multi-select
  batch (one empty, one non-empty) proves the empty one deletes
  immediately while the non-empty one gets its own follow-up. A real,
  visible bug caught by this project's own established "screenshot new
  UI before calling it done" discipline, not by the headless
  assertions: the warning's item-count text initially read "1 folders"
  for a single subfolder — fixed with plain `count == 1` branching
  (matching `confirmAndDelete()`'s own identical singular/plural
  handling), not Qt's `%n` mechanism, since no i18n loading
  infrastructure exists in this project yet to make that machinery
  worth reaching for. Explicit non-goals: `RemoteBackend::deleteEntry()`
  itself unchanged; `ScriptRunner`'s CLI `rm` unchanged (no interactive
  confirmation flow to hook a warning into, and not asked for); no
  progress UI for a large recursive delete (matches this project's
  existing multi-delete UX, which already has none).

  **A real, live-reported performance regression from the Write Into
  crash fix above, found the same way the crash itself was — a real
  user's report, not assumed:** "when I go to transfer a large set of
  data, the app hangs and says preparing transfer" (SFTP, ~10 seconds,
  window stayed responsive — accumulated round-trip latency, not a true
  GUI-thread freeze). Root cause: `TransferManager::
  startFolderFileTransfers()`'s directory-creation loop passed
  `ignoreAlreadyExists=true` to `createDirectory()` UNCONDITIONALLY, for
  every directory in the enumerated tree, regardless of whether the
  transfer was even a Write Into merge. That parameter is not free —
  `SftpBackend`'s implementation does a real, synchronous
  `libssh2_sftp_stat()` round trip before every `mkdir`; `FtpBackend`'s
  does a full parent-directory LISTING — so a folder with many
  subdirectories paid one full network round trip per directory before
  its first `mkdir` even started. For a genuinely NEW top-level
  destination folder (`rootAlreadyExists == false`, confirmed by
  `enqueueFolder()`'s own upfront existence check before enumeration
  even begins) that check can never do anything but waste time: a
  nested path cannot exist unless its parent does, so nothing inside a
  provably-new folder can possibly already exist either — the exact
  same "verify rather than assume" reasoning this project already
  applies to code review applies here structurally, by the nature of a
  hierarchical namespace, not by probability. Fixed by passing
  `rootAlreadyExists` itself as the argument instead of a hardcoded
  `true` — the existence check now runs only for an actual Write Into
  merge, where a nested directory genuinely might already exist, and is
  skipped entirely otherwise, restoring the pre-crash-fix behavior for
  the overwhelmingly common (non-merge) case. The fix applies uniformly
  to `SftpBackend` and `FtpBackend` both, from the one call site, even
  though only SFTP had been tested live when reported.

  No new required-suite target was added for this fix: it changes
  timing, not behavior, and `LocalBackend`'s own version of the
  existence check has no round-trip cost to observe either way, so no
  local backend (real or a new fake/spy one) could actually demonstrate
  the regression or its fix — a new argument-value-spy test would only
  re-assert the one-line diff itself. Instead verified via the existing
  `conflict-resolution-test`'s Phase D/G (which drive the
  `rootAlreadyExists == true` path directly and confirm it still
  receives `ignoreAlreadyExists == true`, unaffected by this change) and
  `folder-transfer-test` (confirms `rootAlreadyExists == false` folder
  transfers still complete correctly end-to-end against a real nested
  tree), plus the full 25-target required suite, all passing.

  **David reported the SAME "Preparing to transfer" symptom again after
  the fix above shipped, with a real caveat: "It may have been a little
  quicker but it left the user in a state where they might be concerned
  the app has frozen."** The v0.7.37 fix above was real, but it only
  ever addressed the directory-CREATION side (the `ignoreAlreadyExists`
  stat/list check). It never touched the actual dominant cost for a
  large tree: `FolderEnumerator`'s own walk, which lists one directory
  per real network round trip, strictly serially (see that class's own
  header comment on why — a real `SftpBackend` has exactly one session,
  so concurrent listing calls would just serialize through libssh2
  anyway). For a tree with many directories, that's still a real,
  structurally-unavoidable amount of network latency before enumeration
  even finishes — but the actual UX bug was separate from that latency
  itself: `MainWindow`'s status bar showed the exact same static
  "Preparing to transfer..." string for the entire walk, with zero
  visible change between `folderTransferStarted` and
  `folderTransferFinished`. A perfectly healthy multi-second walk and a
  genuine hang were, from the user's perspective, indistinguishable.
  Fixed by surfacing progress that was already happening internally but
  never exposed: `FolderEnumerator` gained a new `itemsDiscovered(int
  itemsSoFar)` signal, emitted once per directory listing folded into
  its results (a running total, not a percentage — the tree's total
  size isn't known until the walk is already done, so there's nothing
  to divide by yet); `TransferManager::startFolderEnumeration()`
  connects to it and re-emits a new `folderTransferProgress(folderName,
  itemsFound)` signal; `MainWindow` updates the status bar live —
  `"Preparing to transfer \"%1\"... (%2 items found so far)"` — instead
  of leaving it static. No new round trips, no change to WHEN the
  transfer actually starts — purely additive visibility into work
  that was already in flight.

  Verified non-vacuously, not just by argument: extended
  `folder-transfer-test`'s existing `myfolder` fixture (5 real
  directories: itself, `subdir1`, `subdir2`, `subdir2/nested`,
  `emptydir`) with assertions that `folderTransferProgress` fires at
  least once per directory listed, that its counts are monotonically
  non-decreasing (NOT strictly increasing — a first draft of this
  assertion was itself wrong and caught by the test failing: listing a
  genuinely empty directory like `emptydir` correctly contributes zero
  NEW items, so two consecutive equal counts is valid, not a bug), and
  that the final count matches the whole tree (5 directories + 4 files
  = 9). Full 25-target required suite passed.

  **Three small, direct UI requests, batched into one change: a quick
  connect password field, a protocol-combo wording cleanup, and a
  per-pane refresh button.**

  1. *Quick connect gained its own password field and no longer pops a
     `QInputDialog` prompt.* `onQuickConnectReturnPressed()` used to read
     Host/Username/Port/Protocol straight from the toolbar but still
     asked for the password via a separate modal (`QInputDialog::
     getText(Password)`, matching `SiteManagerDialog::
     onConnectClicked()`'s own pattern) — a real, if minor, contradiction
     of "quick." New `m_quickConnectPasswordEdit` (`QLineEdit`,
     `EchoMode::Password`) sits between Username and Port, matching this
     toolbar's own established real-credential-entry-inline precedent
     (this IS the one-off credential entry point, not a saved site — see
     `CLAUDE.md`'s non-negotiable: never written to `sites.json`, and
     this toolbar never persists anything, matching Recent Connections'
     own "nothing is ever stored" convention). `onQuickConnectReturnPressed()`
     now just reads `m_quickConnectPasswordEdit->text()` directly, with
     no ok/cancel gate — Enter/Connect commits immediately, same as every
     other field on this row already does. The field is cleared after
     connecting alongside Username/Host/Port, unchanged. Private-key auth
     is still explicitly out of scope for this toolbar — no key-path
     field was added; the full Connect dialog remains the only way to
     use a key, exactly the existing documented limitation in README.md.
  2. *Protocol combo entries dropped their explainer suffixes for the
     two protocols that don't need one.* `Protocol::displayNameFor()`
     (the single shared source `ProtocolCombo::populate()` feeds into
     every combo in the app — ConnectionDialog, SiteManagerDialog,
     PreferencesDialog, and this quick connect toolbar) used to read
     "SFTP (SSH File Transfer Protocol)", "FTP (unencrypted)", "FTPS
     (FTP with explicit TLS)", "FTPS (Implicit)" — verbose, and
     inconsistent about WHY the suffix existed: FTPS's two variants
     genuinely need a distinguishing suffix (both would otherwise read
     as bare "FTPS"), but SFTP and FTP are each the only entry of their
     kind in the list, so their suffixes carried no distinguishing
     information, just noise. Now: "SFTP", "FTP", "FTPS (Explicit)",
     "FTPS (Implicit)" — every entry that needs a suffix to stay
     distinguishable keeps one, every entry that doesn't, doesn't. Fixed
     in exactly one place — the whole reason `ProtocolCombo::populate()`
     was extracted in the first place (see that entry's own comment on
     the bug it fixed: a new protocol value silently missing from one of
     three hand-duplicated combos). A stale doc comment on
     `displayNameFor()` claiming it fed "the connection dialog's title"
     was also corrected while touching this — `ConnectionDialog`'s title
     is actually a fixed `tr("Connect to Server")`, not derived from the
     protocol at all; found by reading the code directly rather than
     trusting the existing comment.
  3. *File panes gained a dedicated Refresh button, right after Home in
     the nav row.* A `refresh.svg`-iconed action already existed in the
     right-click context menu (`navigateTo(currentDirectory())` —
     deliberately a fresh re-listing, not a cache read), but there was no
     toolbar equivalent — every other common nav action (Back/Forward/
     Up/Home) already has one. New `m_refreshButton` (`QToolButton`,
     Blue-tinted like its three siblings, same 24px size), wired to the
     identical `navigateTo(currentDirectory())` call the context-menu
     item already uses — `navigateTo()` already self-guards against a
     navigation already in flight (`m_navigationInFlight`), so the button
     needs no additional debouncing of its own. Added to `retintIcons()`
     alongside the other three nav buttons so a theme switch re-tints it
     too, same as them. Deliberately NOT disabled/enabled based on any
     state (matching Up/Home's own always-enabled behavior, unlike
     Back/Forward which track real history) — refreshing the current
     directory is always a valid action whenever a pane has one.

  All three verified visually via a disposable, non-committed probe (a
  temporary CMake target constructing a real `MainWindow`, replicating
  `main.cpp`'s own initial theme-QSS-load step and using an isolated
  `XDG_CONFIG_HOME` for a clean-default `AppSettings` — same two gotchas
  this project's "visually check new UI" discipline has hit before):
  screenshots confirmed the password field renders correctly between
  Username and Port, the protocol popup shows exactly "SFTP / FTP / FTPS
  (Explicit) / FTPS (Implicit)" with no truncation or wrapping, and the
  refresh button renders correctly immediately after Home in both panes.
  No functional/headless test added for any of the three — all three are
  either pure display-string changes or thin, directly-reused wiring
  around an already-tested primitive (`navigateTo()`,
  `startConnection()`), with no new branching logic of their own to
  regression-test. Full 25-target required suite passed unaffected (no
  test depended on the removed `QInputDialog` prompt or the old protocol
  strings). README.md's own Quick Connect feature bullet updated too — it
  had gone stale describing the pre-v0.7.13 single free-text-field
  syntax, never updated after that toolbar redesign; fixed properly
  while already touching this toolbar rather than left stale further.

  **Keyboard shortcuts** — a direct user request ("Enable the delete key
  for deleting file, copy/paste...possibly other keys, shortcuts as
  well"): Delete, F2 (rename), F5 (refresh), Ctrl+A (select all), and
  Ctrl+C/Ctrl+V (copy-then-paste across panes). Scope for the open-ended
  "possibly other keys" was picked deliberately, not exhaustively: every
  shortcut added either directly reuses an existing context-menu action
  (Delete/Rename/Refresh) or an existing `QAbstractItemView` primitive
  (`selectAll()` for Ctrl+A) with zero new logic, or — Copy/Paste — is
  built entirely on the same `enqueueEntries()` `TransferManager`
  plumbing Transfer Selected/drag-and-drop already share. Deliberately
  NOT added: Ctrl+X/Cut. A traditional "cut" implies move-via-copy-then-
  delete-on-success across ANY two locations, semantically distinct from
  (and easy to confuse with) this app's existing "Move Selected" feature
  (a real server-side rename, same-connection only) — implementing a
  correct cross-connection cut (delete only after confirmed transfer
  success, handle partial-batch failure) would be real new transfer
  logic, not shortcut wiring, and wasn't asked for.

  `FileTreeView` gained a `keyPressEvent()` override translating five
  key combinations into five new signals
  (`deleteKeyPressed`/`renameKeyPressed`/`refreshKeyPressed`/
  `copyKeyPressed`/`pasteKeyPressed`) — Ctrl+A is handled entirely
  inside that override (`selectAll()` needs nothing from the owning
  pane). `FilePaneWidget` connects each to a new private slot mirroring
  `showContextMenu()`'s own per-action logic exactly (same
  empty-selection and single-selection guards, same
  `confirmAndDelete()`/`promptAndRename()`/`navigateTo()` calls) — a
  shortcut can never do something its context-menu equivalent wouldn't
  also allow. Also set `m_view->setEditTriggers(QAbstractItemView::
  NoEditTriggers)` in `buildUi()`, explicit rather than relying on Qt's
  default (`DoubleClicked | EditKeyPressed`, the latter's platform key
  being F2 on most platforms) — items are `QStandardItem`-editable by
  default and this app has never handled Qt's own built-in inline cell
  editor's `setData()`, so without this, F2 could additionally pop that
  editor alongside (or instead of) the real `promptAndRename()` dialog.

  Copy/Paste needed real design thought, not just wiring, because of one
  genuine hazard the other four shortcuts don't have: every other
  cross-pane operation this app already had (Transfer Selected, Move
  Selected, drag-and-drop) is a single synchronous user gesture with no
  gap for anything to change in between, but copy-then-paste has an
  inherent, arbitrary time gap BY DESIGN — copy now, possibly navigate
  around, paste later. `TransferManager::enqueue()`/`enqueueFolder()`
  both derive their actual source path from the pane's CURRENT
  directory at the moment they're called, not anything captured
  earlier (see that method's own doc comment) — so a naive
  implementation that just replayed `enqueueEntries(copiedSourcePane,
  destPane, copiedEntries)` at paste time would silently transfer
  whatever now happens to share those names in the source pane's NEW
  directory if it had navigated away, not what was actually selected at
  copy time. Two new `FilePaneWidget` signals, `filesCopied(entries)`
  and `pasteRequested()` (no payload — MainWindow holds the clipboard),
  route through `MainWindow`, which owns the actual clipboard state
  (`m_clipboardSourcePane`/`m_clipboardSourceDirectory`/
  `m_clipboardEntries`) since a paste can target either pane regardless
  of which one the copy happened on, same symmetric ownership every
  other cross-pane operation here already has. `onFilesCopied()`
  captures the source pane's `currentDirectory()` AT COPY TIME;
  `onPasteRequested()` refuses the paste (with a clear status-bar
  message, matching `moveRequested`'s own "explain why, don't guess"
  precedent) if that directory no longer matches the source pane's
  CURRENT one, rather than risk transferring the wrong thing. Also
  refuses pasting into the same pane it was copied from — there's
  nothing for that to mean (no same-pane duplicate feature exists).
  Deliberately NOT handled: a pane's backend being swapped
  (Connect/Disconnect/Reconnect) between copy and paste — judged
  low-risk to leave uncovered rather than hunting down every
  `setBackend()` call site, since a reconnect overwhelmingly likely also
  changes `currentDirectory()` (a fresh connection rarely lands back on
  the exact same path), which the existing directory-match check already
  catches in practice; a noted, deliberate scope boundary, not an
  oversight.

  New 26th required-suite target `keyboard-shortcuts-test` — drives a
  real `MainWindow` (same construct-it-directly pattern `smoke-test`/
  `sync-browsing-test`/`transfer-queue-test` already establish) with
  real `QTest::keyClick()` calls against the real `QTreeView`, not
  simulated by calling the handler slots directly, so it actually proves
  `FileTreeView::keyPressEvent()` itself dispatches correctly. 25
  assertions across seven phases; the two that matter most
  (same-pane-paste refused, stale-source-directory paste refused) were
  proved non-vacuous with a real sabotage-and-restore cycle — temporarily
  disabling the same-pane check made its exact assertion fail on a real
  rebuild+run, then the real code was restored and reconfirmed passing —
  the same before/after-control discipline this project applies to every
  fix, used here for a brand-new feature's own safety checks instead of
  an existing bug. Full 26-target required suite passed.

  **Transfers pane: right-click a tab header for "Clear Completed"/
  "Clear Failed"** — a direct user request ("Transfers pane needs
  right-click clear options for tab headers"). The real design question
  wasn't the UI (a `QTabBar::customContextMenuRequested` handler,
  same shape `FileTreeView`'s own `customContextMenuRequested` wiring
  already uses) but how to actually remove items from
  `TransferManager`'s backing list safely. `m_items` isn't just a plain
  list — several maps (`m_pendingFileConflictChecks`,
  `m_pendingFolderConflictChecks`, `m_pendingMoveConflictChecks`) store
  a raw INDEX into `m_items` across a real async round trip (a
  `checkExists()`/conflict-check response can arrive well after it was
  dispatched). Physically compacting the list on removal
  (`QList::removeAt()`) would shift every LATER item's index out from
  under any of those still in flight, silently corrupting an unrelated,
  genuinely-active item's own conflict resolution the next time one of
  those maps was consulted — a real, serious correctness risk, not a
  theoretical one, given how central these maps are to dispatch.

  Solved by NOT removing anything from `m_items` at all: new
  `TransferItem::clearedFromQueue` (default `false`) is a soft-hide
  flag, the exact same "exists in `m_items`, invisible to the UI" shape
  `isVerificationTask` already established for a different reason (a
  checksum-verification temp-download that was never meant to be
  visible in the first place, vs. this — a user-initiated hide of
  something that WAS visible). New `TransferManager::clearItems(const
  QList<int> &ids)`: for each id, looks it up via the existing
  `indexById()` helper, skips it unless its CURRENT status is already
  terminal (Done/Failed/Cancelled/Skipped — defensive, since `ids` was
  gathered from a tab's row set before the context menu's own
  `menu.exec()` nested event loop ran, and could theoretically have
  changed via an unrelated Retry in the meantime — the identical class
  of "state can change while a modal pumps the event queue" race
  `FilePaneWidget::showContextMenu()`'s own comment already documents
  for `currentDirectory()`), sets the flag, and emits a new
  `itemRemoved(int id)` signal — deliberately distinct from
  `itemUpdated`: it means "stop showing this row," not "re-render it."
  `TransferQueueTable::resortAndRebuild()` gained the matching filter
  (`if (item.clearedFromQueue) continue;`, right beside the existing
  `isVerificationTask` check) — without it, a LATER header-click resort
  would silently resurrect a cleared item straight from
  `m_manager->items()`, since that method itself still returns
  everything, cleared or not (by design — this is the one thing the
  soft-hide approach costs: `items()` callers that don't already filter
  `isVerificationTask` also need to start filtering `clearedFromQueue`,
  though `clearItems()`'s own restriction to already-terminal items
  means the only realistic caller affected is exactly the queue-display
  code this feature touches). `TransferQueueWidget::onItemRemoved()`
  mirrors `onItemUpdated()`'s own category-migration shape exactly
  (look up which tab currently holds the id via `m_categoryById`,
  remove it from that specific `TransferQueueTable`, refresh that tab's
  label count) but for outright removal instead of a move.

  The Active tab's menu item is shown, not hidden, but permanently
  disabled — matching this app's own established "visible-but-disabled
  over hidden" convention (e.g. Disconnect) rather than making a
  right-click there look inert/broken. Nothing in Active is safe to
  silently discard from the visible queue without also cancelling the
  underlying transfer, which this menu deliberately doesn't do (Cancel
  is a different, already-existing action, on a different menu, on the
  per-ROW right-click, not this per-TAB one).

  Verified two ways: a real screenshot of all three tabs' menus
  (Completed/Failed enabled with a trash icon, Active shown-but-dimmed)
  confirmed the UI renders correctly; a new permanent Phase 8 in
  `transfer_queue_test.cpp` (a fresh, isolated `TransferManager`/
  `TransferQueueWidget` pair, same "fresh pair" precedent Phase 4/5
  already establish) drives `clearItems()` directly rather than
  simulating the right-click itself — matching this project's own
  "test at the seam that actually changed" convention when the UI glue
  on top is this thin (recursive-delete-test's own header comment makes
  the same call for its analogous QMessageBox-vs-deleteEntriesAt()
  choice). Confirms per-tab isolation (clearing Completed never touches
  Failed's own 2 items and vice versa), that a cleared item stays
  cleared through a REAL subsequent resort (the actual bug the
  `clearedFromQueue` filter exists to prevent), that an unknown/
  ineligible id is a safe no-op, matching `retryItem()`/`cancelItem()`'s
  own established shape for an unrecognized id. **A real, unrelated bug
  found while writing this test, not a feature bug**: the test's own
  first draft constructed fresh panes, called `navigateTo()`, and
  enqueued in the same synchronous block — the exact
  navigateTo()-races-enqueue() setup race this project's own history
  already documents (v0.7.19) — caught immediately by the test's own
  setup assertion failing (0 completed / 3 failed instead of 1/2, since
  the "real" transfer landed in the wrong directory too), fixed with a
  real settle-poll before enqueueing, matching the established fix
  pattern exactly. Full 26-target required suite passed.

  **Drag files between this app and the OS's own file manager** — a
  direct user request ("allow the dragging of files between the app and
  the system file manager"), explicitly both directions. Every existing
  drag-and-drop in this app was same-process-only (`FileTreeView`'s own
  custom `application/x-zephyrftp-sourcepane` MIME type, see that
  class's own header comment) — real interop with Nautilus/Explorer/
  Finder needed genuinely new plumbing on both sides.

  **Drag IN (OS -> a pane)**: `FileTreeView::dragEnterEvent()`/
  `dragMoveEvent()`/`dropEvent()` now also accept `event->mimeData()->
  hasUrls()` alongside the existing custom-MIME-type check — checked
  second, so an internal same-app drag is completely unaffected. A real
  drop extracts local paths via `QUrl::toLocalFile()` and emits a new
  `externalFilesDropped(QStringList)` signal (parallel to
  `filesDroppedFrom`, forwarded through `FilePaneWidget` the same way),
  routed by `MainWindow::onExternalFilesDropped()` to one of two new
  `TransferManager` entry points: `enqueueExternalUpload()` (mirrors
  `enqueue()`'s own direction-decision shape, but with no source pane at
  all — `item.sourcePane = nullptr`, `item.sourcePath` is the dropped
  absolute path used as-is) and `enqueueExternalFolder()` (mirrors
  `enqueueFolder()`, but `FolderEnumerator` — confirmed by reading it
  directly, it only ever needs a `RemoteBackend*` + root path + root
  name, zero `FilePaneWidget` coupling — runs against a throwaway
  `LocalBackend` constructed just for the walk, parented to `this`,
  `deleteLater()`'d once enumeration finishes). `PendingFolderConflictCheck`
  gained an `isExternal`/`externalLocalRootPath` pair (default-
  initialized, so `enqueueFolder()`'s own existing call site is
  unaffected) so a dropped folder that already exists at the destination
  gets the exact same Write Into dialog and `m_directoryConflictResolution`
  "apply to all" memory an ordinary pane-to-pane folder conflict already
  uses — one shared conflict-resolution code path, not a duplicate.

  **A real, non-obvious risk investigated (not assumed) before writing
  any of this**: could `sourcePane == nullptr` crash something?
  `LocalToLocal`/`LocalToRemote` are the ONLY two directions an external
  upload ever uses, and reading `requiredBackendsForDispatch()`/
  `dispatchActiveItem()`/`poolPaneForItem()` directly confirmed none of
  the three ever dereference `item.sourcePane` for those two directions
  — an upload's "source" was never more than a path string to begin
  with. `EditDownload`/`EditUpload` (Edit-in-place) already prove the
  same "no sourcePane at all" shape is safe elsewhere in this exact
  class. The one other place `item.sourcePane` is read
  (`ChecksumVerifier.cpp`) only does so for `RemoteToLocal`/
  `EditDownload`, never `LocalToLocal`/`LocalToRemote` — confirmed by
  grepping every `.sourcePane`/`->sourcePane` reference in the codebase,
  not assumed from reading `enqueue()` alone.

  **Drag OUT (a pane -> the OS)**: for a LOCAL pane, trivial — the
  selected entries already exist as real files, so `startDrag()` just
  adds `QUrl::fromLocalFile(...)` for each alongside the existing
  internal MIME data. For a REMOTE pane, Qt has no cross-platform
  delayed-rendering drag support (the technique some native OS apps use
  to hand over bytes lazily as the drop target actually asks for them),
  so the only portable option is downloading first — new
  `FileTreeView::downloadForDragOut()`, called from inside `startDrag()`
  itself, which runs a real nested `QEventLoop` (with a visible,
  `Qt::WindowModal` `QProgressDialog` — Cancel genuinely calls
  `TransferManager::cancelItem()` on whatever's still in flight, not
  just abandoned) BLOCKING the drag gesture until every selected file
  has downloaded to a real local temp path via
  `TransferManager::startEditDownload()` — the exact same primitive
  Edit-in-place already uses, reused as-is rather than adding a new
  direction or a hidden-task flag (its own downloads already show
  normally in the visible Transfers queue, judged fine — arguably
  useful feedback for what could be a slow multi-file wait before a drag
  can even start). All-or-nothing: any single failure or a Cancel click
  discards everything (including whatever DID already finish — its temp
  file is removed immediately) rather than ever handing the OS a
  partial, silently-incomplete drag. Deliberately NOT attempted for a
  selection containing any folder — recursively downloading an entire
  tree before a drag gesture can even start would be an unbounded-
  duration UX trap with no way to cancel just that one drag partway
  through; a folder in the selection simply means no real OS file data
  is offered for that drag (the internal same-app drag is completely
  unaffected either way), not an error or a partial attempt.

  **A narrow, deliberate exception to this class's own established
  boundary**: `FilePaneWidget` (and `FileTreeView`, via it) now
  optionally holds a `TransferManager*`, injected the same way
  `AppSettings*` already is (a 4th constructor parameter, defaulting to
  `nullptr` so every existing test call site keeps compiling unchanged).
  This class has never talked to `TransferManager` directly before —
  the established shape is "emit a UI-intent signal, let `MainWindow`
  act on it" — but that shape fundamentally doesn't fit here:
  `startDrag()` must sychronously block on a real download via a nested
  event loop, tied to the ACTIVE mouse-drag gesture Qt is mid-way
  through; there's no way to hand this off to `MainWindow` via a signal
  and get a useful synchronous answer back in time the way every other
  MainWindow-mediated operation this class has can. A null
  `transferManager` (every existing test) just means remote-pane
  drag-out silently isn't offered — internal drag-and-drop is completely
  unaffected regardless.

  New 27th required-suite target `external-drop-test` — Phases A-D
  cover drag-IN (real `LocalBackend`, real temp directories, a real
  `QDropEvent` delivered via `QCoreApplication::sendEvent()` — a
  legitimate way to exercise a protected event handler from outside the
  class, dispatching through `QWidget::event()` exactly as the real
  windowing system would). Phases E-G cover drag-OUT by calling
  `downloadForDragOut()` directly (exposed public specifically for this
  — a real native drag gesture can't be reliably triggered under the
  `offscreen` platform at all, this project's test suite has no
  existing precedent for simulating one, and the thin mouse-gesture
  layer `startDrag()` adds on top is a handful of lines, not worth the
  same investment as the actual download-orchestration logic under it)
  against a small fake non-local `RemoteBackend` — direction-agnostic
  logic, so a fake is exactly as faithful here as a live SFTP server
  would be for this specific purpose. Covers real single- and multi-file
  success (real, distinct downloaded content, real URLs pointing at real
  files) and — the assertion that actually matters — a partial failure
  returning completely empty with no leaked temp file for whatever DID
  succeed first, verified by scanning the real `zephyrftp-staging/`
  directory before and after, not just trusting the code; proved
  non-vacuous by a real sabotage-and-restore cycle (temporarily removing
  the cleanup loop made that exact assertion fail on a rebuild+run).

  **Two real, unrelated bugs found while building this, neither in the
  feature's own logic**: (1) `FileTreeView.cpp` now references
  `TransferManager` symbols directly, which broke the LINK step (not the
  compile) for three existing targets
  (`navigation-test`/`recursive-delete-test`/`sort-and-commands-test`)
  that already linked `FileTreeView.cpp` but never needed
  `TransferManager.cpp` before — caught immediately by a full rebuild,
  fixed by adding it (plus `TransferQueueStore.cpp`) to their dependency
  lists, confirmed with a correct line-based (not regex-across-
  multiline — this project has been burned by that exact mistake once
  already) Python audit across all 45 CMake targets. (2)
  `TransferManager`'s own constructor sweeps the ENTIRE shared
  `zephyrftp-staging/` directory clean on startup (the documented crash/
  leak backstop) — `external-drop-test`'s own Phase E/F/G originally
  scheduled their independent `TransferManager` constructions only
  200ms apart, close enough that a LATER phase's constructor could
  (and, once observed via scrambled assertion print order, demonstrably
  did) sweep away an EARLIER phase's own still-in-flight downloaded temp
  file. Fixed by widening the gap to a full second per phase. Full
  27-target required suite passed, confirmed stable across multiple
  consecutive full runs (this exact class of "passes standalone, fails
  only inside the full sequential suite" symptom is also what led to
  finding the keyboard-shortcuts-test margin issue above — the same
  detection discipline caught both).

  **Fix: Modified column showed a raw ISO 8601 timestamp
  (`yyyy-MM-ddThh:mm:ss`) and opened too narrow to show it in full.**
  A direct, small user report. `FilePaneWidget::rebuildModel()`
  formatted each row's `Modified` cell with `Qt::ISODate`
  (`e.modified.toString(Qt::ISODate)`) — switched to an explicit
  `"yyyy-MM-dd hh:mm:ss"` format string (a space instead of the `T`).
  Sort order is unaffected: the column's sort key is the display text
  itself (see `SortDataRole`'s own class doc comment above), and a
  fixed-width format with a constant separator character sorts
  lexicographically the same regardless of which separator is used.
  Also gave `ColModified` an explicit initial `setColumnWidth(155)`
  (previously unset, relying on `QHeaderView`'s own default section
  size) so the full timestamp is visible without dragging the column
  wider first — the existing `maxWidths` cap of 180 already established
  for the same column (see the `sectionResized` handler above) still
  lets it shrink back down interactively. Verified via a disposable
  visual probe (a real `FilePaneWidget` over a real local directory,
  screenshotted) confirming both the space-separated format and the
  full-width display with no manual drag needed.

  **Fix: Permissions (the last column) had no bounded width and no
  visible end; Name wasn't reliably the widest column on open.**
  Another direct user report. `FileTreeView`'s header is a `QTreeView`,
  which defaults `stretchLastSection` to `true` — the ORIGINAL design
  deliberately relied on that to let Permissions absorb whatever width
  Name/Size/Modified didn't claim, but the practical effect on any
  window wider than a few hundred pixels was Permissions ballooning
  into the widest column by far, the opposite of what a file browser
  should lead with. Fixed by calling
  `m_view->header()->setStretchLastSection(false)` and giving every
  column (`ColName`/`ColSize`/`ColModified`/`ColPermissions`) its own
  explicit `setColumnWidth()` — Name (200px) deliberately the widest
  of the four, Size (78px) sized to show a full 8-digit byte count,
  Modified (155px, unchanged from the fix above), Permissions (95px)
  sized to show its own header label in full alongside a complete
  `rwxr-xr-x` value. All four stay `Interactive` (a user can still drag
  any of them), and `ColPermissions` joined the existing per-column
  `maxWidths` cap (200px) that already capped Name/Size/Modified —
  Permissions is no longer exempt from that map now that it isn't
  stretch-governed.

  Sizing the four widths concretely (not by eyeballing) took two
  rounds: an initial pass sized Name generously (300px) with Size/
  Permissions loose enough (90/130px) to comfortably fit their content
  — but a disposable probe driving a real `MainWindow` at the app's
  OWN fallback default size (`1100x780`, `MainWindow::MainWindow()`)
  showed a horizontal scrollbar immediately on first launch, before any
  resize: the four widths summed past what a default-sized pane
  actually has room for once its own vertical scrollbar (for the
  `~18`-entry `$HOME` listing used in the probe) is accounted for.
  Trimmed to the final numbers above, reconfirmed scrollbar-free at
  `1100x780` via the same probe, and reconfirmed Size/Permissions still
  don't truncate their own content (`60000000`, the "Permissions"
  label itself) via a second probe with a real 60MB sparse file.

  Disabling `stretchLastSection` surfaced a second, purely visual bug
  it had been silently covering for: the header's OWN blank strip past
  the last column (previously nonexistent, since stretch always
  consumed 100% of the width) isn't reached by the existing
  `QHeaderView::section` QSS rule — `::section` only styles real
  section cells — so it fell back to Qt's unstyled native header color,
  a light gray strip cutting across both dark AND light themes right
  where Permissions now ends. Fixed with a new bare `QHeaderView` rule
  in both theme files (background-color + border-bottom matching each
  theme's existing `::section` surface/border tokens) — this also gives
  Permissions itself a real, visible right-hand divider for the first
  time, since it's now a genuinely bounded section like any other
  rather than a stretch region with no defined end. Verified via the
  same disposable probe in both Dark and Light.

  **Dialog-consistency audit across every dialog in the app.** A direct
  user request ("ensure all dialogs are correct and consistent between
  Windows, Mac, and Linux"), scoped down via a clarifying question: a
  general proactive audit (not a specific known-bad dialog), validated
  via static code review plus real screenshots on this Linux sandbox —
  there's no way to literally render on Windows/macOS here. One
  reassuring finding from the static half: zero `#ifdef Q_OS_*`/
  `QSysInfo` branches anywhere in `src/ui/`'s six real `QDialog`
  subclasses (`ConnectionDialog`, `SiteManagerDialog`,
  `PreferencesDialog`, `PermissionsDialog`, `CompareDialog`,
  `CompareDeleteConfirmDialog`) — every dialog is built identically
  regardless of platform, and this app's heavy reliance on a fully
  custom `app.setStyleSheet()` QSS theme (rather than each platform's
  native widget style) is itself most of what keeps cross-platform
  rendering consistent by construction, not by accident.

  A disposable probe constructed all six dialogs at once (mirroring
  `keyboard-shortcuts-test`'s own full dependency list, since it
  already links everything `MainWindow` needs including `SftpBackend`)
  and screenshotted each in both Dark and Light — surfacing three real,
  currently-shipping bugs the functional test suite had no way to
  catch, none previously reported:

  1. **Site Manager's "Simultaneous connections" spin box rendered its
     up/down arrow buttons as blank squares**, in both themes. It's
     the ONLY spin box in the app that keeps its native buttons —
     every other one (`ConnectionDialog`/`SiteManagerDialog`'s own Port
     fields, `PreferencesDialog`'s Proxy Port and Bandwidth Limit)
     calls `setButtonSymbols(QAbstractSpinBox::NoButtons)` deliberately
     (see `SiteManagerDialog.cpp`'s own comment on why this one field
     alone keeps them: a small 1-10 range where up/down nudging is
     genuinely useful). Root cause matches an already-fixed `QComboBox`
     bug from earlier in this file's own history: styling `QSpinBox` at
     all (the shared `QLineEdit, QSpinBox, QComboBox` base rule) makes
     Qt stop painting its native arrow glyphs, and nothing had ever
     supplied a replacement — `QComboBox::down-arrow` already had one,
     `QSpinBox::up-arrow`/`down-arrow` never did, simply because this
     was the only spin box ever exercising that code path. Fixed with
     new `QSpinBox::up-button`/`down-button`/`up-arrow`/`down-arrow`
     rules in both theme files, reusing the existing muted-chevron SVG
     technique (`chevron-down-muted.svg`'s own baked-in
     `--zf-text-secondary` stroke color, since QSS `image: url()` has
     no runtime tinting hook) plus a new sibling `chevron-up-muted.svg`.
     A first pass measured the widget's actual programmatic
     `height()`/`sizeHint()` (both correctly 33px, matching sibling
     `QLineEdit`s) before concluding the earlier `setFixedHeight()` fix
     from this file's own history was still working correctly and the
     visual "taller" impression from eyeballing an unzoomed screenshot
     was wrong — confirmed by a precise pixel-grid crop showing both
     fields genuinely the same height. The real bug was the missing
     arrow glyphs alone, not a size regression.
  2. **Compare Directories' delete-confirmation file list
     (`CompareDeleteConfirmDialog`) rendered a bright native-white
     background in Dark theme.** It's this app's only `QListWidget`,
     so `QTreeView`/`QTableWidget`'s existing shared dark-palette rule
     never reached it — the same root cause ("an unstyled child widget
     falls back to native palette") this project has hit several times
     before, just never yet on this specific widget class. Fixed by
     adding `QListView` (which `QListWidget` is a thin subclass of) to
     the existing shared selector in both theme files. Verified this
     didn't regress `QComboBox`'s own dropdown popup — internally also
     a `QListView` — by opening a real popup via `showPopup()` and
     screenshotting it: the existing, more specific
     `QComboBox QAbstractItemView` rule (a different surface color,
     intentionally) still correctly wins, confirming Qt's usual CSS-
     like specificity cascade held here as expected, not just assumed.
  3. **Compare Directories' own Left/Right columns silently truncated
     their own `sideText()` content** (`"<size>, <modified>"`) with no
     visible hint — `QTreeWidget` doesn't ellipsize a column that's
     merely too narrow for its content the way `TransferQueueTable`'s
     own established convention explicitly plans for elsewhere; it
     just clips. The exact same class of bug this file's own Name-
     column fix (comment right above) already documents finding once,
     never applied to the OTHER three columns. Fixed with explicit
     `setColumnWidth()` calls for Status (110px) and Left (190px) —
     Right, the actual last column, keeps `QTreeView`'s own
     `stretchLastSection` default rather than a fourth explicit width,
     since its content is identical in shape to Left's and stretch
     already delivers at least that much once the other three columns
     have claimed their share — plus widening the dialog's own default
     `resize()` from 760 to 830 so all four columns fit without
     fighting each other for space. While fixing this, also found (by
     literally reading the rendered date text in the screenshot) that
     `sideText()` still formatted `modified` with the same old raw
     `Qt::ISODate` (`yyyy-MM-ddThh:mm:ss`) this file's own earlier
     Modified-column fix had already replaced for the main file pane —
     switched to the identical `"yyyy-MM-dd hh:mm:ss"` format for
     consistency between the two.

  All three verified via the same disposable probe, in both Dark and
  Light, including a real (small, fast-finishing) `DirectoryComparer`
  run rather than a static mock — an early version of the probe used
  `$HOME` for both sides of the comparison and never finished within
  the probe's own screenshot delay; switched to two small throwaway
  `QTemporaryDir`s with a handful of files so the real async compare
  actually completes.

  **Scripting/automation (CLI mode)** — WinSCP's own flagship
  differentiator, picked as the next v2 target once sync/mirror browsing
  was fully shipped (both halves). `--script=<path>` (new
  `QCommandLineParser` option in `src/main.cpp`) runs a plain-text,
  line-oriented script non-interactively (`open`/`cd`/`lcd`/`get`/`put`/
  `ls`/`lls`/`rm`/`mkdir`/`mv`/`mirror`/`echo`/`exit`) and exits with a
  status code — no window is ever shown. `ScriptParser`
  (`src/cli/ScriptParser.h/.cpp`) is a pure, GUI-free free function
  (`parseScript()`); `ScriptRunner` (`src/cli/ScriptRunner.h/.cpp`)
  executes sequentially, each command waiting for its own async
  completion signal(s) before the next one starts — the same "chain to
  the next step only once the previous callback actually fires"
  discipline `transfer-concurrency-test`'s own header comment already
  documents as correct, generalized into a real dispatcher instead of a
  fixed test sequence.

  **A key architectural finding that simplified the design, confirmed by
  reading the code rather than assumed**: `SftpBackend::verifyHostKey()`/
  `askUserToTrustHostKey()` and the identical `FtpBackend::
  askUserToTrustCertificate()` already fail safe when constructed with a
  null `HostKeyVerifier*`/`CertificateVerifier*` — and, critically, a
  host/cert that's *already trusted* (present and matching in the same
  `known_hosts`/`known_certs.json` the GUI populates) never calls into
  the verifier at all (`LIBSSH2_KNOWNHOST_CHECK_MATCH` branches straight
  to `trusted = true`). Only an unknown or changed host/cert reaches the
  verifier, which returns `false` when null, producing a clean
  `connectionFailed("Host key for X was not trusted — connection
  refused")`. **No new "non-interactive trust policy" class was needed**
  — `open` simply constructs `SftpBackend(creds, nullptr)`/
  `FtpBackend(creds, nullptr)`, exercising an existing, already-tested
  contract rather than adding a new one. A script can connect to any
  host already trusted via the GUI at least once; a brand-new host fails
  cleanly and immediately rather than hanging or silently trusting —
  documented as the intended, secure-by-default behavior, not a
  limitation.

  `TransferManager::enqueue()`/`enqueueFolder()`/`moveEntry()` all
  require real `FilePaneWidget*` — not incidentally, they read
  `pane->backend()`/`pane->currentDirectory()` throughout — so
  `ScriptRunner` owns two `FilePaneWidget`s internally exactly the way
  `MainWindow` does: both start on a fresh `LocalBackend` (matching
  `MainWindow`'s own "both panes start local" precedent), `m_localPane`
  stays local, and `m_remotePane`'s backend gets swapped in by `open`
  (mirroring `MainWindow::startConnection()`'s own switch/`moveToThread`/
  `setBackend` sequence, minus the interactive verifiers and status-bar
  messages) or by `attachRemotePaneForTesting()` in tests. Reusing this
  already-tested, already-proven-headless-drivable machinery (the entire
  existing test suite already constructs bare `FilePaneWidget`s under
  `QT_QPA_PLATFORM=offscreen` with zero `MainWindow`) was a deliberate
  choice over adding new `(RemoteBackend*, path)` overloads to
  `TransferManager` — smaller and safer, at the cost of script mode
  still needing Qt Widgets linked (a genuinely GUI-free CLI binary is an
  explicit v1 non-goal).

  `TransferManager::enqueue()` gained one new optional parameter,
  `assumeOverwrite` (already added for Compare-and-Sync's own `Differs`-
  row bulk-copy case — reused here unchanged, not duplicated).
  `get`/`put` deliberately transfer under the SAME filename on both
  sides — `enqueue()`'s `fileName` parameter is used identically for
  source and destination (confirmed by reading its own doc comment),
  so no rename-during-transfer exists anywhere in this codebase; a
  rename needs `mv` afterward.

  `mirror <local> <remote> [--delete]` reuses `DirectoryComparer`/
  `CompareSyncExecutor` from Feature B unchanged: every `OnlyLeft`/
  `Differs` entry is auto-selected and copied (no manual per-row
  checkbox pass, unlike the GUI — the natural default for a scripted
  one-directional backup), and `--delete` additionally removes every
  `OnlyRight` entry, deliberately named/scoped after `rsync --delete`
  for a familiar audience. **Two real bugs found and fixed during manual
  verification, before the automated test was even written**: (1) the
  first working draft never actually issued the `navigateTo()` calls its
  own `waitForNavigation()` listeners were registered to wait for — a
  pure oversight, caught immediately as a hang against a real script;
  (2) `mirror --delete`'s copy phase already had a completion barrier
  (wait for every enqueued `TransferItem` to reach a terminal status
  before advancing — necessary because, unlike the GUI's `CompareDialog`,
  nothing else is watching the transfer queue asynchronously; the next
  script line must not run until transfers genuinely finish), but the
  DELETE phase had none — `CompareSyncExecutor::deleteSelected()`'s
  underlying `deleteEntry()` calls are queued/fire-and-forget, so the
  script could reach `exit` (and the process could exit) before its own
  queued deletes ever actually ran. Confirmed for real: a manual mirror
  `--delete` run left the stale destination-only file in place until a
  second, dedicated completion barrier (`ScriptRunner::runMirrorDeletes()`
  — counts down `directoryListed`/`fileOperationFailed` completions,
  same fire-and-refresh contract `rm`/`mkdir`/`mv` already rely on) was
  added specifically for the delete phase.

  **A third real bug, also found manually**: `rm`/`mkdir`/`mv`/`cd`/`lcd`
  initially passed a script's raw argument straight to the backend
  (`deleteEntry("newdir", ...)`) instead of resolving it against the
  pane's current directory first — every existing UI caller in
  `FilePaneWidget.cpp` already does this join (e.g. `joinPath(directory,
  entry.name)`) before ever reaching a backend call, since the backend
  API itself has no "current directory" concept of its own; everything
  it's given must already be absolute. A relative script argument was
  silently being treated as relative to the *process's* working
  directory (or, worse, `PathUtils.h`'s `joinPath("", name)` producing a
  root-level absolute path when the pane's `currentDirectory()` hadn't
  even settled yet — see the next bug). Fixed with a small
  `ScriptRunner::resolvePath()` helper (absolute arguments starting with
  `/` pass through unchanged; everything else joins against the target
  pane's `currentDirectory()`).

  **A fourth real bug, the actual root cause of the confusing symptom
  above**: `run()` only waited for `m_localPane`'s own initial
  auto-navigate-to-home to settle before dispatching the first script
  command — not `m_remotePane`'s, even though it starts on a placeholder
  `LocalBackend` too and does the identical async connect-then-navigate
  dance in its own constructor. A script command touching the remote
  pane before `open` (or one that never calls `open` at all) could race
  that still-in-flight initial navigation, reading an empty
  `currentDirectory()` — exactly what surfaced as `resolvePath()`
  producing a bogus root-level path. Fixed by having `run()` wait for
  BOTH panes' initial navigation, chained, before `executeNext()` ever
  fires.

  New 22nd required test target, `script-runner-test` — see
  CONTRIBUTING.md's own subsection. `open`'s real `SiteStore`/
  `CredentialStore`/network path is deliberately NOT covered by the
  required suite (no OS keyring dependency there, same reasoning
  `verify-credential-store` is a live-only target) — a separate,
  `EXCLUDE_FROM_ALL` `verify-script-runner-live` covers it against a
  real local `sshd`. Explicit non-goals for this pass: no ad-hoc
  `open host:port` without a saved site (no secret source for one), no
  environment-variable password injection (a real, common CI pattern,
  but a new secret-supply surface deserving its own dedicated review,
  not bundled in here), no `--continue-on-error`, no variable expansion/
  globbing/pipes in the script format, no genuinely Widgets-free CLI
  binary.

  **Light theme** — the last remaining smaller v2 item, picked over
  protocol breadth (WebDAV/S3, which still carries an open "is this
  still an FTP client?" scope question) specifically because it had no
  open scope question of its own and touched a well-understood set of
  files. User decision: live switching, not restart-required — a real
  step up in complexity, since every icon and stylesheet rule already
  applied to existing widgets has to be re-applied in place.

  **Protocol breadth (WebDAV/S3/cloud storage) deliberately deferred**
  (David's explicit call, 2026-08-14, after this feature shipped) — not
  merely unscheduled. It remains the single biggest structural gap
  against Cyberduck/Transmit/WinSCP, but the underlying product-identity
  question needs an actual conversation before it can even be scoped as
  a feature, not an implementation decision like every other item on
  this list was. See `project_competitive_roadmap.md` for the full
  re-derived competitive score reflecting this.

  New `src/Theme.h` (`enum class Theme { Dark, Light }`) — deliberately
  NOT in `IconTheme.h` (Widgets-adjacent) or defined inline in
  `AppSettings.h`: `app-settings-test` links `Qt6::Core` only, so the
  enum needs an equally Core-safe home, mirroring `Protocol.h`'s own
  layering. `AppSettings::theme()`/`setTheme()`/`themeChanged` copy
  `showHiddenFiles`'s exact shape (the one existing precedent for "a
  setting multiple live widgets react to").

  `IconTheme::Gray()`/`GrayMuted()` are now functions, not plain
  constants — confirmed by reading the file that these two are the ONLY
  colors that actually need different values per theme (`Blue`/`Green`/
  `Red`/`Amber` stay fixed constants, saturated enough to read against
  either background, so their call sites needed no changes at all).
  `tintedIcon()`'s existing cache already keys on `resourcePath + color
  + size` (`IconTheme.cpp`), so once `Gray()`/`GrayMuted()` return a
  different value, any FUTURE tint call naturally misses the cache and
  renders fresh — no explicit cache-invalidation mechanism was needed,
  simplifying live-switching meaningfully. The real remaining work was
  narrower than it first looked: of six files calling `IconTheme::`,
  only `FilePaneWidget`'s 4 nav buttons and one `MainWindow` toolbar
  action (`m_preferencesAction`, newly promoted from a local variable to
  a member for exactly this reason) are "set once, never touched again"
  — every other icon (per-row file/status icons, every right-click
  `QMenu`, `TransferQueueWidget`'s progress-bar chunk colors,
  `SiteManagerDialog`'s tree icons) already recomputes on every call, so
  it picks up a new color automatically the next time it renders.
  `SiteManagerDialog`/`CompareDialog` are both freshly-constructed-per-
  open dialogs — a live theme change while either happens to be open
  leaving their already-set button icons stale until next open is an
  accepted, disclosed scope boundary, not something wired up.
  `FilePaneWidget` self-subscribes its own `retintIcons()` to
  `m_settings->themeChanged` (it already takes an `AppSettings*`, same
  as the `showHiddenFilesChanged` precedent); `TransferQueueWidget`
  doesn't take one, so `MainWindow::onThemeChanged()` calls its public
  `retintIcons()` explicitly instead — it just re-runs `onItemUpdated()`
  for every currently-queued item, reusing the existing per-row render
  path rather than duplicating it.

  `resources/theme-light.qss` is a full parallel file, not a smaller
  diff against `theme.qss` — Qt Style Sheets have no shared-variable
  mechanism (confirmed by that file's own header comment), so every one
  of its 9 base tokens needed its own light-appropriate remap. Accent
  colors unchanged; the `rgba(255,255,255,N)` hover/alternate-row
  overlays (which lighten a dark base) become `rgba(0,0,0,N)` (which
  darken a light one).

  **Verified via a disposable, non-committed target** (built, run, then
  fully reverted, per the v0.6.18-era "headless widget construction +
  `QWidget::grab()` screenshot review" technique) rather than a new
  permanent required-suite target — this is a visual/config-value
  feature, not new business logic. **Two real bugs found in the
  verification harness itself, not the feature**, both worth remembering
  for any future headless-widget verification work: (1) the first draft
  never replicated `main.cpp`'s own initial dark-theme load before
  constructing a real `MainWindow`, so its "before" screenshot showed
  Qt's own default (light-ish) style rather than the app's real dark
  theme — a coincidentally-similar-looking false negative, not a true
  baseline; (2) the first draft drove a live theme switch through a
  freshly-constructed, SEPARATE `AppSettings` instance rather than
  `MainWindow`'s own — both write the same `settings.json` (shared test-
  mode path), but `themeChanged` is a signal on one specific QObject
  instance, and `MainWindow` was connected to its OWN `m_settings`, not
  the harness's; file-level persistence sharing a path is not the same
  thing as signal/slot object identity. Fixed by reading `main.cpp`'s
  init sequence into the harness and by using
  `window->findChild<AppSettings*>()` (works because `m_settings` is a
  real QObject child of `MainWindow`, constructed as `new
  AppSettings(this)`) instead of a second instance. Once both were
  fixed, real before/after screenshots confirmed a genuine, correctly-
  colored dark-to-light switch — see the session history for the actual
  captured images.
  **Real, reported bug, fixed: Transfers/Commands panes went missing
  after restoring the window from minimized on Windows, and re-checking
  them via the View menu didn't stick.** Root cause: the View menu's
  Transfers/Commands entries are hand-wired, two-way-bound `QAction`s
  (see this entry's own comment on why they can't just reuse
  `toggleViewAction()` — the icon-bleeding problem) —
  `toggled -> setVisible` one direction, `visibilityChanged -> setChecked`
  the other. `QDockWidget::visibilityChanged` also fires when the WHOLE
  main window is minimized (confirmed directly: a disposable probe
  driving a real `MainWindow` through `showMinimized()`/`showNormal()`
  showed it firing with `visible=false`), and `QAction::setChecked()`
  emits `toggled()` by default whenever the checked state actually
  changes — so the minimize-driven `visibilityChanged(false)` drove
  `setChecked(false)`, which re-emitted `toggled(false)`, which called
  `m_transfersDock->setVisible(false)` for real. That's the critical
  difference: minimizing only hides a dock IMPLICITLY (an ancestor
  became invisible), which Qt automatically reverses once the window is
  restored — but this explicit `setVisible(false)` call set
  `WA_WState_ExplicitShowHide`, which restoring the window does NOT
  reverse, so the dock (and its menu checkmark) stayed hidden for good
  after every minimize/restore, and manually re-checking it only lasted
  until the next transient hide re-triggered the same loop. Fixed by
  wrapping the `visibilityChanged -> setChecked` sync in a
  `QSignalBlocker` — that direction only ever needs to keep the
  checkmark's VISUAL state honest, never to re-drive `setVisible()`
  itself, so it must never re-emit `toggled()`. Confirmed directly, not
  assumed: the disposable probe reproduced the exact bug against the
  pre-fix code (`WA_WState_ExplicitShowHide` flips to `true` after
  minimize, dock stays hidden after `showNormal()`) and confirmed the
  fix across two consecutive minimize/restore cycles
  (`WA_WState_ExplicitShowHide` stays `false`, dock and checkmark both
  correctly reappear). The toolbar's own Transfers/Commands buttons,
  which reuse each dock's real `toggleViewAction()`, were never affected —
  Qt's own internal implementation connects to `triggered()` (real user
  clicks only), not `toggled()` (any programmatic state change), which
  is exactly the distinction this fix restores for the hand-wired menu
  actions too.

**`UpdateChecker` (Help menu's "Check for Updates...") is the first
thing in this codebase to use Qt's own HTTP stack
(`QNetworkAccessManager`) rather than a raw socket** — every transfer
protocol goes through `FtpBackend`/`SftpBackend`/libssh2 instead, so
there was nothing here to reuse. A one-shot GET against GitHub's real
release list (`/repos/arelas/zephyrftp/releases`, NOT `/releases/latest`
— see below), compared against `APP_VERSION` via `QVersionNumber`,
respecting the same global proxy setting every SFTP/FTP/FTPS
connection already does (`AppSettings::resolvedProxyConfig()` ->
`QNetworkProxy`). Manual only, deliberately — automatic/background
checking was explicitly scoped OUT of this feature given the direct
tension with this project's own "no account, nothing phones home"
marketing message; that remains a separate, not-yet-built, explicitly
opt-in follow-up if it's ever wanted.
**Non-vacuous testing against the REAL API immediately caught a real
bug before it ever shipped**: `/releases/latest` deliberately excludes
prereleases, and every release this project has EVER published is
flagged prerelease (it matches this project's actual beta status), so
that endpoint 404s, always, for this specific repo — confirmed
directly against the live API, not assumed. Checking whether the
marketing site's own `site/latest.php` (gitignored, deployed
separately — see CONTRIBUTING.md) had the identical bug found that it
did, and had for its ENTIRE deployed life: its own "live version"
fetch had been silently 503'ing on every real page load since the file
was written (confirmed: its own disk cache file had never once been
successfully created), meaning the site's dynamic download-link
feature had never actually worked — it only ever looked current
because of manual static-HTML version bumps after every release. Both
fixed identically: fetch the full releases list (newest first) and
take the first non-draft entry, instead of relying on `/releases/latest`
at all.

**The Transfers pane's tab bar took four real iterations to get right
on macOS, and the final shape is a reusable lesson for any future
per-platform `QStyle` override in this codebase.** The underlying need
— `QTabBar` rendering left-aligned (`QMacStyle` centers tabs by
default) with no eliding, themed by this app's own QSS rather than
native macOS chrome — went through: (1) `QTabWidget::
setDocumentMode(true)`, which fixed alignment/eliding but visibly put
the tab bar onto QMacStyle's native "unified toolbar" rendering path,
reported directly as a white seam plus a tab bar that followed the
OS's own system light/dark appearance instead of this app's theme;
(2) a `QProxyStyle` overriding just the two relevant `styleHint()`
queries, installed on the tab bar itself via `QWidget::setStyle()` —
still showed the identical white background in a real screenshot; (3)
swapping to Qt's fully cross-platform `Fusion` style, same per-widget
installation — produced the *pixel-identical* white background despite
being a completely different style object, which was the actual
diagnostic turning point: the variable was never *which* style, it was
calling `QWidget::setStyle()` on that widget at all. Qt's own
documentation warns this disconnects a widget from the app's
stylesheet cascade; alignment/eliding (plain `styleHint()`/property
mechanisms) kept working across every attempt while only QSS
`background-color` painting failed — exactly that documented split.
(4) **The actual fix**: a new `TransferQueueWidget::
installTabBarAlignmentFix()`, called once from `main.cpp` *before*
`QApplication::setStyleSheet()` and before any widget exists, using
`QApplication::setStyle()` instead of any per-widget call —
`QApplication::setStyle()` composes correctly with stylesheets by
design, so every widget (including this one) still gets Qt's normal
automatic `QStyleSheetStyle` wrapping, same as if the function had
never been called at all. The installed `QProxyStyle` overrides only
`SH_TabBar_Alignment`, delegating everything else unchanged, so it has
zero visual effect on any other widget in the app despite being
installed application-wide; `QTabBar::setElideMode(Qt::ElideNone)`
stays a plain widget property, unrelated to any of this, set directly
in the constructor. Confirmed via a real macOS screenshot: dark
background flowing seamlessly under all three tab labels into the
table below, left-aligned, full text. Caught and fixed a real bug in
the fix itself along the way, before it ever left the sandbox:
attempt (2)'s `QWidget::setStyle()` doesn't take ownership, and
parenting the new style object to the tab bar (the obvious fix for
that) caused a genuine SIGSEGV in `QApplication::~QApplication()` on
every test that tears down a real `MainWindow`, confirmed via `gdb` —
`QApplication::setStyle()`, used in the final fix, takes ownership
correctly on its own, no manual lifetime workaround needed there.
**The generalizable lesson**: any per-platform `QStyle::StyleHint`
override in a Qt app using global stylesheets belongs at the
`QApplication::setStyle()` level, called once at startup before the
stylesheet loads — never via `widget->setStyle()` on the specific
widget that needs the fix, however tempting that narrower-looking
scope seems.

**Two smaller Site Manager/Preferences fixes from the same session**:
`SiteManagerDialog` gained a right-click "Rename Group..." on a group
folder in the site tree — groups were never a separate stored entity
(a group is just a string every member site happens to share, see
`m_groupCombo`'s own doc comment), so renaming one restamps that
string on every site currently in it; renaming onto an existing
different group's name is allowed deliberately, merging the two.
`PreferencesDialog`'s external-editor field was clipping its own
placeholder text ("Leave blank to use your system's default
application") because `QLineEdit::sizeHint()` doesn't account for
placeholder length at all — fixed by sizing the field's minimum width
explicitly from the placeholder's real rendered width via
`QFontMetrics::horizontalAdvance()`. Unlike the macOS-only bugs above,
this one reproduced cleanly on Linux too — a disposable probe measured
the unfixed field clipping by a real 124px.

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
  **A dedicated code review of this file found one more real efficiency
  issue on top of the caching fix above, plus two known, currently
  unreachable limitations, all now documented in the code itself:**
  every cache MISS still parsed the same SVG resource twice — once each
  for the base and `@2x` renders, via two separate
  `QSvgRenderer(resourcePath)` constructions — when the renderer only
  needs parsing once and can be rendered from repeatedly against
  different target `QPainter`s. Fixed by parsing once in `tintedIcon()`
  and passing the renderer by reference into `renderTinted()`; confirmed
  correctness-preserving (not just "doesn't crash") via
  `sort-and-commands-test`'s existing pixel-identity and
  cache-key-discrimination checks, all of which still pass. Also noted,
  not fixed: the `@2x` variant is hardcoded to `devicePixelRatio` 2.0
  rather than the display's actual scale factor, visibly softer than a
  purpose-rendered icon on a fractional-scaling display (1.25x/1.5x) —
  real, but `tintedIcon()` has no widget/screen context to read a real
  ratio from, and a naive `QGuiApplication::primaryScreen()` read would
  still be wrong on a multi-monitor setup with per-monitor scaling, so
  this needs real design work rather than a quick patch that could make
  things worse in some cases. And: a failed render (invalid
  `resourcePath`) is never cached, so a bad key would re-parse and
  re-fail on every call rather than being memoized — not fixed since
  it's currently unreachable, every call site in this codebase passes a
  literal `":/icons/*.svg"` path to a real bundled resource.
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

## Windows, macOS, and Linux builds (CI)

`.github/workflows/build.yml` produces all three platforms' release
binaries and, on a `v*` tag, attaches all of them to the same GitHub
Release. All five build jobs actually run the full required test suite
as part of the job, not just link it — matching CONTRIBUTING.md's
"these need to actually pass" rule for CI too, not only local
development.

**Every test/verify target links one shared `zephyrftp_core` `OBJECT`
library instead of each independently enumerating its own subset of
source files** (2026-08). Before this, a target that happened to need
`MainWindow.cpp` (say) compiled it fresh, on its own, every single
time any OTHER target that also needed it was built in the same job —
confirmed directly before touching anything: `MainWindow.cpp` was
being compiled 14 separate times across the 44 real/test targets that
existed then, `TransferManager.cpp` 34 times, `FilePaneWidget.cpp` 36
times, 1.4GB of object files for the test suite alone. CI's own
"Build test suite" step measured at 6m06s versus 1m53s to actually
*run* all 27 required tests — the build step, not the tests
themselves, was the real cost. Collapsing every target's source list
into one shared `OBJECT` library (linked `PUBLIC`, so include
dirs/compile definitions/link libraries all propagate automatically)
shrank every target to a two-line pattern — its own entry-point `.cpp`
plus `target_link_libraries(<name> PRIVATE zephyrftp_core)` — and
dropped `CMakeLists.txt` from 1481 to 688 lines. Verified byte-for-byte
behavior-identical before/after: the same 27 tests, same 100% pass
rate, same run timing, only the compile step got cheaper. Also
structurally closes a real, previously-recurring gotcha: a target
referencing a `Q_OBJECT` header without also compiling its matching
`.cpp` used to fail with a confusing link error — every `Q_OBJECT`
header's `.cpp` is now always compiled exactly once, in
`zephyrftp_core`, before anything can link it.

**`workflow_dispatch` can scope a manual run to a single build job**
(2026-08), via a `job` choice input (`gh workflow run build.yml --ref
main -f job=build-macos`, default `all`) — added after repeatedly
burning the other four jobs' ~20-25 minutes of CI time to verify a
single macOS-only UI/packaging change. Each build job's own `if:`
short-circuits true (runs unconditionally) for anything that ISN'T a
`workflow_dispatch` event, so a real `v*` tag push or a pull request
always runs every job regardless — this only ever narrows an
on-demand manual run. Verified directly: a scoped dispatch targeting
one job showed the other four resolving to `skipped` within seconds
(confirmed via the Actions API's own `conclusion` field, not just the
CLI's summary view), while the targeted job ran and completed
normally. The `release` job is unaffected either way — it only ever
runs on a real tag push (`if: startsWith(github.ref, 'refs/tags/v')`),
never on `workflow_dispatch`, with or without this input.

**`build-windows`** cross-compiles with MinGW from a `fedora:44`
container on `ubuntu-latest`, rather than building natively on
`windows-latest` — no `cl.exe`, no Qt-for-Windows download, no vcpkg.
This replaced an earlier MSVC+vcpkg pipeline entirely (its own real
incidents are kept below as history, since the failure modes of
Windows-targeting CI are worth knowing about regardless of which
toolchain produces the `.exe`). `tools/collect-win-runtime.sh` collects
the runtime DLLs and Qt plugins — there's no windeployqt for this
toolchain — and the test suite runs for real under `wine` (wrapped in
`xvfb-run`; see below). Full local-reproduction details, including every
gotcha below, are in CONTRIBUTING.md's "Cross-compiling for Windows
locally" section.

**Blocked releases for days (2026-08) with a genuine CI-infrastructure
bug, not a code regression** — every run of this job's "Run test suite
under wine" step failed identically: the virtual X server (`Xvfb`, via
`xvfb-run`) died partway through the ~20-test sequence, zero test
output produced, never reproducing in an identical local
podman/`fedora:44`/wine repro across many attempts. Root cause,
confirmed via a targeted diagnostic (dropping just the LAST test in the
sequence made the whole step pass): all ~20 sequential `wine`
invocations in that one step share a single `WINEPREFIX`, hence one
long-lived background `wineserver` daemon, and resources/state
accumulated in it across enough invocations to eventually crash Xvfb.
`LIBGL_ALWAYS_SOFTWARE=1` (tried first, based on a `libEGL`/DRI3
warning that appeared alongside every failure) genuinely eliminated
that specific warning but had zero effect on the actual crash — a real,
confirmed instance of a visible symptom not being the cause, worth
remembering before chasing the next one. Fixed for real by killing and
waiting for the wineserver (`wineserver -k -w`) after *every single*
test invocation in this step, forcing a fully fresh one each time —
see CONTRIBUTING.md's "Cross-compiling for Windows locally" section for
the reusable local-reproduction version of this lesson. A second,
unrelated bug in `sync-browsing-test`'s own setup code surfaced once
the above was fixed and the test could finally run at all — see
CONTRIBUTING.md's `sync-browsing-test` entry for that one.

**`build-linux`** is a plain native build on `ubuntu-latest` — same
dependencies CONTRIBUTING.md documents for local Linux development
(`cmake build-essential qt6-base-dev qt6-svg-dev libssh2-1-dev
libsecret-1-dev pkg-config`). Ships just the binary: a deliberate choice to match this
project's existing "no bundled libraries" approach elsewhere
(`SftpBackend` wraps libssh2 directly, `FtpBackend` is hand-rolled
rather than pulling in libcurl) rather than building a portable
self-contained artifact like an AppImage. The tradeoff: it only runs on
a machine with matching Qt6/libssh2 packages already installed — worth
revisiting if that stops being the right call. No wine/Xvfb needed here
at all; these are native binaries, and Qt's own `QT_QPA_PLATFORM=offscreen`
needs no display of any kind on its own turf (confirmed directly in a
genuinely headless container with no X/Wayland session — unlike wine's
situation in the Windows job, described below).

**`build-macos`** is the same shape as `build-linux` — a plain native
build, this time on `macos-latest` — using Homebrew (`brew install qt
libssh2 openssl@3 pkg-config`) instead of apt/dnf. **Apple Silicon
(arm64) only, deliberately not a universal binary**: `macos-latest` is
a native arm64 runner and Homebrew's Qt6 is single-arch; a true
universal (arm64+Intel) build would need a second, Rosetta-emulated
Homebrew prefix plus `lipo`-merging every bundled Qt framework/plugin
dylib — real CI complexity taken on for a shrinking population of Intel
Macs, judged not worth it for now. `CredentialStore`'s third backend
(Keychain Services, `Security.framework` — see that section above)
exists specifically for this job; `verify-credential-store` runs here
same as every other platform, and is the one target that actually
proves that backend round-trips against a real, live keychain rather
than just compiling. Packaging differs from every other job, and
differently than it used to (twice now): `CMakeLists.txt`'s `if(APPLE)`
block sets `MACOSX_BUNDLE` properties (bundle identifier, `.icns`,
version) plus `OUTPUT_NAME "ZephyrFTP"` and
`install(TARGETS zephyrftp BUNDLE DESTINATION .)` — `OUTPUT_NAME` is
what actually determines the on-disk bundle folder/executable name
(`ZephyrFTP.app`, `Contents/MacOS/ZephyrFTP`); the CMake TARGET itself
stays the lowercase `zephyrftp` identifier `--target` flags use
throughout this file, unaffected. This split matters: before
`OUTPUT_NAME` was set (2026-08), the bundle was still named after the
target, and a real screenshot of the packaged `.dmg` showed Finder's
icon-view label reading lowercase "zephyrftp" underneath the app
icon — `MACOSX_BUNDLE_BUNDLE_NAME`'s own Info.plist entry (unaffected,
still `ZephyrFTP`) has no bearing on what Finder actually displays as
that folder's name.
The `.dmg` itself is built via `create-dmg` (Homebrew) — not CPack's
`DragNDrop` generator, and not hand-rolled `hdiutil` either, its own
immediate predecessor (2026-08): `create-dmg` gives the packaged `.dmg`
a real drag-to-install layout (positioned app/Applications icons, a
custom background, hidden Finder chrome) instead of a bare file grid,
requested directly ("make our mac dmg look neat, like others do").
It stages from the exact `build/ZephyrFTP.app` tree the deploy steps
below already fixed in place — a plain filesystem copy of whatever
folder you point it at, same as the `hdiutil` step's own `cp -R`
before it, so it can't reintroduce the `DragNDrop` bug described just
below. `resources/dmg/background.png` deliberately uses this app's
own LIGHT-theme palette, not the dark one the rest of the app defaults
to: a dark background was tried first and, confirmed via a real
screenshot, made Finder's own black icon-view label text unreadable —
Finder's DMG label-text color isn't tied to the Mac's system light/
dark appearance the way it might seem, it's effectively a fixed
default, and there's no supported way to make a DMG's own background
switch with system appearance at all (`.DS_Store`-based background
customization is a static image, full stop) — a light background is
the actual, permanent fix, and the reason nearly every polished Mac
app's own `.dmg` already uses one.

Neither `create-dmg` nor its `hdiutil` predecessor ever risked
reintroducing the ORIGINAL packaging bug this project hit first,
CPack's `DragNDrop` generator — found the hard way
(2026-08, see the packaging-bug writeup below): `cpack -G DragNDrop`
re-stages the bundle from that `install()` rule's own build-time
manifest rather than copying whatever the build tree currently
contains, silently dropping `Contents/Frameworks` — added to the
tree *after* that manifest was fixed, by the deploy steps described
below — every time. The `.icns` itself is generated fresh in this job
(assembling an `.iconset` from the same committed PNGs
`tools/generate_app_icon.cpp` produces, then `iconutil -c icns`) —
`iconutil` only exists on macOS, so unlike every other icon asset in
this repo, `.icns` is never committed.

Unlike every other build job in this workflow (Windows, Linux, RPM,
and AppImage all run on `ubuntu-latest`, either natively or inside a
container), this is the first one running on a genuinely different,
native runner OS — meaning it's also the only job whose CMake code
paths (the `APPLE` branches throughout `CMakeLists.txt` and
`CredentialStore.cpp`) can't be exercised at all on the Linux sandboxes
this project has otherwise been developed and verified in.
**Released for real as v0.7.0** — `build-macos` green end to end
(build, full required test suite including `verify-credential-store`,
`.icns` generation, `.dmg` packaging) on a genuine GitHub Actions macOS
runner, and the `release` job actually ran and published all 6 assets.
Took five real CI iterations to get there, not one — this sandbox has
no macOS hardware, so nothing macOS-specific could be pre-validated
locally, and every fix below was diagnosed from a real CI failure and
re-verified by pushing again:

1. Before ever pushing, a `navigation-test` hang was found and fixed
   *locally* (not CI) — reproducible in this project's own long-lived
   local sandbox, confirmed via `git stash` to already exist on
   unmodified `main`, so unrelated to the macOS work itself, just
   never previously triggered.
2. First `workflow_dispatch` run: `build-macos` failed on a
   `transfer-pause-test` timing flake (fixed 250ms delay, not enough
   on a slower/colder macOS runner than any Linux CI container ever
   exposed).
3. Second `workflow_dispatch` run: green. Tag `v0.7.0` pushed,
   triggering the real release pipeline.
4. First release run: `build-linux-appimage` failed on an unrelated,
   transient network blip (confirmed transient — passed cleanly on
   every other run with no code changes); `build-macos` failed on a
   SECOND, different `navigation-test` flake (the file's opening
   back/forward/up sequence, same fixed-delay root cause as #2 but a
   different check, "navigated to filesystem root"). Tag moved to the
   fix, re-pushed.
5. Second release run: `build-macos` failed a THIRD time, on
   `file-operations-test`'s `setPermissions()` readback (same root
   cause again). Tag moved to the fix, re-pushed. Third release run:
   green end to end, release published.

See CONTRIBUTING.md's own entries on each of the three CI-discovered
timing fixes for the mechanism-level detail.

**A real, end-user-breaking packaging bug shipped undetected through
v0.7.0-v0.7.2, and took three separate fixes across two releases
(v0.7.3, v0.7.4) to actually resolve** — `cpack -G DragNDrop` packaged
whatever the raw `cmake --build` output was, with no framework-bundling
step at all, so the `.app`'s Qt/libssh2/openssl load commands still
pointed at the CI runner's own Homebrew prefix (`/opt/homebrew/...`).
This job's own "Run test suite" step never caught it because it runs
the binary on the very machine that prefix already resolves on — the
bug was only visible to a real end user on a different Mac, who hit an
`EXC_CRASH`/`DYLD ... Library not loaded` on launch. Found via exactly
that (a user reporting a launch crash on a released `.dmg`):

1. **First fix (shipped as v0.7.3, still broken)**: added `macdeployqt`
   (Qt's own frameworks/plugins) and `dylibbundler` (everything
   macdeployqt doesn't know about — libssh2, openssl@3, both outside
   the Qt prefix) before packaging, plus an `otool -L` check confirming
   the binary's load commands no longer reference `/opt/homebrew`. That
   check passed — but proved nothing about whether the referenced files
   actually shipped. They didn't: the SAME user hit the exact same
   crash on the "fixed" release, with a diagnostic this time showing
   the load command was correctly `@executable_path/../Frameworks/...`
   but the file at that path simply didn't exist in the `.dmg`.
2. **Second fix (still within the v0.7.4 cycle)**: root cause was
   `cpack -G DragNDrop` itself re-staging the bundle from CMake's own
   `install()` manifest — see the packaging paragraph above — silently
   dropping `Contents/Frameworks`. Fixed by dropping CPack for macOS
   entirely and building the `.dmg` directly with `hdiutil` from the
   exact tree the deploy steps had just modified in place, plus a
   SECOND verification step that mounts the actual built `.dmg` and
   confirms the framework file genuinely exists inside — the `otool -L`
   check alone had already proven insufficient once.
3. **That fix's very next CI run still failed** — the NEW mount-and-
   verify check caught it before it could ship again: `dylibbundler
   -od` (used for the libssh2/openssl step) doesn't mean "overwrite
   individual files," it means "erase the entire destination
   *directory*" — confirmed directly in the job log (`rm -r
   Contents/Frameworks/` followed by `mkdir -p`) — silently deleting
   everything `macdeployqt` had just bundled moments earlier in the
   SAME job. Fixed by switching to `-of` (overwrite files).
   **v0.7.4, released after this third fix, is the actual working
   one** — confirmed via both verification checks passing on a real
   CI run. If recommending a download, v0.7.3 should never be pointed
   at for macOS specifically.

**Lesson, worth remembering for any future "deploy dependencies into a
packaged artifact" work on any platform**: verify the actual shipped
artifact's contents directly (mount/extract it, check for real files)
— not the intermediate build tree, not a tool's own "success" exit
code, not even a check on the pre-packaging state. Three genuinely
different bugs in this exact class each looked fixed at every
intermediate step while the final artifact was still broken.

**Confirmed working end-to-end on GitHub's own runners**, not just
locally: the Windows and Linux build jobs pass, the full test suite
passes on both platforms, and a real tagged release (`v0.2.0`)
exercised the `release` job for real — a GitHub Release with both
`zephyrftp-windows-x64.zip` and `zephyrftp-linux-x64.tar.gz` attached.
The Windows `.exe` specifically has also been run on real Windows
hardware directly (not just under wine), launches as a proper GUI app,
and connects to a real SFTP server.

Every stage of this pipeline surfaced at least one real, non-obvious bug
along the way — worth knowing about if it ever needs touching again.
**Current MinGW/Linux-CI era:**
- libssh2 discovery is toolchain-specific, not just OS-specific: the
  `WIN32` CMake branch assumed vcpkg's CMake config package, which
  doesn't exist in the mingw64 sysroot (it ships a `.pc` file like
  Linux); the check is actually `WIN32 AND NOT MINGW`
- `smoke_test.cpp` never propagated failure to its exit code, unlike
  every sibling test — invisible when reading `qDebug()` PASS/FAIL text
  directly, but a real gap once `qDebug()` output turned out not to
  reach the terminal under wine at all (see below) and exit code became
  the only signal
- `site_store_test.cpp` left a `QFile` handle open across a later
  delete of the same path — POSIX `unlink()` tolerates that, Windows
  `DeleteFile` doesn't, so the empty-store check genuinely failed under
  wine until the handle was closed explicitly
- wine needs a real, even virtual, X display for its own internal
  window management, independent of Qt's `QT_QPA_PLATFORM=offscreen` —
  every test driving a real `QMessageBox` failed with `CreateWindowEx
  failed (Invalid window handle.)` in a genuinely headless container
  until every `wine` invocation was wrapped in `xvfb-run`
- GitHub Actions sets `$HOME=/github/home` for container jobs, and wine
  refuses to create its default `~/.wine` there (an ownership check
  failure, the same category of issue as git's "dubious ownership") —
  never reproduced locally under `podman` (plain root, normal `$HOME`),
  only caught on GitHub's own runners; fixed by pinning `WINEPREFIX` to
  a scratch directory the job creates and owns outright
- `pkg-config` isn't pulled in by `build-essential` on a clean Ubuntu
  install — CMake's libssh2 discovery needs it, and this only surfaced
  in an actual from-scratch container build, not on a desktop machine
  that already happened to have it

**Earlier MSVC+vcpkg era** (the pipeline this replaced):
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

**`build-windows-native`** (2026-08) — a real native MSVC/Qt/vcpkg
build on a self-hosted Windows runner, effectively finishing what the
"Earlier MSVC+vcpkg era" above started and abandoned. It's viable now
for a reason that era didn't have: a **self-hosted, persistent**
runner rather than an ephemeral GitHub-hosted one — the toolchain
(git/cmake/ninja/NSIS/Python/VS Build Tools, Qt, vcpkg's built
libssh2/openssl) installs once and survives across runs, so there's no
`actions/cache` thrashing to manage and no vcpkg binary-cache backend
needed at all (the exact thing that era's own `x-gha` removal bullet
above ran into). Discovered via `gh api repos/.../actions/runners`:
three self-hosted runners (`EAS-DEFIANT`/Windows, `eas-ubuntu26`/
Linux, `Mac`/macOS) were registered but completely unused by this
workflow. **Deliberately never used on `pull_request`** — this repo is
public, and a self-hosted runner on a PR-triggered workflow is
GitHub's own documented anti-pattern (a stranger's PR could execute
code directly on real, persistent hardware rather than an ephemeral
sandboxed VM); PR-triggered runs stay on the GitHub-hosted jobs above.

Machine was completely bare when first probed: no git/cmake/Qt/MSVC/
NSIS, only `winget`. Bootstrap is idempotent throughout (`winget list`
check before every install) so only the FIRST run on the machine pays
real cost — later runs are fast no-ops. Qt installed via pip's
`aqtinstall` (`win64_msvc2022_64`, fixed `C:\Qt` root) rather than the
GUI Maintenance Tool — no Qt account needed, same LGPL binaries.
libssh2/openssl via `vcpkg` (`x64-windows` triplet, fixed `C:\vcpkg`
root) — `openssl` is a slow from-source build (~12 min first time,
instant after). `cl.exe`/`link.exe` are deliberately never on PATH
after a VS Build Tools install (by design, to avoid version/arch
conflicts) — sourced from `vcvars64.bat` (located via `vswhere`) once
per job and persisted to later steps via `GITHUB_ENV`/`GITHUB_PATH`
(a fixed list of the vars `vcvars64.bat` is known to set, not a
wholesale environment dump, so it composes with what earlier steps
already added rather than clobbering it).

**Real, previously-undiscovered bugs found getting this working, each
only visible from an actual native-Windows run** — nothing here
reproduces under wine, which is exactly why this job exists alongside
the cross-compile one rather than replacing it outright (yet):
- NSIS's own installer, unlike Git's/CMake's MSIs, never registers
  itself on PATH at all — confirmed directly (`winget` reported
  "Successfully installed", `makensis` still unresolvable after a
  registry-based PATH refresh that worked for every other tool). Fixed
  by adding its known install directory explicitly.
- A bare `python` call in a *later* step hit Windows' own `python.exe`
  App Execution Alias stub in `WindowsApps` ("Python was not found;
  run without arguments to install from the Microsoft Store") instead
  of the real interpreter just installed — `Get-Command python`
  resolved correctly in the SAME step Python was installed (PATH
  rebuilt directly from the registry, in a known order) but a *later*
  step's PATH is assembled by the runner from accumulated
  `GITHUB_PATH` entries in write order, which put `WindowsApps` ahead
  of the real interpreter's directory. Fixed by persisting the real
  interpreter's full path once (`PYTHON_EXE`) and invoking it directly
  everywhere after, sidestepping PATH resolution (and the alias)
  entirely rather than trying to out-order it.
- `aqt install-qt ... -m qtsvg` failed outright ("packages ['qtsvg']
  were not found") — confirmed via `aqt list-qt --modules` that Svg
  has no separate addon for this version/arch at all, unlike
  `qtcharts`/`qtmultimedia`; it's already in the base essential
  download (independently rediscovering the exact same fact the
  "Earlier MSVC+vcpkg era" bullet list below already recorded — worth
  reading that section BEFORE the next native-Windows Qt change, not
  after re-deriving it a second time).
- The job initially had no `actions/checkout` step at all (a plain
  oversight, copy-pasted the toolchain-setup steps without the pattern
  every other job already follows) — `cmake -B build` failed with "does
  not appear to contain CMakeLists.txt" until added.
- Every test binary's PASS/FAIL reporting is exclusively `qDebug()` —
  the first real test run produced **zero visible output for any
  test, including ones that passed** (not a symptom of the one that
  failed). `QT_FORCE_STDERR_LOGGING=1` fixed it — a different root
  cause than this project's own already-documented Fedora
  `qtlogging.ini` case (see the Qt-debug-logging project memory) but
  the same class of "qDebug() silently not reaching the console"
  gotcha, this time specific to GUI-subsystem Qt apps on Windows.
- With logging visibility fixed, real output showed `QFontDatabase:
  Cannot find font directory ... Qt no longer ships fonts` followed by
  `QFont::setPointSizeF: Point size <= 0` warnings — the `offscreen`
  QPA platform uses Qt's own FreeType-based font engine (no real
  display/GDI to fall back on) and this bare Windows box has zero font
  files available to it, unlike Linux/macOS CI jobs, which get fonts
  transitively via fontconfig/system integration. Fixed by pointing
  `QT_QPA_FONTDIR` at Windows' own real `C:\Windows\Fonts` rather than
  bundling a font.
- Fixture paths for `transfer-queue-test`/`folder-transfer-test`
  target `C:\tmp\...` rather than `$env:RUNNER_TEMP`: the test C++
  sources hardcode the literal string `/tmp/transfer_test` etc.
  directly (confirmed by reading the source, not assumed), and Qt
  resolves a leading-slash path on Windows as relative to the current
  drive root — so the CI fixture setup has to create files at the same
  drive-relative location the app will actually look, not a
  Windows-idiomatic temp path. Confirmed correct via the real test
  passing, not just reasoned about.

**One real, structural gap remains, deliberately left open rather than
chased further for now**: `navigation-test`'s `fileOpRace`/
`renameRace` phases each drive a real right-click context menu via
`QMenu::exec()`, using this project's own documented, already-correct
technique for making `exec()` return under `offscreen`
(`QTest::mouseClick()` at the action's own `actionGeometry()`, not a
raw `trigger()` — see the Qt-debug-logging project memory's "Also
learned" section, where this was first established). On Windows
specifically, `QApplication::activePopupWidget()` never reflects the
menu as active at all — confirmed via temporary tick-by-tick
instrumentation logging both `activePopupWidget()`/
`activeModalWidget()`: the popup stays `null` for the entire window
the dialog-pump timer runs, even though the platform's own "does not
support raise()"/"does not support grabbing the keyboard" warnings
fire (proving `QMenu::exec()` genuinely IS being called and attempting
to show). This is a real, structural difference in how the `offscreen`
platform's popup/window-activation model behaves on Windows versus
Linux/macOS — not an environment variable away, and NOT the same class
of gotcha the `QTest::mouseClick()` fix already covers (that fix makes
`exec()` *return* once a click lands; this gap is that the click never
has anywhere real to land, because the menu was never recognized as
active in the first place). **Scoped and confirmed via a full run of
all 27 other required targets on this job — every one of them passes
clean, including `keyboard-shortcuts-test` and `external-drop-test`
(both also drive UI interaction, initially suspected as being at
similar risk) and `verify-credential-store` (the real Windows
Credential Manager backend, tested for the first time ever, genuinely
round-trips)**. `navigation-test`'s failure is tolerated at exactly
this one call site (`RunTest navigation-test -AllowFailure` in
`build.yml`) — its own real PASS/FAIL output still prints in full, a
`::warning::` annotation flags it visibly in the Actions UI, and every
other test still hard-fails the step normally. Revisit as its own
investigation if this ever becomes worth the time; don't resurrect the
"quick env var" instinct that fixed the font/logging issues above —
this one is a different, deeper class of problem.

**Packaging (`windeployqt` + `vcpkg`'s DLLs + NSIS installer + a real
native install/launch/uninstall check) worked on the first real attempt
for everything except the installer itself** — `windeployqt` handles
Qt's own DLLs/plugins the MinGW cross-compile job has no equivalent
tool for; `vcpkg`'s whole `installed\x64-windows\bin\` gets copied
wholesale for libssh2/openssl/zlib (`windeployqt` has no idea they
exist at all, the same real gap this project's earlier abandoned
MSVC+vcpkg attempt already hit — see its own bullet list above). The
NSIS step needed two real iterations, though, both root-caused rather
than guessed around:
1. **Official NSIS ships zero `amd64-unicode` stub support at all** —
   confirmed directly by extracting both the plain `nsis-3.12.zip` and
   the `setup.exe` winget installed; neither has anything but
   `x86-ansi`/`x86-unicode` in its `Stubs/` directory.
   `windows-installer.nsi`'s own `Target amd64-unicode` directive
   (needed since this project ships 64-bit only) has no official source
   to build against.
2. A first attempt vendored the amd64-unicode stubs from Fedora's own
   `mingw64-nsis` package (the exact package `build-windows`'s
   cross-compile job already depends on, license `Zlib AND CPL-1.0`) —
   this let `makensis` succeed, but the resulting installer **crashed**
   (`STATUS_ACCESS_VIOLATION`, `0xC0000005`) the moment it actually ran
   on real Windows, before creating so much as its install directory.
   Root-caused via web research rather than more blind iteration:
   SourceForge NSIS bug #1198 — official NSIS's own amd64-unicode
   implementation doesn't support callbacks, and `MUI2.nsh`'s
   `MUI_PAGE_STARTMENU` macro (this script's Start Menu folder page)
   uses exactly one. Wine's looser emulation tolerates it (the
   cross-compile job's own installer, from the identical class of
   Fedora-built stub, passes wine-based verification every release);
   real Windows does not. Genuinely surprising given the earlier
   assumption that "Fedora's package works, so its stub binaries must
   be fine anywhere" — they're fine for producing an installer that
   *wine* can run, not one real Windows can.
3. Fixed for real with the [negrutiu/nsis](https://github.com/negrutiu/nsis)
   fork via its own purpose-built
   [negrutiu/nsis-install](https://github.com/negrutiu/nsis-install)
   GitHub Action (`arch: amd64`, pinned to an exact commit SHA rather
   than a floating tag — supply-chain hygiene for a third-party action,
   though it only ever runs on non-PR triggers same as everything else
   in this job) — a genuine native `amd64` NSIS build, not another
   cross-compiled repackaging of the same broken official
   implementation, so it doesn't hit bug #1198 at all. The vendored
   Fedora stub files were removed once this was confirmed working; don't
   resurrect that approach if this ever needs revisiting.
4. Install/launch/uninstall verification passed cleanly once the
   installer itself stopped crashing — same file/shortcut/registry
   checks the wine-based job already does, plus a genuine "does the
   installed app actually launch and stay running" check (start the
   process, confirm it's still alive 3 seconds later, stop it) that the
   wine-based job never attempted.

**`build-macos` on a self-hosted Mac runner (2026-08-21) — wired up the
same way, and everything works except one real, structural, exhaustively
root-caused gap.** Real hardware (bare-metal 2023 MacBook Pro, arm64),
wired to the same non-PR-only self-hosted pattern as `build-linux`/
`build-windows-native`. Two real setup issues found before the app's own
tests even ran:
- The runner's own GitHub Actions agent had been installed from the
  `osx-x64` package on genuinely Apple Silicon hardware — the whole
  process ran under Rosetta translation the entire time (confirmed via
  `sysctl.proc_translated: 1` alongside `hw.optional.arm64: 1`, and a
  second, corroborating signal: Homebrew's own package-index cache
  filename lacked the `arm64_` prefix it has on a genuinely native
  install). Fixed by reinstalling the runner from the correct
  `osx-arm64` package — reverified via the same `sysctl` check
  (`proc_translated: 0`) before trusting it.
- `cmake` isn't in this job's own `brew install` list at all — it was
  never needed before because GitHub-hosted `macos-latest` ships it
  preinstalled in the runner image. A genuinely bare self-hosted Mac has
  no such image; `cmake: command not found` on the very first real run
  here. Fixed by adding it to the list (harmless on `macos-latest` too,
  where it's just a fast no-op alongside the one already on PATH).

**`verify-credential-store` fails on this specific runner with
`SecKeychainCopySettings: User interaction is not allowed` — a real,
persistent gap, exhaustively root-caused rather than guessed past, and
currently tolerated (`|| echo "::warning::..."` around just that one
call, everything else in the step stays strict `set -e`).** Six real
fix-and-reverify cycles, each a genuine hypothesis tested against actual
hardware, not speculation stacked on speculation:
1. Suspected a stale runner-service session predating a config change —
   ruled out: identical failure after a real reboot.
2. Suspected the runner was a LaunchDaemon (system-level, no GUI/
   keychain access ever) rather than a LaunchAgent — ruled out:
   confirmed correctly placed at
   `~/Library/LaunchAgents/actions.runner.*.plist`.
3. Suspected a VM without a properly-attached virtual display (a
   documented cause of exactly this error on macOS CI, since WindowServer
   sometimes won't grant a real graphic-access session without one) —
   ruled out: this is bare metal.
4. Suspected clamshell mode (lid closed, no external display — the
   laptop equivalent of the same VM/headless issue) — ruled out: lid
   open, display genuinely on, confirmed directly.
5. Suspected a launchd boot-time race (the LaunchAgent's `RunAtLoad`
   firing a moment before loginwindow finishes flagging the session as
   interactive, a state that would persist for that process's entire
   lifetime regardless of later reboots) — ruled out: forcing a live
   restart with `launchctl kickstart -k
   gui/<uid>/actions.runner.arelas-zephyrftp.Mac` while already
   actively logged in produced the identical error.
6. Suspected unsigned-binary identity churn — newer macOS versions
   restricting Keychain access for binaries with no stable code-signing
   identity, meaning every fresh CI build looks like a brand-new,
   never-approved app to the Keychain ACL system — tested directly with
   a real ad-hoc `codesign --force -s -` experiment on the test binary
   before it runs. Ruled out: byte-for-byte identical failure either
   way; the experiment step was reverted afterward.

**None of the standard causes for this class of macOS CI issue apply
here.** Worth remembering as a genuinely open question if this ever
gets revisited — the next candidate, if anyone wants to keep digging,
would likely be something CredentialStore's own macOS backend does when
constructing its `SecItemAdd`/`SecItemCopyMatching` queries (an
explicit `kSecUseDataProtectionKeychain` mismatch, or an ACL/
`SecAccess` configuration issue specific to how a *new* keychain item's
initial access group gets set up) rather than anything about the
runner's environment — every environmental cause remotely diagnosable
without deep Keychain-API-level debugging has now been exhausted.
Every other test target on this job passes clean, including a real
full Qt6 Homebrew build from scratch on native arm64.

## Known gaps (flagged, not fixed)

- **FTP/FTPS has now actually touched a real server on every one of its
  code paths, not just the happy path — control connection, PASV AND
  active/PORT data connections, real transfers, the `AUTH TLS` upgrade,
  the LIST fallback, and a full encrypted transfer, all confirmed
  working, not just unit-tested in isolation.** `src/verify_ftp_live.cpp`
  (the `verify-ftp-live` `EXCLUDE_FROM_ALL` CMake target — not part of
  the required self-contained suite, since it needs external servers
  already running) drives a real `FtpBackend` through five phases
  against five throwaway local servers
  (`tools/local-test-servers/start-ftp.sh`, `start-ftps.sh`,
  `start-ftp-legacy-list.sh`, `start-ftps-trusted.sh`,
  `start-ftp-active-only.sh`): (1) plain FTP connect/list/download/upload,
  content confirmed both client-side and read back directly off the
  server's own disk; (2) FTPS against a self-signed cert with no
  certificate verifier wired up — confirms the `AUTH TLS` handshake
  completes and `FtpBackend` fails closed with nobody to ask, same "fails
  safe" contract `SftpBackend::askUserToTrustHostKey()` already has (see
  `verify-ftps-trust` below for the real trust-prompt flow with a
  verifier actually wired up); (3) the same round trip as (1) against a
  server with MLSD genuinely disabled (`ftp_server.py --legacy-list`
  deletes it from `proto_cmds`, a real 502 from a real server, not a
  crafted client-side toggle) — closes the gap that only the fallback
  parser's logic had been exercised, never the real trigger; (4) the same
  round trip again against a server with PASV/EPSV genuinely disabled
  (`--no-pasv`), forcing `FtpBackend`'s real active/PORT fallback — a
  connection the SERVER dials back to US, completing a full round trip;
  (5) the same round trip over FTPS, with this harness (pure test code,
  no `FtpBackend` change) pre-trusting a throwaway CA via
  `QSslConfiguration::setDefaultConfiguration()` so the leaf certificate
  `start-ftps-trusted.sh` presents genuinely validates — proves a full
  ENCRYPTED transfer completes end to end, the one case (1)/(3)/(4) don't
  cover and (2) deliberately doesn't reach. Confirmed reliably across
  multiple repeated runs, not a one-off. **What this alone doesn't
  cover** (closed separately, see the vendor-diversity entry right
  below): every server above is pyftpdlib underneath, not a genuinely
  different, independently-implemented one.
- **The legacy-LIST fallback is now also confirmed against real,
  independently-implemented FTP server software, not just pyftpdlib —
  and that verification caught a real gap in the fallback trigger
  itself before it shipped.** `tools/local-test-servers/containers/`
  (`Containerfile.vsftpd`, `Containerfile.proftpd`, both `fedora:44`,
  the same base image `build-windows`'s CI job already uses) build real
  vsftpd and real proftpd, each in its own throwaway `podman` container,
  started via `start-vsftpd.sh`/`start-proftpd.sh`.
  `src/verify_ftp_vendors.cpp` (the `verify-ftp-vendors`
  `EXCLUDE_FROM_ALL` target) drives `FtpBackend` through a full
  connect/list/download/upload round trip against each, verified content
  both ways (uploads are confirmed by downloading them back through the
  same protocol session, not by reading the container's filesystem
  directly — these containers are self-contained, no host-mounted
  scratch directory). vsftpd is the highest-value case: it has never
  implemented MLSD/RFC 3659 in any version, so it's a real server
  hitting the real fallback trigger, not pyftpdlib with a flag forcing
  MLSD off. proftpd is a second, independently-coded implementation
  (its own `LIST`-format quirks) — its packaged build has `mod_facts`
  compiled in, so MLSD is denied via a `<Limit>` block instead of being
  genuinely absent, which turned out to matter: that denial replies
  `550`, not the `500`/`502` a truly-unimplemented command gets, and
  `FtpBackend.cpp`'s fallback trigger originally only checked for
  `500`/`502`. Caught by actually running this against a real server
  configuration, not reasoned about in advance — the trigger now also
  treats `550` as a fallback signal, since "MLSD present but
  administratively denied" is a real, plausible server configuration,
  not a hypothetical one. **FTPS against these same two real vendors is
  now also confirmed — a full working round trip against vsftpd, and an
  honest, understood, real rejection against proftpd — see the
  TLS-session-ticket-reuse entry below for the full story.**
- **SSH-daemon diversity for SFTP has a real, honest ceiling: OpenSSH's
  own `sftp-server` is very likely the SFTP implementation underneath
  almost anything you'll connect to, no matter which daemon fronts it.**
  Checked directly rather than assumed before building
  `tools/local-test-servers/containers/Containerfile.dropbear`: Dropbear
  (a genuinely different, independent SSH daemon from OpenSSH) ships no
  `sftp-server` binary of its own at all (`rpm -ql dropbear` on Fedora
  has none) — it shells out to an external one, configured here to be
  OpenSSH's (`/usr/libexec/openssh/sftp-server`). `start-dropbear.sh` +
  `src/verify_sftp_vendors.cpp` (the `verify-sftp-vendors` target) do
  confirm real value from this: a genuinely different SSH transport/
  session/auth daemon, with password auth (vs. `start-sftp-pubkey.sh`'s
  pubkey-only OpenSSH setup) adding auth-method diversity too — but not
  SFTP-wire-protocol-implementation diversity. A real, independently
  implemented SFTP *server* (not just a different SSH daemon delegating
  to OpenSSH's) wasn't found to be practically available to containerize
  for this project; revisit if one becomes practical.
- **FTPS certificate verification is now a real trust-on-first-use (TOFU)
  model, not fail-closed-only — confirmed end to end against a real
  server, including the mismatch/decline path.** `CertificateVerifier`
  (`src/ui/CertificateVerifier.h/.cpp`) mirrors `HostKeyVerifier` exactly,
  including the two real bugs a dedicated code review found and fixed in
  both together — see `HostKeyVerifier`'s own entry above for the
  missing-port and nullptr-parent detail, both fixed identically here:
  an unverifiable certificate routes to a real person via the same
  blocking-cross-thread-call pattern, and the decision persists to
  `AppConfigLocation/known_certs.json` (a certificate fingerprint is
  public information, same status as an SSH host-key fingerprint already
  stored in plaintext `known_hosts`). `src/verify_ftps_trust.cpp` (the
  `verify-ftps-trust` `EXCLUDE_FROM_ALL` target) drives this against the
  real self-signed `start-ftps.sh` server, reusing
  `verify-sftp-pubkey`'s "poll for the active modal, click its button"
  technique (fully automated, not a manual click-through) for three
  phases: first-ever sighting → prompt fires, auto-accepted, connects and
  lists for real; a second connection to the same, unchanged certificate
  → NO prompt at all, proving the fingerprint was actually persisted and
  reused, not just accepted in memory; the stored fingerprint corrupted
  to a value that can't match (simpler and fully deterministic than
  regenerating the server's actual certificate) → the mismatch warning
  fires, and declining it (auto-clicked here too) fails the connection
  closed, same as declining a changed SSH host key. One real bug this
  verification caught before it shipped: the active/PORT data-connection
  helper (`SslAcceptingTcpServer`) originally parented each accepted
  socket to the listening `QTcpServer`, so deleting that server right
  after extracting the socket (per `finalizeDataChannel()`'s own
  sequencing) also deleted the socket out from under its new owner — a
  dangling-pointer crash only a real accept-then-use cycle surfaced, not
  reasoning about the code.
- **FTPS against real vendor servers is now fully working end to end —
  vsftpd AND proftpd, including proftpd's strict TLS-session-reuse
  enforcement, which used to be an honest, documented failure.** An
  earlier pass through this exact problem found and documented a real,
  negative result: this project's TLS-1.3-ticket-based reuse
  (`QSslConfiguration::sessionTicket()`) does not read as genuine
  session reuse to proftpd's `mod_tls` (`"client did not reuse TLS
  session, rejecting data connection"`), and forcing TLS 1.2 through
  Qt's own API didn't help either — `QSslSocket::newSessionTicketReceived()`
  simply never fires under TLS 1.2, and neither `QSslConfiguration` nor
  `QSslSocket` expose any way to drive classic TLS 1.2 session-ID/RFC
  5077 ticket resumption directly (checked directly against Qt 6.11's
  public headers, no `nativeHandle()`-style escape hatch either). That
  investigation concluded a genuine fix needed raw OpenSSL, and left it
  there as a real, disclosed limitation rather than attempting it
  without solid evidence it would actually work.

  This session picked that back up and finished it. `FtpTlsSocket`
  (`src/backends/FtpTlsSocket.h/.cpp`) is a from-scratch raw-OpenSSL TLS
  layer, used for FTPS's control AND data connections in place of
  `QSslSocket` (plain FTP is completely unaffected — still `QSslSocket`
  used purely as TCP, via the small `QtSocketAdapter` forwarding wrapper
  in `FtpSocket.h`). Forces exactly TLS 1.2 (confirmed, below, to be
  what makes genuine session-ID resumption possible at all), performs
  its own TOFU certificate verification (`SSL_VERIFY_NONE` at the TLS
  layer — this project's real trust model is fingerprint pinning, not
  CA-chain validation, so `SSL_get_verify_result()` is consulted
  afterward purely to feed `FtpBackend::verifyTlsPeer()` the same
  "problem description" role `QSslSocket::sslErrors()` used to serve —
  see that method and `FtpTlsSocket::handshake()`), and captures/reuses
  a real `SSL_SESSION` via `SSL_get1_session()`/`SSL_set_session()`.
  Blocking I/O is a hand-rolled `poll()`/`WSAPoll()` retry loop directly
  on the raw socket descriptor (same shape as `SftpBackend.cpp`'s
  `waitForSocketReady()` for libssh2, since this class has no libssh2
  involvement to delegate that to) — the same "let `QTcpSocket` own
  DNS/connect/close, but never call its own read/write once something
  else drives the fd directly" pattern `SftpBackend.cpp` already
  established for the identical reason.

  Getting from "does a single reused session satisfy proftpd's check at
  all" (yes — confirmed first via a disposable, non-Qt raw-OpenSSL probe
  with both a positive and a negative control, before writing any
  production code) to "does a REAL session — list, download, upload, a
  second download, all over one FTPS connection — actually work end to
  end" took finding and fixing one more real, non-obvious bug:

  - **A `SSL_SESSION*` can only ever be used in ONE `SSL_connect()` call,
    even up-ref'd, even re-fetched fresh — confirmed directly against a
    real proftpd container, not documented anywhere obvious.** The
    first data connection to reuse the control connection's session
    always succeeded (`SSL_session_reused()==1`, real data came back);
    every data connection after that — a second download, an upload —
    silently fell back to a full, unresumed handshake
    (`SSL_session_reused()==0`) and got the exact `"client did not
    reuse TLS session, rejecting data connection"` rejection, even when
    handed the identical, still-valid session object that had just
    worked, or a session freshly re-captured from the connection that
    HAD just resumed successfully. A disposable multi-connection raw
    probe (no Qt, no app code — isolating this from any possible
    `FtpTlsSocket` bug) reproduced it identically, ruling out a client
    logic bug; three different proftpd-side session-cache
    configurations (`TLSSessionCache`/`mod_tls_shmcache`,
    `TLSSessionTickets on`) changed nothing. The actual fix, found via
    the same probe: **deep-duplicate the session (DER
    `i2d_SSL_SESSION`/`d2i_SSL_SESSION` round trip) into a genuinely
    independent object immediately before every single
    `SSL_set_session()` call**, rather than reusing (even via
    `SSL_SESSION_up_ref()`) the same live object across multiple
    handshakes — OpenSSL's client-side `SSL_SESSION` object evidently
    picks up some internal "already used in a handshake" state on its
    first use that a fresh, independently-parsed duplicate doesn't
    carry. See `FtpTlsSocket::handshake()`'s own comment for where this
    lives; `FtpBackend::finalizeDataChannel()` also rotates the control
    connection's own cached session to each data connection's freshly
    negotiated one afterward (harmless and occasionally load-bearing,
    e.g. if a server DOES rotate tickets — not what actually fixed
    this, but kept since it costs nothing).

    `verify_ftp_vendors.cpp`'s `proftpd-ftps` phase (full list,
    download, upload, download-back round trip, confirmed byte-for-byte)
    passes reliably and repeatably against `containers/proftpd.conf`'s
    unmodified, still-default strict `mod_tls` config (no
    `NoSessionReuseRequired` relaxation) — the exact scenario the
    earlier pass through this problem left as a documented failure.
  - **vsftpd: a full round trip (list, download, upload, content
    verified both ways) now genuinely completes — but only with
    `require_ssl_reuse` left off, and that's a deliberate, documented
    trade, not a cop-out.** With `require_ssl_reuse=YES`, vsftpd's
    AUTH TLS handshake and login succeed, but the subsequent data
    connection then just hangs indefinitely — confirmed reproducible on
    freshly rebuilt containers, confirmed NOT specific to session-ticket
    reuse (the identical hang happens with `FtpBackend`'s own reuse
    attempt disabled entirely), and — the deciding piece of evidence —
    confirmed to depend on `ptrace` observation: attaching `strace` to
    the whole process tree reliably makes vsftpd send a clean `"522 SSL
    connection failed: session reuse required"` within seconds, while
    the *identical* scenario left completely undisturbed never resolves
    even after 4+ minutes. That points at a genuine deadlock inside
    vsftpd's own privilege-separated architecture under this specific
    container environment (very plausibly interacting with
    `seccomp_sandbox=NO`, a setting this container already needs just
    to run at all) — a real vsftpd/environment bug, not a `FtpBackend`
    one, and not something further client-side changes can fix since
    the server itself never responds. `vsftpd.conf` ships with
    `require_ssl_reuse=NO` as a result — the scenario this project's
    own code is confirmed correct and fast for — with the reasoning and
    the option to flip it back for further investigation documented
    directly in that file, plus permanent protocol-level logging left
    on for whoever picks it back up.

  Three real, independent client-side bugs found and fixed along the
  way — none of them specific to either vendor, all genuine gaps a
  single-attempt, happy-path test was never going to hit:
  1. `listDirectoryInternal()`'s read loop used to block exclusively on
     the data connection for the full 15s timeout, only ever checking
     the control connection's reply *after* giving up — discarding a
     perfectly good, already-arrived, specific error reply in favor of
     a generic "timed out" message whenever a server's data-connection
     close didn't also produce a clean disconnect Qt's
     `waitForReadyRead()` would notice promptly (confirmed via strace:
     vsftpd closing a completed data connection without a TLS
     `close_notify` first — `state()` never leaves `ConnectedState`).
     Now polls in short slices, checking the control connection for a
     decisive reply in between, and treats one further idle poll after
     any data has arrived as "done" (safe specifically because a real
     directory listing is always small and arrives essentially all at
     once — a large transfer legitimately pausing mid-stream is a
     different case, deliberately not covered by this same heuristic).
  2. `downloadFile()`'s read loop had the identical blind spot, fixed
     the same way, plus a second, more precise fix available here that
     `listDirectoryInternal()` doesn't have: once at least as many bytes
     have arrived as the server's own `SIZE` reply promised, the
     transfer is unambiguously complete regardless of what the
     connection's close behavior does or doesn't signal afterward.
  3. `uploadFile()` used to call `close()` (abrupt) on the data socket
     once the last byte was written, rather than `disconnectFromHost()`
     (graceful — flushes and sends a proper TLS `close_notify` before
     the TCP connection actually closes) — against a real vsftpd
     container, the abrupt close made every otherwise-correct upload
     fail with vsftpd's own `"Failure reading network stream"` error,
     the exact same class of "unclean-shutdown" problem as bug 1/2,
     just triggered from the opposite direction (the *client's* close
     being unclean this time, not the server's).
- **FTP now falls back to active/PORT mode when a server genuinely
  refuses PASV, confirmed against a real server that does exactly
  that.** `openDataChannel()` tries PASV first always (unchanged
  NAT-friendly default for every server that accepts it); only on an
  outright PASV refusal does `openActiveDataChannel()` open a local
  `QTcpServer` and tell the server to connect back via `PORT`.
  `verify-ftp-live`'s active-mode phase runs against
  `start-ftp-active-only.sh` (PASV/EPSV genuinely deleted from the
  server's own `proto_cmds`, a real refusal, not a client-side toggle)
  and confirms a full list/download/upload round trip completes over the
  connection the server dialed back to us. **A related bug found and
  fixed the same way (v0.2.12, chasing a real anomaly from manual GUI
  testing, not reasoned about in advance):** `openDataChannel()`'s PASV
  failure check (`!pasvReply.isValid() || pasvReply.code != 227`)
  funneled a dead/timed-out control connection into the same
  active-mode fallback as a genuine PASV refusal — despite the code's
  own comment claiming otherwise. On a dead control connection,
  `openActiveDataChannel()`'s follow-up IPv4 check reads a disconnected
  socket's empty `localAddress()` as "not IPv4" and reports the
  misleading `"Active mode requires an IPv4 control connection"`
  instead of the real problem. Confirmed with a standalone Qt program
  (not guessed): a live `QSslSocket::localAddress().protocol()` reports
  `IPv4Protocol` correctly for a `127.0.0.1` connection, but reports
  `UnknownNetworkLayerProtocol` once disconnected — exactly the
  mismatch that was firing. Fixed by only falling back to active mode
  on a genuine PASV error reply (`pasvReply.isValid() && code != 227`);
  an absent reply now reports `"Lost connection to the server"`
  directly instead.
- **`SftpBackend::checkExists()` is now confirmed against a real
  server, for all four cases that matter — including the
  ambiguous-stat-failure fallback, which used to just guess wrong.**
  Extended into `verify_sftp_pubkey.cpp` (see the public-key auth entry
  below for the harness itself): four concurrent `checkExists()` calls,
  matched back by `requestId` — the same disambiguation contract
  `TransferManager` relies on for real, exercised with more than one
  call in flight at once, not just the single best case — against a
  real existing file (`exists=true, isDir=false`), a real existing
  directory (`exists=true, isDir=true`), a path that genuinely doesn't
  exist (`exists=false`), and a real permission-denied path
  (`start-sftp-pubkey.sh` creates `restricted/secret.txt` under a
  `chmod 000` directory — genuinely existing, genuinely unstat()able,
  a real EACCES from `libssh2_sftp_stat()`, not simulated). All four
  confirmed correct. The bug this last case caught: `libssh2_sftp_stat()`
  failing for ANY reason — including `LIBSSH2_FX_PERMISSION_DENIED`,
  not just `LIBSSH2_FX_NO_SUCH_FILE` — used to be reported as
  `exists=false`, a guess that was actively wrong for a path that's
  genuinely there but couldn't be stat()'d (`stat()` needs
  execute/traverse permission on every ancestor directory, not
  read/write permission on the target itself, so this is a real,
  reachable case, not a hypothetical one). Now only
  `LIBSSH2_FX_NO_SUCH_FILE` is treated as confirmed nonexistence;
  anything else reports `exists=true` — the safe direction to be wrong
  in, since the one production caller
  (`TransferManager::onDestinationExistsChecked()`) uses this to decide
  whether to show an Overwrite/Skip prompt, and an unnecessary prompt is
  a much smaller problem than a silent overwrite of something that was
  actually there.
- **Directory deletion is never recursive, on either backend, by
  design.** Deleting a non-empty folder fails with a clear error rather
  than removing its contents first. This wasn't an oversight or a
  missing feature so much as a deliberate scope decision — recursive
  delete is a meaningfully bigger, more dangerous feature (a bug there
  could delete far more than intended, with no undo) than what was
  actually asked for. Worth revisiting explicitly if "delete this folder
  and everything in it" turns out to be something people actually want.
- **Public-key authentication is now confirmed against a real server,
  including the specific assumption that used to be a real risk.**
  `tools/local-test-servers/start-sftp-pubkey.sh` spins up a real,
  throwaway local `sshd` (public-key-only, no system config touched —
  see the script's own header comment) and `src/verify_sftp_pubkey.cpp`
  (the `verify-sftp-pubkey` `EXCLUDE_FROM_ALL` target, same "not part of
  the required suite" reasoning as the FTP one above) drives a real
  `SftpBackend` through `connectToHost()` with `SftpAuthMethod::PublicKey`
  — including the real host-key trust-on-first-use prompt, driven the
  same way `conflict_resolution_test.cpp` drives a live `QMessageBox` —
  then a real `listDirectory()`, `downloadFile()` (byte content
  confirmed), and `uploadFile()` (confirmed landed server-side by
  reading the file back directly off the server's own disk). Confirmed
  reliably across multiple repeated runs. This also directly exercises
  the specific assumption flagged as a real risk before: the key used is
  a conventionally-named `ssh-keygen`-generated ed25519 pair (the
  private key path plus its `.pub` sibling), and `SftpBackend` correctly
  derived the public key from that sibling file rather than needing it
  supplied separately — confirmed, not assumed. **Not covered by this**:
  any key type other than ed25519, a private key with a passphrase (the
  test key has none), and a key where the conventional `.pub` sibling is
  missing (the documented fallback path for that case remains
  unverified).
- **Remote-to-remote transfers are now supported, staged through a local
  temp file — fully verified by a deterministic fake-backend test, and
  partially verified against real servers, with the remaining gap named
  explicitly rather than assumed closed.** `remote-to-remote-test`
  (`src/remote_to_remote_test.cpp`, two independent fake `RemoteBackend`s)
  covers the orchestration completely and deterministically: direction/
  phase assignment on `enqueue()`, the download-phase -> upload-phase
  transition (including confirming `m_currentBackend` is genuinely
  re-pointed at the destination backend, not left stale), a real temp file
  actually existing on disk mid-transfer and actually being deleted after
  success, cleanup after cancellation during *either* phase (including the
  case where phase 1's download already completed and phase 2's partial
  upload needs cleaning up too), and `retryItem()` correctly resetting
  back to the download phase with a fresh temp path rather than trying to
  re-upload a file `cleanupTempFile()` already deleted. All 11 assertions
  pass reliably — genuinely reliably, not just "passed enough times
  locally," after two real problems surfaced and were fixed along the
  way, both in the test itself, not in `TransferManager`:
  1. A narrow test-fixture race: `QTimer::stop()` doesn't retract an
     already-queued timeout event, so a simulated failure followed
     immediately by a state check could occasionally see one more stray
     progress tick land after the "failure" — fixed with an explicit
     `m_finished` guard in the fake backend.
  2. **A structurally fragile design, not just a tuning problem.** An
     earlier version drove each scenario from fixed, generous
     (double-nominal) `QTimer` delays — 20+ repeated local runs all
     passed, but it still failed on a real GitHub Actions runner sharing
     the job with concurrent `linuxdeploy` work, and reproducing that
     locally under deliberate heavy CPU contention (14 busy-loop
     processes pinning a 16-core machine) made the fixed-delay version
     fail 15/15. There is no fixed delay that's safe against arbitrary
     system load. Rewritten as an explicit event-driven state machine
     that reacts to the real `itemUpdated` signal each step is actually
     waiting for (gated on a genuine progress tick — `bytesDone > 0` —
     specifically because `dispatchActiveItem()`'s own status update
     fires before the queued backend call has even run, so acting
     earlier than that risks racing `beginTransfer()`'s own state reset),
     with a single generous absolute-deadline `QTimer` as a safety net
     (not the primary mechanism) so a genuine bug breaking an expected
     transition fails fast and clearly instead of hanging the test (and
     the CI job) forever. Confirmed against the same deliberate
     CPU-contention setup that broke the old version: 40+ consecutive
     clean runs, runtime staying close to nominal rather than degrading.
     One more real subtlety caught while rewriting: `simulateFailure()`/
     `retryItem()` both synchronously (same-thread, direct connection)
     emit `itemUpdated` before returning, re-entering the state-machine
     lambda before the outer call completes — the state has to be
     advanced *before* triggering either, or the reentrant call sees the
     stale stage and the real transition it was waiting for never gets
     matched.
  **Manually run against two real, independent local SFTP servers**
  (`tools/local-test-servers/start-sftp-pubkey.sh`, twice, on different
  ports) via an earlier throwaway harness (not committed — this project's
  established pattern for one-off verification passes): confirmed a real
  20MB file's download half completing correctly over a real SSH
  connection with real chunked progress reporting, the phase transition to
  the upload half firing correctly against real I/O (`bytesDone` reset,
  `phase` flipped, matching the fake-backend test's own findings), and —
  in one run — a real mid-transfer cancel against a live server producing
  the correct `Cancelled` result. At the time, a full small-file transfer
  completing to `Done` end-to-end against two real servers in one clean
  run was **not** cleanly, repeatably confirmed — that throwaway harness
  had its own sequencing bugs, not a suspected defect in `TransferManager`.
  **Now closed** by a proper, committed, automated live-two-server
  harness: `verify-remote-to-remote-live`
  (`src/verify_remote_to_remote_live.cpp`, `EXCLUDE_FROM_ALL`, needs two
  `start-sftp-pubkey.sh` instances on different ports already running),
  mirroring `verify-sftp-pause-cancel`'s pattern — two real servers, a
  real end-to-end transfer between them, real content verified
  byte-for-byte on the destination server's own disk afterward. Fixed
  three real bugs along the way, all in the harness, not
  `TransferManager` (the file's own header comment has the full account):
  the two the throwaway version's notes already named (not waiting for
  BOTH panes' `connected` signal before calling `enqueue()`; a real
  destination conflict from both independent servers happening to have
  their own identically-named `sample.txt` fixture, which a uniquely-named
  fixture this harness creates itself avoids), plus a third found only
  once this version actually ran repeatedly: re-running the harness
  against the same already-running servers (a completely reasonable
  thing to do) left the *previous* run's completed transfer sitting at
  the destination, producing a real conflict prompt this headless `main()`
  had no way to dismiss — fixed by deleting any stale destination copy up
  front, making the harness safely idempotent across repeated runs
  instead of requiring a fresh server restart each time. Confirmed stable
  across multiple consecutive clean runs against the same live server
  pair after that fix.
  **A second scenario, added after Pause/resume was enabled for this
  direction, found and closed the general `resumeItem()`
  destination-conflict bug documented in `TransferManager`'s own entry
  above.** Pauses and resumes a real transfer during BOTH phases against
  the two real servers, using the same deterministic "trigger on the
  first real progress tick" technique `verify_sftp_pause_cancel.cpp`
  uses. Before the fix, this hung indefinitely at 90 seconds: resuming
  phase 2 (upload) found the destination already had this same item's
  own partial upload sitting there from before the pause, triggered a
  real `askConflict()` `QMessageBox` this headless harness has no way to
  dismiss, and the item never reached `Done`. After the fix: all pass,
  confirmed byte-for-byte content correctness surviving two real
  pause/resume cycles against genuine libssh2 I/O, stable across
  repeated runs against the same already-running server pair.
  **Known, accepted gap, not attempted here:** if the app closes while a
  `RemoteToRemote` item is still mid-flight, its temp file leaks for that
  run — `closeEvent()` tears down both panes' backends without
  `TransferManager` ever getting a completion/failure signal for whatever
  was still active. Partially mitigated: `TransferManager`'s constructor
  sweeps `zephyrftp-staging/` clean of anything left over from a
  *previous* run on startup, so this can't accumulate indefinitely, but a
  leak within the *same* run until the next launch is real and undefended
  against — correctly distinguishing "genuinely orphaned" from "a
  still-running transfer needs this a moment longer" would need more
  machinery than this pass attempts.
- **Server-side Move (a single-round-trip rename between two panes on the
  same connection) is now supported, covered by a deterministic
  fake-backend test, a direct real-`LocalBackend` check, AND real
  two-connection SFTP and FTP servers, including the specific
  directory-rename claim that used to be unconfirmed for both
  protocols.** `move-entry-test`
  (`src/move_entry_test.cpp`) covers `TransferManager::moveEligible()`'s
  `connectionIdentity()`-equality guard both ways (two backends reporting
  the same identity dispatch through `moveEntry()`; two reporting
  different identities are rejected with no item queued and no backend
  call made at all), that a single-file move calls the destination
  backend's `moveEntry()` exactly once (never the source backend's, and
  never `downloadFile()`/`uploadFile()`), and that a whole-folder move
  issues exactly one `moveEntry()` call against the folder's root path
  with `listDirectoryForEnumeration()` never called on either backend —
  confirming the tree genuinely relocates via one rename rather than
  `FolderEnumerator` silently still walking it unobserved. Separately,
  the same test drives a real `LocalBackend` against real temp files: a
  plain file move, a move onto an *existing* destination file (confirming
  it overwrites, unlike `renameEntry()`), and a folder move including its
  nested contents. **Also covers the one path this entry used to list as
  manual-only**: a folder move onto a destination that already exists,
  resolved as "Write Into," fails cleanly with an error mentioning the
  merge limitation rather than dispatching a doomed rename or silently
  doing nothing — driven via a REAL `QMessageBox`
  (`conflict-resolution-test`'s own technique: a timer scheduled during
  the dialog's still-blocking `exec()` call, which pumps the event loop
  internally, applied here as a continuous poller rather than a single
  fixed-delay shot, since this test doesn't know in advance exactly when
  the dialog will appear), not simulated or skipped. Confirmed stable
  under the same deliberate CPU-contention stress test (14 busy-loop
  processes) that broke an earlier fixed-delay design elsewhere in this
  project (see `remote-to-remote-test`'s own entry below) — 8+ clean
  runs plus 4 more under contention, all passing. **Also covers the
  three real bugs a code review found after this feature had already
  shipped** (see `TransferManager`'s own entry above for the full
  detail): a multi-select Move scenario (two `moveEntry()` calls fired
  synchronously back to back, reproducing `MainWindow::moveEntries()`'s
  loop exactly) confirming BOTH entries now get a real `TransferItem` and
  reach `Done`, not just the last one; a conflict-resolution isolation
  scenario confirming a Move's "apply to all, Write Into" choice from one
  conflict does NOT silently suppress the real conflict dialog for a
  second, unrelated Move conflict; and confirming `retryItem()` on the
  existing "Write Into fails" Failed item is a safe no-op rather than a
  misdispatch.
  `verify-sftp-move` (`src/verify_sftp_move.cpp`, `EXCLUDE_FROM_ALL`, needs
  `tools/local-test-servers/start-sftp-pubkey.sh` already running) closes
  the real-server gap: two independent `SftpBackend` connections to the
  SAME server — a real `connectionIdentity()` match discovered at
  runtime, not two fakes hardcoded to report a matching string — moving a
  real file and a real whole folder from the server's root into its
  `uploads/` subdirectory. Confirms, against a real server, exactly the
  claim that used to be unconfirmed: `libssh2_sftp_rename()` genuinely
  relocates a **directory**, not just a file (verified via the nested
  fixture — `a.txt`, two subdirectories including one genuinely empty one
  — all surviving intact at the new path). Made idempotent across
  repeated runs against the same already-running server after an early
  attempt found a real "stale destination from a previous run" conflict
  the headless harness had no way to dismiss (see the file's own header
  comment) — the same category of finding, not a coincidence, as the
  next entry's bug #3.
  `verify-ftp-move` (`src/verify_ftp_move.cpp`, `EXCLUDE_FROM_ALL`, needs
  `tools/local-test-servers/start-ftp.sh` already running) is the FTP
  counterpart, same shape and same result: two independent `FtpBackend`
  connections to the same server, confirming `RNFR`/`RNTO` genuinely
  relocates a directory over FTP specifically, not just SFTP. No
  `QThread` needed here — unlike `SftpBackend`'s blocking libssh2 calls,
  `FtpBackend` is fully async/event-driven (`QTcpSocket`), the same
  choice `verify-ftp-live` already made. The server's own
  `start-ftp.sh` fixture has no folder to move (just `sample.txt` +
  `uploads/`), so this harness creates its own nested "movetest" folder
  directly on the server's real filesystem, restored idempotently the
  same way as `verify-sftp-move`'s fixtures.
  **Not covered by any of these**: FTPS specifically (`FtpsMode::Explicit`)
  — `verify-ftp-move` only exercises plain FTP, though `connectionIdentity()`
  deliberately doesn't distinguish FTP from FTPS (see
  `FtpBackend::connectionIdentity()`'s own doc comment), so there's no
  reason to expect the rename call itself behaves differently once
  encrypted.
- **`SftpBackend`'s `listDirectoryForEnumeration()` and the full
  `FolderEnumerator` recursive walk are now confirmed against a real
  server, including the trickiest cases.** Same harness as the
  public-key auth entry below: `tools/local-test-servers/start-sftp-pubkey.sh`
  seeds a real multi-level tree on the server (mirroring
  `folder-transfer-test.cpp`'s local fixture exactly — nesting three
  levels deep, plus a genuinely empty leaf directory), and
  `verify_sftp_pubkey.cpp` runs a real `FolderEnumerator` — not just the
  bare primitive — against it over the actual `SftpBackend`. Confirms
  all 4 real files found (directories correctly excluded from that
  count), the file three levels deep found (the walk doesn't stop after
  one level), the genuinely empty directory included despite
  contributing zero files, and all 5 real directories found. Confirmed
  reliably across multiple repeated runs, including a from-scratch
  first-ever connection. **Not covered by this**: an enumeration failure
  partway through a walk against a real server (permission denied on a
  subdirectory, say) — only the successful-walk path has been tried.
- **`SftpBackend`'s real mid-transfer cancel and pause/resume are now
  confirmed against a real server, in both directions.**
  `verify_sftp_pause_cancel.cpp` (the `verify-sftp-pause-cancel`
  `EXCLUDE_FROM_ALL` target, same "not part of the required suite"
  reasoning as the other live-server harnesses) drives a real
  `SftpBackend` through a genuinely large (300MB) upload/download
  against `tools/local-test-servers/start-sftp-pubkey.sh`'s server,
  triggering `requestCancel()`/`requestPause()` — called as plain direct
  method calls, per `RemoteBackend`'s own thread-safety contract, not a
  queued slot — the instant real progress is seen (after exactly one
  480000-byte chunk, confirmed via the actual `bytesDone` reported, not
  assumed), a fast and deterministic trigger rather than a wall-clock
  guess. Confirms, for real: cancelling produces `transferFailed` with
  reason `"Cancelled"` and leaves a genuinely incomplete partial file on
  the server (smaller than the source, read directly off its disk);
  pausing produces `transferPaused` with a real nonzero `bytesDone`;
  resuming (`uploadFile()`/`downloadFile()` called again with that exact
  offset — the same contract `TransferManager` uses) picks up at or
  above that offset rather than restarting from zero; and the fully
  resumed transfer's final content matches the source byte-for-byte, in
  **both** directions (upload pause/resume, then download pause/resume
  of the same file) — not just inferred from a `transferFinished`
  signal. Confirmed reliably across multiple runs, including a
  from-scratch first-ever connection. This is what
  `transfer-pause-test`'s fake-backend orchestration test always
  correctly flagged as a different, narrower claim than "resuming a
  paused SFTP transfer actually picks up where it left off on a live
  server" — that broader claim is now proven too, not just the
  `TransferManager`-level orchestration around it. **Not covered by
  this**: cancel/pause of a transfer that's already very close to
  finishing (a race this harness deliberately avoids by design, since
  the whole point is triggering deterministically early), and
  pause/resume surviving a full app restart (the resume offset is only
  ever kept in memory between pause and resume within a single run, in
  both the real app and this harness).
  **A real bug found by code review in exactly that flagged-as-uncovered
  race, fixed, not yet independently re-verified against a live server.**
  `SftpBackend::downloadFile()`'s/`uploadFile()`'s read/write loops only
  checked `m_cancelRequested`/`m_pauseRequested` AFTER a successful chunk
  transfer, not on the loop's EOF-triggered `break` — so a cancel/pause
  requested in the exact narrow window right as the transfer legitimately
  finishes could be silently lost, with `transferFinished` firing instead
  of `transferFailed("Cancelled")`/`transferPaused`. Low-stakes for a
  single-phase transfer on its own (it just finished a moment before it
  would have anyway) but worse now that a `RemoteToRemote` item's
  `onBackendFinished()` reacts to phase-1 "finished" by immediately
  starting phase 2's upload — a lost cancel there means an entire unwanted
  upload phase runs too. Fixed by moving both checks to the top of each
  loop iteration, matching `FtpBackend`'s own equivalent loops, which
  already did this correctly (discovered while fixing `SftpBackend`'s
  version — a real, useful asymmetry to have noticed). Compiles and
  matches the established pattern; this specific narrow-window race
  hasn't been re-exercised against a real server the way the broader
  mid-transfer cancel/pause claims above have been.
- **The remaining ~1.6-1.8x gap to the ~40MB/s comparison
  (FileZilla/Termius/SMB) is now confirmed closed against a real,
  non-loopback server, not just unverified.** Three rounds of
  evidence-based tuning (pipelining, buffer alignment to libssh2's exact
  30000-byte packet size, `TCP_NODELAY`) each confirmed a real
  improvement on a real connection — ~4MB/s to ~18MB/s to ~22-25MB/s —
  but whether the remaining gap was a fixable lever or an inherent
  libssh2 ceiling stayed unknown because every local test server here
  was loopback (~0ms RTT), exactly the variable this gap is about. A new
  harness, `verify-sftp-throughput` (`src/verify_sftp_throughput.cpp`,
  env-var-configured since it needs a real externally-provided server
  this project can't spin up a container for), measured this app against
  a real server (~6-7ms RTT) alongside OpenSSH's own `scp` as a
  same-link, same-moment baseline — not a years-old number from a
  different network. Result, confirmed across two runs at two file
  sizes (100MB, 300MB): ZephyrFTP's `SftpBackend` reached ~29-31MB/s
  upload / ~37-39MB/s download, `scp` reached ~29-36MB/s / ~35-37MB/s on
  the identical link — a ~0.93-1.13x ratio, i.e. real parity with a
  native reference SFTP client, not a 1.6-1.8x shortfall. Since `scp` is
  itself libssh2-free, matching it directly is strong evidence there's
  no further fixable client-side bottleneck left in `SftpBackend`. The
  likeliest explanation for the original ~40MB/s comparison: it was
  FileZilla/Termius/SMB on a different network at a different time —
  SMB in particular is a different transport/protocol family entirely,
  so it was never a fully apples-to-apples number, and that network's
  real RTT/congestion was never controlled for. No code change made
  here — the honest result is "parity confirmed on a real link," and
  this gap no longer needs chasing further unless a future real-server
  measurement shows otherwise.
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
