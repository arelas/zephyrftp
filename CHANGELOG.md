# Changelog

All notable changes to ZephyrFTP are documented here. Format loosely
follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/); this
project doesn't yet promise strict [Semantic Versioning](https://semver.org/)
guarantees — it's pre-1.0 (see [Known limitations](README.md#known-limitations)
in the README), so anything may still change between 0.x releases.

## [Unreleased]

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

[Unreleased]: https://github.com/arelas/zephyrftp/compare/v0.2.7...HEAD
[0.2.7]: https://github.com/arelas/zephyrftp/releases/tag/v0.2.7
[0.2.6]: https://github.com/arelas/zephyrftp/releases/tag/v0.2.6
[0.2.5]: https://github.com/arelas/zephyrftp/releases/tag/v0.2.5
[0.2.4]: https://github.com/arelas/zephyrftp/releases/tag/v0.2.4
[0.2.3]: https://github.com/arelas/zephyrftp/releases/tag/v0.2.3
[0.2.2]: https://github.com/arelas/zephyrftp/releases/tag/v0.2.2
[0.2.1]: https://github.com/arelas/zephyrftp/releases/tag/v0.2.1
[0.2.0]: https://github.com/arelas/zephyrftp/releases/tag/v0.2.0
[0.1.0]: https://github.com/arelas/zephyrftp/releases/tag/v0.1.0
