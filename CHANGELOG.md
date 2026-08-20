# Changelog

All notable changes to ZephyrFTP are documented here. Format loosely
follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/); this
project doesn't yet promise strict [Semantic Versioning](https://semver.org/)
guarantees — it's pre-1.0 (see [Known limitations](README.md#known-limitations)
in the README), so anything may still change between 0.x releases.

## [Unreleased]

### Changed (developer-facing only, no shipped behavior)

- **CMakeLists.txt: every test/verify target now links one shared
  `zephyrftp_core` `OBJECT` library instead of each enumerating its own
  subset of source files.** Requested directly ("ensure efficiency and
  speed without sacrificing accuracy" across the test suite). Confirmed
  the real cost before touching anything: CI's own "Build test suite"
  step took 6m06s versus 1m53s to actually *run* all 27 required
  tests — `MainWindow.cpp` was being compiled 14 separate times,
  `TransferManager.cpp` 34 times, `FilePaneWidget.cpp` 36 times, 1.4GB
  of object files for the test suite alone, because every target that
  named a shared source file triggered its own independent compile.
  `CMakeLists.txt` shrank from 1481 to 688 lines in the process — every
  target is now two lines (its entry-point `.cpp` + linking
  `zephyrftp_core`) instead of ~30. Locally, a full clean build of the
  app plus all 27 required targets now takes ~52s (was CI-measured at
  well over 6 minutes for the test-build step alone), and the test
  object directory shrank from 1.4GB to 144MB. Zero change to any
  test's logic, assertions, or runtime behavior — confirmed by running
  the complete 27-target required suite before and after: identical
  100% pass, and run timing byte-for-byte unchanged (both ~107.4s
  total), since nothing about how the tests execute changed, only how
  their shared source files get compiled. Also structurally closes a
  real, previously-documented AUTOMOC gotcha (a target referencing a
  `Q_OBJECT` header without also compiling its `.cpp` used to fail with
  a confusing link error) — every `Q_OBJECT` header is now always
  compiled together, once, before any target links it. See
  CONTRIBUTING.md's "The shared `zephyrftp_core` object library"
  section for how to add a new test target under this model.

## [0.8.4] — Fix dialog input fields being inconsistent widths across platforms

### Fixed

- **Dialog input fields were inconsistent widths across platforms** —
  reported directly from real hardware testing on all three: fields
  ran full dialog width on Windows, were narrowest on macOS, and
  varied on Linux depending on the system theme in use. Root cause:
  every dialog's `QFormLayout` (Connect, Site Manager, Preferences)
  left its field-growth behavior at the platform style's own default
  (`QStyle::SH_FormLayoutFieldGrowthPolicy`), which genuinely differs
  by native style — this app's QSS theme can't reach it at all, since
  it's a layout policy, not a paintable property, so the earlier
  dialog-consistency audit (v0.7.45, focused on QSS-level rendering)
  never would have caught it. Fixed by explicitly setting
  `QFormLayout::AllNonFixedFieldsGrow` on all 6 form layouts across the
  three dialogs, so every field is exactly as wide as its dialog on
  every platform — matching the full-width Windows behavior. Could not
  visually reproduce the platform difference in this environment
  (headless Linux `offscreen` rendering already defaults to full-width
  fields either way, confirmed with a byte-identical before/after
  screenshot) — verified at the code level instead (zero prior
  `setFieldGrowthPolicy` calls anywhere, confirmed via direct search)
  and via Qt's own documented per-style default mechanism, consistent
  with the direct cross-platform report. Full 27-target required suite
  passed.

## [0.8.3] — Fix v0.8.2's .rpm Packager fix, which didn't actually work

### Fixed

- **v0.8.2's `CPACK_RPM_PACKAGE_PACKAGER` fix for the .rpm's null
  Packager field didn't actually do anything** — confirmed directly
  against the real published v0.8.2 `.rpm`: `rpm -qp --queryformat
  "%{PACKAGER}"` still printed `(none)`. Root cause, found by reading
  CMake's own `CPackRPM.cmake` module source directly this time (not
  assumed from the variable's plausible-looking name): its built-in
  spec-file template has no `Packager:` line at all, for any
  RPM-generator variable whatsoever — `grep -ri packager` across the
  entire module finds zero matches. `CPACK_RPM_PACKAGE_VENDOR` (set
  alongside it, and genuinely working) only works because the template
  happens to already have a `Vendor:` line; nothing about Packager was
  ever wired up by CMake, so no value set for it could ever have shown
  up regardless. Real fix needs a user-supplied spec template
  (`CPACK_RPM_USER_BINARY_SPECFILE`) — new
  `resources/packaging/rpm-spec.in`, a byte-for-byte copy of that same
  built-in template with exactly one `Packager:` line added. Verified
  with a real local `.rpm` build this time (this environment now has
  `rpmbuild`, installed specifically to close this verification gap):
  `rpm -qip` on the real output shows `Packager: ZephyrFTP Maintainers
  <https://github.com/arelas/zephyrftp>`, with every other field
  (Vendor, Summary, Description, full file list) still intact — the
  hand-copied template didn't silently break anything else in the
  process.

## [0.8.2] — Fix missing publisher/version metadata in the .rpm, .dmg, and Windows .exe

### Fixed

- **Three packages/installers were missing real publisher metadata,
  found while checking a user report that the .rpm's author field
  showed null.** Confirmed each directly against the actual shipped
  v0.8.1 artifacts, not assumed: the `.rpm`'s Packager field really was
  `(none)` (`CPACK_RPM_PACKAGE_PACKAGER` was never set — unlike the
  `.deb`'s Maintainer field, RPM's generator doesn't fall back to
  `CPACK_PACKAGE_CONTACT` automatically); the macOS `.app`/`.dmg`'s
  `Info.plist` had an `NSHumanReadableCopyright` key present but
  genuinely empty (`MACOSX_BUNDLE_COPYRIGHT` was never set); and —the
  most significant gap — the actual `zephyrftp.exe` binary itself (both
  the portable `.zip` and inside the new installer) carried NO
  version-resource metadata at all: `CompanyName`/`FileDescription`/
  `LegalCopyright`/`ProductName`/`FileVersion` were absent from the
  compiled binary entirely, since `resources/app-icon.rc` only ever
  embedded the icon, never a `VERSIONINFO` block. Fixed by templating
  it (`app-icon.rc.in` + `configure_file()`) so the real version number
  and publisher info are substituted in from `PROJECT_VERSION` at
  configure time rather than hardcoded or left out. The `.deb`'s
  Maintainer field was already correct (confirmed, not assumed) and
  needed no change. Verified directly: rebuilt the Windows `.exe`
  locally and confirmed all the expected UTF-16LE-encoded strings
  (`CompanyName`, `FileDescription`, `LegalCopyright`, `ProductName`,
  `FileVersion`, the real version number) are now present in both the
  compiled `.res` object and the final linked binary. The RPM and
  macOS fixes couldn't be verified locally (no `rpmbuild`, no macOS
  toolchain in this environment) — first real verification is CI's own
  `build-linux-rpm`/`build-macos` jobs.

## [0.8.1] — Add a real Windows installer

### Added

- **A real Windows installer.** `zephyrftp-windows-x64-setup.exe`
  (NSIS) now ships alongside the existing portable `.zip` — installs to
  Program Files, adds a Start Menu shortcut and an optional Desktop
  shortcut, and registers a proper uninstaller in Add/Remove Programs.
  Packages the exact same `dist/` folder the `.zip` is built from, so
  it can't drift out of sync with `collect-win-runtime.sh`'s computed
  DLL closure. CI verifies a full silent install → launch → uninstall
  cycle under `wine`, not just that the installer builds — confirmed
  files, both shortcuts, and both registry keys are present after
  install and genuinely gone after `uninstall.exe /S`. **Not
  code-signed yet** — Windows SmartScreen will flag it until a signing
  path (SignPath Foundation or Azure Trusted Signing) is set up, see
  README.md's feature-freeze note. See CONTRIBUTING.md's "Building the
  Windows installer" section for the full local build/verify recipe,
  including a real Fedora packaging gotcha (`mingw64-nsis` alone isn't
  enough — `makensis` needs `mingw32-nsis`'s x86 stub too just to get
  past its own startup check, even though the actual output is amd64).

## [0.8.0] — First beta: feature freeze heading toward 1.0

### Changed

- **The project moves from alpha to beta, and enters feature freeze.**
  From this release forward, work heading toward 1.0 is bug fixes and
  small additions that close an existing, documented gap — not new
  capabilities or protocol/backend breadth. A signed, installer-based
  Windows build (replacing today's unzip-and-run `.zip`) is planned at
  some point during this phase. No functional/behavioral change in this
  release beyond the status update itself — the About dialog, README,
  and AppStream metadata now say "beta" instead of "alpha," and
  CONTRIBUTING.md documents the freeze for contributors.

## [0.7.45] — Fix dialog-consistency bugs found by a full audit

### Fixed

- A dialog-consistency audit across every dialog in the app (Connect,
  Site Manager, Preferences, Permissions, Compare Directories, Compare's
  delete-confirmation list) found and fixed three real, currently-
  shipping rendering bugs: Site Manager's "Simultaneous connections"
  spin box — the only spinner in the app that keeps its up/down
  buttons — rendered them as blank squares with no visible arrow glyph
  in either theme; Compare Directories' delete-confirmation file list
  showed a bright native-white background even in Dark theme; and
  Compare Directories' own Left/Right columns silently truncated their
  "<size>, <modified>" text with no visual hint — and was still using
  the old raw `yyyy-MM-ddThh:mm:ss` format, now matching the main file
  pane's own Modified-column fix (v0.7.43).

## [0.7.44] — Fix file pane column defaults

### Fixed

- The file pane's column layout gave Permissions (the last column) an
  unbounded, auto-stretched width — on most windows that made it, not
  Name, the widest column on open, and its header had no visible end:
  the blank space past it fell back to Qt's unstyled native header
  color instead of the app's own theme. Every column now gets an
  explicit, sane initial width, with Name always the widest on open;
  Permissions is now a genuine bounded column with a proper divider
  on its right edge, and the trailing space past it matches the
  pane's own background in both themes.

## [0.7.43] — Fix Modified column's date format and width

### Fixed

- The file pane's **Modified** column showed timestamps in raw ISO 8601
  form (`yyyy-MM-ddThh:mm:ss`) — now shown as `yyyy-MM-dd hh:mm:ss`
  (a space instead of the `T`). The column also now opens wide enough
  to show the full timestamp without needing to be dragged wider first.

## [0.7.42] — Drag files between the app and the OS file manager

### Added

- **Drag files between this app and your OS's own file manager**, not
  just between the two panes. Drag a file or folder from Nautilus,
  Explorer, or Finder onto a pane to upload or copy it in, with the same
  Overwrite/Skip and Write Into prompts as any other transfer. Dragging
  a file out of a local pane works immediately; dragging one out of a
  remote pane downloads it first (with a visible, cancellable progress
  dialog). Only individual files can be dragged out of a remote pane —
  see Known limitations.

## [0.7.41] — Clear Completed/Clear Failed on Transfers tab headers

### Added

- **Right-click a Transfers tab header for "Clear Completed"/"Clear
  Failed"** — clears just that tab, leaving the other alone. Active has
  no clear option (shown, but disabled): everything in it is still
  queued, running, paused, or waiting to reconnect, so it needs
  cancelling, not hiding.

## [0.7.40] — Keyboard shortcuts, fix default sort order

### Added

- **Keyboard shortcuts**: Delete to delete the selection, F2 to rename,
  F5 to refresh, Ctrl+A to select all, and Ctrl+C/Ctrl+V to copy a
  selection from one pane and transfer it into the other. Pasting into
  the same pane it was copied from isn't offered, and a paste is refused
  (with a clear message) if the source pane has navigated elsewhere
  since the copy.

### Fixed

- **The default file listing order was inconsistent with itself.** A
  fresh directory listing showed folders first, then files Z-A — but
  clicking the Name column header once flipped straight to A-Z, a
  jarring reversal on the very first click. The default now sorts
  folders first, then name A-Z, matching what clicking Name actually
  does.

## [0.7.39] — Quick connect password field, shorter protocol names, pane refresh button

### Added

- **Quick connect now has its own password field.** Fill in Host,
  Username, Password, Port, and Protocol, then press Enter or click
  Connect — it connects immediately, with no separate password prompt
  dialog in between.
- **A Refresh button on each file pane's toolbar**, right after Home —
  the same fresh re-listing the right-click Refresh menu item already
  did, now one click away without opening a menu.

### Changed

- **Protocol dropdown entries are shorter**: SFTP, FTP, FTPS (Explicit),
  FTPS (Implicit) — dropped the explainer text for SFTP and FTP, which
  don't need it (each is the only entry of its kind); kept
  (Explicit)/(Implicit) on FTPS, which does.

## [0.7.38] — Live progress during folder-transfer enumeration

### Fixed

- **"Preparing to transfer..." still looked frozen for a large folder,
  even after the previous fix.** The bigger, remaining cost was never
  the extra directory-creation check — it's the folder-tree walk
  itself, which lists one directory per real network round trip
  (SFTP/FTP), strictly one at a time, and the status bar showed the
  exact same unchanging text the whole time it did that. Now updates
  live with a running item count while the walk is in progress, so a
  slow tree reads as "still working" instead of "might be frozen."

## [0.7.37] — Fix folder transfers hanging on "Preparing to transfer"

### Fixed

- **A real performance regression from the previous crash fix: a folder
  transfer over SFTP (and equally, FTP/FTPS) could sit on "Preparing to
  transfer..." for several seconds before starting, for an entirely
  ordinary transfer into a brand-new destination folder.** The crash
  fix's `ignoreAlreadyExists` existence check was being paid for every
  single directory in the tree, unconditionally — a real network round
  trip each (`SftpBackend`: a `libssh2_sftp_stat()` call; `FtpBackend`:
  a full parent-directory listing) — even though a genuinely new
  top-level destination folder can't possibly already contain anything,
  making the check provably unnecessary in that case. Now only paid for
  an actual Write Into merge onto an existing folder, where it's
  actually needed; skipped entirely otherwise, same as before the crash
  fix.

## [0.7.36] — Recursive delete, with a warning

### Added

- **Recursive delete, with a warning.** Deleting a folder that turns
  out to have contents is no longer just refused — you get a second,
  content-aware warning ("isn't empty — it contains 12 files and 3
  folders. Deleting it will permanently delete everything inside...")
  and, only if you say yes, everything inside is deleted for real. The
  plain delete confirmation is unchanged for files and empty folders.

## [0.7.35] — Fix a Windows crash from Write Into onto an existing folder tree

### Fixed

- **A real Windows crash (stack overflow) after choosing Write Into for
  a folder that already has nested subdirectories at the destination —
  a completely normal case, since Write Into means "merge into
  whatever's already there".** Every already-existing nested directory
  popped its own "Create folder failed" dialog, and those dialogs could
  stack up unboundedly instead of showing one at a time, crashing the
  app. Fixed at the source (an already-existing directory during a
  Write Into merge is no longer treated as an error at all) and with a
  general safeguard against any other burst of dialogs stacking the
  same way.

## [0.7.33] — Fix Write Into/Overwrite asked separately for every top-level folder

### Fixed

- **Dropping several top-level folders at once could ask Write Into/
  Overwrite separately for each one, instead of honoring the first
  answer (including "apply to all") for the rest of the batch.** A real
  dialog-stacking reentrancy: the first folder's conflict dialog pumps
  the whole app's event loop while it's open, which could deliver a
  second folder's own conflict check before the first dialog was
  answered — popping its own independent dialog on top rather than
  waiting its turn.

## [0.7.31] — Fix Transfers/Commands panes vanishing after minimize on Windows

### Fixed

- **Windows: the Transfers and Commands panes went missing after
  restoring the window from minimized, and re-enabling them via the
  View menu didn't stick.** A feedback loop in the View menu's
  Transfers/Commands toggles converted the transient, harmless hiding
  that happens on minimize into a permanent one that restoring the
  window couldn't undo.

## [0.7.30] — Implicit FTPS

### Added

- **Implicit FTPS.** A fourth protocol choice, alongside SFTP/FTP/FTPS,
  in the Connect... dialog and Site Manager — TLS from the very first
  byte of the control connection (no `AUTH TLS` command, no plaintext
  phase at all), conventionally on port 990, for the older servers that
  only offer that mode rather than explicit FTPS. Not reachable via
  Quick Connect's URL scheme — `ftps://` still means explicit FTPS only;
  use Connect... or Site Manager for implicit.

### Fixed

- Site Manager's port field never auto-updated when switching the
  protocol dropdown on a new, unsaved site — unlike the Connect...
  dialog, which already did this. Pre-existing for FTP/FTPS too, but
  went unnoticed since every existing protocol's default port (21 or
  22) was close enough to fail quietly; implicit FTPS's very different
  990 made it worth fixing now.

## [0.7.29] — Recent Connections

### Added

- **Recent Connections.** Each pane's Sites/Connect/Disconnect menu
  (the icon at the left of the path bar) now has a Recent Connections
  submenu listing the last 10 servers you connected to ad-hoc (Quick
  Connect or the plain Connect... dialog) without saving them as a
  site — pick one to reconnect instantly, prompted for the password or
  key passphrase again just like any first connection. Nothing is ever
  stored on disk for these — stricter than Site Manager's own opt-in
  password saving. Sites already saved in Site Manager aren't
  duplicated into this list.

## [0.7.28] — Fix a clipped progress dialog label

### Fixed

- The "Verifying..." progress dialog shown while a checksum
  verification is running (0.7.27) clipped its own filename label at
  the dialog's right edge, with no margin. Cosmetic only — verification
  itself was unaffected.

## [0.7.27] — Transfer checksum verification

### Added

- **Verify a completed transfer's checksum.** Right-click a finished
  item in the transfer queue and choose Verify Checksum... to confirm
  it matches on both ends (SHA-256). Manual and on-demand, not
  automatic — for anything other than a local-to-local copy, verifying
  means genuinely re-reading the remote file a second time over the
  network (neither SFTP nor FTP offer a usable server-side hash), so
  this only runs when you ask for a specific file, never silently in
  the background. Not offered for server-to-server transfers.

## [0.7.26] — Fix a compressed field in Site Manager at its default size

### Fixed

- The "Starting directory" field in Site Manager rendered compressed
  at the dialog's default size (fine once you resized the window
  larger). The dialog's fixed default size hadn't been grown to
  account for the "Simultaneous connections" row added in 0.7.24, so
  Qt's layout engine squeezed this one field to make everything else
  fit. Grown with real headroom this time, so the next field added to
  this dialog doesn't reopen the same bug.

