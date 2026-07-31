# Contributing to ZephyrFTP

Looking for how to *use* ZephyrFTP? See [README.md](README.md) instead.
This document is for building, testing, and modifying it.
[ARCHITECTURE.md](ARCHITECTURE.md) has the full technical picture —
verification status, component design, and known gaps in detail.

## Building

```
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Debug
make -j$(nproc)
```

Dependencies (Debian/Ubuntu): `cmake build-essential qt6-base-dev qt6-svg-dev libssh2-1-dev`

Windows builds run via GitHub Actions (`.github/workflows/windows-build.yml`) —
see ARCHITECTURE.md's "Windows builds (CI)" section for what that pipeline
does and the real, sometimes non-obvious bugs it surfaced along the way.
That workflow still builds with MSVC+vcpkg; it hasn't been migrated to
the MinGW cross-compilation path below yet.

### Cross-compiling for Windows locally (MinGW, from Fedora)

This is a separate path from CI, useful for testing a Windows build
without waiting on GitHub Actions. Dependencies (Fedora):
`mingw64-gcc-c++ mingw64-qt6-qtbase mingw64-qt6-qtsvg mingw64-libssh2
mingw64-cmake wine` — `wine` is only needed for the local verification
step below, not the build itself.

```
mingw64-cmake -S . -B build-win
cmake --build build-win --target zephyrftp
```

Same pattern for the test targets, e.g. `cmake --build build-win --target
smoke-test`.

**libssh2 discovery is toolchain-specific, not just OS-specific.**
`CMakeLists.txt`'s Windows branch originally assumed vcpkg's libssh2
port, which ships a CMake config package (`Libssh2Config.cmake`) — that
doesn't exist in the mingw64 sysroot, which ships a `.pc` file instead,
same as Linux/macOS. The `WIN32` check that picks between them is
actually `WIN32 AND NOT MINGW`: CMake sets `MINGW` true for any
GCC-targeting-Windows toolchain regardless of host, which is exactly the
split needed here — native MSVC+vcpkg still gets the CONFIG path, both
native mingw-w64 and this Fedora cross toolchain get pkg-config.

**AUTOMOC/AUTORCC didn't need explicit host-tool pointing here, but
watch for it.** The obvious risk with cross-compiling a Qt app is that
`moc`/`rcc`/`uic` need to run natively on the host at build time even
though everything else targets Windows. In practice this worked with no
extra CMake flags, because `qt6-qtbase-devel` (the *host* Fedora
package, not `mingw64-qt6-qtbase`) happened to already be installed and
CMake's default cross-compile program search (host `PATH`, not the
mingw sysroot) found its `moc`/`rcc` on its own. If a fresh machine
doesn't have `qt6-qtbase-devel` installed, expect this to break, and
expect the fix to be pointing `QT_HOST_PATH` (or the individual
`QT_MOC_EXECUTABLE`/etc. cache vars) at wherever that host Qt lives —
that hasn't actually been needed here, so it's unverified, flagged
rather than assumed.

**There is no windeployqt for this toolchain.** `tools/collect-win-runtime.sh
<build-dir>` is the substitute: it walks `objdump -p` import tables
recursively (the same information windeployqt itself works from) from
the mingw sysroot to compute the transitive DLL closure a set of `.exe`s
actually need, then copies those DLLs plus the specific Qt plugins this
app uses (platforms, the SVG icon engine, the native style, all three
TLS backends since FtpBackend drives FTPS through `QSslSocket`) into the
right plugin subdirectories next to each exe. Run it after building:

```
tools/collect-win-runtime.sh build-win
```

**Local verification: `wine`, but with a real caveat.** With the DLLs
collected, `wine ./build-win/smoke-test.exe` (and the other nine test
`.exe`s, same env vars and fixture setup as the Linux commands above —
`QT_QPA_PLATFORM=offscreen` still applies) is the local check. One thing
that will cost real time if it's not known going in: **`qDebug()` output
does not reach the terminal under this Wine setup at all** — confirmed
directly with an isolated single-file Qt program: plain `fprintf(stderr,
...)` shows up, `qDebug()` from the same process doesn't, and it isn't
going through `OutputDebugString` either. Root cause not chased further
than that (a Wine console-emulation gap, not something in this project's
control), but the practical upshot is: **judge pass/fail here by exit
code, not by reading PASS/FAIL text**, which is why every test target's
`main()` needs to actually `return`/`app.exit()` a nonzero code on
failure — `smoke_test.cpp` didn't (it always returned `app.exec()`'s
natural 0 regardless of outcome, the one test target that didn't follow
the same `app.exit(allPass ? 0 : 1)` pattern every sibling test uses)
and was fixed to match once this surfaced.

