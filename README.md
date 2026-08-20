# ZephyrFTP

A dual-pane SFTP client for Windows, macOS, and Linux — browse your
local files and a remote server side by side, then drag, drop, or
double-click to move things between them. Think FileZilla or WinSCP,
built fresh in Qt6.

**Current version: 0.8.5 — beta.** Real functionality, with real but
narrowing gaps — see [Known limitations](#known-limitations) before
relying on this for anything you can't afford to get wrong. [Releases](https://github.com/arelas/zephyrftp/releases)
has downloadable Windows, macOS, and Linux builds; [CHANGELOG.md](CHANGELOG.md)
tracks what's changed between them.

**The project is now in feature freeze, heading toward 1.0.** From here,
releases are bug fixes and small additions closing existing gaps, not
new capabilities. A Windows installer (`.exe`, alongside the existing
unzip-and-run `.zip`) is now available — it's not yet signed, so
Windows SmartScreen will flag it until a code-signing path (SignPath
Foundation or Azure Trusted Signing) is set up, planned at some point
during this phase.

![Dual-pane browsing, connected to a real SFTP server](docs/screenshots/dual-pane-browsing.png)

![A real transfer in progress in the Transfers panel](docs/screenshots/active-transfer.png)

## Features

- **Dual-pane browsing** — your computer, a server, or two servers at
  once, side by side. Either pane can connect independently (click its
  own path-bar icon), so you're not limited to "your computer on one
  side, a server on the other" — connect both panes to different servers
  and transfer between them directly. Back, forward, up, home, and
  refresh buttons beside each pane's location bar, same as any file
  manager.
- **Filter by name** — a field under each pane's path bar narrows a
  busy directory listing as you type (case-insensitive, matches
  anywhere in the name). Each pane's filter is independent, so you can
  narrow one side without affecting the other. Hide it if you don't
  want it via View → Filename Filter.
- **File management from the right-click menu** — create a new file or
  folder, rename, or delete, on either side (your computer or the
  server). Delete works on multiple selected items at once, with a
  confirmation first. A folder that turns out to have contents isn't
  just refused — you get a second, more specific warning telling you
  exactly how much is inside ("isn't empty — it contains 12 files and 3
  folders") before permanently deleting the whole thing, so a misclick
  can't wipe out more than you actually meant to.
- **Keyboard shortcuts** — Delete to delete the current selection, F2 to
  rename, F5 to refresh, Ctrl+A to select everything in the focused
  pane, and Ctrl+C then Ctrl+V to copy a selection from one pane and
  transfer it into the other (paste into the same pane you copied from
  isn't offered — copy always means "send to the other side"). Every one
  of these is the exact same action already available from the
  right-click menu or toolbar; the shortcut is just a faster way to
  reach it.
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
- **Import/export saved sites** — Export... in Site Manager writes your
  whole site list to a JSON file (safe to hand to someone else or move
  to another machine: it's the same secret-free format `sites.json`
  itself already is — no password is ever in it, saved or not). Import...
  reads one back in and adds those sites to what you already have,
  never replacing it; each imported site gets a fresh identity, so it
  starts fresh with no assumptions about a password having come along
  with it — you'll just be prompted the first time you connect, same as
  any new site.
- **Quick connect** — a toolbar row for a fast one-off connection: Host,
  Username, Password, Port, and Protocol fields plus a Connect button.
  Fill in what you need and press Enter (or click Connect) — it
  connects immediately, no separate dialog to open or password prompt
  to dismiss first. Protocol defaults to whatever Preferences has set
  as your default protocol. Hide the row if you don't want it via
  View → Quick Connect Toolbar.
- **Recent Connections** — every pane's own Sites/Connect/Disconnect
  menu (click the icon at the left of the path bar) now has a Recent
  Connections submenu listing the last 10 servers you connected to
  ad-hoc (Quick Connect or the plain Connect... dialog) without saving
  them as a site. Pick one to reconnect instantly — you're prompted for
  the password (or key passphrase) again, the same as any first
  connection; nothing is ever stored. Connecting to something already
  in the list just moves it back to the top instead of duplicating it.
  Sites you've explicitly saved in Site Manager aren't duplicated
  here — that list is already one click away there.
- **Proxy support (SOCKS5 or HTTP CONNECT)** — set one in Preferences
  and every SFTP/FTP/FTPS connection goes through it, no per-site setup
  needed. For anyone behind a corporate network that requires an
  outbound proxy, this is the difference between being able to use
  ZephyrFTP at all and not — previously there was no way to connect
  through one. A proxy password (if the proxy needs one) gets the same
  OS-credential-store treatment described under Site Manager below.
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
  if the certificate ever changes. Covers both FTPS variants — explicit
  (upgrades a plaintext connection via `AUTH TLS`, the common case, still
  the default) and implicit (TLS from the very first byte, conventionally
  port 990, offered as its own choice in the Connect... dialog and Site
  Manager for servers that only speak that older mode).
- **Drag-and-drop or multi-select transfers — whole folders too, not just
  files.** Drag a folder from one pane to the other (or select several,
  mixing files and folders freely) and everything inside gets recreated
  on the other side, nested structure and all, including empty
  subfolders. Each file inside shows up in the transfer queue like any
  other transfer — same pause/resume/cancel, same progress and speed.
- **Drag files in from — or out to — your OS's own file manager**, not
  just between the two panes. Drag a file or folder from Nautilus,
  Explorer, or Finder onto a pane to upload or copy it in, with the same
  Overwrite/Skip and Write Into prompts as any other transfer. Dragging
  a file out of a local pane works immediately, the same as dragging it
  from any other folder; dragging one out of a remote (SFTP/FTP) pane
  downloads it first (with a visible progress dialog — Cancel aborts
  cleanly) so the OS has something real to hand to whatever you drop it
  on. Only offered for a pure file selection — see Known limitations.
- **Move, not just copy, when both panes are on the same server (or both
  on your computer).** Right-click a selection and choose "Move Selected"
  to relocate it server-side — a single instant rename, not a
  download-then-upload — instead of the usual Transfer/drag copy. Works
  for whole folders too. Only offered between two panes that are
  genuinely on the same connection; otherwise you'll see a short message
  explaining why, rather than the option silently doing nothing.
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
  one place. Active, Completed, and Failed transfers are split into
  their own tabs, so a finished batch or a file worth retrying doesn't
  get lost in a long list of everything still queued. Right-click a tab
  itself (not a row) for "Clear Completed"/"Clear Failed" once you're
  done with what's there — Active has no clear option, since everything
  in it is still queued, running, paused, or waiting to reconnect, and
  the actual transfer needs cancelling first, not just hiding. **Survives
  closing the app, too**: anything still queued, paused, or mid-transfer
  when you quit is there again next time you launch — a transfer to/from
  a server picks back up automatically the moment you reconnect to that
  server (shown as "Waiting to reconnect" in the meantime), never
  before, and never with a password remembered or a connection attempted
  on its own.
- **Transfers run concurrently when they don't conflict** — uploading
  from one pane and downloading on the other now happen at the same
  time instead of one waiting for the other, automatically, no setting
  to turn on. Two transfers that would use the *same* connection still
  run one after another, since a single SFTP/FTP session can't safely
  do two things at once.
- **Simultaneous connections per site** — a site can open more than one
  connection at once (Site Manager, 1-10, off by default) so a batch of
  many files transfers in genuine parallel instead of one at a time
  through a single connection. Opens extra connections on demand, using
  the same credentials as the main one — no extra password or
  server-identity prompts.
- **Verify a transfer's checksum** — right-click a completed transfer in
  the queue and choose Verify Checksum... to confirm the file matches on
  both ends (SHA-256). For anything involving a server, this means a
  genuine second read of the remote file over the network (there's no
  cheaper way to check — neither SFTP nor FTP offer a usable server-side
  hash here), so it's a deliberate, on-demand action, not something run
  automatically in the background.
- **Bandwidth limit per transfer** — set a KB/s cap in Preferences
  (blank/0 means unlimited, the default) and every SFTP/FTP transfer
  paces itself down to that rate. Per-transfer, not one shared cap
  across everything at once — two transfers running at the same time
  can together use up to roughly twice the number you set.
- **Synchronized browsing** — turn it on (View menu) and navigating one
  pane (Back, Forward, Up, Home, path bar, double-click) drives the
  other to the same relative path automatically. Off by default. If the
  other side doesn't have a matching subfolder, that pane just shows the
  same "couldn't open that folder" message it always would — nothing
  moves there, the side that navigated is unaffected.
- **Compare Directories...** (View menu) — recursively diffs the two
  panes' current directories (size + modified time, no content hashing)
  and shows the full tree with a status per item, with checkboxes for
  bulk copy in either direction. Deleting files not present at the
  source is a separate, opt-in, off-by-default action, itemized in its
  own confirmation step before anything is removed.
- **Scripting** — run `zephyrftp --script=path/to/file` to drive the app
  non-interactively from a plain-text script (open/get/put/ls/rm/mkdir/
  mv/mirror/exit — see [Scripting](#scripting) below) instead of the GUI,
  for backups, CI pipelines, or anything else you'd rather automate.
- **A live Commands pane** — a real-time, read-only log of protocol
  traffic for both panes, modeled on FileZilla's own message log, docked
  between the toolbar and the file panes by default.
- **Click a column header to sort, click it again to reverse** — Name,
  Size, Modified, or Permissions on the file panes; File, Direction,
  Status, Progress, or Speed on the transfer queue. File panes default to
  folders first, then name ascending (A-Z); the transfer queue defaults to the
  order things were added.
- **Dark or Light theme** (Preferences), switching immediately with no
  restart — built around a small set of colors that mean the same thing
  everywhere in the app regardless of which one you pick (green =
  connect/upload/success, red = disconnect/delete/error, blue =
  navigation/download, amber = caution) so you can read the state of
  things at a glance.
- **Preferences** (Edit menu) — a "Show hidden files" toggle for both
  panes, a default protocol for new connections, and the command used to
  open a file for editing (see Edit-in-place below; leave it blank to
  use your system's default application for that file type). Window
  size and panel layout — including either dock (Transfers/Commands),
  if you've moved, resized, closed, or detached it — are remembered
  across restarts too, with no setting required: whatever the View
  menu's Transfers/Commands toggles show when you close the app is
  exactly what you'll see again next time.
- **Edit-in-place** — right-click a file on a remote pane and choose
  Edit to download it to a temporary local copy, open it in your editor
  of choice, and have every save quietly re-uploaded back to the server
  in the background. Re-opening the same file while it's already being
  edited just brings up your editor again instead of downloading a
  second copy. Shows up in the transfer queue like any other transfer,
  and if a save can't be uploaded (connection dropped, say), you're
  told clearly — with a Retry — rather than silently losing the edit,
  which stays safe on disk either way.
- **Change permissions** — right-click a file or folder (local, SFTP, or
  FTP) and choose Permissions... for a checkbox grid of the standard
  Unix owner/group/other read/write/execute bits, with a live octal
  readout as you toggle them. Works the same way on every backend:
  `chmod()` locally, SFTP's real `setstat`, and FTP's `SITE CHMOD` (a
  widely-supported extension most servers accept, but it's not part of
  the FTP standard, so a server that doesn't implement it will tell you
  so rather than silently doing nothing).

## Download

Grab the latest build from [Releases](https://github.com/arelas/zephyrftp/releases):

- **Windows**: two options. `zephyrftp-windows-x64-setup.exe` installs
  to Program Files with a Start Menu shortcut and a real uninstaller
  (Add/Remove Programs); `zephyrftp-windows-x64.zip` is the portable,
  unzip-and-run alternative if you'd rather not install anything. Neither
  is code-signed yet (see the feature-freeze note above), so the
  installer will get a Windows SmartScreen warning on first run.
- **macOS**: `ZephyrFTP.dmg` — Apple Silicon only (no Intel Mac build
  yet, see Known limitations). Open the `.dmg` and drag ZephyrFTP into
  Applications. The build is unsigned and unnotarized (no paid Apple
  Developer account behind this project), so the first launch needs a
  right-click → Open to get past Gatekeeper's "cannot be opened because
  Apple cannot check it for malicious software" warning — after that
  first confirmation, it opens normally.
- **Linux**: four options.
  - `zephyrftp_<version>_amd64.deb` — Debian/Ubuntu, `apt install
    ./zephyrftp_*.deb` resolves dependencies automatically.
  - `zephyrftp-<version>-1.x86_64.rpm` — Fedora/openSUSE/RHEL-family,
    `dnf install ./zephyrftp-*.rpm` (or your distro's equivalent).
    Double-clicking the `.deb`/`.rpm` to open it in a GUI software
    center (KDE Discover, GNOME Software) may show it as an unnamed
    package with "unknown" publisher/group — that's PackageKit's own
    local-file preview being sparse (confirmed directly: even fields
    genuinely present in the package, like Group, show as "unknown"
    through it), not a problem with the package itself. `dnf install`/
    `apt install` on the file read its real metadata correctly, same
    as `rpm -qi`/`dpkg -I` do.
  - `zephyrftp-linux-x64.tar.gz` — extracts to a single `zephyrftp`
    binary, no install/root needed, for anything else.

    These three are all dynamically linked against system Qt6/libssh2
    (not a bundled/portable build), so any of them needs those already
    installed (the exact packages CONTRIBUTING.md's build instructions
    use). If your distro doesn't have matching versions, use the
    AppImage below instead, or build from source (see
    [CONTRIBUTING.md](CONTRIBUTING.md)), which is quick on any recent
    distro.
  - `ZephyrFTP-<version>-x86_64.AppImage` — self-contained: Qt6,
    libssh2, and libsecret are all bundled, so no matching system
    packages are needed. `chmod +x` it and run it directly, no
    install/root/package-manager needed either. Only relies on the host
    for things every real desktop already has anyway (graphics drivers,
    fonts).

Every release is built and tested (the full automated test suite, not
just "it compiled") by [`.github/workflows/build.yml`](.github/workflows/build.yml)
before being attached — see ARCHITECTURE.md's "Windows, macOS, and
Linux builds (CI)" section for exactly what that verifies.

## Getting started

1. **Launch the app.** The left pane shows your local files right away —
   ZephyrFTP works as a plain two-pane local file manager even before
   you connect to anything.
2. **Click Connect** in the toolbar (or the **Connection** menu) for a
   one-off connection — fill in the server's host, port (22 by default),
   your username, and either a password or a private key file. If it's a
   server you'll come back to, use **Sites** instead: same information,
   but saved for next time (a "New Site" button, and a tree on the left
   to organize them). The toolbar always connects the right pane; click
   either pane's own icon at the left edge of its path bar for the same
   Connect/Sites/Disconnect choices targeting that specific pane instead
   — this is how you connect the *left* pane, or get two servers
   connected at once for a server-to-server transfer.
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

## Scripting

Run `zephyrftp --script=path/to/file` to drive the app non-interactively
— no window is ever shown, output goes to stdout/stderr, and the process
exits with a status code (`0` = ran to completion, `1` = couldn't read or
parse the script, `2` = a command failed at runtime — the script aborts
at the first failure). On a machine with no display at all (a headless
server, a cron job, CI), also pass Qt's own `-platform offscreen`.

Scripts are plain text, one command per line; `#` starts a comment,
blank lines are ignored, and an argument with spaces needs
`"double quotes"`:

```
# back up today's exports, mirroring deletions too
open my-backup-server
lcd /home/me/exports
cd /backups/exports
mirror /home/me/exports /backups/exports --delete
echo backup complete
exit
```

| Command | Effect |
|---|---|
| `open <site-name>` | Connect using a saved site (Site Manager) |
| `cd <path>` / `lcd <path>` | Change the remote / local directory |
| `get <file>` / `put <file>` | Download / upload, same filename both sides |
| `ls` / `lls` | List the remote / local current directory |
| `rm <path>` | Delete a file or empty directory |
| `mkdir <path>` | Create a directory |
| `mv <old> <new>` | Rename, within the current connection |
| `mirror <local> <remote> [--delete]` | One-directional sync (size + modified time, no content hashing); `--delete` also removes files present at the destination but not the source |
| `echo <text>` | Print a line |
| `exit` / `bye` | End the script |

`open <site-name>` only works against a site that's already trusted (you've
connected to it at least once through the GUI, so its host key/certificate
is already in the trust store) **and** has a stored credential — check
"Save password" in Site Manager first. Scripts never prompt: a site
that isn't already trusted, or has no stored password/passphrase, fails
immediately with a clear message rather than hanging.

## Known limitations

This is young software — a few things intentionally aren't supported yet
rather than being half-implemented:

- **FTP and FTPS work against a real server, but only controlled local
  ones so far — not yet a production server out in the wild.** Connecting,
  browsing, and transferring files over plain FTP, active-mode fallback,
  the legacy `LIST` directory-listing fallback, and a full encrypted
  transfer over both FTPS variants (explicit and implicit, including
  trusting a certificate and reusing that trust) have all been confirmed
  against real, independently-implemented FTP/FTPS server software
  (vsftpd and proftpd, not just this project's own test stand-in), not
  just tested in isolation. Still newer and less battle-tested than SFTP;
  expect some rough edges against servers this
  hasn't specifically been tried against.
- **Dragging a folder out of a remote pane to the OS isn't offered —
  only individual files.** Downloading an entire folder tree before a
  drag gesture can even start would mean an unpredictable, possibly very
  long wait with no good way to cancel just that one drag partway
  through; a folder in the selection simply means no real file data is
  offered to the OS for that drag (dragging within the app between panes
  still works regardless). There's also no delayed/lazy handoff the way
  some native OS apps support — the whole file downloads up front, every
  time, even for a drag you end up cancelling.
- **Server-to-server transfers are now supported, but staged through a
  local temporary file rather than moving directly between the two
  servers.** Both panes can now connect independently — click either
  pane's own path-bar icon for a per-pane Connect/Sites/Disconnect menu,
  not just the toolbar's (which still targets the right pane, as a
  shortcut). Dragging or transferring a file between two connected
  servers downloads it to a temporary local file first, then uploads it
  to the destination — no protocol lets one server send a file straight
  to another, so this is the only mechanism possible. In practice this
  means roughly twice the transfer time of a direct copy, and briefly
  uses local disk space equal to the file's size. Verified against a
  real fake-backend orchestration test (direction/phase handling,
  temp-file cleanup on success and on cancellation during either half)
  and a real, automated, repeatable live-two-server test (`verify-remote-to-remote-live`,
  mirroring `verify-sftp-pause-cancel`'s pattern) confirming a real file
  transfers end to end between two genuinely independent local SFTP
  servers with the destination's content matching the source
  byte-for-byte — see ARCHITECTURE.md's `TransferManager` entry for the
  full detail.
- **Move can't merge into a folder that already exists at the
  destination.** A server-side rename can relocate a whole folder in one
  step, but it can't combine it with an existing folder of the same name
  the way a copy's "write into" can (transferring file by file) — if you
  choose to write into an existing folder for a Move, it fails with a
  clear explanation instead. Move a folder to a new name, or use the
  ordinary Transfer/drag copy if you need a real merge.
- **Pause only works for transfers involving a server.** You can pause an
  upload, a download, or a server-to-server transfer (either half) and
  pick it back up later. A purely local copy (between two folders on your
  own computer) can only be cancelled and retried, not paused, since
  there's nothing meaningful to interrupt mid-copy on a local transfer.
- **Edit-in-place's editor command is a single command string, appended
  the file path as its only argument — no `{file}`-style template
  substitution.** Works for most editors launched from a terminal
  (`code`, `gedit`, `notepad++`, ...); an editor that needs its own
  specific argument order or flags around the path isn't supported yet.
  It also doesn't detect if the same file is being edited somewhere
  else at the same time (another ZephyrFTP window, a different tool
  connected to the same server) — the last save wins, same as most
  editors' own behavior with local files.
- **Queue persistence only covers a normal quit, not a crash — and not
  every kind of transfer.** The queue is written to disk when you close
  the app; it isn't continuously saved while running, so a crash or a
  forced quit still loses whatever was in flight, the same as before
  this feature existed. Server-to-server transfers (both panes
  connected to different servers) aren't restored either — they're
  the newest, most complex kind of transfer this app supports, and
  preserving one mid-transfer across a restart would mean also
  preserving its partial local staging file; dropped rather than risked.
  Finished, failed, cancelled, and skipped items aren't kept either —
  this restores what was still pending, not a permanent history.
- **Changing permissions covers one file or folder at a time, and just
  the standard 9 read/write/execute bits.** No multi-select or
  recursive "apply to everything inside," and no setuid/setgid/sticky
  — the same restraint this app already applies to renaming (one entry
  at a time).
- **The proxy is one global setting, not per-site** — every connection
  uses the same proxy (or none). If you only need a proxy for some
  servers, there's no way yet to say so; it's all connections or
  nothing. Also, an FTP/FTPS data connection only goes through the
  proxy in the normal case (passive mode, which is what this app always
  tries first) — the rare fallback to active mode, where the *server*
  has to connect back to *you*, can't be proxied by anything on the
  client side, proxy or not.
- **Ctrl+C/Ctrl+V always means "copy to the other pane," never "duplicate
  in place" or "move."** There's no same-pane paste (nothing meaningful
  for it to do — this app has no duplicate-file feature) and no Ctrl+X
  cut — for moving instead of copying, use the existing right-click Move
  Selected (same-connection only) or drag with the ordinary copy
  semantics. If the pane you copied from navigates elsewhere before you
  paste, the paste is refused with a clear message rather than silently
  transferring whatever now happens to share those names in the new
  directory — copy again from wherever you actually want to paste from.
- **Quick connect is password-only and always targets the right pane.**
  There's no way to specify a private key through the quick-connect
  field — use Connect... for that, same as it's always worked. It also
  always connects the right-hand pane, the same fixed shortcut the
  toolbar's own Connect... button already uses, not whichever pane you
  last clicked in.
- **The filename filter is a simple substring match, and doesn't
  remember what you typed.** No wildcards or regular expressions, and
  each pane's filter text is cleared the moment you navigate away or
  close the app — only whether the filter row is shown at all is
  remembered across restarts, not what was typed into it.
- **Export always exports every saved site — there's no way to export
  just a folder or a selection.** If you only want to hand someone a
  few sites, the exported file is plain JSON text, so trimming it by
  hand afterward works fine in the meantime.
- **The macOS build is Apple Silicon only, and unsigned.** No Intel Mac
  build yet — a universal binary would need a meaningfully more complex
  CI setup for a shrinking population of Intel Macs, not attempted so
  far. The `.dmg` is also unsigned and unnotarized (no paid Apple
  Developer account exists for this project), so Gatekeeper blocks the
  first launch until you right-click → Open — see Download above.
- **Checksum verification isn't offered for server-to-server transfers,
  and never runs automatically.** Both are deliberate: verifying always
  means re-reading a remote file over the network a second time (no
  server-side hash is reachable through SFTP or FTP), so it's an
  on-demand action for a specific completed transfer, not a background
  check — and a server-to-server transfer already involved a local
  staging copy on the way through, which the "both ends" check doesn't
  cleanly map onto.

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