## [0.7.25] — Fix a visual height mismatch in Site Manager

### Fixed

- The new "Simultaneous connections" field in Site Manager (added in
  0.7.24) rendered visibly taller than every other field in the form.
  Cosmetic only — the value itself was always correct.

## [0.7.24] — Simultaneous connections per site

### Added

- **A site can now open multiple connections to the same server**
  (Site Manager > Simultaneous connections, 1-10, default 1 — existing
  sites are unaffected unless you raise it). Previously, every file in
  a batch transfer serialized through one connection no matter how many
  were queued; this is the same gap FileZilla's and WinSCP's own
  "simultaneous connections" setting exists to close. Connections are
  opened lazily, only as actually needed, using the same credentials as
  the main connection (no extra password or host-key prompts).

### Fixed

- A rare local-only race in the SFTP host-key trust file could, in
  principle, cause two connections to the same never-before-seen server
  starting at nearly the same moment to silently drop one of their
  saved host keys. The FTPS equivalent (the trusted-certificate file)
  had the same latent gap and is now protected the same way — worth
  fixing now that opening several connections to one server at once is
  a supported, common case instead of a rare coincidence.

## [0.7.23] — Fix a real freeze when sorting the transfer queue mid-transfer

### Fixed

- **Clicking a column header to sort the transfer queue while a
  transfer was running could freeze the app**, worse the larger the
  queue. Root cause: rebuilding the sorted table re-inserted every row
  one at a time, which is cheap on a hidden table but pays for real
  layout/paint work once the table is actually visible (as it always
  is in the real app) — measured directly, a 1500-item rebuild went
  from 89ms hidden to over a full second once shown, and got worse
  than linearly slower as the queue grew. Fixed by resizing the table
  once instead of growing it row by row; the same rebuild now takes
  well under 200ms. Also stopped needlessly recomputing a progress
  bar's color on every single progress update when it hadn't actually
  changed.

## [0.7.22] — Fix doubled border on the new Transfers tabs

### Fixed

- **The Transfers dock's new Active/Completed/Failed tabs (v0.7.21)
  showed what looked like a doubled border, in both themes.** The app
  had no styling at all for `QTabWidget`/`QTabBar` (it's the only tab
  widget in the app), so the tab bar fell back to the platform style's
  default light appearance sitting directly above the dark table below
  it. Fixed with theme-matched tab styling in both `theme.qss` and
  `theme-light.qss`. A second, related gap surfaced while fixing this:
  `QDockWidget` itself had no background color of its own (only its
  title bar did), which was invisible before this tab bar existed
  because the queue table always painted edge-to-edge; the tab bar
  doesn't extend past its own tabs, briefly exposing that gap as a
  stray light strip until this was also fixed.

## [0.7.21] — Split the transfer queue into Active/Completed/Failed tabs

### Added

- **The transfer queue is now split into three tabs — Active,
  Completed, and Failed — instead of one flat table.** Watching a
  batch finish, or finding a failed item to retry, used to mean
  scrolling or sorting through everything else still queued or already
  done to find it. Items move between tabs live as their status
  changes — most visibly, retrying a Failed item moves it straight
  back to the Active tab the instant the retry re-queues it. The
  Failed tab always contains exactly the items `TransferManager`'s own
  Retry action accepts (Failed, Cancelled, or Skipped), so everything
  shown there is genuinely retry-eligible.

### Fixed

- **`conflict-resolution-test` failed consistently on repeated local
  runs (always passed on CI).** It never wiped its own `/tmp/conflict_test`
  scratch directory before running, unlike its sibling tests. A stale
  file left behind by an earlier run caused a real (but test-unhandled)
  second conflict dialog to open, which a later phase's automated click
  ended up hitting instead of the dialog it was actually meant for.
  Fixed by wiping that directory at the start of each run, matching the
  convention its sibling tests already use.

## [0.7.20] — Fix GUI freeze during local file transfers

### Fixed

- **The app still froze during a large transfer (window couldn't be
  moved or interacted with) even after v0.7.19's dispatch-overhead
  fixes.** Root cause: `LocalBackend::downloadFile()`/`uploadFile()`
  called `QFile::copy()` directly, inline, on the GUI thread —
  `QFile::copy()` is a single, uninterruptible OS call with no
  chunking or yield points, so it blocked the entire GUI thread (the
  same thread that handles window moves, repaints, and input) for its
  full duration. The actual copy now runs on a background thread-pool
  task instead; every other `LocalBackend` operation (listing, delete,
  rename, etc.) is unchanged, since those were never the problem.
  Verified concretely: a direct call to `downloadFile()` now returns
  before the transfer completes, and the event loop keeps responding
  while a 50MB copy is still in flight.

## [0.7.19] — Fix remaining large-transfer hang + scroll bars

### Fixed

- **The app could still hang on a large transfer even without ever
  sorting the Transfers panel** (v0.7.18 only fixed the sorted case).
  Root-caused to three separate O(N)-or-worse-per-event costs:
  `TransferQueueWidget::rowForId()`'s linear scan on every progress
  tick, `TransferManager::startNext()` always rescanning from index 0
  on an ever-growing list, and — the dominant one — `enqueue()` calling
  a full dispatch scan unconditionally for every new item even when
  every backend was already known to be busy. That last one alone made
  queuing 15,000 files take over 10 seconds before any real transfer
  work even started. All three fixed; the same 15,000-file scenario now
  completes start to finish in well under 10 seconds total.
- **Commands and Transfers panel vertical scroll bars didn't have a
  real, grabbable size** in either theme — the QSS gave the scroll bar
  no explicit width/height and no minimum handle size, both real gaps
  once any custom scroll bar styling is in play.

## [0.7.18] — Fix runaway memory/hang on large batch transfers

### Fixed

- **Transferring a large number of files while the Transfers panel was
  sorted (any column header clicked) could hang the app and consume
  memory at a runaway rate, sometimes crashing outright** — reported
  live on Windows, matching a Windows Error Reporting
  `RADAR_PRE_LEAK_64` / "stopped interacting with Windows" hang.
  Root cause: adding a queued item while a sort was active triggered a
  synchronous full rebuild of the entire Transfers table (every row's
  cells *and* widgets recreated from scratch) — enqueueing N files in
  one batch (any folder transfer or multi-select drag-drop) fired this
  once per file, for O(N²) total widget churn. A few thousand files was
  enough to hang the GUI thread and exhaust memory. Fixed by coalescing
  every addition within one synchronous batch into a single deferred
  rebuild instead of one per file — confirmed directly with a
  before/after control: the old code didn't finish a 1,500-file test
  batch in 90 seconds; the fix completes the same batch in ~200ms.

## [0.7.15] — UI cosmetics pass, part 4

### Changed

- **Site Manager's Group dropdown "(none)" option** is now a real,
  visibly-labeled row instead of a blank string relying solely on
  placeholder text — the dropdown popup previously showed a genuinely
  empty first row. Selecting it still persists as ungrouped, not a
  literal "(none)" group.
- **Quick connect toolbar field order**: Host now leads before Username
  (previously Username, Host) — final order is Host, Username, Port,
  Protocol, Connect.

## [0.7.14] — UI cosmetics pass, part 3

### Changed

- **Site Manager's Group dropdown, Authentication radios, and starting-
  directory options were invisible in light mode.** The Group field is
  this app's only *editable* combo box, which draws its closed-state
  text through a separate child `QLineEdit` the general combo-box
  styling never reached; `QRadioButton` (Password/Private key, Home/
  Specific directory) had no styling at all, same class of bug as the
  checkbox fix in the previous release. Both fixed in `theme.qss`/
  `theme-light.qss`.
- **Disabled fields now render in a legible, theme-consistent muted
  color** instead of falling back to Qt's own native disabled palette —
  the starting-directory field (disabled while "Home directory" is
  selected) was the specific reported case, but this applies to every
  disabled text field, combo, radio, checkbox, and button.
- **Quick connect toolbar field order**: protocol now sits right before
  the Connect button instead of leading the row.
- **File pane navigation button contrast increased further**: icons
  enlarged (20→24px), and `IconTheme::tintedIcon()` now generates an
  explicit, readable disabled-state icon instead of relying on Qt's own
  auto-desaturation, which was washing the disabled Back/Forward icons
  out to near-invisibility.

## [0.7.13] — UI cosmetics pass, part 2

### Changed

- **Quick connect toolbar redesigned.** Replaced the single free-text
  `[protocol://][user@]host[:port]` field with separate protocol
  dropdown, Username, Host, and Port fields plus an explicit Connect
  button — the old syntax wasn't discoverable.
- **Checkbox and dropdown text visibility in light theme.** Checkbox
  labels ("Show hidden files", "Save password") and combo-box dropdown
  popups had no explicit styling in either theme and fell back to
  native/system palette rendering — invisible-looking text in light
  mode specifically. Both now styled explicitly in `theme.qss` and
  `theme-light.qss`.
- **Bandwidth limit unit clarified.** "Limit per transfer" now always
  shows "(KB/s)" in its own row label — the spin box's unit suffix
  disappeared entirely at the default "Unlimited" value, leaving no
  indication of what unit a value would even be in.
- **File pane navigation buttons** (Back/Forward/Up/Home) now use the
  app's own documented "primary/navigation" blue accent instead of
  plain gray, for better contrast in both themes.
- **Compare Directories' Name column** now starts wide enough to show
  Status/Left/Right without needing to scroll.

## [0.7.12] — UI cosmetics pass

### Changed

- **Quick connect has its own toolbar**, docked directly under the main
  icon toolbar instead of sharing it — the View menu's "Quick Connect
  Field" toggle now shows/hides that whole row.
- **View menu is plain checks, no icons.** Transfers/Commands previously
  reused the toolbar's own `toggleViewAction()`, which meant its icon
  bled into the menu too (one shared `QAction`, visible everywhere it's
  added) — each now has its own menu entry, kept in sync with the dock's
  visibility both directions.
- **Menu bar is now a View-menu toggle**, with a hold-Alt-to-reveal
  fallback when it's hidden — otherwise there'd be no way back to the
  very toggle that hid it.
- **Light theme contrast pass.** The menu bar previously had no styling
  of its own in either theme (inherited a mismatched color/background
  combination from `QMainWindow` that happened to look fine on dark, but
  produced invisible text on light) — both `theme.qss` and
  `theme-light.qss` now style it explicitly. Light theme's neutral
  icon/text colors were also measurably too pale against a white
  background (as low as ~2.9:1 contrast) and are darkened.
- **Preferences dialog**: real divider lines ahead of the Proxy/Bandwidth
  sections, increment arrows removed from the port and bandwidth-limit
  fields (typed values, not nudged), and the confusing standalone "Local
  & remote panes:" label next to the Show Hidden Files checkbox is gone
  — the checkbox's own text now stands alone.