This local Wine pass **did catch a genuine, Windows-specific bug**, not
just prove the environment works: `site_store_test.cpp`'s empty-store
check (load-with-no-file-yet should return an empty list) failed for
real on Windows. Cause: the test opens the written `sites.json` earlier
to inspect its raw JSON, reads it, but never closed that `QFile` before
later deleting the same path — invisible on Linux, where `unlink()`
doesn't care whether a file is still open, but real on Windows, where
`DeleteFile` fails outright while any handle to the file remains open.
Fixed by closing the handle before the delete. All ten test targets pass
under `wine` (by exit code) as of this writing.

**Wine is not a substitute for a real Windows machine**, just a faster
local loop than CI — it doesn't prove real GPU/Direct3D rendering, and
its own console I/O has already shown it isn't perfectly Windows-faithful
(see the `qDebug()` caveat above). Treat a real Windows run as the actual
source of truth the way ARCHITECTURE.md already does for the rest of
this project's Windows-specific claims.

### A build gotcha worth knowing before it costs you an hour

Any test target that pulls in a `Q_OBJECT` header (`RemoteBackend.h` is
the usual one) without also compiling a `.cpp` that uses it must list
that header explicitly in the target's sources. Otherwise AUTOMOC never
generates its vtable and you get a link error that points nowhere near
the actual cause. This has bitten essentially every new test target
added to this project — check it first when a new target won't link.

## Running the test suites

Ten `EXCLUDE_FROM_ALL` CMake targets — not part of a normal `make`, built
and run explicitly:

```
cmake --build build --target smoke-test
QT_QPA_PLATFORM=offscreen ./build/smoke-test
```

```
cmake --build build --target transfer-queue-test
rm -rf /tmp/transfer_test
mkdir -p /tmp/transfer_test/src_dir /tmp/transfer_test/dst_dir
head -c 500000 /dev/urandom > /tmp/transfer_test/src_dir/testfile.bin
QT_QPA_PLATFORM=offscreen ./build/transfer-queue-test
```

```
cmake --build build --target site-store-test
XDG_CONFIG_HOME=/tmp/zephyrftp_test_config QT_QPA_PLATFORM=offscreen ./build/site-store-test
```

```
cmake --build build --target navigation-test
QT_QPA_PLATFORM=offscreen ./build/navigation-test
```

```
cmake --build build --target transfer-pause-test
QT_QPA_PLATFORM=offscreen ./build/transfer-pause-test
```

```
cmake --build build --target file-operations-test
QT_QPA_PLATFORM=offscreen ./build/file-operations-test
```

```
cmake --build build --target folder-transfer-test
rm -rf /tmp/folder_transfer_test
mkdir -p /tmp/folder_transfer_test/src/myfolder/subdir1 \
         /tmp/folder_transfer_test/src/myfolder/subdir2/nested \
         /tmp/folder_transfer_test/src/myfolder/emptydir \
         /tmp/folder_transfer_test/dst
echo "hi from a" > /tmp/folder_transfer_test/src/myfolder/a.txt
echo "hi from b" > /tmp/folder_transfer_test/src/myfolder/subdir1/b.txt
echo "hi from c" > /tmp/folder_transfer_test/src/myfolder/subdir1/c.txt
echo "hi from d" > /tmp/folder_transfer_test/src/myfolder/subdir2/nested/d.txt
QT_QPA_PLATFORM=offscreen ./build/folder-transfer-test
```

```
cmake --build build --target conflict-resolution-test
QT_QPA_PLATFORM=offscreen ./build/conflict-resolution-test
```

```
cmake --build build --target ftp-parsing-test
QT_QPA_PLATFORM=offscreen ./build/ftp-parsing-test
```

```
cmake --build build --target protocol-selection-test
XDG_CONFIG_HOME=/tmp/zephyrftp_proto_config QT_QPA_PLATFORM=offscreen ./build/protocol-selection-test
```

The `rm -rf` lines on `transfer-queue-test` and `folder-transfer-test`
aren't optional either, and they're the reason those two commands start
by deleting a directory: `mkdir -p` won't clear a destination that
already holds the previous run's output, so without the teardown both
suites pass the first time and fail every time after — one because a
phase re-transfers a file already sitting at the destination, the other
because `dst/myfolder` already exists. That failure looks exactly like a
code regression and isn't one, so run the setup as written rather than
skipping straight to the binary.

