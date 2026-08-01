# Changelog

All notable changes to ZephyrFTP are documented here. Format loosely
follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/); this
project doesn't yet promise strict [Semantic Versioning](https://semver.org/)
guarantees — it's pre-1.0 (see [Known limitations](README.md#known-limitations)
in the README), so anything may still change between 0.x releases.

## [Unreleased]

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

[Unreleased]: https://github.com/arelas/zephyrftp/compare/v0.2.1...HEAD
[0.2.1]: https://github.com/arelas/zephyrftp/releases/tag/v0.2.1
[0.2.0]: https://github.com/arelas/zephyrftp/releases/tag/v0.2.0
[0.1.0]: https://github.com/arelas/zephyrftp/releases/tag/v0.1.0