- **Site Manager**: New Site/Duplicate/Delete/Export/Import buttons are
  now a normalized, consistent width instead of each sizing to its own
  label length.

## [0.7.11] — Light theme

### Added

- **Light theme.** A new "Theme: Dark/Light" choice in Preferences,
  switching immediately with no restart required — the app-wide
  stylesheet, every toolbar/pane icon, and the transfer queue's status
  colors all re-render in place. Dark stays the default; accent colors
  (blue/green/red/amber) are unchanged between the two, only the
  backgrounds/borders/neutral text actually differ.

## [0.7.10] — Scripting/automation (CLI mode)

### Added

- **`--script=<path>` command-line flag.** Runs a plain-text, line-oriented
  automation script non-interactively and exits with a status code — no
  window is ever shown. Commands: `open <site-name>`, `cd`/`lcd`,
  `get`/`put`, `ls`/`lls`, `rm`, `mkdir`, `mv`, `mirror <local> <remote>
  [--delete]` (one-directional sync built on Compare-and-Sync's diff
  engine), `echo`, `exit`/`bye`. `open` only ever connects to a site
  that's already trusted (connected once via the GUI) and has a stored
  credential — scripts never prompt, and fail immediately with a clear
  message rather than hanging if either isn't true. See the README's new
  "Scripting" section for the full command reference.

## [0.7.9] — Directory Compare-and-Sync

### Added

- **Compare Directories...** (View menu). Recursively diffs the two
  panes' current directories and shows the full tree with a status per
  item (Only in Left / Only in Right / Differs / Identical, by size and
  modified time — no content hashing), with checkboxes for bulk "Copy
  Selected to Right/Left". Deletion of files not present at the source
  is a separate, opt-in, off-by-default action — itemized in a dedicated
  confirmation step before anything is removed, and implemented as many
  individual, ordinary (non-recursive) deletes rather than any new
  recursive-delete capability. Independent of Synchronized Browsing;
  works for two local panes too.

## [0.7.8] — Fix a cross-connection destination race

### Fixed

- **A rare double-dispatch/silent-clobber risk when both panes connect
  to the same server.** Two transfers to the identical destination
  path, dispatched via two different connections to that server (or
  two different local-filesystem transfers to the same local path),
  could both pass the destination-conflict check and silently overwrite
  each other, since nothing coordinated between separate connection/
  backend instances even when they were the same actual destination.

## [0.7.7] — Internal cleanup, no user-visible changes

### Changed (developer-facing only, no shipped behavior)

- **Four cleanup-grade findings from the code review behind v0.7.6,
  fixed with no user-visible behavior change:** `TransferManager`'s
  dispatch-time backend selection is now derived from
  `requiredBackendsForDispatch()` itself instead of independently
  re-picking a backend per direction in a second `switch` that had to
  be kept in sync with it by hand; the five independent, byte-for-byte
  identical copies of a `joinPath()` helper scattered across
  `FolderEnumerator.cpp`/`EditSessionManager.cpp`/`TransferManager.cpp`/
  `FilePaneWidget.cpp`/`MainWindow.cpp` are now one shared
  `src/PathUtils.h`; Site Manager caches each site's
  "has a stored password" check for the rest of a dialog session instead
  of querying the OS credential store fresh on every single tree click;
  and `SftpBackend`/`FtpBackend`'s upload/download loops build their
  pause/cancel check closure once per transfer instead of once per
  chunk.

## [0.7.6] — Concurrency and synchronized-browsing bug fixes

### Fixed

- **Synchronized Browsing could drive the other pane into the wrong
  directory.** The anchored-subtree check was a raw text-prefix match
  with no path-separator boundary, so a sibling directory that merely
  shared the anchor as a text prefix (e.g. anchor `/data` matching
  `/data2/photos`) was wrongly treated as "inside" the anchored
  subtree, computing a bogus relative path and navigating the other
  pane somewhere the user never intended.
- **A rare crash/data-corruption risk in the destination-conflict
  dialog under concurrent transfers**, confirmed via AddressSanitizer
  as a genuine heap-use-after-free. The conflict dialog's handler held
  a reference into the transfer queue across the dialog's own nested
  event loop; with real concurrent transfers, another in-flight
  conflict response could append a new item to the queue while that
  dialog was still open, reallocating the queue's backing storage out
  from under the held reference.
- **Resuming a paused server-to-server transfer could silently stall**
  if it had been paused mid-upload and something else was using the
  source server's connection — the resume wrongly waited for that
  unrelated connection to free up, even though the resume itself no
  longer needed it.
- **A rare double-dispatch risk on the same server connection** if a
  pane's connection changed while a transfer to it was still waiting
  on a destination-conflict check — could let two transfers run against
  the same connection at once, confirmed via a dedicated test as a
  genuine race, not just a theoretical one.
- **Merging into an existing folder ("Write Into") always showed a
  confusing "Create folder failed... Something with that name already
  exists" error dialog**, even though the merge itself completed
  successfully. The folder's own root was always re-created as part of
  the merge, which was always guaranteed to fail since Write Into only
  ever applies to a folder that's already there.

## [0.7.5] — Windows build is back

### Fixed

- **Windows build was missing from the last two releases (0.7.3,
  0.7.4).** A CI infrastructure issue caused the Windows build to fail
  its test suite unpredictably, so it was left out rather than ship
  something unverified. Root-caused and fixed; the Windows `.zip` is
  included in this release and will be going forward.

## [0.7.4] — macOS packaging fix, take two

### Fixed

- **macOS build still crashed on launch after 0.7.3's fix.** That fix
  correctly rewrote the app's internal library references, but the
  actual library files themselves still didn't make it into the
  shipped `.dmg` due to a packaging step silently dropping them after
  the fact. Fixed by changing how the `.dmg` itself gets built, plus
  an added check that verifies the shipped `.dmg` genuinely contains
  what it's supposed to, not just that the app's internal references
  look correct.

## [0.7.3] — Synchronized browsing

### Added

- **Synchronized browsing.** A new "Synchronized Browsing" toggle in
  the View menu (off by default) — once on, navigating one pane (Back,
  Forward, Up, Home, the path bar, double-clicking into a folder) drives
  the other pane to the same relative path automatically. If the other
  side doesn't have a matching subfolder, that pane just shows its
  ordinary "couldn't open that folder" message — nothing moves there,
  and the pane you actually navigated is never affected by what happens
  on the other side. Reconnecting either pane while this is on turns it
  back off automatically, rather than continuing to track against
  wherever the old connection used to be.

### Fixed

- **macOS build crashed on launch on any machine other than the one
  that built it.** Every released `.dmg` through 0.7.2 packaged the
  app without bundling its Qt/libssh2/openssl libraries, so it only
  ever ran on the CI runner itself. Fixed by properly deploying those
  libraries into the app bundle before packaging.

## [0.7.2] — Bandwidth throttling

### Added

- **Bandwidth limit per transfer.** A new "Limit per transfer" field in
  Preferences (KB/s, blank/0 means unlimited, the default) — every
  SFTP/FTP transfer paces itself down to that rate. Per-transfer, not
  one shared cap across every transfer at once: two transfers running
  at the same time (see [0.7.1]'s concurrent transfers) can together use
  up to roughly twice the configured number, since neither knows about
  the other — the simpler of two ways to do this, chosen deliberately.
  Local-to-local copies aren't throttled (nothing to pace — a local copy
  is one uninterruptible OS call, the same reason it can't be paused
  either). Takes effect on the next connection made after changing it,
  not retroactively on one already open.

## [0.7.1] — Concurrent transfers

### Changed

- **Transfers now run concurrently when they don't conflict.** Two
  transfers that each involve a different connection — e.g. uploading
  from the left pane while downloading on the right — now run at the
  same time instead of one waiting for the other. Two transfers that
  would use the *same* connection still run one after another, as
  before: a single SFTP/FTP session can't safely do two things at once,
  so that part is unchanged. Nothing to turn on — it's automatic, and
  never runs more transfers at once than your panes actually have
  distinct connections open.

## [0.7.0] — macOS build

### Added

- **macOS build (Apple Silicon only).** A native `.dmg`, built and
  tested on a real macOS CI runner — drag ZephyrFTP into Applications
  like any other Mac app. Unsigned/unnotarized (no paid Apple Developer
  account behind this project), so the first launch needs a right-click
  → Open past Gatekeeper's warning; no Intel Mac build yet. Saved
  passwords now use real macOS Keychain Services via a new
  `CredentialStore` backend, alongside the existing Windows Credential
  Manager and Linux libsecret ones — same opt-in "Save password"
  behavior, same guarantee that `sites.json` never contains a secret.

## [0.6.29] — Import/export saved sites

### Added

- **Import/export saved sites.** Export... in Site Manager writes your
  whole site list to a JSON file — safe to hand to someone else or move
  to another machine, since it's the same secret-free format
  `sites.json` itself already is (no password is ever in it, saved or
  not). Import... reads one back in and adds those sites to what you
  already have, never replacing it; each imported site gets a fresh
  identity, so you're just prompted for its password the first time
  you connect, same as any new site. Export always exports every saved
  site — no per-folder/selection scoping yet.

## [0.6.28] — Filter by name

### Added

- **Filter by name.** A field under each pane's path bar narrows a busy
  directory listing as you type — case-insensitive, matches anywhere in
  the name. Each pane's filter is independent; hide the field via View →
  Filename Filter if you don't want it. Simple substring match only (no
  wildcards or regular expressions), and only whether the field is shown
  is remembered across restarts, not what was typed into it.

## [0.6.27] — Quick connect

### Added

- **Quick connect.** A toolbar field for a fast one-off connection:
  type `[protocol://][user@]host[:port]` and press Enter — you're
  prompted for the password right there, same as Site Manager's own
  Connect button. No Connect dialog to open first. Hide it via View →
  Quick Connect Field if you don't want it. Password-only (no
  private-key auth) and always connects the right-hand pane, matching
  the toolbar's existing Connect... shortcut.

## [0.6.26] — Proxy support (SOCKS5 / HTTP CONNECT)

### Added

- **Proxy support (SOCKS5 or HTTP CONNECT).** Set one in Preferences
  and it applies to every SFTP/FTP/FTPS connection — no per-site setup.
  Previously there was no way to connect through a proxy at all, which
  meant anyone behind a corporate network requiring one couldn't use
  this app. A proxy password, if needed, gets the same OS-credential-
  store treatment as a saved site's password. One global setting, not
  per-site; an FTP/FTPS data connection is only proxied in the normal
  (passive-mode) case — the rare active-mode fallback can't be, on any
  client, proxy or not.

## [0.6.25] — Chmod / permissions editing

### Added

- **Change permissions.** Right-click a file or folder — local, SFTP,
  or FTP — and choose Permissions... for a checkbox grid of the
  standard Unix owner/group/other read/write/execute bits, with a live
  octal readout. Works via `chmod()` locally, SFTP's real `setstat`,
  and FTP's `SITE CHMOD` (a widely-supported but non-standard
  extension — a server that doesn't implement it says so rather than
  silently no-op'ing). Verified against real vsftpd and proftpd
  servers, not just this project's own stand-ins. Single file/folder
  at a time, and just the standard 9 bits — no multi-select, no
  recursive apply, no setuid/setgid/sticky.

## [0.6.24] — Transfer queue persistence

### Added

- **The transfer queue now survives closing the app.** Anything still
  queued, paused, or mid-transfer when you quit is there again next
  time you launch. A transfer to/from a server shows "Waiting to
  reconnect" until you reconnect to that same server, then picks back
  up automatically from wherever it left off — never before, and never
  with a password remembered or a connection attempted on the app's own
  initiative. Only a normal quit is covered (a crash still loses the
  queue), and server-to-server transfers aren't restored (see Known
  limitations in the README for the full, honest boundary).

## [0.6.23] — Edit-in-place, and a toolbar worth having

### Changed

- **The toolbar has four new icons**: the View menu's Transfers/Commands
  toggles (the same checkable actions the menu already used — toggling
  either from the toolbar now visibly highlights while its dock is
  shown), Preferences, and About. Toolbar group dividers are slightly
  brighter now too, so the new, busier icon groups still read as
  distinct at a glance.

### Added

- **Edit-in-place.** Right-click a file on a remote pane and choose Edit
  to download it to a temporary local copy, open it in your editor of
  choice, and have every save quietly re-uploaded back to the server in
  the background. Re-opening a file that's already being edited just
  brings your editor back to the front instead of downloading a second
  copy. Shows up in the Transfers pane like any other transfer; a save
  that can't be uploaded (connection dropped, say) shows a clear warning
  with a Retry, rather than silently losing the edit — it stays safe on
  disk either way. A new "External editor command" field in Preferences
  configures which editor to use; left blank, your system's own default
  application for that file type opens instead.

## [0.6.22] — View menu toggles are now the persisted dock-visibility source of truth

### Changed

- **Removed the "Show Transfers/Commands pane on start" checkboxes from
  Preferences.** The View menu's Transfers/Commands toggles (and each
  dock's own titlebar close button) are now the sole source of truth
  for whether a dock reopens on the next launch — closing one and
  restarting the app now genuinely remembers that, the same way window
  size/position already does. Previously, that separate Preferences
  checkbox silently overrode whatever the live toggle actually did,
  so closing a dock via the View menu had no lasting effect unless the
  matching checkbox was also unchecked by hand.

## [0.6.21] — Double-click to connect; fix dropdown-button corners

### Added

- **Double-clicking a saved site in the Site Manager now connects**,
  same as selecting it and clicking Connect — a shortcut every similar
  app offers. Double-clicking a folder item is excluded on purpose: it
  has no site behind it, so it isn't treated as "connect."

### Fixed

- **Dropdown buttons (`QComboBox`) in the Site Manager and Connect to
  Server dialogs now have rounded corners matching the input boxes
  they sit in.** The default style drew the arrow subcontrol as its
  own square-cornered panel, which visibly notched the two right-hand
  corners of an otherwise-rounded box. Fixed by taking over that
  subcontrol's styling entirely (removing its border, supplying a
  small muted chevron glyph in its place) rather than leaving it to
  the platform style's default painting.

## [0.6.20] — Transfers panel columns are now structurally conserved

### Changed

- **The Transfers panel's column layout is now properly conserved,
  not just capped.** Direction/Status/Progress/Speed no longer resize
  at all — their content is short and fixed-shape (a small icon, a
  status word, a progress bar, a speed reading), so there was nothing
  to gain from letting them be dragged, only a route back to columns
  crowding each other off screen. File is now the sole flexible
  column: it absorbs all extra space when the window is wide, and is
  the one column that shrinks first when the window narrows — the
  other four stay put either way. This replaces the per-column
  maximum-width clamps from the last two releases with something
  structurally simpler: with only one resizable column and nothing
  else draggable, there's no "columns pushing each other off screen"
  class of bug left to have.
