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
  targets, not part of the main ten-target suite since they need those
  external servers already running) drive real `SftpBackend`/`FtpBackend`
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
- `FtpBackend` (`src/backends/FtpBackend.h/.cpp`) — FTP and explicit FTPS,
  hand-rolled directly on `QTcpSocket`/`QSslSocket`. Implements the full
  `RemoteBackend` interface and runs on a dedicated worker thread under
  the same threading contract as `SftpBackend`. **Reachable from the UI**
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
  unchanged for every server that accepts it. FTPS is explicit only
  (`AUTH TLS` upgrading an existing plaintext control connection).
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
  prompt, treated as suspicious rather than something to ask about. Data
  connections also attempt real TLS session-ticket reuse
  (`QSslConfiguration::sessionTicket()`/`setSessionTicket()`, refreshed on
  `newSessionTicketReceived()`) before their own handshake — a real
  best-effort attempt at the session reuse RFC 4217 permits servers to
  require, not a guarantee (see Known gaps for what is and isn't
  confirmed about it).
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
- `Protocol` / `ConnectionRequest` (`src/backends/Protocol.h`,
  `ConnectionRequest.h`) — the seam between "the user picked a protocol"
  and "construct the matching backend." `Protocol` is a three-value enum
  (Sftp/Ftp/Ftps) with small helpers hanging off it: the conventional
  port, the combo-box label, whether key auth applies, and stable string
  keys for JSON. `ConnectionRequest` pairs a `Protocol` tag with both
  credential structs, only the relevant one populated.
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
- `CredentialStore` (`src/backends/CredentialStore.h/.cpp`) — the OS
  credential store, opt-in, and the only place a secret from this app
  is ever written to disk. `save`/`load`/`remove`/`hasSecret`, keyed by
  a `SavedSite`'s `id`. Two platform backends behind `#ifdef _WIN32`
  (one file, not separate ones — the amount of platform-specific code
  is small enough that CMake source-list conditionals would be more
  ceremony than the split is worth): libsecret on Linux (the
  freedesktop Secret Service — GNOME Keyring, KWallet's compatibility
  layer, whichever the desktop provides), the real Win32 Credential
  Manager API (`wincred.h`, `CredWriteW`/`CredReadW`/`CredDeleteW`) on
  Windows. Deliberately NOT a bundled cross-platform wrapper library —
  Fedora ships `qtkeychain-qt6` for native Linux, but only a Qt5 build
  for the mingw64/Windows cross-target, a real ABI mismatch with this
  project's Qt6 Windows build — so this follows the same
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
  **Not covered by an automated test**: none of the five fixes above
  have regression coverage — `CredentialStore` writes to the real OS
  credential store with no test-friendly override the way `SiteStore`
  has `QStandardPaths::setTestModeEnabled()`, so an automated test would
  leave real test secrets in whoever's keyring runs it, the same
  category of risk this project avoids elsewhere (e.g. `site-store-test`'s own config
  isolation). Verified by direct code reading and reasoning through each
  call path instead, consistent with this project's "say so explicitly
  rather than letting it read as verified" standard where a real test
  isn't a safe option.
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
  `showTransfersOnStart`/`showCommandsOnStart` (both default true) are
  applied as an explicit override right after `restoreState()` runs in
  `MainWindow`'s constructor — deliberately overriding whatever dock
  visibility `restoreState()` itself just restored, since otherwise
  reopening a dock once (even by accident) would make the saved layout
  reopen it on every later launch too; turning the preference off is the
  only way to keep a dock closed permanently.
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
  across all three backends (folders first, then name descending, by the
  entry's real name rather than the later `"[folder]"`-wrapped display
  text) — previously only `LocalBackend` sorted its own results at all,
  making the "default" order silently backend-dependent. Sorting
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
- `TransferManager` — owns the transfer queue, processes it **serially**
  (one item at a time — SftpBackend holds a single libssh2 session, and
  concurrent transfers on the same session aren't safe without more
  synchronization than this app has yet). `enqueue()` figures out
  direction (local->remote / remote->local / local-copy / remote-to-remote)
  from each pane's `isLocalFilesystem()`, then dispatches to whichever
  backend actually owns the "remote" side of the operation.
  Reconnects its progress/finished/failed signal listeners to whichever
  backend is executing the current item — `RemoteBackend` objects persist
  across multiple transfers, so `connectToBackend()` explicitly disconnects
  the previous backend before wiring up the next to avoid stacking
  duplicate connections.
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
  indexing that broke the instant rows moved. **Deliberately not folded
  into "the ten-target suite"** CLAUDE.md/CONTRIBUTING.md already
  establish as a fixed, documented count — self-contained and
  `EXCLUDE_FROM_ALL` like the ten, but kept as an explicit eleventh,
  separately-documented target rather than triggering a rename sweep
  across every place that number appears; see CONTRIBUTING.md's "Running
  the test suites" section for how to build and run it.
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

## Windows and Linux builds (CI)

`.github/workflows/build.yml` produces both platforms' release binaries
and, on a `v*` tag, attaches both to the same GitHub Release. Both build
jobs actually run the full ten-target test suite as part of the job, not
just link it — matching CONTRIBUTING.md's "all ten need to actually
pass" rule for CI too, not only local development.

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

