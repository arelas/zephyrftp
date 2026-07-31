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

## Running the test suites

Nine `EXCLUDE_FROM_ALL` CMake targets — not part of a normal `make`, built
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

All nine need to actually pass — not just build — before a change is
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