The `XDG_CONFIG_HOME` override on `site-store-test` isn't optional —
without it, `SiteStore` writes to your actual config directory, and the
test's own cleanup phase will delete whatever `sites.json` it finds
there. `navigation-test` creates its own scratch directory tree under
`/tmp/nav_test` and doesn't touch anything outside it. `transfer-pause-test`
uses a fake in-process backend (no real server, no real files) — see its
own header comment for exactly what it does and doesn't prove.
`file-operations-test` creates its own scratch tree under
`/tmp/file_ops_test` and tests `LocalBackend` directly (not through the
UI, since its prompts can't be driven headlessly) — see its own header
comment for what it does and doesn't cover. `folder-transfer-test` needs
its nested directory structure created by hand first (shown above) —
unlike the other tests, it doesn't build its own fixture data, since the
structure itself (multi-level nesting, a genuinely empty leaf directory)
is part of what's being verified. `conflict-resolution-test` actually
drives the real conflict dialog (finds it via
`QApplication::activeModalWidget()` while its `QMessageBox::exec()` call
is still blocking, toggles its real checkbox, clicks its real button) —
it's not a mock of the dialog, so this is the one test in this list
where "this compiles and calls the right functions" was never good
enough to have shipped; see its own header comment for the reasoning.
`ftp-parsing-test` needs no fixtures or environment setup at all — it
calls `FtpBackend`'s two directory-listing parsers as pure functions
against sample data, with no network I/O and no server. That narrowness
is the point: it's the only part of the FTP/FTPS feature that *can* be
verified here, and it deliberately claims nothing about the rest of the
protocol. See its header comment and ARCHITECTURE.md's Known gaps for
what's still unproven.
`protocol-selection-test` needs the same `XDG_CONFIG_HOME` isolation
`site-store-test` does, and for the same reason: it exercises SiteStore
against real files, so without the override it writes — and its
migration phase deliberately overwrites — whatever `sites.json` is in
your actual config directory. It constructs a real `ConnectionDialog`
and drives its protocol combo, so it needs a `QApplication` and the
offscreen platform, not just `QCoreApplication`.

All ten need to actually pass — not just build — before a change is
considered done. See ARCHITECTURE.md's "Verification status" section for
what each test actually proves and why it exists.

## The core discipline this codebase runs on

This project has been built with one rule that matters more than any
style guideline: **flag what's unverified, and go verify things instead
of assuming them.** A few concrete examples from this codebase's own
history, worth internalizing before making changes:

- `SftpBackend`'s host-key type constants and the knownhost API's mask
  bits are *not* the same numbering — a naive cast would have silently
  misclassified every key type. Caught by reading `libssh2.h` directly
  instead of assuming a pattern held.
- Two POSIX-only bugs (`sys/socket.h` headers, `mode_t`/`S_IRUSR` macros)
  sat in `SftpBackend.cpp` undetected until the first real Windows build,
  because the code had only ever been compiled on Linux.
- `QFile::copy()` silently refuses to overwrite an existing destination
  file — found by a test that transferred the same file twice, not by
  reading Qt's docs closely enough beforehand.
- A `resources/icons.qrc` path bug (`:/icons/icons/plug.svg` instead of
  `:/icons/plug.svg`) was caught by a throwaway test that actually
  rendered every icon and counted opaque pixels, not by trusting that a
  clean compile meant the resource paths were right.

None of these were found by reasoning about the code in the abstract —
they were found by building it, running it, and checking the actual
output against what was expected. If a change touches something that
can be tested (a real file transfer, a real icon render, a real thread
lifecycle), test it for real rather than trusting that it compiles.

When something genuinely can't be verified in the environment at hand —
this codebase has largely been developed without a live SFTP server or a
real display available — say so explicitly rather than letting it read
as proven. ARCHITECTURE.md's "Known gaps" and "Still not verified"
sections exist for exactly this, and are meant to stay accurate, not
aspirational.

Two specific habits that follow from all this:

- **Verify visual changes visually.** "It compiles and sets the right
  stylesheet" has repeatedly not meant "it looks right." The pattern
  used throughout this project: render the widget offscreen, save a PNG,
  sample actual pixel colors, and *look at the image*. That's how the
  dialogs-ignoring-the-dark-theme bug and the illegible-16px-icon bug
  were both caught. It's fiddlier than a plain assert, which is exactly
  why it's worth writing down as an expectation rather than leaving to
  discretion.
- **Parse CI YAML before trusting it.** The workflow file has caused
  more than its share of real failures — Windows PowerShell quoting
  quirks, vcpkg pinning, a `GITHUB_TOKEN` permissions gap that 403'd on
  release creation. Run it through a parser
  (`python3 -c "import yaml,sys; yaml.safe_load(open('.github/workflows/windows-build.yml'))"`)
  before calling a change done, and when there's a choice between two
  approaches, prefer the one with fewer new failure modes over the one
  that's more clever.

## Code conventions

- Comments explain *why*, not *what* — especially for anything
  non-obvious, platform-specific, or where an earlier, more naive
  approach was tried and replaced. Future readers (human or otherwise)
  should be able to tell why a workaround exists without archaeology.
- New backends implement the `RemoteBackend` interface
  (`src/backends/RemoteBackend.h`) — the UI layer should never need to
  know or care which concrete backend it's talking to.
- Icon/color choices follow `ICON-MAP.md` from the design package (not
  currently vendored into this repo's history as a standalone file —
  see ARCHITECTURE.md's "Design system" section for where the mapping
  actually lives in code: `FilePaneWidget::iconForEntry()` and
  `TransferQueueWidget::statusIcon()`).
