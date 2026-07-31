# Handoff notes

Written at a context-window boundary, for whichever Claude picks this
project up next. Not a technical reference — ARCHITECTURE.md,
README.md, CONTRIBUTING.md, and CHANGELOG.md already cover the
technical state thoroughly and are kept current; read those for
"what's built and how." This file covers what those don't: the
working pattern between David and Claude, and where things stand right
now at a glance.

**Delete or rewrite this file once it's stale** — it's a snapshot, not
a living doc. If it stops matching CHANGELOG.md/git log, trust those.

## Read these first, in this order

1. `README.md` — what the app does, current version, known limitations
2. `ARCHITECTURE.md` — the real technical depth: every component, a
   "Verification status" section that's blunt about what's actually
   proven vs. assumed, a "Known gaps" section same spirit
3. `CONTRIBUTING.md` — build/test commands, exact and copy-pasteable
4. `CHANGELOG.md` — what shipped in each tagged version

## Where things stand right now

**Version 0.1.0, tagged and released.** First real release. Alpha —
David was explicit that this should NOT be 1.0.0 yet, given real,
honestly-documented gaps (see below).

**Core SFTP client is feature-complete and well-tested:** dual-pane
browsing, navigation, Site Manager (saved connections + groups +
starting directory), a real transfer queue (pause/resume/cancel/retry/
speed), whole-folder transfer (recursive, mirrors structure including
empty subdirectories), destination conflict resolution (Overwrite/Skip
for files, Write Into/Skip for folders, with an "apply to all"
option), file management (create/rename/delete — deliberately
non-recursive for folder deletion), a real dark theme, real host-key
TOFU verification. Ten automated test suites, all passing.

**FTP/FTPS backend exists but isn't wired up yet.**
`src/backends/FtpBackend.h/.cpp` implements the full `RemoteBackend`
interface — hand-rolled directly on `QTcpSocket`/`QSslSocket`, not
libcurl (confirmed libcurl doesn't parse FTP's LIST output either
before choosing this, so the one big thing a library might have
bought didn't actually apply). MLSD tried first for directory
listings, LIST as best-effort fallback. Passive mode only. Explicit
FTPS only (`AUTH TLS`). The directory-listing parsers — the highest-risk
part, since FTP's LIST format isn't standardized — are verified via 34
tests against realistic sample data (`ftp-parsing-test`). **What's
missing:** no UI to actually select FTP/FTPS as a protocol
(`ConnectionDialog`/`SiteManagerDialog` only offer SFTP right now), no
`MainWindow` wiring to construct an `FtpBackend`, and zero live-network
verification (no FTP/FTPS server reachable from the dev sandbox). This
is probably the single largest piece of unfinished work — comparable
in scope to what's already in `FtpBackend.cpp`, just on the UI side.

**Windows CI works end-to-end, including the release pipeline now.**
Confirmed on real hardware: builds, launches without a console window,
connects to a real SFTP server. The release job just succeeded for
real after fixing a `GITHUB_TOKEN` permissions gap (see git log —
`contents: write` wasn't declared, defaulted to read-only, 403'd on
release creation; fixed with a job-scoped `permissions:` block, not a
blanket grant).

**Known gaps, most likely to matter next** (full list in
ARCHITECTURE.md — this is just what's probably highest-value):
- FTP/FTPS UI wiring (see above)
- Public-key SFTP auth has never been tried against a real key
- Pause/resume/cancel mid-transfer is unverified against a real SFTP
  server actually interrupting an in-flight transfer (the
  orchestration logic is tested via a fake backend, which is a
  different, narrower claim)
- SFTP throughput: real, measured fixes took it from ~4MB/s to
  ~22-25MB/s (pipelining, buffer alignment to libssh2's actual packet
  size, TCP_NODELAY), still short of a ~40MB/s FileZilla/Termius
  comparison. David explicitly deferred closing this further to a
  "fine-tuning phase" — don't restart this uninvited.
- No macOS/Linux packaging — Windows CI only
- Directory deletion is deliberately non-recursive on every backend —
  a scope decision, not a bug, revisit only if actually asked

## How David and Claude actually work together (not in the other docs)

- **Claude has no direct push access to github.com/arelas/zephyrftp.**
  Every round: edit files in a sandboxed clone at
  `/home/claude/zephyrftp`, run the real test suite, commit locally,
  zip the repo, hand it over via `present_files`. David unzips,
  force-pushes to `main` himself. Git history in this repo is
  therefore Claude's own construction each time, not a continuous
  history — that's expected and fine.
- **Real hardware testing happens on David's end, not Claude's.**
  Claude has no live SFTP/FTP server, no real Windows machine. When
  David reports back concrete results (a real speed number, a real
  error message, "that worked"), that's the actual verification —
  Claude's own testing is necessarily limited to what's reachable from
  a sandboxed Linux container (local filesystem, no network to a real
  server). Being explicit about this boundary in every response has
  been a consistent, deliberate pattern — keep doing that, don't let
  "the tests pass" imply more than it does.
- **The CI YAML has bitten this project more than once** — Windows-
  specific PowerShell quirks, vcpkg pinning, and now the release
  permissions gap. Validate YAML changes by actually parsing them
  (`python3 -c "import yaml; ..."`) before calling them done, and
  favor the option with fewer new failure modes when there's a choice
  (e.g. `generate_release_notes: true` over slicing a changelog
  section out via PowerShell).
- **A recurring CMake gotcha:** any minimal test target that links
  `RemoteBackend.h` (or any other Q_OBJECT header) without also
  including a `.cpp` that uses it needs that header listed explicitly
  in the target's sources, or AUTOMOC won't generate its vtable and
  you'll get a mysterious link error. Hit this many times across new
  test targets — check this first if a new test target won't link.
- **Testing discipline:** before treating anything as done, run the
  full suite (`CONTRIBUTING.md` has the exact commands, including the
  `XDG_CONFIG_HOME` isolation `site-store-test` needs and the fixture
  data `folder-transfer-test` needs built by hand). Screenshot-based UI
  verification (grab a widget, save PNG, pixel-sample it, and actually
  view it) has been used repeatedly for anything visual — worth reusing
  the same pattern rather than skipping visual verification because
  it's fiddlier than a plain assert.
- **David gives direction at the feature level** ("let's add FTP
  support," "cut an actual release") and expects Claude to make the
  detailed technical calls autonomously — but for decisions with real,
  hard-to-reverse infrastructure cost (a new external dependency, a
  versioning scheme), Claude has checked in first rather than
  unilaterally deciding. Worth continuing that judgment call rather
  than defaulting either always-ask or always-decide.

## Starting the next conversation

David: upload the current repo zip (or point Claude at this project's
files) and paste this file's contents, or just say "read HANDOFF.md
first." Everything else needed to get productive fast is either in
this file or the four docs listed at the top.