- **The Commands pane's new bottom-of-pane element is a plain divider
  line now, not a status bar.** The line-count text it briefly had
  read as more chrome than this particular pane actually needed —
  there's no ongoing summary worth showing the way the file panes'
  own item-count footer has.

## [0.6.19] — Polish pass on navigation order and column headers

### Changed

- **File panes' navigation order is now Back, Forward, Up, Home** — the
  Home button (added last release) moves from between Forward and Up to
  the rightmost position.
- **The Commands pane now ends in a small footer line** ("N line(s)",
  or "No output yet" when empty) instead of stopping abruptly at the
  log's own edge — the same footer-line treatment every other pane
  already has.
- **Column headers are less tall** — vertical padding trimmed from 5px
  to 3px on each side.

### Fixed

- **The Transfers panel's Direction/Status/Progress columns had the
  identical off-screen-dragging bug File did, missed by the previous
  fix.** That fix capped the whole header to one maximum width, which
  stopped any SINGLE column from getting too wide — but Direction,
  Status, and Progress could each still grow to that same generous
  cap independently, and their combined width could still push later
  columns unreachably off screen. Each column now has its own
  appropriately-sized cap instead of one shared one.
- **The file panes had the identical same-shared-cap gap on Size/
  Modified**, for the same reason — same per-column fix applied there
  too.

## [0.6.18] — Add per-pane Home button; fix column headers pushing off-screen

### Added

