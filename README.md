# ZephyrFTP

A dual-pane SFTP client for Windows and Linux — browse your local files
and a remote server side by side, then drag, drop, or double-click to
move things between them. Think FileZilla or WinSCP, built fresh in Qt6.

**Current version: 0.2.10 — alpha.** Real functionality, but real gaps
too — see [Known limitations](#known-limitations) before relying on
this for anything you can't afford to get wrong. [Releases](https://github.com/arelas/zephyrftp/releases)
has downloadable Windows and Linux builds; [CHANGELOG.md](CHANGELOG.md)
tracks what's changed between them.

## Features

- **Dual-pane browsing** — your computer on one side, the server on the
  other, both visible at once. Back, forward, and up buttons beside each
  pane's location bar, same as any file manager.
- **File management from the right-click menu** — create a new file or
  folder, rename, or delete, on either side (your computer or the
  server). Delete works on multiple selected items at once, with a
  confirmation first. Deleting a folder only works if it's empty —
  there's no "delete everything inside it too" here, on purpose, so a
  misclick can't wipe out more than you meant to remove.
- **SFTP with password or private-key login.**
- **Site Manager** — save connections you use often, organized into
  folders (type a new folder name right on the site's details, or pick
  an existing one from the dropdown), so you don't have to re-type the
  host, port, and username every time. Each saved site can also default
  to a specific starting folder on the server instead of your home
  directory — handy if you always go straight to the same upload folder.
  Your password (or a private key's passphrase) is never written to
  ZephyrFTP's own config file — you'll always still be asked for it when
  you connect. Check "Save password" and it's remembered for next time
  using your operating system's own secure credential storage (the same
  kind of protected store your browser or system keychain uses, not a
  plain file this app controls) — that prompt still appears every time,
  just pre-filled, so it's never a silent auto-connect. Leave the box
  unchecked and nothing changes from before.
- **Host-key verification** — the first time you connect to a server,
  ZephyrFTP shows you its identity fingerprint and asks you to confirm
  it. If that fingerprint ever changes on a later connection, you get a
  clear warning instead of a silent, invisible risk. This is the same
  protection SSH itself uses, and it's on by default here.
- **Certificate verification for FTPS**, the same trust-on-first-use idea
  as host-key verification above — the first time you connect to a
  server whose certificate can't be automatically verified (a self-signed
  one, for example), you're shown its fingerprint and asked to trust it.
  That decision is remembered for next time, and you get a clear warning
  if the certificate ever changes.
- **Drag-and-drop or multi-select transfers — whole folders too, not just
  files.** Drag a folder from one pane to the other (or select several,
  mixing files and folders freely) and everything inside gets recreated
  on the other side, nested structure and all, including empty
  subfolders. Each file inside shows up in the transfer queue like any
  other transfer — same pause/resume/cancel, same progress and speed.