**Confirmed working end-to-end on GitHub's own runners**, not just
locally: both build jobs pass, the full test suite passes on both
platforms, and a real tagged release (`v0.2.0`) exercised the `release`
job for real — a GitHub Release with both `zephyrftp-windows-x64.zip`
and `zephyrftp-linux-x64.tar.gz` attached. The Windows `.exe` specifically
has also been run on real Windows hardware directly (not just under
wine), launches as a proper GUI app, and connects to a real SFTP server.

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

## Known gaps (flagged, not fixed)

- **FTP/FTPS has now actually touched a real server on every one of its
  code paths, not just the happy path — control connection, PASV AND
  active/PORT data connections, real transfers, the `AUTH TLS` upgrade,
  the LIST fallback, and a full encrypted transfer, all confirmed
  working, not just unit-tested in isolation.** `src/verify_ftp_live.cpp`
  (the `verify-ftp-live` `EXCLUDE_FROM_ALL` CMake target — not part of
  the ten-target self-contained suite, since it needs external servers
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
  (`src/ui/CertificateVerifier.h/.cpp`) mirrors `HostKeyVerifier` exactly:
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
- **FTPS against real vendor servers is now fully working end to end
  (vsftpd) — and confirmed, honestly, that this project's TLS
  session-ticket reuse does NOT satisfy a genuinely strict server
  (proftpd), plus three real client-side bugs found and fixed along the
  way that a headless, single-shot test harness was never going to
  surface.** The control connection's session ticket
  (`QSslConfiguration::sessionTicket()`, refreshed on
  `newSessionTicketReceived()` since TLS 1.3 tickets commonly arrive
  post-handshake) is applied to each data connection's
  `QSslConfiguration` before its own handshake — a real resumption
  attempt, not a comment, addressing the RFC 4217 anti-hijacking
  requirement some servers enforce. `verify-ftp-live`'s FTPS phases
  (pyftpdlib, which doesn't enforce or even check reuse either way) had
  been the only prior evidence, confirming non-regression but nothing
  about whether a strict server would actually accept it.

  `verify_ftp_vendors.cpp`'s two FTPS phases
  (`tools/local-test-servers/containers/vsftpd.conf`;
  `proftpd.conf`'s `mod_tls` left at its own default strict
  session-reuse enforcement — see that file's comment on why omitting
  `TLSOptions NoSessionReuseRequired` is what keeps it on) both confirm
  the AUTH TLS handshake and CA-signed-certificate trust genuinely work
  against a real vendor's TLS stack. What happened next, at the data
  connection, took real investigation (strace against live containers,
  an independent reference client, a deliberately controlled variable
  elimination — not guessed at):

  - **proftpd: a clean, consistent, real rejection — the actual answer
    to "does our reuse satisfy a strict server."** Its own `TLSLog`
    reports it directly — `"client did not reuse TLS session, rejecting
    data connection"` — meaning this project's TLS-1.3-ticket-based
    reuse does not read as genuine session reuse to `mod_tls`'s check
    (most likely a real protocol-generation gap: `mod_tls`'s
    reuse-detection predates TLS 1.3's PSK-based ticket resumption
    model and may simply not recognize it as equivalent to the
    session-ID-based reuse it was written to check for). A real,
    negative, genuinely informative answer — not previously known
    either way, and the container is deliberately left this way rather
    than relaxed so this stays confirmed on every future run.

    **The obvious fix was tried and falsified.** Forcing max TLS
    protocol version 1.2 on both the control and data `QSslSocket`s
    (`QSslConfiguration::setProtocol(QSsl::TlsV1_2)`) — a real-world
    workaround other FTPS clients use for exactly this class of
    strict-reuse rejection, on the theory that classic TLS 1.2
    session-ID/ticket resumption is what `mod_tls`'s check actually
    recognizes — does *not* work, and not for a subtle reason: with TLS
    1.2 forced, `QSslSocket::newSessionTicketReceived()` never fires at
    all. Confirmed directly (temporary debug instrumentation, since
    reverted) rather than assumed. That signal is TLS-1.3-only in Qt's
    public API — the RFC 8446 post-handshake `NewSessionTicket`/PSK
    mechanism specifically — and neither `QSslConfiguration` nor
    `QSslSocket` expose any equivalent for classic TLS 1.2 session-ID or
    RFC 5077 session-ticket resumption anywhere in Qt 6.11's public
    headers (`qsslsocket.h`, `qsslconfiguration.h` — checked directly,
    no `nativeHandle()`/`sslHandle()`-style escape hatch to the
    underlying OpenSSL `SSL*` either). So capping to TLS 1.2 doesn't
    trade an unrecognized-but-attempted resumption for a
    recognized-and-working one — it removes resumption entirely, since
    Qt gives the application no way to drive TLS 1.2's session
    cache/session-ID resumption at all. Confirmed via proftpd's own
    `TLSLog`: the control handshake correctly negotiates TLSv1.2, but
    the data connection is still rejected with the identical "client
    did not reuse TLS session" message. The change was fully reverted;
    `FtpBackend.cpp` is back to its original state. A genuine fix would
    require bypassing `QSslSocket` for the data connection's handshake
    entirely (raw OpenSSL socket/BIO plumbing to call
    `SSL_set_session()`/`SSL_get1_session()` directly) — a much larger
    change than a config tweak, not yet attempted, and worth weighing
    against just documenting this as a permanent limitation against
    reuse-strict servers.
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
  the ten-target suite" reasoning as the FTP one above) drives a real
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
  `EXCLUDE_FROM_ALL` target, same "not part of the ten-target suite"
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