- **A Home button on each file pane**, to the left of the existing "Up
  one level" button — returns to wherever that pane's backend first
  landed after connecting (your OS home directory for the local pane,
  the server's resolved home/starting directory for SFTP/FTP/FTPS).

### Fixed

- **Dragging the Transfers panel's "File" column wide enough pushed
  Direction/Status/Progress/Speed entirely off the right edge, with no
  way to scroll back to them.** That column had no maximum width, and
  the table's stretch-last-section behavior actively avoids showing a
  horizontal scrollbar, so those columns became genuinely unreachable,
  not just scrolled out of view. Capped at a generous but bounded
  maximum instead.
- **The same unbounded-width problem existed on both file panes' "Name"
  column** — same cause, same fix.

### Changed

- **File pane and transfer queue column headers are more readable**:
  header text brightened to the same high-contrast color used
  everywhere else in the app (was a dim secondary gray), and a faint
  vertical divider now separates each column header from the next.

## [0.6.17] — Fix FTPS data connections rejected by strict TLS-session-reuse servers

### Fixed

- **FTPS data connections now genuinely satisfy a strict server's TLS
  session-reuse requirement (RFC 4217's anti-hijacking check), closing
  a limitation this project previously disclosed as unfixed.**
  Confirmed against a real proftpd server (default `mod_tls` config,
  strict reuse enforcement on) doing a full list/download/upload
  session — every data connection, not just the first. FTPS's control
  and data connections now use a from-scratch raw-OpenSSL TLS layer
  (`FtpTlsSocket`, forced to TLS 1.2) instead of `QSslSocket`, since
  Qt's own API has no way to drive the classic session resumption a
  strict server's check actually recognizes. Plain FTP is unaffected.
  See ARCHITECTURE.md's Known gaps entry for the full technical story,
  including a real, non-obvious OpenSSL behavior found along the way
  (a session object can only be used in one TLS handshake, ever, even
  up-ref'd — fixed by deep-duplicating it before each reuse).
- **`verify-ftp-move`'s FTP/FTPS move-to-another-directory test could
  hang indefinitely** — a real signal-connection-ordering bug in the
  test harness itself (calling `FilePaneWidget::navigateTo()`
  synchronously from inside the same `directoryListed` signal delivery
  that `FilePaneWidget`'s own internal handler needed to run first),
  unrelated to `FtpBackend`. Fixed by deferring that call to the next
  event-loop turn.

### Changed (developer-facing only, no shipped behavior)

- New build dependency: OpenSSL (direct, not just transitively through
  Qt), needed by `FtpTlsSocket`. Discovered the same way this project's
  existing libssh2 dependency already is — `find_package(OpenSSL CONFIG
  REQUIRED)` for MSVC, `pkg_check_modules(... IMPORTED_TARGET openssl)`
  for MinGW/Linux. No packaging changes needed on Linux (OpenSSL is
  already a near-universal system dependency); the Windows build's
  `collect-win-runtime.sh` needed no changes either — it already
  discovers DLL dependencies by walking the built `.exe`'s import table,
  not a hardcoded list.
- `tools/local-test-servers/containers/proftpd.conf` gained
  `AllowOverwrite on` — proftpd's default (off) rejected
  `verify-ftp-vendors`' repeat-run STOR to an already-existing path with
  a real, if here misleading, "Overwrite permission denied", found by
  actually re-running that harness twice against the same
  not-recreated-per-run container.
- `verify_ftp_vendors.cpp`'s upload round-trip now uses a
  per-server-tag remote filename — the plain and FTPS phases against
  the SAME proftpd container used to share one hardcoded path, so the
  FTPS phase's own upload could collide with a file the plain-FTP phase
  had already created earlier in the same run.

## [0.6.16] — Fix real bugs found by code review of CommandsPaneWidget and HostKeyVerifier/CertificateVerifier

### Fixed

- **Scrolling up in the Commands pane to re-read an earlier line, while
  a transfer was still running, immediately snapped back down to the
  bottom the moment the next line arrived** — making it impossible to
  actually review protocol traffic while it was still live. The log now
  only auto-scrolls to follow new lines when you were already at the
  bottom; scroll up and it stays put until you scroll back down
  yourself.
- **The "unknown host" / "host key changed" and "untrusted certificate" /
  "certificate changed" prompts showed only the server's hostname, never
  the port** — two saved sites on the same hostname but different ports
  (e.g. two SFTP servers, or an SFTP and an FTPS service on the same
  machine) got an identical prompt with no way to tell which one was
  actually being verified. Both now show `host:port`, matching what's
  actually being remembered.
- **Those same two prompts could occasionally open unfocused, behind the
  main window, or on a different virtual desktop**, making the app look
  like it had frozen (it was really just waiting on a security prompt
  you couldn't see). They're now properly attached to the main window.

### Changed (developer-facing only, no shipped behavior)

- `FolderEnumerator.cpp` got its first dedicated code review — a
  genuinely clean, small file with no new functional bugs found. Two
  inaccurate code comments were corrected: one cited itself as its own
  justification for a threading assumption instead of the real reason,
  and another claimed a stronger guarantee ("structurally impossible")
  than what the code actually delivers.
- Icons are rendered slightly more efficiently on a cache miss (first
  use of a given icon/color/size combination) — the source SVG was
  being parsed twice (once each for the normal and HiDPI versions)
  instead of once.
- `PreferencesDialog.cpp`'s first dedicated code review found no
  correctness bugs, but did find the Connect dialog, Site Manager, and
  Preferences dialogs each hand-wrote an identical, three-way-duplicated
  bit of protocol-combo-box setup code. Extracted to a shared helper so
  a future new protocol only needs to be added in one place instead of
  three.
- `HostKeyVerifier.cpp`/`CertificateVerifier.cpp` each hand-wrote an
  identical trust-prompt dialog dispatch. Extracted to a shared helper.
  Both also get their first-ever automated test coverage
  (`trust-prompt-test`).

## [0.6.15] — Fix a real crash-corruption bug found by code review of AppSettings

### Fixed

- **A crash, power loss, or full disk while saving a preference (window
  size/position, "Show hidden files," default protocol, or either
  panel's on-start visibility) could wipe out ALL of your other
  saved preferences, not just the one being written**, the next time
  you launched the app. Settings are now written the safer way (a
  temporary file, only swapped in once it's fully and correctly
  written), so an interrupted save can no longer corrupt or lose
  anything already saved.

### Changed (developer-facing only, no shipped behavior)

- `AppSettings.cpp` got its first-ever dedicated code review and its
  first-ever automated test coverage (`app-settings-test`).

## [0.6.14] — Fix a real bug found by code review of CredentialStore

### Fixed

- **A failed "Save password" could leave you thinking your password was
  saved when it wasn't.** If your system's credential store rejected the
  save (locked, unreachable, or — on Windows — a very long saved
  password/passphrase exceeding a hard OS size limit), the checkbox
  stayed checked with no indication anything had gone wrong; you'd only
  find out on your next visit to Site Manager, when the checkbox quietly
  unchecked itself. You're now told right away if a save didn't work.

### Changed (developer-facing only, no shipped behavior)

- `CredentialStore.cpp` — the one file this project's own contributor
  guidelines single out as security-critical — got its first-ever
  dedicated code review, and its first-ever automated test coverage
  (`verify-credential-store`, exercised against a real OS credential
  store). A minor internal encoding inconsistency was also cleaned up
  for clarity, and a Linux-only compiler warning was fixed.

## [0.6.13] — Fix a real CI-only test bug that blocked the v0.6.12 release

### Changed (developer-facing only, no shipped behavior)

- **`file-operations-test` had a flaky-timing bug that surfaced as a real
  `build-linux-rpm` CI failure blocking the v0.6.12 release**, unrelated
  to that release's actual code changes. Three check phases (the
  rename-conflict-rejection, download-rollback, and move-rollback
  checks) each assumed a flat 200ms window was always enough time for a
  queued backend dispatch plus real disk I/O to finish before reading
  the result. Fixed by polling for the actual signal-driven state change
  instead, matching `navigation-test`'s and `move_entry_test`'s own
  established pattern; confirmed with 30/30 clean stress-test runs
  afterward.

## [0.6.12] — Fix four real issues found by code review of ConnectionDialog

### Fixed

- **Typing a port that happened to match FTP/FTPS's default (21) while
  connecting via SFTP could get silently discarded** if you then
  switched the protocol dropdown away and back — your deliberately
  chosen port reset to SFTP's default (22) with no warning.
- **A username with accidental leading/trailing whitespace (common when
  pasting from a credentials email or spreadsheet) would fail to
  authenticate** with a generic error, giving no hint the whitespace was
  the actual problem — usernames are now trimmed the same way the host
  field already was.

## [0.6.11] — Fix five real issues found by code review of TransferQueueWidget

### Fixed

- **The transfer queue's Direction column could show a misleading
  "sorted" arrow before anything was actually sorted**, and clicking
  that header sorted by an arbitrary internal order not tied to
  anything visible on screen — both fixed to reflect the queue's real
  state.
- **An actively-copying local-to-local transfer looked identical to one
  that hadn't started yet** in the queue's direction icon.

## [0.6.10] — Fix a real crash-on-close bug found by code review of MainWindow

### Fixed

- **Closing the app while a connection attempt was still in progress
  could crash it** — the fix now refuses to close until that attempt
  finishes or fails, instead of letting the window close anyway.

## [0.6.9] — Fix nine real bugs found by a dedicated code review of FilePaneWidget

### Fixed

- **A background operation's own directory refresh (delete/rename/new
  file/new folder) could be mistaken for the response to a navigation
  still in progress**, opening a brief window where a second action
  (Back, another navigation) silently corrupted the pane's history.
- **A failed Back/Forward navigation could permanently break Back/Forward
  for the rest of that pane's session** — the next successful navigation
  silently stopped updating history.
- **Selecting a file, then navigating to a different folder that happens
  to contain a same-named file (e.g. `README.md`, `.gitignore`), could
  silently auto-select that unrelated file** with no user action —
  dangerous if the next action is Delete or Move.
- **Rename could silently target the wrong directory** if a navigation
  completed while the Rename dialog was still open.
- **Pressing Enter in the path bar while a navigation was still in
  progress silently did nothing**, with no indication why, and the
  freshly typed path could be overwritten moments later.
- **Sorting by the Name column no longer keeps folders grouped together
  and no longer compares names correctly** for anything outside plain
  ASCII — fixed to match every other sort order in the app.
- A remote (SFTP/FTP) pane's current directory could theoretically race
  between the UI and the connection's own background thread. Fixed with
  proper synchronization.

### Changed (developer-facing only, no shipped behavior)

- File and folder icons are now cached instead of being re-rendered from
  their source SVG on every row of every directory listing — noticeably
  less redundant work when browsing a large directory.

## [0.6.8] — Fix a second CI-only test bug that also broke the v0.6.7 release build

### Changed (developer-facing only, no shipped behavior)

- **`file-operations-test`'s directory-symlink-deletion regression
  relied on `QFile::link()` to simulate a real directory symlink —
  which only creates one on Unix.** On Windows, `QFile::link()` writes
  a small Shell-Shortcut-style file instead, which `QFileInfo` reports
  as neither a directory nor a symlink — confirmed with a standalone
  probe built and run under `wine`, mirroring CI's own `build-windows`
  container. The test's hardcoded "this is a directory" assumption then
  made `deleteEntry()` wrongly attempt to remove that plain file as a
  folder and fail, which is what kept `build-windows` failing even
  after the v0.6.7 fix above. Fixed by skipping this Unix-only phase on
  Windows rather than asserting something false there.

## [0.6.7] — Fix a CI-only test bug that broke the v0.6.6 release build

### Changed (developer-facing only, no shipped behavior)

- **`file-operations-test`'s download-rollback regression relied on
  `chmod 000` to simulate an unreadable source file — which only blocks
  reads for a non-root user.** This project's own container-based CI
  jobs (`build-linux-appimage`, `build-linux-rpm`, `build-windows`) run
  their test suite as root inside Docker, where root bypasses Unix
  permission bits entirely, so the "unreadable" source was actually
  still readable there and the test's rollback assertions failed —
  which is exactly what broke three of four v0.6.6 release build jobs
  (no GitHub Release was ultimately published under that tag). Fixed
  by using a directory as the copy source instead, which
  `QFile::copy()` refuses to open regardless of privilege — confirmed
  both as a normal user and as root via a real `fedora:44` container.

## [0.6.6] — Fix seven more real bugs found by code review of LocalBackend and the UI layer

### Fixed

- **Overwriting a local file or folder during a transfer, rename, or
  Move could permanently lose the ORIGINAL destination content if the
  operation then failed partway through** (a full disk, a permission
  problem, or the source disappearing mid-copy) — the old content was
  deleted before the replacement was confirmed to actually succeed.
  Fixed: the existing destination is now moved aside first and only
  cleaned up (or restored) once the outcome is known.
- **Renaming a local file to a name that differs only by letter case
  (e.g. "readme.txt" to "README.txt") was incorrectly rejected as
  "already exists"** on Windows and macOS, where filenames aren't
  case-sensitive.
- **A directory symlink in the local pane could never actually be
  deleted** — it always failed with a misleading "may not be empty"
  message regardless of whether it (or its target) was genuinely
  removable.
- **Running two copies of ZephyrFTP at once and dragging a file from a
  pane in one onto a pane in the other could crash the receiving copy**
  instead of being safely ignored — a drag-and-drop payload from a
  different running instance was mistakenly treated as if it came from
  the same one.
- **Dragging a file whose name contained a line break (unusual, but
  legal on Linux) could transfer the wrong file, or silently drop part
  of the selection.**
- **A hand-edited or otherwise corrupted `sites.json` with two saved
  sites sharing the same internal ID could make an action on one site
  silently affect the other instead.**
- **Clearing a saved site's Host field (or switching to "Specific
  directory" before typing a path) and then clicking away without
  finishing the edit could silently and permanently blank out that
  setting**, with no way to recover it short of retyping it from memory.

## [0.6.5] — Fix five more real bugs found by code review of TransferManager and the UI layer

### Fixed

- **Cancelling a transfer whose connection had already been torn down
  (e.g. Disconnect while it was still running) could leave the whole
  queue permanently stuck** — the cancelled item stayed stuck In
  Progress forever, and nothing behind it in the queue would ever start.
  Now resolves cleanly to Cancelled and lets the rest of the queue run.
- **A rare cancel/pause timing overlap could mislabel a completely
  unrelated LATER transfer's real failure as "Cancelled," hiding its
  actual error message.**
- **Clicking Back (or Forward) twice quickly could skip past the
  directory you meant to land on and silently lose your forward
  history.**
- **Toggling "Show hidden files" or hitting Refresh while files were
  selected silently cleared the selection**, losing the target set of
  a Transfer/Move you were about to start with no indication why.
- **The transfer queue's sort order broke as soon as a new transfer was
  added** — sort by any column, then start a new transfer, and the new
  row landed at the bottom out of order instead of where it belonged.

## [0.6.4] — Fix four real bugs found by code review of TransferManager and the UI layer

### Fixed

- **Clicking Connect again (or Disconnect) on a pane whose previous
  connection attempt hadn't resolved yet — a slow or packet-dropping
  host — could freeze the entire app** until the OS's own connection
  timeout eventually gave up. Both actions now refuse with a status-bar
  message instead of attempting a teardown that couldn't actually
  interrupt the stuck connection attempt.
- **Dragging two folders onto a pane at the same time could silently
  drop the first one — no error, it just never transferred.** The
  second drag's setup overwrote the first's in-progress state before it
  had a chance to actually start. Fixed, and along the way this also
  caught (and fixed) a second bug it had been hiding: two folders
  enumerating at once could briefly mix up which files belonged to
  which folder.
- **A transfer that was paused, then cancelled instead of resumed, then
  retried, could silently overwrite whatever now existed at its
  destination** instead of asking first — a retry is supposed to treat
  the destination as a fresh unknown, but it was still carrying a
  leftover "skip the check" flag from the earlier pause.
- **A server-side Move could get stuck "In Progress" forever** if
  another transfer started against a different server while the Move's
  own confirmation was still in flight — the new transfer's setup could
  accidentally sever the Move's connection to its own result.

## [0.6.3] — Fix a known_hosts race between concurrent SFTP connections

### Fixed

- **Two `SftpBackend` instances connecting around the same time (Move or
  a server-to-server transfer between two SFTP servers, for example)
  could race on `known_hosts`, silently dropping one of their host-key
  trust decisions.** Each connection loaded the whole file into its own
  in-memory copy and later overwrote the whole file with it; a second
  connection finishing first could erase whatever the first had just
  persisted, with nothing telling you it happened — you'd just get asked
  to trust that host again next time. Reliably reproducible (8/8 runs in
  a repeated stress test), not a rare edge case. Fixed by guarding the
  read-modify-write with a file lock. See ARCHITECTURE.md's `SftpBackend`
  entry for the full before/after verification.

## [0.6.2] — Fix nine real bugs found by code review of FTP/SFTP auth and protocol code

### Fixed

- **A different-but-similarly-certified certificate on an FTPS data
  connection could be silently accepted instead of failing closed.**
  The fingerprint check that's supposed to catch this only ran when Qt
  itself flagged a TLS error; a certificate that happened to validate
  cleanly on its own (e.g. a real CA-issued cert that just isn't the
  SAME one the control connection already trusted) skipped the check
  entirely.
- Fixed a real (if narrow) use-after-scope-exit bug in FTPS certificate
  verification, and an active-mode connection fallback that could
  incorrectly refuse to even try on some dual-stack systems.
- A malformed or malicious PASV server reply could produce a nonsensical
  connection target instead of a clean error.
- **Connecting to a real host key that legitimately differs by port
  (two SSH services on the same hostname, different ports) could show a
  scary "host key changed — possible MITM!" warning for what was
  actually just a different, never-before-seen service.** Host keys are
  now correctly tracked per host *and* port.
- A connect-time failure (wrong password, rejected host key, etc.) could
  leak a socket and an SSH session; retrying after fixing the problem
  leaked another one each time.
- A failure to save a newly-trusted host key to disk (a full disk, a
  briefly read-only config directory) was silent — every later
  connection would keep re-prompting with no indication why.
- Connecting with a private key that has no `.pub` sibling file now
  works (previously failed authentication outright, even though the
  private key itself was valid).
- A private key whose fingerprint can't be computed (a rare crypto-backend
  limitation) now says so plainly in the trust dialog instead of showing
  a blank fingerprint.

## [0.6.1] — Fix a general resume bug and five real credential-storage bugs

### Fixed

- **Resuming ANY paused transfer (not just server-to-server) could show
  a real, spurious "file already exists" prompt for the transfer's own
  in-progress content.** Resuming re-checked the destination for a
  conflict the same way a brand-new transfer does — but the destination
  already legitimately has the bytes already sent before the pause, so
  a real server would (correctly) report "yes, something's there,"
  triggering an Overwrite/Skip prompt for what was never actually a
  conflict. Choosing "Skip" there would abandon the exact transfer you
  just asked to continue. Found via live-server pause/resume
  verification (`verify-remote-to-remote-live`) — every fake backend in
  the test suite always reported "doesn't exist" regardless of real
  state, so this was invisible without a real backend and real
  destination content. Predates this release; affected any resumed
  upload or download.
- **Deleting a saved site never removed its stored password/passphrase**
  from the OS credential store — it stayed there permanently, orphaned,
  with no way left in the app to ever remove it. Deleting a site now
  also removes its saved secret, if it had one.
- **Switching a saved site's sign-in method (password <-> private key)
  could carry an old saved secret forward as if it were the new kind**,
  offering (and, if accepted as-is, silently saving) an old password as
  a private key's passphrase or vice versa. Switching the sign-in method
  now clears the old saved secret instead.
- A non-ASCII saved password or passphrase could be silently corrupted
  on Linux systems using a non-UTF-8 locale, due to an encoding mismatch
  between saving and loading.
- The Site Manager's Connect button could prompt to unlock your OS
  keyring twice in a row for a single click, due to a redundant lookup.

## [0.6.0] — Pause/resume for server-to-server transfers; real RemoteToRemote bugs fixed

### Added

- **Server-to-server transfers can now be paused and resumed, in either
  half.** Previously cancel-only, on the theory that resuming a staged
  two-phase transfer would need extra work to preserve which half was
  active, the resume offset, and the temporary file across the pause.
  That reasoning turned out to be stale: none of the three ever actually
  needed special handling. Closed by adding the missing test coverage
  (pausing and resuming during both the download and upload half) and
  then enabling it.

### Fixed

- **A `RemoteToRemote` transfer's upload phase could silently redirect to
  the wrong destination if the destination pane's connection changed
  while the download phase was still running.** Introduced the moment
  "either pane can connect independently" made a mid-transfer pane swap
  possible for the first time — the upload phase re-fetched the
  destination pane's *current* backend instead of the one active when
  the transfer started; swapping to a different server (or back to
  `LocalBackend`) mid-download would silently upload there instead,
  reported as a normal success. Fixed by capturing the destination
  backend once, at the start of the transfer; if that connection is
  gone by the time the upload phase would start, the transfer now fails
  with a clear explanation instead of silently redirecting.
- **A cancel or pause requested right as an SFTP transfer reached EOF
  could be silently lost**, letting the transfer complete instead —
  low-stakes on its own, but meant a lost cancel on a `RemoteToRemote`
  item's download phase would also let an entire unwanted upload phase
  run afterward. `FtpBackend`'s equivalent loop already checked
  correctly; `SftpBackend`'s now matches it.
- The transfer queue's progress bar stayed green for the entire
  duration of a `RemoteToRemote` transfer, contradicting its own
  phase-aware status icon and text (blue while downloading, green while
  uploading) — now consistent.

### Changed (developer-facing only, no shipped behavior)

- `remote-to-remote-test` restored 10 individually-named assertions that
  an earlier CI-flakiness fix had collapsed into implicit,
  timeout-backed stage gates — same event-driven design, precise
  PASS/FAIL diagnostics back to what they were — plus 10 new ones for
  the pause/resume scenario above (31 checks total, up from 11).
- Deduplicated the staging-directory path expression, previously
  repeated verbatim in two places.

## [0.5.2] — Fix two real Move bugs found by code review

### Fixed

- **Multi-select Move silently dropped every selected entry but the last
  one.** Selecting several files/folders and choosing "Move Selected"
  only actually moved the last item in the selection — every earlier
  one silently vanished with no error. Caused by `TransferManager`'s
  conflict-check stage stashing each in-flight Move's state in shared
  scalar members rather than tracking it per-request, so
  `MainWindow::moveEntries()`'s synchronous per-entry loop clobbered
  each call's state before its own response arrived. Found by code
  review, not a user report — introduced with the Move feature itself
  (v0.5.0) and present in v0.5.0/v0.5.1.
- **A Move's "apply to all" conflict-resolution choice could silently
  leak into a completely unrelated later transfer.** Choosing "apply to
  all remaining conflicts" + Write Into during a Move could cause a
  LATER, unrelated ordinary transfer's own folder conflict to be
  silently resolved the same way with no prompt — Move's conflict
  resolution shared state with the ordinary transfer pipeline but never
  triggered that pipeline's own reset. Move now tracks its own,
  separate resolution state. Also found by code review; present in
  v0.5.0/v0.5.1.
- `retryItem()` now guards against `TransferDirection::Move` directly
  (previously enforced only by the transfer queue disabling the Retry
  action for Move items, not by `TransferManager` itself) — closes a
  latent misdispatch path a future caller could otherwise have hit.

## [0.5.1] — Real-server verification for Move and remote-to-remote transfers

### Changed (developer-facing only, no shipped behavior)

- **Three new automated live-server verification harnesses**
  (`verify-sftp-move`, `verify-ftp-move`, `verify-remote-to-remote-live`),
  closing gaps ARCHITECTURE.md previously flagged as unconfirmed against
  a real server: whether a server-side Move genuinely relocates a
  *directory* (not just a file) over both SFTP and FTP specifically, and
  a full remote-to-remote transfer completing end-to-end between two
  genuinely independent live SFTP servers with byte-for-byte content
  confirmed on the destination's own disk. All three found and fixed
  real conflict-collision bugs in the harnesses themselves along the way
  (not in `TransferManager`) — see each `.cpp`'s own header comment,
  ARCHITECTURE.md's `RemoteBackend`/`TransferManager` Verification status
  entries, and CONTRIBUTING.md's live-server verification section for the
  full detail.
- **`move-entry-test` now also covers the "Write Into an existing folder
  fails" Move-conflict path**, previously the one manual-only gap in its
  own coverage — driven via a real `QMessageBox`
  (`conflict-resolution-test`'s established live-dialog technique, applied
  here as a continuous poller rather than a single fixed-delay shot),
  confirmed stable under the same deliberate CPU-contention stress test
  that caught a real flake elsewhere in this project earlier.

## [0.5.0] — Server-side Move between panes on the same connection

### Added

- **Move, not just copy, between two panes on the same server (or both on
  your computer)** — a new "Move Selected" context-menu action, alongside
  the existing Transfer, relocates a selection server-side via a single
  instant rename instead of a download-then-upload copy. Works for whole
  folders too, moving the entire subtree in one round trip; the one real
  limit is that a folder Move can't *merge* into an existing folder of
  the same name at the destination the way a copy's "write into" can — it
  fails with a clear message instead of attempting a doomed rename. Only
  offered when both panes are genuinely on the same connection (same
  server, or both local) — otherwise a short message explains why, rather
  than silently doing nothing. Verified by a new, deterministic
  fake-backend regression test plus a direct real-`LocalBackend` check
  against real temp files; see ARCHITECTURE.md's `RemoteBackend`/
  `TransferManager` entries and [Known limitations](README.md#known-limitations)
  for the full detail, including what isn't yet covered.

## [0.4.0] — Either pane can connect; server-to-server transfers

### Added

- **Either pane can now connect to a remote server, not just the right
  one** — click a pane's own path-bar icon (the same one that already
  showed a laptop/server indicator) for a Connect/Sites/Disconnect menu
  targeting that specific pane. The toolbar's Connect/Sites/Disconnect
  still work exactly as before, as a shortcut to the right pane.
- **Server-to-server transfers**, staged through a local temporary file
  (download from the source server, then upload to the destination —
  no protocol lets one server send a file straight to another, so this
  is the only mechanism possible). Cancel works; pause doesn't yet, for
  the same reason local-to-local copies can't be paused either, plus one
  more specific to this case — see [Known limitations](README.md#known-limitations)
  for the full explanation. Verified by a new, deterministic fake-backend
  regression test (direction/phase handling, temp-file cleanup on success
  and on cancellation during either half, retry correctly restarting from
  the download half rather than the deleted upload); also manually
  confirmed against two real local SFTP servers — see ARCHITECTURE.md's
  `TransferManager` and Known gaps entries for exactly what that did and
  didn't establish, including one gap (a fully clean, repeatable,
  automated live-two-server test) intentionally left for a follow-up
  rather than glossed over.

### Fixed

- **Closing the app while the LEFT pane was connected to a server would
  have reproduced the exact crash 0.2.5 already fixed for the right
  pane** — `closeEvent()` only ever tore down `m_rightPane`, since that
  was the only pane that could hold a thread-owning backend before this
  release. Now tears down both. Manually re-confirmed against two real
  SFTP connections (one per pane): closing the window doesn't crash.

### Added (developer-facing only, no shipped behavior)

- **`remote-to-remote-test`**, a new self-contained `EXCLUDE_FROM_ALL`
  regression test for the staged remote-to-remote transfer logic above —
  see its own entry in the user-facing Added section for what it covers.
  Same "additional, not folded into the fixed count" treatment as
  `sort-and-commands-test`; all four `build.yml` jobs build and run it
  too. **Rewritten as an event-driven state machine after a real CI
  flake**: an initial version (generous, doubled-nominal fixed `QTimer`
  delays) passed 20+ local runs but failed on a real GitHub Actions
  runner sharing its job with concurrent `linuxdeploy` work — reproduced
  locally under deliberate heavy CPU contention, where the fixed-delay
  version failed 15/15. Rewritten to react to the actual `itemUpdated`
  signal each step is waiting for instead of guessing a wall-clock delay,
  with a generous absolute-deadline timer as a safety net (not the
  primary mechanism) rather than the sole guard against a hang. Confirmed
  against the same contention setup that broke the old version: 40+
  consecutive clean runs.
- **`sort-and-commands-test`**, a new self-contained `EXCLUDE_FROM_ALL`
  regression test covering two things that had only ever been checked by
  screenshotting the running app: `CommandsPaneWidget`'s log and
  `FilePaneWidget`'s forwarding of `RemoteBackend::commandLogged`
  (including across a `setBackend()` swap), and the file panes' default
  folders-first/name-descending sort order, a real numeric Size sort, and
  `entryForRow()`'s row-independence after that sort. Deliberately kept
  as an explicit eleventh target rather than folded into "the ten"
  CONTRIBUTING.md/CLAUDE.md already treat as a fixed, documented count —
  see CONTRIBUTING.md's "Running the test suites" section for how to
  build and run it. **All four `build.yml` jobs now build and run it
  too**, alongside (not folded into) the ten.

## [0.3.9] — Default file panes to folders-first/name-descending; a Commands pane welcome line

### Changed

- **File panes now apply one consistent default sort order across all
  three backends, instead of an order that quietly depended on which one
  you were looking at.** Previously only `LocalBackend` sorted its own
  results (folders first, then name); `SftpBackend`/`FtpBackend` returned
  whatever order the server happened to list in. `rebuildModel()` now
  sorts every listing itself — folders first, then name descending, by
  the entry's real name rather than the later `"[folder]"`-wrapped
  display text — before a column header click (see 0.3.8) overrides it.
- **The Commands pane no longer starts out blank** — a one-line welcome
  message appears before any connection has produced real protocol
  traffic to show.

## [0.3.8] — Click-to-sort on both file panes and the transfer queue

### Added

- **Clicking a column header now sorts that column**, on both file panes
  (Name, Size, Modified, Permissions) and the transfer queue (File,
  Direction, Status, Progress, Speed); clicking the same header again
  reverses it. Size sorts numerically, not as text (its unpadded byte
  count previously would have put "10" before "9"). The transfer queue's
  Direction and Progress columns are Qt cell widgets, which
  `QTableWidget::sortItems()` doesn't move along with a sort — sorting
  there is done manually instead, rebuilding the table from a
  freshly-sorted copy of `TransferManager::items()` rather than risking a
  widget/row mismatch.
- Sorting the file panes surfaced a real latent bug: row-lookup methods
  (`onRowDoubleClicked`, `selectedEntryName`, `selectedFileNames`,
  `selectedEntries`) all indexed a parallel entry list by row position, an
  invariant that sorting breaks outright once rows move. Fixed by tagging
  each row with its entry's real name and adding `entryForRow()` to look
  entries up by that instead of position.

### Fixed

- **The transfer queue's File column had no working drag handle**, the
  same bug already fixed for the file panes' Name column in 0.3.7 —
  `Stretch` resize mode meant its width was purely a side effect of
  dragging the OTHER columns' handles. Switched to `Interactive`; an
  explicit `setStretchLastSection(true)` turned out to be needed
  alongside it (unlike the file panes' `QTreeView`, `QTableWidget`
  doesn't default that to true), caught by actually screenshotting the
  running app rather than assumed.

## [0.3.7] — Even out dock/window sizing; show-on-start prefs for Transfers/Commands

### Added

- **Two new preferences** (Edit > Preferences): "Show Transfers pane on
  start" and "Show Commands pane on start," both defaulting to on.
  Applied as an explicit override right after the window restores its
  saved dock layout, so turning either off is the only way to keep that
  dock closed permanently — otherwise reopening it once (even by
  accident) would make the saved layout reopen it on every later launch
  too.

### Changed

- Commands' and Transfers' first-run heights now both target ~200px
  (previously 120px for Commands, whatever `QTableWidget`'s own
  `sizeHint()` claimed for Transfers), and the fallback startup window
  size grew from 1100x650 to 1100x780 to give the file panes enough room
  underneath two 200px docks.
- The transfer queue's "File" header is now explicitly left-aligned,
  matching its own left-aligned cells (fixed for the cells themselves in
  0.3.6) — the header row otherwise rides Fusion's default centered
  label alignment.

## [0.3.6] — A live Commands pane; transfer queue alignment fixes

### Added

- **A live Commands pane** — a real-time, read-only log of protocol
  traffic, modeled on FileZilla's own message log, docked between the
  toolbar and the file panes by default (View > Commands to toggle,
  undockable/floatable like the Transfers pane). Deliberately no
  raw-command input: letting someone inject arbitrary commands into a
  live control connection risks leaving this app's own state (current
  directory, an in-flight transfer) out of sync with what the server
  actually did. `RemoteBackend` gains a `commandLogged(QString)` signal:
  `FtpBackend` emits genuine raw command/reply lines straight off the
  control connection, with `PASS`'s argument masked — verified live
  against a real vsftpd container that the actual password never reaches
  the log; `SftpBackend` has no textual wire protocol to show, so it
  emits human-readable descriptions of each high-level operation instead
  (`Status: Connecting`, `Command: LIST`/`GET`/`PUT`/`RENAME`/`MKDIR`/...),
  matching FileZilla's own approach for SFTP.

### Fixed

- **The file panes' Name column had no working drag handle** — it was
  left in `Stretch` resize mode while every other column was
  `Interactive`, so its width was purely a side effect of dragging the
  OTHER columns' handles. Switched to `Interactive` with a sensible
  starting width; `QTreeView`'s own `stretchLastSection` default
  (Permissions, the real last column) now absorbs leftover space instead.
- **Transfer queue cell alignment**: File is now explicitly left-aligned,
  Direction/Status/Speed are centered, and inline progress bars are
  vertically centered in their row instead of pinned to the top. The
  Direction column needed a different fix than the other two —
  `Qt::TextAlignmentRole` only affects an item's text, not its
  decoration, so an icon-only item ignored it entirely; switched that
  column to a `QLabel` cell widget instead, which actually centers.

### Investigated

- **The FTPS TLS-1.2-cap fix for proftpd's strict session-reuse check was
  tried and confirmed to be a dead end.** Capping max TLS protocol
  version to 1.2 on both the control and data `QSslSocket`s was the
  leading hypothesis for satisfying proftpd's strict TLS-session-reuse
  check, but Qt's `newSessionTicketReceived()`/session-ticket API turns
  out to be TLS-1.3-only in the public API — confirmed directly via
  temporary debug instrumentation, not assumed. With TLS 1.2 forced, that
  signal never fires at all, so resumption is removed entirely rather
  than fixed; proftpd's own `TLSLog` still rejects the data connection
  identically. The code change was fully reverted — this entry just
  records the finding so the same dead end isn't retried. See
  ARCHITECTURE.md's Known Gaps for the full writeup.

## [0.3.5] — Fix the AppImage release upload sweeping up build tools too

### Fixed

- **The `build-linux-appimage` CI job's release upload used a bare
  `*.AppImage` glob, which also matched `linuxdeploy-x86_64.AppImage`
  and `linuxdeploy-plugin-qt-x86_64.AppImage`** —
  `tools/build-appimage.sh` downloads both of those into the same
  working directory as build-time dependencies, and they rode along
  into the real v0.3.4 GitHub Release as extra, unwanted assets. Found
  by actually inspecting the published release, not assumed correct
  because CI was green. Fixed by narrowing the glob to
  `ZephyrFTP-*.AppImage`, the actual app output; the two erroneous
  assets were also removed from the already-published v0.3.4 release.

## [0.3.4] — A real, self-contained AppImage; Flatpak built and verified (not yet on Flathub)

### Added

- **A real, self-contained AppImage**, built via `linuxdeploy` and
  verified end to end across several fresh, unrelated containers with
  zero Qt/libssh2/libsecret installed — the actual point of an
  AppImage. Built deliberately on Debian 12, not the newest available
  base: AppImages link dynamically against the build system's own
  glibc, so the standard advice is to build on the oldest base that
  still works, and Debian 12 is the oldest that also satisfies this
  project's own Qt 6.3+ requirement (Ubuntu 22.04 has older glibc but
  only Qt 6.2.4 — confirmed via a real failed build, not assumed).
  Testing surfaced three real gaps `linuxdeploy`'s automatic
  (`ldd`-based) dependency scan misses — fontconfig, harfbuzz, and
  freetype, all reached via `dlopen()` rather than direct linking — now
  bundled explicitly in `tools/build-appimage.sh`. The GL/EGL/GLX stack
  and fontconfig's own configuration are deliberately left to the host,
  confirmed (not assumed) to be a safe assumption: every real desktop
  already has both, or its own UI couldn't render.
- **A real, working Flatpak manifest** (`io.github.arelas.zephyrftp.yml`)
  — builds cleanly, installs, and runs, with its two non-standard
  permissions (`--filesystem=host`, `--talk-name=org.freedesktop.secrets`)
  individually confirmed actually working from inside the sandbox, not
  just declared. **Deliberately not submitted to Flathub yet** — their
  own current requirements flag broad-scope file managers as a category
  needing extra scrutiny (waived for submissions from the actual
  upstream project with a demonstrated maintenance history), worth
  having more of that history first. Build and test it locally per
  CONTRIBUTING.md's "Flatpak" section.
- **CI now builds the AppImage automatically on every tagged release**
  — a new `build-linux-appimage` job (Debian 12) runs the full
  ten-target test suite independently on top of building it; attached
  to the GitHub Release alongside the existing tarball/zip/`.deb`/`.rpm`.

## [0.3.3] — Real .deb/.rpm Linux packages

### Added

- **Real Linux distro packages** — `.deb` and `.rpm`, built via CPack
  from the same source the existing plain tarball already ships,
  alongside it (not a replacement). A proper system install layout
  (`/usr/bin`, a `.desktop` file, hicolor-theme icons at every size),
  with dependencies **auto-detected from the actual linked binary**
  (`dpkg-shlibdeps` for `.deb`, rpmbuild's own scanner for `.rpm`)
  rather than a hand-maintained list that could quietly drift from what
  the binary actually needs. Verified end to end in disposable
  containers before ever being wired into CI: built in one container,
  then installed in a *separate*, completely clean container with no
  build tooling at all — dependencies resolved automatically, the
  `.desktop` file validated, and the installed binary actually ran.
- **CI now builds both automatically on every tagged release** — a new
  `build-linux-rpm` job (Fedora 44 container, matching `build-windows`'s
  existing pattern) runs the full ten-target test suite independently
  on top of building the `.rpm`; the existing `build-linux` job now also
  produces the `.deb`. Both get attached to the GitHub Release alongside
  the existing Windows zip and Linux tarball.
- **CONTRIBUTING.md's dependency list now covers building the packages
  themselves** locally (`cpack -G DEB` / `cpack -G RPM`), documenting
  exactly what was verified and how.

## [0.3.2] — Real screenshots, and a verified per-distro dependency list

### Added

- **Two real screenshots in the README** — the actual app, actually
  connected to a real local SFTP server via the genuine Connect dialog
  flow (not a mockup): a calm dual-pane browsing view, and a real 300MB
  transfer genuinely in progress with a live speed reading. Neither is a
  staged image; both were captured driving the real `MainWindow` through
  the same UI path a person uses.
- **A real per-distro dependency list in CONTRIBUTING.md**, replacing
  the previous Debian/Ubuntu-only line. Debian/Ubuntu, Fedora, and Arch
  are all directly verified — Arch via a real `archlinux:latest`
  `podman` container (`pacman -Sy` against the live repos, a real
  `cmake`/`make` build, and a real headless run of the resulting
  binary, not just a successful link), Fedora against this project's
  own development environment (confirmed via `rpm -q`). openSUSE is
  included by package-name convention only, explicitly flagged as
  unverified rather than presented with false confidence.

## [0.3.1] — Preferences, persisted window layout, and three real UI bugs fixed

### Added

- **A real settings/preferences system**, where none existed before —
  `AppSettings`, persisted as `settings.json` alongside `sites.json` (same
  hand-written-JSON convention, not `QSettings`). First put to use for:
  - **Window geometry and dock layout now persist across restarts** —
    previously the app always reopened at a fixed 1100x650 with the
    Transfers dock in its default spot, every time.
  - **A "Show hidden files" toggle** (Edit > Preferences), applied
    uniformly to the local pane and both remote protocols. This also
    fixes a real, pre-existing three-way inconsistency: the local pane
    silently excluded every dotfile with no way to reveal them, while
    the SFTP and FTP panes silently showed every dotfile with no way to
    hide them — neither behavior was a deliberate choice, just whatever
    each backend happened to already do. Whole-folder transfers
    (drag-and-drop or "Transfer Selected" on a folder) are unaffected
    either way — a folder's dotfiles were always included in what
    actually gets recursively copied, on all three backends, since "hide
    from view" and "don't transfer" were never the same thing and still
    aren't.
  - **A default protocol for new connections** (also in Preferences) —
    the plain Connect dialog now preselects whichever protocol you use
    most, instead of always defaulting to SFTP.

### Fixed

- **The status bar showed a permanently stale "Connecting to <host>..."
  message, even long after the connection had actually succeeded or
  failed.** Nothing ever followed up that message — `MainWindow` never
  listened for `connected()`/`connectionFailed()` on the backend it had
  just created, so the status bar was stuck reporting an in-progress
  connect indefinitely, including in the specific reported case of it
  still reading "Connecting..." after already being connected. Now shows
  "Connected to <host>" or "Failed to connect to <host>: <reason>" once
  the outcome is actually known.
- **The Transfers dock, once undocked (floated) and then closed, had no
  way to be brought back.** A floating `QDockWidget` gets a real,
  WM-drawn close button on its own top-level window regardless of this
  dock's own `DockWidgetClosable` feature (which wasn't even set), and
  closing it that way was a dead end — reported as "double-clicking it
  made it disappear with no way to bring it back." A new **View** menu
  now hosts the dock's `toggleViewAction()`, a checkable action Qt keeps
  in sync with the dock's actual visibility in both directions, so the
  Transfers panel can always be shown again no matter how it was hidden.
- **Switching the Connect dialog's Protocol dropdown away from SFTP (on
  an already-open dialog — the normal way anyone actually does this,
  since it always opens on SFTP first) left dead space where the
  Authentication row used to be, instead of the window shrinking to
  match.** A dialog opened directly in FTP/FTPS mode already looked
  correctly compact — only the live switch was affected, which is also
  the case that matters in practice. Found during a systematic
  dialog-consistency screenshot pass (offscreen renders of every custom
  dialog, both on Linux and cross-compiled under `wine` for a real
  Windows comparison), not reported by a user first. Root cause:
  `QFormLayout` doesn't shrink a row's reserved space just because its
  widgets are hidden, and the fix (`adjustSize()` after toggling
  visibility) needed its own care — called too early, it read a stale,
  not-yet-recomputed layout size hint, which only would have made the
  bug appear one switch late instead of fixed. See ARCHITECTURE.md for
  the full root-cause writeup.

## [0.3.0] — Both core protocols now verified against real, independent servers

Since 0.2.0 first wired FTP/FTPS into the UI unverified, every 0.2.x
release chased down a real, evidence-backed answer for one gap at a
time: FTP and FTPS confirmed end to end against real, independently-implemented
server software (vsftpd, proftpd), not just this project's own test
stand-ins; SFTP's public-key auth, `checkExists()`, the recursive
folder-transfer walk, and mid-transfer cancel/pause/resume all confirmed
against a real server; the SFTP throughput gap that was an honest
unknown (no real non-loopback server to test it against) now confirmed
closed, at parity with `scp` on a real link; and — most recently —
opt-in password storage confirmed round-tripping through the real
credential store on both platforms this app ships for, Linux's Secret
Service and Windows' actual Credential Manager, not just compiling and
running crash-free under `wine`. That's the same kind of milestone
0.2.0 itself marked (a real capability landing, not just a fix) — this
one closes the verification gaps that release opened, for both
protocols this app speaks. What's left in
[ARCHITECTURE.md](ARCHITECTURE.md)'s Known Gaps is now either a
deliberate, documented scope boundary (no recursive delete, no
remote-to-remote transfers, the SSH-daemon-diversity ceiling) rather
than something still owed, or the [README](README.md#known-limitations)'s
honest, ongoing list — this changelog doesn't duplicate either.

### Changed

- Live Speed column smoothing (exponential moving average) and the
  new `verify-sftp-throughput` real-server harness — see 0.2.16 below,
  the last patch release before this one — are the concrete changes
  underlying this milestone's throughput claim.

## [0.2.16] — SFTP throughput gap confirmed closed; smoother live speed

### Fixed

- **The SFTP throughput gap — previously ~1.6-1.8x behind a
  FileZilla/Termius/SMB comparison, with the remaining cause left
  explicitly unknown for lack of a real non-loopback test server — is
  now confirmed closed.** A new harness, `verify-sftp-throughput`
  (`src/verify_sftp_throughput.cpp`, opt-in and env-var-configured
  since it needs a real externally-provided server), measured this
  app's `SftpBackend` against a real server (~6-7ms round-trip time)
  alongside OpenSSH's own `scp` as a same-link, same-moment baseline.
  Result, confirmed across two runs at two file sizes: ~29-31MB/s
  upload / ~37-39MB/s download here vs. `scp`'s own ~29-36MB/s /
  ~35-37MB/s on the identical link — real parity with a native
  reference SFTP client, not the previously-reported gap. No
  `SftpBackend` code change was needed; the honest result is "parity
  confirmed," and the likely explanation for the original number is
  that it came from a different network at a different time, compared
  against SMB (a different transport family entirely). See
  ARCHITECTURE.md's Known Gaps for the full writeup.

### Changed

- **The live Speed column now smooths its readout instead of showing
  each raw 250ms sample untouched.** Confirmed by direct comparison
  that other SFTP clients' visibly calmer live speed numbers come from
  smoothing, not from being more accurate — each of this app's own raw
  samples was already a real, unlagged measurement, just with nothing
  carried over between windows, so natural transfer burstiness (TCP
  window dynamics, disk flush stalls, scheduler jitter) showed up
  directly as visible jumpiness. An exponential moving average
  (`TransferManager`, alpha = 0.3) now smooths across samples the same
  way, trading a small amount of display lag for a steadier number;
  the underlying 250ms raw sampling itself is unchanged.

## [0.2.15] — Fix a real SFTP checkExists() false-negative

### Fixed

- **`SftpBackend::checkExists()` reported a permission-denied path as
  "doesn't exist," a real, reachable false negative, not a hypothetical
  one.** `libssh2_sftp_stat()` fails for more reasons than "not
  found" — a `stat()` call needs execute/traverse permission on every
  ancestor directory, not read/write permission on the target itself,
  so a path can be denied without being missing. Only
  `LIBSSH2_FX_NO_SUCH_FILE` is now treated as confirmed nonexistence;
  any other stat failure (permission denied, or the server's own
  generic "something went wrong" code) reports `exists=true` instead —
  the safe direction to be wrong in, since the one place this feeds
  into (the Overwrite/Skip conflict prompt before a transfer) would
  otherwise have silently overwritten something that was actually
  there. Confirmed against a real server: a genuine `chmod 000`
  directory reproduces a real `EACCES` from `libssh2_sftp_stat()`, not
  simulated.

## [0.2.14] — FTPS against real vendor servers now works end to end

### Fixed

- **Three real, independent bugs in how `FtpBackend` handles a data
  connection a real server closes uncleanly (no TLS `close_notify`)
  once it's genuinely done with it, found chasing FTPS against real
  vsftpd/proftpd containers (not reasoned about in advance):**
  1. `listDirectoryInternal()`'s and `downloadFile()`'s read loops used
     to block exclusively on the data connection, only checking the
     control connection's reply *after* giving up — discarding a
     perfectly good, already-arrived, specific server error in favor of
     a generic "timed out" message whenever the server's close didn't
     also produce a clean disconnect Qt's `waitForReadyRead()` would
     notice promptly (confirmed via strace: a real vsftpd closing a
     completed data connection without a TLS `close_notify` first).
     Both now poll in short slices and check the control connection for
     a decisive reply in between; `downloadFile()` additionally exits
     early once it's received as many bytes as the server's own `SIZE`
     reply promised, since that's an unambiguous "done" regardless of
     what the connection's close behavior does or doesn't signal.
  2. `uploadFile()` closed its data socket abruptly (`close()`) rather
     than gracefully (`disconnectFromHost()`, which sends a proper TLS
     `close_notify` before the TCP connection actually closes) — against
     a real vsftpd container, the abrupt close made every
     otherwise-correct upload fail with vsftpd's own `"Failure reading
     network stream"` error. Same underlying problem as (1), just
     triggered from the client's side of the connection instead of the
     server's.
- **FTPS against a real vendor server (vsftpd) now completes a genuine
  full round trip — connect, list, download, upload, content verified
  both ways.** Previously only ever tested against this project's own
  pyftpdlib stand-in. Also confirmed, honestly: this project's TLS
  session-ticket reuse does NOT satisfy a genuinely strict server
  (proftpd's `mod_tls`, left at its own default strict enforcement) — a
  real, informative negative result, not a bug to fix. See
  ARCHITECTURE.md's Known Gaps for the complete investigation,
  including a real, confirmed deadlock inside vsftpd's own
  privilege-separated architecture (unrelated to this project's code)
  that shows up specifically when its own `require_ssl_reuse` strict
  mode is enabled in this container environment.

## [0.2.13] — Fix a misleading FTP active-mode error message

### Fixed

- **A dead/timed-out FTP control connection was misreported as an IPv4
  problem.** `openDataChannel()`'s PASV failure check treated "no reply
  at all" (control connection dropped) the same as "server replied with
  an error code" (genuine PASV refusal), funneling both into the
  active/PORT fallback — despite the code's own comment claiming
  otherwise. On a dead connection, the active-mode path's IPv4 check
  then read the disconnected socket's empty `localAddress()` as "not
  IPv4" and reported `"Active mode requires an IPv4 control connection"`
  instead of the real problem. Found chasing a real anomaly hit during
  manual GUI testing, confirmed against a standalone Qt program rather
  than guessed: `QSslSocket::localAddress().protocol()` genuinely
  returns `IPv4Protocol` for a live `127.0.0.1` connection but
  `UnknownNetworkLayerProtocol` once disconnected. Now reports
  `"Lost connection to the server"` directly when the control
  connection itself is gone, and only falls back to active mode on an
  actual PASV refusal reply.

## [0.2.12] — Fix a crash on transfer after disconnect/reconnect

### Fixed

- **Crash on transferring a file after disconnecting and reconnecting.**
  `TransferManager` tracked the backend executing the active transfer
  (`m_currentBackend`) as a plain raw pointer. Disconnecting a pane
  (`FilePaneWidget::setBackend()`) `deleteLater()`s its old backend;
  reconnecting creates a new one at a different address. The next
  transfer's `connectToBackend()` still tried to `disconnect()` the
  stale pointer from the destroyed backend — a use-after-free, SIGSEGV.
  Found via manual GUI testing (connect → transfer → disconnect →
  reconnect → transfer), not caught by the existing headless test
  suite, none of which exercises a disconnect/reconnect cycle mid-session.
  Fixed by making `m_currentBackend` a `QPointer<RemoteBackend>`, which
  self-nulls when the backend it points to is destroyed.

## [0.2.11] — Fix a real FTP fallback gap; add a real-vendor test testbed

### Fixed

- **FTP's `MLSD`-not-supported fallback missed a real, plausible server
  response.** `FtpBackend` fell back to the legacy `LIST` format only on
  a `500`/`502` ("command not implemented") reply to `MLSD` — a server
  that has `MLSD` but is configured to explicitly deny it (a real
  proftpd configuration, not a hypothetical one) replies `550`
  instead, which wasn't handled: the listing would fail outright rather
  than falling back. Found and fixed via a new real-vendor test
  container, not reasoned about in advance.

### Changed (developer-facing only, no shipped behavior)

- **A new local container testbed for testing against real, independent
  FTP/SFTP server implementations** — real vsftpd, proftpd, and
  Dropbear, each in its own throwaway `podman` container
  (`tools/local-test-servers/start-vsftpd.sh`/`start-proftpd.sh`/
  `start-dropbear.sh`), plus two new automated verification harnesses
  (`verify-ftp-vendors`, `verify-sftp-vendors`). Closes the gap that
  this project's FTP/FTPS test coverage was previously all against one
  Python-based stand-in server (pyftpdlib), never a genuinely different
  vendor's implementation.

## [0.2.10] — Fix SFTP/FTP display parity bugs; leaner CI

### Fixed

- **SFTP entries never reported being a symlink** (`isSymlink` was
  always `false`), unlike the local and FTP backends, which both detect
  it correctly. No visible effect yet — nothing in the UI reads this
  field today — but a real data-model inconsistency worth closing.
- **FTP's MLSD-sourced modification times displayed in UTC instead of
  local time**, unlike the local and SFTP panes, which both show local
  time for the same instant. A file's modification time could show a
  different hour (and a trailing "Z") in the FTP pane than the exact
  same moment would show in the other two.

### Changed

- **CI no longer runs the full build+test pipeline on every push to
  `main`** — only on an actual release tag or a pull request, to save
  time and Actions minutes on routine commits. Still runnable on demand
  anytime via `gh workflow run build.yml` (see CONTRIBUTING.md).

## [0.2.9] — Fix local pane permissions display

### Fixed

- **The local (your-computer) pane's permissions column only ever showed
  a single `r` or `-` character** — just the owner's read bit, discarding
  write/execute and the entire group/other columns — unlike the SFTP
  pane, which shows a full `rwxr-xr-x`-style string. Both panes now
  render permissions the same way.

## [0.2.8] — Close the remaining FTP/FTPS gaps

### Added

- **FTPS now has a real trust-on-first-use prompt for an unverifiable
  certificate**, the same model SSH host keys already use. A self-signed
  or otherwise unverifiable certificate is no longer a silent, permanent
  refusal — you're shown its fingerprint and asked to trust it, once;
  that decision is remembered (`known_certs.json`, alongside
  `known_hosts`) and checked again on every future connection. If the
  certificate ever changes, you get the same kind of strong warning a
  changed SSH host key produces, defaulting to no.
- **FTP now falls back to active (`PORT`) mode** if a server outright
  refuses passive mode — passive stays the default for every server that
  accepts it (still the NAT-friendly choice essentially every FTP client
  makes), but a server that genuinely requires active mode now works
  instead of failing outright.
- **FTPS data connections now attempt to reuse the control connection's
  TLS session**, addressing an anti-hijacking check some strict FTPS
  servers require (RFC 4217). Best-effort — see ARCHITECTURE.md for what
  is and isn't confirmed about it.

### Fixed

- **A crash in the new active-mode data-connection path** — caught by
  this release's own live-server verification before it ever shipped,
  not by a user report.

### Verified

- **A full encrypted transfer over FTPS, with a certificate that
  genuinely validates** — previously only the `AUTH TLS` handshake and
  expected self-signed rejection had been confirmed; the actual
  encrypted-data-channel transfer path had not.
- **The legacy `LIST` directory-listing fallback, against a real server
  that genuinely doesn't support the modern `MLSD` format** — previously
  only unit-tested against crafted sample data.

## [0.2.7] — Fix Site Manager field height glitches

### Fixed

- **Site Manager's Port field rendered noticeably taller than every
  other field, and the starting-directory field rendered too short.**
  Both real, reported layout bugs — the Port spinbox is now the same
  height as its siblings, and the dialog is tall enough that Qt's
  layout no longer has to squeeze the starting-directory field to fit.

## [0.2.6] — Optional password saving; relicensed to GPL-3.0-or-later

### Added

- **Site Manager can now remember a site's password or key passphrase**,
  opt-in via a new "Save password" checkbox (unchecked by default).
  Nothing is ever written to ZephyrFTP's own config file — the secret
  goes into your operating system's own protected credential store
  (the same kind of secure storage your browser or system keychain
  uses), and you're still asked to confirm it every time you connect
  (just pre-filled, never silent). Unchecking the box removes anything
  saved for that site immediately.

### Changed

- **License changed from MIT to GPL-3.0-or-later.** See
  [LICENSE](LICENSE). The vendored Tabler Icons set is unaffected,
  still under its own original MIT license.

## [0.2.5] — Fix a real crash closing the app while connected

### Fixed

- **Closing the app while connected to a server no longer crashes.**
  It previously aborted (SIGABRT) every time — confirmed from a real
  crash report, not a hypothetical. The connection's background thread
  wasn't being shut down cleanly before the window closed; it now is.

## [0.2.4] — Real cancel/pause/resume and transfer-queue rendering verified

### Added

- **SFTP cancel and pause/resume verified against a real server**, in
  both directions — not just `TransferManager`'s orchestration against
  a fake backend. A genuinely interrupted, genuinely resumed,
  byte-for-byte-correct transfer confirmed for real using
  `tools/local-test-servers/`. Dev-only tooling; nothing shipped
  changed.
- **The transfer queue's progress bars and status icons confirmed
  rendering correctly with a real active transfer** (previously only
  checked with an empty queue). Also dev-only — a one-off verification
  pass, not new tooling kept in the repo.

## [0.2.3] — FTP/FTPS and public-key SFTP auth verified against a real server

### Added

- **Local throwaway SFTP/FTP/FTPS test servers** (`tools/local-test-servers/`)
  and harnesses that drive the real backend code against them — closing
  four gaps that were previously untested against any real server:
  SFTP public-key authentication, `checkExists()`, the recursive
  folder-transfer walk, and FTP/FTPS (control connection, real
  transfers, and the `AUTH TLS` handshake). Dev-only tooling — nothing
  shipped or user-facing changed, but see the Changed entry below for
  what this means for FTP/FTPS's real-world status.

### Changed

- **FTP/FTPS's Known Limitations wording updated** to reflect that it's
  now confirmed against a real server (previously "completely untested,
  not once") — still not tried against a real-world production server,
  the older `LIST`-format fallback parser specifically, or a fully
  encrypted FTPS transfer with a trusted certificate.

## [0.2.2] — A Connection menu, next to Help

### Added

- **A "Connection" menu in the menu bar**, next to Help — Connect...,
  Sites..., and Disconnect, the same three actions the toolbar buttons
  already offer, now also reachable via the menu bar/keyboard. The
  toolbar itself is unchanged.

## [0.2.1] — Linux builds now published alongside Windows

### Added

- **A Linux build is now attached to every release**, alongside the
  Windows one — `zephyrftp-linux-x64.tar.gz`, a single dynamically
  linked binary (needs system Qt6/libssh2 already installed; see the
  README's Download section for exact packages). Built and tested by CI
  the same way the Windows build is, not just compiled.

## [0.2.0] — FTP/FTPS wired to the UI, Windows CI moved off MSVC+vcpkg

### Added

- **FTP and FTPS can now be selected** in both the connection dialog and
  the Site Manager, and saved per site. The backend behind them already
  existed; what landed here is the protocol choice and everything it
  drives — the port defaulting to 21 (or 22 for SFTP) unless you've
  typed your own, the authentication choice disappearing for protocols
  that only do passwords, and the right backend actually being
  constructed when you connect. FTPS means explicit TLS (`AUTH TLS`) on
  the normal FTP port.
  **Not yet verified against a real FTP or FTPS server** — see the
  README's Known limitations, which says so plainly rather than letting
  the feature's presence imply otherwise.

### Changed

- Sites saved before this release load unchanged and read back as SFTP,
  which is what they were — no migration step, nothing to re-enter.
- FTPS certificate failures now name the actual problem (for example, a
  self-signed certificate) instead of reporting a generic TLS error.
  ZephyrFTP still refuses to connect in that case; only the explanation
  improved.
- The Windows build (this release's `.exe`) now comes from MinGW
  cross-compilation on Linux CI instead of MSVC+vcpkg on a Windows
  runner — same test suite, now actually run under `wine` as part of
  the build rather than only linked. No user-visible behavior change;
  noted here because it's a real change in how the shipped binary is
  produced.

## [0.1.0] — first tagged release

The first release with an actual version number and a place to point
people who want a specific, known-working build rather than whatever's
on `main` at any given moment. "0.1.0," not "1.0.0," on purpose — this
is alpha software. Real functionality, real (if incomplete) test
coverage, but real gaps too; see the README's Known Limitations section
for the honest list, which this changelog doesn't duplicate.

### Added

- Dual-pane file browsing (your computer on one side, a server on the
  other), with back/forward/up navigation per pane
- SFTP support — password or private-key authentication, real
  trust-on-first-use host-key verification (not silently accepted or
  silently rejected)
- Site Manager — saved connections, organized into groups, each with an
  optional starting directory
- A real transfer queue: live progress and speed, pause/resume (server
  transfers), cancel, retry
- Whole-folder transfers — drag or select a folder and everything
  inside transfers recursively, mirroring the nested structure
  including empty subdirectories
- Destination conflict handling — asked before anything gets
  overwritten (files: Overwrite/Skip; folders: Write Into/Skip), with a
  "apply to the rest of this batch" option
- File management from the right-click menu: create file/folder,
  rename, delete (non-recursive for folders, on purpose), refresh
- A real dark theme and icon set, not a default-Qt-widgets look
- A Windows CI pipeline producing an actual runnable `.exe`, verified
  against real hardware

### Known unverified

Not a defect list — just what genuinely hasn't been proven yet, so
nobody mistakes silence for a claim of correctness:

- Public-key authentication has never been tried against a real key file
- Pause/resume and cancel mid-transfer have real implementations for
  SFTP but haven't been exercised against a live server interrupting an
  actual in-flight transfer
- FTP/FTPS support exists at the backend level (hand-rolled on
  `QTcpSocket`/`QSslSocket`, no UI wiring yet) but has never touched a
  real FTP server

[Unreleased]: https://github.com/arelas/zephyrftp/compare/v0.8.4...HEAD
[0.8.4]: https://github.com/arelas/zephyrftp/releases/tag/v0.8.4
[0.8.3]: https://github.com/arelas/zephyrftp/releases/tag/v0.8.3
[0.8.2]: https://github.com/arelas/zephyrftp/releases/tag/v0.8.2
[0.8.1]: https://github.com/arelas/zephyrftp/releases/tag/v0.8.1
[0.8.0]: https://github.com/arelas/zephyrftp/releases/tag/v0.8.0
[0.7.45]: https://github.com/arelas/zephyrftp/releases/tag/v0.7.45
[0.7.44]: https://github.com/arelas/zephyrftp/releases/tag/v0.7.44
[0.7.43]: https://github.com/arelas/zephyrftp/releases/tag/v0.7.43
[0.7.42]: https://github.com/arelas/zephyrftp/releases/tag/v0.7.42
[0.7.41]: https://github.com/arelas/zephyrftp/releases/tag/v0.7.41
[0.7.40]: https://github.com/arelas/zephyrftp/releases/tag/v0.7.40
[0.7.39]: https://github.com/arelas/zephyrftp/releases/tag/v0.7.39
[0.7.38]: https://github.com/arelas/zephyrftp/releases/tag/v0.7.38
[0.7.37]: https://github.com/arelas/zephyrftp/releases/tag/v0.7.37
[0.7.36]: https://github.com/arelas/zephyrftp/releases/tag/v0.7.36
[0.7.35]: https://github.com/arelas/zephyrftp/releases/tag/v0.7.35
[0.7.33]: https://github.com/arelas/zephyrftp/releases/tag/v0.7.33
[0.7.31]: https://github.com/arelas/zephyrftp/releases/tag/v0.7.31
[0.7.30]: https://github.com/arelas/zephyrftp/releases/tag/v0.7.30
[0.7.29]: https://github.com/arelas/zephyrftp/releases/tag/v0.7.29
[0.7.28]: https://github.com/arelas/zephyrftp/releases/tag/v0.7.28
[0.7.27]: https://github.com/arelas/zephyrftp/releases/tag/v0.7.27
[0.7.26]: https://github.com/arelas/zephyrftp/releases/tag/v0.7.26
[0.7.25]: https://github.com/arelas/zephyrftp/releases/tag/v0.7.25
[0.7.24]: https://github.com/arelas/zephyrftp/releases/tag/v0.7.24
[0.7.23]: https://github.com/arelas/zephyrftp/releases/tag/v0.7.23
[0.7.22]: https://github.com/arelas/zephyrftp/releases/tag/v0.7.22
[0.7.21]: https://github.com/arelas/zephyrftp/releases/tag/v0.7.21
[0.7.11]: https://github.com/arelas/zephyrftp/releases/tag/v0.7.11
[0.7.10]: https://github.com/arelas/zephyrftp/releases/tag/v0.7.10
[0.7.9]: https://github.com/arelas/zephyrftp/releases/tag/v0.7.9
[0.7.8]: https://github.com/arelas/zephyrftp/releases/tag/v0.7.8
[0.7.7]: https://github.com/arelas/zephyrftp/releases/tag/v0.7.7
[0.7.6]: https://github.com/arelas/zephyrftp/releases/tag/v0.7.6
[0.7.5]: https://github.com/arelas/zephyrftp/releases/tag/v0.7.5
[0.7.4]: https://github.com/arelas/zephyrftp/releases/tag/v0.7.4
[0.7.3]: https://github.com/arelas/zephyrftp/releases/tag/v0.7.3
[0.7.2]: https://github.com/arelas/zephyrftp/releases/tag/v0.7.2
[0.7.1]: https://github.com/arelas/zephyrftp/releases/tag/v0.7.1
[0.7.0]: https://github.com/arelas/zephyrftp/releases/tag/v0.7.0
[0.6.29]: https://github.com/arelas/zephyrftp/releases/tag/v0.6.29
[0.6.28]: https://github.com/arelas/zephyrftp/releases/tag/v0.6.28
[0.6.27]: https://github.com/arelas/zephyrftp/releases/tag/v0.6.27
[0.6.26]: https://github.com/arelas/zephyrftp/releases/tag/v0.6.26
[0.6.25]: https://github.com/arelas/zephyrftp/releases/tag/v0.6.25
[0.6.24]: https://github.com/arelas/zephyrftp/releases/tag/v0.6.24
[0.6.23]: https://github.com/arelas/zephyrftp/releases/tag/v0.6.23
[0.6.22]: https://github.com/arelas/zephyrftp/releases/tag/v0.6.22
[0.6.21]: https://github.com/arelas/zephyrftp/releases/tag/v0.6.21
[0.6.20]: https://github.com/arelas/zephyrftp/releases/tag/v0.6.20
[0.6.19]: https://github.com/arelas/zephyrftp/releases/tag/v0.6.19
[0.6.18]: https://github.com/arelas/zephyrftp/releases/tag/v0.6.18
[0.6.17]: https://github.com/arelas/zephyrftp/releases/tag/v0.6.17
[0.6.16]: https://github.com/arelas/zephyrftp/releases/tag/v0.6.16
[0.6.15]: https://github.com/arelas/zephyrftp/releases/tag/v0.6.15
[0.6.14]: https://github.com/arelas/zephyrftp/releases/tag/v0.6.14
[0.6.13]: https://github.com/arelas/zephyrftp/releases/tag/v0.6.13
[0.6.12]: https://github.com/arelas/zephyrftp/releases/tag/v0.6.12
[0.6.11]: https://github.com/arelas/zephyrftp/releases/tag/v0.6.11
[0.6.10]: https://github.com/arelas/zephyrftp/releases/tag/v0.6.10
[0.6.9]: https://github.com/arelas/zephyrftp/releases/tag/v0.6.9
[0.6.8]: https://github.com/arelas/zephyrftp/releases/tag/v0.6.8
[0.6.7]: https://github.com/arelas/zephyrftp/releases/tag/v0.6.7
[0.6.6]: https://github.com/arelas/zephyrftp/releases/tag/v0.6.6
[0.6.5]: https://github.com/arelas/zephyrftp/releases/tag/v0.6.5
[0.6.4]: https://github.com/arelas/zephyrftp/releases/tag/v0.6.4
[0.6.3]: https://github.com/arelas/zephyrftp/releases/tag/v0.6.3
[0.6.2]: https://github.com/arelas/zephyrftp/releases/tag/v0.6.2
[0.6.1]: https://github.com/arelas/zephyrftp/releases/tag/v0.6.1
[0.6.0]: https://github.com/arelas/zephyrftp/releases/tag/v0.6.0
[0.5.2]: https://github.com/arelas/zephyrftp/releases/tag/v0.5.2
[0.5.1]: https://github.com/arelas/zephyrftp/releases/tag/v0.5.1
[0.5.0]: https://github.com/arelas/zephyrftp/releases/tag/v0.5.0
[0.4.0]: https://github.com/arelas/zephyrftp/releases/tag/v0.4.0
[0.3.9]: https://github.com/arelas/zephyrftp/releases/tag/v0.3.9
[0.3.8]: https://github.com/arelas/zephyrftp/releases/tag/v0.3.8
[0.3.7]: https://github.com/arelas/zephyrftp/releases/tag/v0.3.7
[0.3.6]: https://github.com/arelas/zephyrftp/releases/tag/v0.3.6
[0.3.5]: https://github.com/arelas/zephyrftp/releases/tag/v0.3.5
[0.3.4]: https://github.com/arelas/zephyrftp/releases/tag/v0.3.4
[0.3.3]: https://github.com/arelas/zephyrftp/releases/tag/v0.3.3
[0.3.2]: https://github.com/arelas/zephyrftp/releases/tag/v0.3.2
[0.3.1]: https://github.com/arelas/zephyrftp/releases/tag/v0.3.1
[0.3.0]: https://github.com/arelas/zephyrftp/releases/tag/v0.3.0
[0.2.16]: https://github.com/arelas/zephyrftp/releases/tag/v0.2.16
[0.2.15]: https://github.com/arelas/zephyrftp/releases/tag/v0.2.15
[0.2.14]: https://github.com/arelas/zephyrftp/releases/tag/v0.2.14
[0.2.13]: https://github.com/arelas/zephyrftp/releases/tag/v0.2.13
[0.2.12]: https://github.com/arelas/zephyrftp/releases/tag/v0.2.12
[0.2.11]: https://github.com/arelas/zephyrftp/releases/tag/v0.2.11
[0.2.10]: https://github.com/arelas/zephyrftp/releases/tag/v0.2.10
[0.2.9]: https://github.com/arelas/zephyrftp/releases/tag/v0.2.9
[0.2.8]: https://github.com/arelas/zephyrftp/releases/tag/v0.2.8
[0.2.7]: https://github.com/arelas/zephyrftp/releases/tag/v0.2.7
[0.2.6]: https://github.com/arelas/zephyrftp/releases/tag/v0.2.6
[0.2.5]: https://github.com/arelas/zephyrftp/releases/tag/v0.2.5
[0.2.4]: https://github.com/arelas/zephyrftp/releases/tag/v0.2.4
[0.2.3]: https://github.com/arelas/zephyrftp/releases/tag/v0.2.3
[0.2.2]: https://github.com/arelas/zephyrftp/releases/tag/v0.2.2
[0.2.1]: https://github.com/arelas/zephyrftp/releases/tag/v0.2.1
[0.2.0]: https://github.com/arelas/zephyrftp/releases/tag/v0.2.0
[0.1.0]: https://github.com/arelas/zephyrftp/releases/tag/v0.1.0