- **You're asked before anything gets overwritten.** If a file already
  exists at the destination, you'll be asked to Overwrite or Skip it —
  with a checkbox to apply that answer to everything else in the same
  batch, so you're not clicking through a dialog for every file. Folders
  work the same way: if a folder with that name already exists, you
  choose to write into it (merging with what's already there) or skip it
  entirely.
- **A real transfer queue** — see what's moving and how fast, pause a
  transfer to a server and pick it back up later without starting over,
  cancel something outright, or retry something that failed, all from
  one place.
- **Dark theme**, built around a small set of colors that mean the same
  thing everywhere in the app (green = connect/upload/success, red =
  disconnect/delete/error, blue = navigation/download, amber = caution)
  so you can read the state of things at a glance.

## Download

Grab the latest build from [Releases](https://github.com/arelas/zephyrftp/releases):

- **Windows**: `zephyrftp-windows-x64.zip` — unzip it, no installer, just
  launch `zephyrftp.exe`.
- **Linux**: `zephyrftp-linux-x64.tar.gz` — extracts to a single
  `zephyrftp` binary. Dynamically linked against system Qt6/libssh2
  (not a bundled/portable build), so it needs those already installed —
  the exact packages CONTRIBUTING.md's build instructions use. If your
  distro doesn't have matching versions, build from source instead (see
  [CONTRIBUTING.md](CONTRIBUTING.md)), which is quick on any recent distro.

Every release is built and tested (the full automated test suite, not
just "it compiled") by [`.github/workflows/build.yml`](.github/workflows/build.yml)
before being attached — see ARCHITECTURE.md's "Windows and Linux builds
(CI)" section for exactly what that verifies.

## Getting started

1. **Launch the app.** The left pane shows your local files right away —
   ZephyrFTP works as a plain two-pane local file manager even before
   you connect to anything.
2. **Click Connect** in the toolbar (or the **Connection** menu) for a
   one-off connection — fill in the server's host, port (22 by default),
   your username, and either a password or a private key file. If it's a
   server you'll come back to, use **Sites** instead: same information,
   but saved for next time (a "New Site" button, and a tree on the left
   to organize them).
3. **First connection to a new server?** You'll be asked to confirm its
   identity fingerprint. This is expected and normal — it's the same
   prompt any SSH client shows the first time. Confirming it once means
   ZephyrFTP will recognize that server automatically on future
   connections, and will warn you loudly if its identity ever changes.
4. **Browse and transfer.** Once connected, the right pane shows the
   remote server. Double-click a file to send it to the other pane, drag
   files between panes, or select several and right-click → "Transfer
   Selected."
5. **Watch the Transfers panel** at the bottom for progress and speed.
   Right-click any transfer to cancel it, pause it (server transfers
   only — see Known limitations), resume a paused one, or retry
   something that failed or that you skipped.

## Known limitations

This is young software — a few things intentionally aren't supported yet
rather than being half-implemented:

- **FTP and FTPS work against a real server, but only controlled local
  ones so far — not yet a production server out in the wild.** Connecting,
  browsing, and transferring files over plain FTP, active-mode fallback,
  the legacy `LIST` directory-listing fallback, and a full encrypted
  transfer over FTPS (including trusting a certificate and reusing that
  trust) have all been confirmed against real FTP/FTPS servers, not just
  tested in isolation. Still newer and less battle-tested than SFTP;
  expect some rough edges against servers this hasn't specifically been
  tried against — a real-world server's exact `LIST` output format in
  particular varies enough between vendors that this hasn't seen every
  variant.
- **FTPS data connections reusing the control connection's TLS session
  is best-effort, not guaranteed.** Some strict FTPS servers require this
  (RFC 4217) as an anti-hijacking measure; ZephyrFTP now attempts real
  session-ticket reuse, but whether that satisfies a genuinely strict
  server hasn't been confirmed either way.
- **Server-to-server transfers aren't supported** — ZephyrFTP always
  transfers between your computer and one server, not between two
  remote servers directly.
- **Pause only works for transfers involving a server** — you can pause
  an upload or download to/from a server and pick it back up later, but
  a purely local copy (between two folders on your own computer) can
  only be cancelled and retried, not paused, since there's nothing
  meaningful to interrupt mid-copy on a local transfer.

## For developers and tinkerers

Want to build it from source, understand how it works internally, or
contribute? Start with [CONTRIBUTING.md](CONTRIBUTING.md) for build/test
instructions, then [ARCHITECTURE.md](ARCHITECTURE.md) for the full
technical picture — what's been verified and how, the design of each
component, and every known gap in detail.

## License

GPL-3.0-or-later — see [LICENSE](LICENSE).

The vendored icon set (`resources/icons/`) is [Tabler Icons](https://tabler.io/icons),
MIT-licensed (a permissive license, fully compatible with being bundled
into a GPL project); its license text is included separately at
`resources/icons/LICENSE-tabler-icons.txt` since it's a distinct
copyright holder from this project, still under its own original terms.
