# ZephyrFTP

A dual-pane SFTP client for Windows and Linux — browse your local files
and a remote server side by side, then drag, drop, or double-click to
move things between them. Think FileZilla or WinSCP, built fresh in Qt6.

## Features

- **Dual-pane browsing** — your computer on one side, the server on the
  other, both visible at once. Back, forward, and up buttons beside each
  pane's location bar, same as any file manager.
- **SFTP with password or private-key login.**
- **Site Manager** — save connections you use often, organized into
  folders (type a new folder name right on the site's details, or pick
  an existing one from the dropdown), so you don't have to re-type the
  host, port, and username every time. Each saved site can also default
  to a specific starting folder on the server instead of your home
  directory — handy if you always go straight to the same upload folder.
  Your password is never saved to disk, though — you'll be asked for it
  each time you connect. Same for a private key's passphrase, if it has
  one.
- **Host-key verification** — the first time you connect to a server,
  ZephyrFTP shows you its identity fingerprint and asks you to confirm
  it. If that fingerprint ever changes on a later connection, you get a
  clear warning instead of a silent, invisible risk. This is the same
  protection SSH itself uses, and it's on by default here.
- **Drag-and-drop or multi-select transfers** — drag files from one pane
  to the other, or select several at once and right-click → Transfer.
- **A real transfer queue** — see what's moving and how fast, pause a
  transfer to a server and pick it back up later without starting over,
  cancel something outright, or retry something that failed, all from
  one place.
- **Dark theme**, built around a small set of colors that mean the same
  thing everywhere in the app (green = connect/upload/success, red =
  disconnect/delete/error, blue = navigation/download, amber = caution)
  so you can read the state of things at a glance.

## Download

Pre-built Windows binaries aren't published as a formal, versioned
release yet — until then, grab the latest successful build from the
[Actions tab](https://github.com/arelas/zephyrftp/actions/workflows/windows-build.yml):
click the most recent green run, then download the `zephyrftp-windows-x64`
artifact at the bottom of the page. It's a ready-to-run folder — no
installer, just unzip it and launch `zephyrftp.exe`.

Linux users: there's no pre-built binary yet — see
[CONTRIBUTING.md](CONTRIBUTING.md) for building from source, which is
quick on any recent distro.

## Getting started

1. **Launch the app.** The left pane shows your local files right away —
   ZephyrFTP works as a plain two-pane local file manager even before
   you connect to anything.
2. **Click Connect** in the toolbar for a one-off connection — fill in
   the server's host, port (22 by default), your username, and either a
   password or a private key file. If it's a server you'll come back to,
   use **Sites** instead: same information, but saved for next time (a
   "New Site" button, and a tree on the left to organize them).
3. **First connection to a new server?** You'll be asked to confirm its
   identity fingerprint. This is expected and normal — it's the same
   prompt any SSH client shows the first time. Confirming it once means
   ZephyrFTP will recognize that server automatically on future
   connections, and will warn you loudly if its identity ever changes.
4. **Browse and transfer.** Once connected, the right pane shows the
   remote server. Double-click a file to send it to the other pane, drag
   files between panes, or select several and right-click → "Transfer
   Selected."
5. **Watch the Transfers panel** at the bottom for progress. Right-click
   any transfer to cancel it, or to retry one that failed.

## Known limitations

This is young software — a few things intentionally aren't supported yet
rather than being half-implemented:

- **Whole folders can't be transferred** — only individual files.
  Selecting or dragging a folder currently does nothing.
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

MIT — see [LICENSE](LICENSE).

The vendored icon set (`resources/icons/`) is [Tabler Icons](https://tabler.io/icons),
also MIT-licensed; its license text is included separately at
`resources/icons/LICENSE-tabler-icons.txt` since it's a distinct
copyright holder from this project.
