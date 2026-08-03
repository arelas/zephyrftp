# Changelog

All notable changes to ZephyrFTP are documented here. Format loosely
follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/); this
project doesn't yet promise strict [Semantic Versioning](https://semver.org/)
guarantees — it's pre-1.0 (see [Known limitations](README.md#known-limitations)
in the README), so anything may still change between 0.x releases.

## [Unreleased]

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

[Unreleased]: https://github.com/arelas/zephyrftp/compare/v0.3.1...HEAD
[0.3.1]: https://github.com/arelas/zephyrftp/releases/tag/v0.3.1
[0.2.7]: https://github.com/arelas/zephyrftp/releases/tag/v0.2.7
[0.2.6]: https://github.com/arelas/zephyrftp/releases/tag/v0.2.6
[0.2.5]: https://github.com/arelas/zephyrftp/releases/tag/v0.2.5
[0.2.4]: https://github.com/arelas/zephyrftp/releases/tag/v0.2.4
[0.2.3]: https://github.com/arelas/zephyrftp/releases/tag/v0.2.3
[0.2.2]: https://github.com/arelas/zephyrftp/releases/tag/v0.2.2
[0.2.1]: https://github.com/arelas/zephyrftp/releases/tag/v0.2.1
[0.2.0]: https://github.com/arelas/zephyrftp/releases/tag/v0.2.0
[0.1.0]: https://github.com/arelas/zephyrftp/releases/tag/v0.1.0
