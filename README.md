# Commander Transfer (working title)

Dual-pane, Qt6-based file transfer client. Local <-> SFTP for now, FTP/FTPS
backend to follow using the same `RemoteBackend` interface.

## Verified in this pass

- CMake configures cleanly against Qt6 (Widgets, Network, Concurrent) and
  libssh2 via pkg-config.
- Full build compiles and links with zero errors on Ubuntu 24.04
  (Qt 6.4.2, libssh2 1.11.0, GCC 13.3.0).
- Resulting binary correctly links `libQt6Widgets`, `libQt6Gui`,
  `libQt6Core`, `libssh2`.
- App starts and runs its event loop headlessly (`QT_QPA_PLATFORM=offscreen`)
  without crashing.
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

- **The transfer queue actually moves files, verified byte-for-byte.**
  `src/transfer_queue_test.cpp` (built via the `transfer-queue-test` CMake
  target) creates two real `FilePaneWidget`s on real `LocalBackend`s
  pointed at real temp directories, writes a 500KB random test file,
  enqueues a transfer through the real `TransferManager`, and confirms:
  the destination file exists, its MD5 hash matches the source exactly,
  the queue reported `Queued -> InProgress -> Done`, and the progress
  numbers reported along the way were plausible (`0/500000` ->
  `500000/500000`). This test is also what caught a real gap during
  development — `LocalBackend`'s copy methods weren't emitting
  `transferProgress` at all, which would have left the queue's progress
  column blank for local transfers; fixed before this note was written,
  not after. Run it with:
  ```
  cmake --build build --target transfer-queue-test
  mkdir -p /tmp/transfer_test/src_dir /tmp/transfer_test/dst_dir
  head -c 500000 /dev/urandom > /tmp/transfer_test/src_dir/testfile.bin
  QT_QPA_PLATFORM=offscreen ./build/transfer-queue-test
  ```

**Still not verified:** an actual SFTP round-trip (auth, directory listing,
file transfer) against a real server, and real window rendering on a real
display — no live SFTP server or windowing system was available in the
environment this was built in.

## Build

```
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Debug
make -j$(nproc)
```

Dependencies (Debian/Ubuntu): `cmake build-essential qt6-base-dev libssh2-1-dev`

## Architecture

- `RemoteBackend` — abstract interface (`connectToHost`, `listDirectory`,
  `downloadFile`, `uploadFile` + signals for results/progress). Everything
  in the UI layer talks to this, never to a concrete backend type.
- `LocalBackend` — wraps `QDir`/`QFile`. Runs on the GUI thread; local
  listing/copy is fast enough that this hasn't been a problem, but it's a
  design decision worth revisiting if it ever needs to handle slow
  network-mounted paths.
- `SftpBackend` — wraps libssh2's synchronous API directly. Runs on a
  dedicated `QThread`, confirmed by the smoke test above. Its four
  operations (`connectToHost`, `listDirectory`, `downloadFile`,
  `uploadFile`) are declared `public slots:` in the `RemoteBackend` base
  class specifically so `QMetaObject::invokeMethod(..., Qt::QueuedConnection)`
  can reach them by name across the thread boundary — the base class's
  moc-generated slot thunk dispatches through the vtable, so the derived
  override still runs (Qt's "virtual slot" pattern).
- `ConnectionDialog` — host/port/username/password form. No saved-site
  list, no input validation beyond "port is a number" — both reasonable
  next additions.
- `FilePaneWidget` — one side of the dual-pane view. Holds a
  `QStandardItemModel`, doesn't know or care which backend it's attached
  to. `setBackend(backend, thread)` swaps backends at runtime: if a
  `QThread` is passed, the pane does NOT parent the backend to itself
  (Qt disallows reparenting across thread boundaries) and instead manages
  its lifetime manually — `deleteLater()` on the backend (runs on its own
  thread's queue) followed by `thread->quit()` + `thread->wait()`. Passing
  `thread=nullptr` (e.g. for `LocalBackend`) falls back to normal
  parent-child ownership.
- `TransferManager` — owns the transfer queue, processes it **serially**
  (one item at a time — SftpBackend holds a single libssh2 session, and
  concurrent transfers on the same session aren't safe without more
  synchronization than this app has yet). `enqueue()` figures out
  direction (local->remote / remote->local / local-copy / unsupported
  remote-to-remote) from each pane's `isLocalFilesystem()`, then dispatches
  to whichever backend actually owns the "remote" side of the operation.
  Reconnects its progress/finished/failed signal listeners to whichever
  backend is executing the current item — `RemoteBackend` objects persist
  across multiple transfers, so `connectToBackend()` explicitly disconnects
  the previous backend before wiring up the next to avoid stacking
  duplicate connections.
- `TransferQueueWidget` — dumb table view mirroring `TransferManager`'s
  `itemAdded`/`itemUpdated` signals. All state lives in the manager.
- `MainWindow` — two `FilePaneWidget`s in a `QSplitter`, plus a
  `QDockWidget` at the bottom holding the transfer queue. Left pane is
  always `LocalBackend`. Right pane starts on `LocalBackend` and the
  toolbar's "Connect..." action swaps it for a live `SftpBackend` + worker
  `QThread` via `ConnectionDialog`; "Disconnect" swaps back to
  `LocalBackend`. Double-clicking a file in either pane calls
  `TransferManager::enqueue()` with that pane as source and the other as
  destination; `TransferManager::transferSucceeded` triggers a refresh of
  both panes' listings.

## Windows builds (CI)

`.github/workflows/windows-build.yml` builds this on GitHub's
`windows-latest` runner: MSVC + Qt6 (via `jurplel/install-qt-action`) +
libssh2 (via vcpkg) + Ninja, then `windeployqt` to bundle the Qt DLLs.
Runs on every push to `main` and on `v*` tags; tag pushes also attach the
build as a zipped GitHub Release asset.

**UNVERIFIED — has not run yet.** The YAML syntax is valid (checked with
`yaml.safe_load`), and `CMakeLists.txt` now branches on `WIN32` to use
vcpkg's `Libssh2::libssh2` CMake target instead of pkg-config, but nothing
about the actual Windows build — the Qt arch string, the vcpkg commit pin,
whether `windeployqt` picks up everything needed — has been proven against
a real Windows runner. First push to GitHub will be the first real test;
check the Actions tab for the initial run.

## Known gaps (flagged, not fixed)

- **No host-key verification.** `SftpBackend::ensureSession()` skips
  `known_hosts` checking entirely — do not point this at anything but a
  disposable test server until that's in.
- **Password-only auth.** No public-key auth path yet.
- **Remote-to-remote transfers are unsupported**, not silently dropped —
  `TransferManager::enqueue()` marks them `Failed` immediately with an
  explanatory message. Would need a stage-through-a-local-temp-file
  fallback (download then upload) to support.
- **No drag-and-drop, no multi-select transfers, no cancel/retry.**
  Double-click one file at a time is the whole interaction model right now.
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
