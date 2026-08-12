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

Every dependency below is the same eight things — a C++ toolchain,
CMake, `pkg-config`, Qt6's base and SVG modules (dev/headers package),
libssh2, OpenSSL (dev/headers package — `FtpTlsSocket`'s raw-OpenSSL
FTPS TLS layer, see ARCHITECTURE.md's Known gaps entry on why `QSslSocket`
alone can't do real TLS-session reuse), and libsecret (`CredentialStore`'s
Linux backend — the OS keychain SiteManagerDialog's "Save password"
checkbox writes to; see ARCHITECTURE.md's `CredentialStore` entry) —
just under each distro's own package names. None of this is needed for
the MinGW/Windows build further down; that path uses the real Win32
Credential Manager API (`wincred.h`) instead, already present in the
mingw sysroot.

**Debian/Ubuntu** (`apt`), directly verified — a from-scratch container
build with exactly this line, nothing assumed from a desktop machine
that already happened to have some of it installed:

```
sudo apt install cmake build-essential qt6-base-dev qt6-svg-dev \
    libssh2-1-dev libssl-dev libsecret-1-dev pkg-config
```

`pkg-config` specifically isn't pulled in by `build-essential` on a
clean install (CMake's libssh2 discovery needs it) — confirmed directly
after a from-scratch container build failed on `Could NOT find
PkgConfig` before this line included it explicitly.

**Fedora** (`dnf`), also directly verified — these are the exact
packages already installed on this project's own Fedora 44 development
environment, confirmed via `rpm -q` against a real successful native
build and full test-suite run, not just plausible package names:

```
sudo dnf install cmake gcc-c++ qt6-qtbase-devel qt6-qtsvg-devel \
    libssh2-devel libsecret-devel pkgconf-pkg-config
```

**Arch** (`pacman`), directly verified — a real `archlinux:latest`
`podman` container, `pacman -Sy` against the live package repos, this
exact line, then a real `cmake`/`make` build and a real headless run
(`QT_QPA_PLATFORM=offscreen`, clean exit) of the resulting binary, not
just a successful link:

```
sudo pacman -S --needed cmake base-devel qt6-base qt6-svg libssh2 openssl libsecret pkgconf
```

**openSUSE** (`zypper`), by package-name convention — **not directly
verified**, unlike the three lines above, since it isn't available in
this environment to actually build against. Flagged rather than
presented with the same confidence; please open an issue if it needs a
correction.

```
sudo zypper install cmake gcc-c++ qt6-base-devel qt6-svg-devel \
    libssh2-devel libopenssl-devel libsecret-devel pkg-config
```

Windows, macOS, and Linux release builds all run via GitHub Actions
(`.github/workflows/build.yml`) — see ARCHITECTURE.md's "Windows,
macOS, and Linux builds (CI)" section for the full pipeline history,
including the Windows job's older MSVC+vcpkg era and the real,
sometimes non-obvious bugs that surfaced getting each stage working.
The Windows job (`build-windows`) runs on `ubuntu-latest` inside a
`fedora:44` container and cross-compiles with MinGW — the same path
documented below, down to wrapping every wine test run in `xvfb-run`
(see "Local verification: `wine`" below for why). The Linux job
(`build-linux`) is a plain native build on `ubuntu-latest` using the
exact dependency line above, ships just the binary (dynamically linked
against system Qt6/libssh2, not a bundled/portable build), and needs no
wine/Xvfb at all since it's running native binaries directly. The
macOS job (`build-macos`) is the same shape as `build-linux` — a plain
native build, this time on `macos-latest` (Apple Silicon only; see
ARCHITECTURE.md for why not universal) using Homebrew instead of
apt/dnf — and packages a `.dmg` via `cpack -G DragNDrop` instead of a
`.deb`/`.rpm`. All three jobs actually run the full required test
suite (see "Running the test suites" below for the current count), not
just link it.
On a `v*` tag, the `release` job packages all three
(`zephyrftp-windows-x64.zip`, `zephyrftp-linux-x64.tar.gz`,
`ZephyrFTP.dmg`) onto the same GitHub Release, alongside the `.deb`/
`.rpm`/AppImage. All of this has been confirmed on GitHub's own
runners, not just locally — including a real tagged release (see the
`v0.2.0` release) that exercised the `release` job for real, which is
what caught the one thing local `podman` testing couldn't: GitHub
Actions sets `$HOME=/github/home` for container jobs, and wine refuses
to create its default `~/.wine` there (ownership check failure) —
fixed by pinning `WINEPREFIX` to a scratch dir the job creates and owns
outright.

**This pipeline does NOT run automatically on every push to `main`** —
only on a `v*` tag (an actual release) or a pull request. Ordinary
pushes rely on the local build/test steps in this document instead,
since the full pipeline (native Linux build+test, MinGW cross-compile +
wine build+test) costs real time and Actions minutes that add up fast
against routine, non-release commits. Run it on demand anytime without
pushing a tag via `workflow_dispatch`:

```
gh workflow run build.yml --ref main
```

(or the "Run workflow" button on the Actions tab) — same jobs, same
verification, just opt-in instead of automatic.

### Cross-compiling for Windows locally (MinGW, from Fedora)

Useful for a faster local loop than waiting on GitHub Actions, and for
diagnosing a CI failure without needing a CI run to do it. Same steps
CI itself runs. Dependencies (Fedora):
`mingw64-gcc-c++ mingw64-qt6-qtbase mingw64-qt6-qtsvg mingw64-libssh2
mingw64-openssl mingw64-cmake wine` — `wine` is only needed for the
local verification step below, not the build itself.

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

**Wine needs a real, even virtual, X display for itself — separate from
Qt's own `QT_QPA_PLATFORM=offscreen`.** Confirmed directly in a headless
Fedora 44 container with no X/Wayland session at all (the environment
CI actually runs in): every test that drives a real `QMessageBox`
(`conflict-resolution-test`) failed with `CreateWindowEx failed (Invalid
window handle.)` — Wine's own internal window management needed a
display regardless of what Qt's platform plugin was doing. This was
invisible running the same commands directly on a desktop machine that
already had a real graphical session for Wine to use, which is exactly
why it's worth writing down: wrap every `wine` invocation in `xvfb-run
-a` (`xorg-x11-server-Xvfb` package) whenever there's no real session
already available — a plain SSH-only box or a container, not just CI:

```
xvfb-run -a wine ./build-win/smoke-test.exe
```

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

### Linux distro packages (.deb/.rpm)

`cpack` (bundled with CMake, no extra tool to install) builds real
`.deb` and `.rpm` packages from the same `zephyrftp` target the plain
build above produces — a proper system install layout (`/usr/bin`,
a `.desktop` file, hicolor-theme icons at every size
`tools/generate_app_icon.cpp` already produces), not just the bare
binary the tarball release ships. Both formats coexist deliberately:
the tarball needs no install and no root and just needs matching
system packages already present; the `.deb`/`.rpm` integrate with a
desktop environment's app launcher and icon theme in exchange for
actually installing.

```
cmake --build build --target zephyrftp
cd build
cpack -G DEB   # needs dpkg-dev + file (for real dependency scanning)
cpack -G RPM   # needs rpm-build (only on an RPM-based distro, obviously)
```

Dependencies in both packages are **auto-detected from the actual
linked binary**, not a hand-maintained list — `dpkg-shlibdeps` for
`.deb` (`CPACK_DEBIAN_PACKAGE_SHLIBDEPS`), rpmbuild's own scanner for
`.rpm` (`CPACK_RPM_PACKAGE_AUTOREQPROV`) — so they can't silently drift
from what the binary actually needs the way a manually-written
dependency line eventually would.

**Both fully verified end to end, not just "cpack didn't error," in
real, disposable `podman` containers — a fresh build container, then a
*separate*, completely clean install container with no build tooling at
all, matching how a real user's machine would look:**
- `.deb`: built in `debian:stable` (`apt install` the exact
  Debian/Ubuntu dependency line from earlier in this document, plus
  `dpkg-dev`/`file` for the dependency scan), then `apt install
  ./zephyrftp_*.deb` in a fresh `debian:stable` container — dependencies
  resolved automatically from the package's own `Depends:` field,
  `desktop-file-validate` passed, and the installed binary actually ran
  headlessly (`QT_QPA_PLATFORM=offscreen`, clean exit).
- `.rpm`: same shape, `fedora:latest` both times (this project's own
  Fedora dependency line plus `rpm-build`/`file`), `dnf install
  ./zephyrftp-*.rpm` resolving dependencies from the package's own
  `Requires:` automatically.

One real, non-obvious gap this caught: `CPACK_PACKAGE_DESCRIPTION_SUMMARY`
alone is enough for the `.deb`'s one-line `Description:`, but the RPM
generator silently falls back to a generic CPack-authored placeholder
("This is an installer created using CPack...") for the long
description without `CPACK_RPM_PACKAGE_DESCRIPTION` set explicitly —
caught by actually inspecting a real built `.rpm` (`rpm -qip`), not by
assuming the generic `CPACK_PACKAGE_DESCRIPTION` variable would be
enough (it wasn't; RPM needed its own).

### Flatpak

`io.github.arelas.zephyrftp.yml` (repo root, matching where Flathub
requires the manifest to eventually live) builds a real Flatpak locally
right now — **deliberately not yet submitted to Flathub**. Flathub's own
current requirements explicitly flag broad-scope file managers as a
category needing extra scrutiny (waived for submissions actually coming
from the upstream project with a demonstrated maintenance history) —
worth having more of that history first. Build and test it locally:

```
flatpak install --user flathub org.flatpak.Builder org.kde.Platform//6.11 org.kde.Sdk//6.11
flatpak run --user org.flatpak.Builder --user --force-clean --repo=flatpak-repo build-flatpak io.github.arelas.zephyrftp.yml

flatpak remote-add --user --if-not-exists --no-gpg-verify zephyrftp-local-repo ./flatpak-repo
flatpak install -y --user zephyrftp-local-repo io.github.arelas.zephyrftp
flatpak run --user io.github.arelas.zephyrftp
```

Runtime is `org.kde.Platform`/`org.kde.Sdk` `//6.11` — ships Qt6
directly (unlike `freedesktop-sdk`, which would need Qt bundled as an
extra extension), and happens to be the exact Qt version this project
already develops against locally. libssh2 and libsecret aren't part of
that runtime, so the manifest builds both from source as their own
modules: libsecret's module is the **real, current recipe pulled
directly from `github.com/flathub/shared-modules`**, not hand-guessed;
no equivalent shared module exists for libssh2 (checked directly, not
assumed), so that one is hand-written against libssh2's own upstream
CMakeLists.txt (confirmed directly: `CRYPTO_BACKEND` auto-detects
OpenSSL, already part of the runtime, with no explicit flag needed).
Both pin the same versions (1.11.1, 0.21.7) already used everywhere
else in this project's own dependency lines.

`resources/zephyrftp.desktop`, `resources/zephyrftp.metainfo.xml`, and
the hicolor-theme icons are the exact same files the `.deb`/`.rpm`
packaging above already installs — one source of truth, not a
Flatpak-specific copy. The manifest's `rename-desktop-file`/
`rename-appdata-file`/`rename-icon` directives rewrite them to the full
`io.github.arelas.zephyrftp` naming automatically at build time; the
Metainfo file is deliberately written with the plain `zephyrftp` ID
(confirmed via `appstreamcli validate` both before the rename, where it
produces exactly one expected warning about not being reverse-DNS yet,
and after simulating the rename by hand, where it validates completely
clean).

**Verified for real, not just "the build didn't fail":**
- The full manifest builds cleanly end to end (`flatpak-builder`,
  `appstreamcli compose` succeeding as part of that).
- Installed from the local build output into a real, separate
  `flatpak remote` and actually launched — ran headlessly
  (`QT_QPA_PLATFORM=offscreen`) for a full 8 seconds with a clean kill,
  not a crash.
- **The two permissions unique to this app were individually confirmed
  to actually work, not just declared in `finish-args`:**
  `flatpak run --command=ls io.github.arelas.zephyrftp /home/david`
  genuinely lists the real host home directory (`--filesystem=host`
  actually in effect), and a direct `gdbus call` to
  `org.freedesktop.secrets` from inside the sandbox succeeds
  (`--talk-name=org.freedesktop.secrets` actually reaches the real
  Secret Service `CredentialStore` needs).

**The one real, open tradeoff, called out directly in the manifest's
own `--filesystem=host` comment, not glossed over:** a dual-pane
commander-style file manager wants continuous, arbitrary-path browsing
— typing any path, reaching external drives under `/media`/`/run/media`
— which no existing XDG portal covers the way the one-shot
file-open/save portals do. `--filesystem=host` is the pragmatic choice
for now; it's also exactly the kind of static, broad permission
Flathub's requirements ask submissions to minimize, and the first thing
worth revisiting (narrowing, or leaning harder on portals for the
cases that can tolerate it) before an actual Flathub submission at
beta, not something settled here.

### AppImage

`tools/build-appimage.sh <build-dir>` builds a real, self-contained
AppImage from an already-built `zephyrftp` (`linuxdeploy` +
`linuxdeploy-plugin-qt`, pinned versions):

```
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target zephyrftp
VERSION=<version> tools/build-appimage.sh build
```

**Built on Debian 12 (bookworm), not the newest available base** —
confirmed directly, not assumed: AppImages link dynamically against the
build system's own glibc, and glibc is forward- but not
backward-compatible, so the standard AppImage advice is to build on the
oldest base that still works. Ubuntu 22.04 was tried first (older
glibc, 2.35) but only has Qt 6.2.4, which is too old for this project's
own `qt_standard_project_setup()` CMake call (needs Qt 6.3+) — a real
build failure, not a hypothetical concern. Debian 12 (glibc 2.36, Qt
6.4.2) is the oldest base that actually satisfies both constraints.

**Verified end to end across several real, disposable containers, not
just "linuxdeploy didn't error"** — build in one container, then run
the resulting `.AppImage` in a *separate*, completely unrelated distro
with zero Qt/libssh2/libsecret installed at all (the actual point of an
AppImage). This surfaced real, fixed-in-the-script gaps one at a time,
each confirmed by an actual missing-library crash, not anticipated in
advance:

- `libfontconfig`/`libharfbuzz`/`libfreetype` are bundled explicitly
  (`linuxdeploy --library=`) — `linuxdeploy-plugin-qt`'s automatic
  dependency scan is `ldd`-based and misses these, since Qt reaches
  them via `dlopen()` at runtime rather than a direct link-time
  dependency. Without this, the AppImage failed to even start on a
  fresh distro, one missing `.so` at a time, until all three were added.
- The Qt "offscreen" platform plugin is bundled manually (copied into
  `AppDir` after the main `linuxdeploy` run) — `linuxdeploy-plugin-qt`
  only auto-bundles platform plugins it detects as actually used during
  a normal run (`xcb`); "offscreen" is a headless-testing-only backend
  real users never select, but this project's whole test/CI story runs
  via `QT_QPA_PLATFORM=offscreen`, so it needs to work here too.

**Deliberately NOT bundled — confirmed to be fine relying on the host
for, not just assumed** — a bare-minimum container missing these isn't
representative of any real desktop:
- The GL/EGL/GLX/OpenGL stack (`mesa-libEGL`, `mesa-libGL`,
  `libglvnd-opengl`, `libglvnd-glx`) — must match the host's actual GPU
  driver (proprietary vs. Mesa vs. whatever else), and every real
  desktop already has some working version of it, or the desktop
  environment itself couldn't render anything. Confirmed directly:
  installing just those five packages (nothing Qt/libssh2/libsecret
  related) on an otherwise bare system was enough for the AppImage to
  run cleanly.
- fontconfig's own *configuration* (`/etc/fonts/fonts.conf` and actual
  font files) — the library is bundled (see above), but real font data
  isn't; every real desktop already has both, and bundling a fixed font
  set would silently override the user's own choices for no real
  benefit. Confirmed directly: the one remaining error
  (`Fontconfig error: Cannot load default config file`) on an otherwise
  fully-bundled AppImage disappeared entirely once the host's own
  `fontconfig` package (config + fonts, not a rebuild) was installed —
  and the app never actually crashed even without it, just logged the
  warning and kept running.

## Running the test suites

Seventeen `EXCLUDE_FROM_ALL` CMake targets make up the required suite as
of this writing — not part of a normal `make`, built and run
explicitly, and all of them (not just a "core" subset) need to actually
pass before a change is done. The count keeps growing as the project
does; rather than renumbering everything and rewriting this whole
section each time, new targets get their own short subsection further
below instead. Don't trust "fifteen" to still be accurate by the time
you're reading this — this paragraph is the one place that number lives,
so if it's wrong, the fix is here, not a hunt through the rest of this
file. The original ten, each with its own run command:

```
cmake --build build --target smoke-test
QT_QPA_PLATFORM=offscreen ./build/smoke-test
```

```
cmake --build build --target transfer-queue-test
QT_QPA_PLATFORM=offscreen ./build/transfer-queue-test
```

```
cmake --build build --target site-store-test
QT_QPA_PLATFORM=offscreen ./build/site-store-test
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
QT_QPA_PLATFORM=offscreen ./build/protocol-selection-test
```

`transfer-queue-test` and `folder-transfer-test` both wipe and
regenerate their own `/tmp/transfer_test`/`/tmp/folder_transfer_test`
scratch trees at the top of `main()` now, so re-running either back to
back needs nothing beyond the two lines above. That wasn't always true:
both used to require an external `rm -rf` + fixture-recreation dance
before every run, because `TransferManager` correctly detects a
previous run's leftover output as a real destination conflict and pops
a real, unanswered `QMessageBox` — a failure that looks exactly like a
code regression and isn't one. Fixed 2026-08-02 by making each test
generate and clean its own fixtures instead of depending on the operator
remembering a shell incantation first.

`site-store-test` calls `QStandardPaths::setTestModeEnabled(true)` at
the top of `main()` — not optional, and no longer an `XDG_CONFIG_HOME`
environment-variable override either. That override used to be how this
worked, and it was a real bug: `XDG_CONFIG_HOME` is a Linux/XDG-only
convention, so it silently did nothing to isolate this test on native
Windows (only masked by CI's Windows job running under wine, not native
Windows) — `SiteStore` would write to, and the test's own cleanup phase
would delete, whatever `sites.json` a real developer actually had saved
there. `setTestModeEnabled()` is Qt's own cross-platform mechanism for
this exact problem, so it isolates `AppConfigLocation` identically on
every platform.

`site-store-test` also covers `SiteStore::loadFromFile()`/
`saveToFile()` (what `SiteManagerDialog`'s Export.../Import... buttons
actually call) — round-tripped against a temp path distinct from the
real `sites.json`, confirming the real one is left untouched by an
export; the exported file gets the identical raw-JSON no-password-key
inspection the real `sites.json` already gets. Import's id-regeneration
behavior (every imported site gets a fresh id, never the one from the
file — see ARCHITECTURE.md's `SiteStore`/`SavedSite` entry for why)
is exercised directly here too, at the `SiteStore` level, without
constructing a real `SiteManagerDialog` — that dialog has no direct
test coverage of any kind in this codebase, and this doesn't change
that; New/Duplicate/Delete aren't click-tested either, only their
underlying `SiteStore` calls are, and the new Export/Import buttons
follow that same established precedent.

`navigation-test` creates its own scratch directory tree under
`/tmp/nav_test` and doesn't touch anything outside it. Two of its own
fixture bases (`fileOpRaceBase`, `renameRaceBase`) now `removeRecursively()`
before recreating themselves — a real hang found and fixed while
building the macOS CI job: both create a file via the app's own real
"New File..."/rename UI path rather than test setup code, so a
previous run getting killed before cleanup (this exact hang, or a
`Ctrl-C`'d local run) leaves that file behind; the next run's real
file-creation/rename then hits a genuine "already exists" conflict,
pops a `QMessageBox` the dialog-automation timer doesn't expect (it
only watches for `QInputDialog`), and the nested `QMenu::exec()` call
underneath blocks forever waiting for a dialog that will never close.
Never seen in CI (a fresh container every run has no leftover `/tmp`
state to trip over), only in a long-lived local sandbox reused across
many sessions — confirmed via `git stash` that the hang reproduced
identically on unmodified `main`, so it predates and is unrelated to
the macOS work that surfaced it. `transfer-pause-test`
uses a fake in-process backend (no real server, no real files) — see its
own header comment for exactly what it does and doesn't prove.
`file-operations-test` creates its own scratch tree under
`/tmp/file_ops_test` and tests `LocalBackend` directly (not through the
UI, since its prompts can't be driven headlessly) — see its own header
comment for what it does and doesn't cover. Two of its regression
phases simulate a filesystem failure in ways that don't hold on every
platform this test actually runs on in CI — see the "core discipline"
section below for both (root bypassing `chmod 000` in the
container-based CI jobs; `QFile::link()` not producing a real symlink
under `build-windows`'s `wine` job) rather than repeating the detail
here. It also covers `setPermissions()` — a real `chmod` against a real
temp file, confirmed by reading the mode back via `QFileInfo` — and, as
pure-logic checks needing no I/O at all, `PermissionsDialog.h`'s
`permissionsStringToMode()`/`modeToPermissionsString()` round trips
(hence this target's otherwise-GUI-free `Qt6::Widgets` link dependency
now: those two free functions live in `PermissionsDialog.cpp`, but
constructing an actual `PermissionsDialog` is not needed to test them). `folder-transfer-test` needs
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
is deliberate — it's not a live-server test and doesn't claim to be one.
The rest of FTP/FTPS (and SFTP public-key auth) genuinely can be
verified locally now, just not through this self-contained suite — see
"Live-server verification" below. See this test's header comment and
ARCHITECTURE.md's Known gaps for what's still unproven even after that.
`protocol-selection-test` needs the same `setTestModeEnabled()`
isolation `site-store-test` does, and for the same reason: it exercises
SiteStore against real files, so without it, it writes — and its
migration phase deliberately overwrites — whatever `sites.json` is in
your actual config directory. It constructs a real `ConnectionDialog`
and drives its protocol combo, so it needs a `QApplication` and the
offscreen platform, not just `QCoreApplication`. It also covers
`parseQuickConnectString()` (`QuickConnectParser.h/.cpp`, the toolbar
quick-connect field's parser) — pure-logic checks needing no
`ConnectionDialog`/`QApplication` machinery at all, added here rather
than a new target since this file already covers protocol-selection-
adjacent logic and the function has no UI dependency of its own to
justify a separate one.

These ten, plus every target documented in the subsections immediately
below, need to actually pass — not just build — before a change is
considered done. See ARCHITECTURE.md's "Verification status" section for
what each test actually proves and why it exists.

### `sort-and-commands-test`

Added after the original ten above — self-contained and
`EXCLUDE_FROM_ALL` like they are (no external server, no `podman`), and
just as required; kept in its own subsection rather than folded into a
renumbered list above, the pattern every later addition here follows
too. Covers `CommandsPaneWidget`'s log and `FilePaneWidget`'s forwarding
of `RemoteBackend::commandLogged` (via a minimal fake backend, same
technique `transfer-pause-test`'s `FakePausableBackend` uses), and the
file panes' default sort order, numeric Size sort, and
`entryForRow()`'s row-independence after a real
`QTreeView::sortByColumn()` call — see `src/sort_and_commands_test.cpp`'s
own header comment and ARCHITECTURE.md's `CommandsPaneWidget` entry for
the full detail. All four `.github/workflows/build.yml` jobs' "Build
test suite"/"Run test suite" steps include it. Run it locally the same
way:

```
cmake --build build --target sort-and-commands-test
QT_QPA_PLATFORM=offscreen ./build/sort-and-commands-test
```

Needs no fixtures or environment overrides beyond `QT_QPA_PLATFORM` — it
builds and wipes its own scratch directory (`/tmp/sort_test`) at the top
of `main()`, same convention `transfer-queue-test`/`folder-transfer-test`
use.

Also covers the per-pane filename filter: narrowing/restoring
`rowCount()` against the same real `LocalBackend` fixture, and (via a
`FakeLoggingBackend`-supplied dotfile, since `LocalBackend` itself never
returns dotfiles — see ARCHITECTURE.md's `FilePaneWidget` entry) that it
composes with `showHiddenFiles` as AND, not two independent filters
that happen to both work alone. That composition check is the one place
in this file that constructs a real `AppSettings` — a real,
intermittent bug found by running this test twice in a row: the
test-mode `settings.json` path persists ACROSS separate runs of the
binary (unlike a fresh in-memory default), so a freshly-constructed
`AppSettings` there could start with `showHiddenFiles()` already `true`
left over from a previous run, silently invalidating the "starts off"
assumption the first check depends on — fixed by explicitly resetting
it right after construction rather than trusting the default.

### `remote-to-remote-test`

Same treatment as `sort-and-commands-test` above — self-contained,
`EXCLUDE_FROM_ALL`, added to all four `build.yml` jobs. Covers
`TransferManager`'s remote-to-remote
staged-transfer orchestration (direction/phase assignment, the
download-to-temp → upload-from-temp phase transition and
`m_currentBackend` re-pointing, temp-file cleanup on success and on
cancellation during either phase, and `retryItem()`'s phase reset) against
two independent fake `RemoteBackend`s representing two different "remote"
servers — see `src/remote_to_remote_test.cpp`'s own header comment and
ARCHITECTURE.md's `TransferManager` entry for the full detail. Run it
locally the same way:

```
cmake --build build --target remote-to-remote-test
QT_QPA_PLATFORM=offscreen ./build/remote-to-remote-test
```

Needs no fixtures or environment overrides — its temp files live under
`QStandardPaths::TempLocation`'s own `zephyrftp-staging/` subdirectory,
same one the real app uses, cleaned up by the test itself (and by
`TransferManager`'s own constructor, which sweeps any leftovers from a
previous run on startup).

### `move-entry-test`

Same treatment as `sort-and-commands-test`/`remote-to-remote-test`
above — self-contained, `EXCLUDE_FROM_ALL`, added to all four
`build.yml` jobs. Covers the
server-side Move feature: `TransferManager::moveEligible()`'s
`connectionIdentity()`-equality guard (both the eligible and ineligible
cases), `moveEntry()`/`moveFolder()`'s request-id-correlated dispatch to a
backend's `moveEntry()` — confirming a folder move issues exactly one
backend call against the folder's root path with
`listDirectoryForEnumeration()` never called, i.e. `FolderEnumerator` is
genuinely skipped, not just unobserved — a real `LocalBackend`
exercised against real temp files/directories (a plain file move, a move
onto an existing destination file to confirm it overwrites, and a folder
move including its nested contents), and a folder move onto an existing
destination resolved as "Write Into" failing cleanly with a merge-limitation
error rather than dispatching a doomed rename, driven via a REAL
`QMessageBox` using `conflict-resolution-test`'s own live-dialog technique
(a continuous poller here, not a single fixed-delay shot). Also covers
three real bugs a code review found in the Move implementation after it
had already shipped (multi-select Move dropping every entry but the
last one; a Move's "apply to all" conflict choice leaking into an
unrelated later transfer; `retryItem()` having no guard for
`TransferDirection::Move`) — see ARCHITECTURE.md's `TransferManager`
entry for the full detail on each. See `src/move_entry_test.cpp`'s own
header comment and ARCHITECTURE.md's `RemoteBackend`/`TransferManager`
entries for the full detail. Run it
locally the same way:

```
cmake --build build --target move-entry-test
QT_QPA_PLATFORM=offscreen ./build/move-entry-test
```

Needs no fixtures or environment overrides beyond `QT_QPA_PLATFORM` — it
builds and wipes its own scratch directory (`/tmp/move_entry_test`) at the
top of `main()`, same convention `sort-and-commands-test` uses.

### `edit-session-test`

Same treatment as the targets above — self-contained, `EXCLUDE_FROM_ALL`,
added to all four `build.yml` jobs. Covers `EditSessionManager`'s
edit-in-place lifecycle end to end against a real `LocalBackend` standing
in for "the remote" (see the test's own header comment for why that's a
legitimate stand-in, same reasoning `navigation-test`/`transfer-pause-test`
already rely on for their own `LocalBackend`-backed panes): a real
download producing byte-exact temp file content, a real
`QFileSystemWatcher`-detected save (writing new content directly to the
temp path, the same thing an external editor's own save does) triggering
a debounced re-upload that lands the new content back at the original
path, re-editing an already-open file NOT triggering a second download,
and session teardown actually deleting the temp file from disk. See
`src/edit_session_test.cpp`'s own header comment and ARCHITECTURE.md's
`EditSessionManager` entry for the full detail, including the two new
`TransferDirection` values (`EditDownload`/`EditUpload`) and why this
routes through `TransferManager` rather than talking to a `RemoteBackend`
directly. Run it locally the same way:

```
cmake --build build --target edit-session-test
QT_QPA_PLATFORM=offscreen ./build/edit-session-test
```

Needs no fixtures or environment overrides beyond `QT_QPA_PLATFORM` — it
builds and wipes its own scratch directory (`/tmp/edit_session_test`) at
the top of `main()`, same convention `move-entry-test`/`sort-and-commands-test`
use. **Not covered, and documented as such rather than faked**: actually
launching an external editor (`QProcess::startDetached()`/
`QDesktopServices::openUrl()`) isn't something a headless test can verify
beyond "the right command was invoked" — the test points
`externalEditorCommand` at `/bin/true` specifically to exercise
`launchEditor()`'s `QProcess` path without spawning anything that could
hang or need a display, not to prove real editor integration. Verify
that manually: configure a real editor command in Preferences, Edit a
real file against a real local SFTP test server
(`tools/local-test-servers/start-sftp-pubkey.sh`), confirm the editor
opens, edit and save, confirm the change lands on the server, confirm
re-editing the same file reuses the session without re-downloading.

### `queue-persistence-test`

Same treatment as the targets above — self-contained, `EXCLUDE_FROM_ALL`,
added to all four `build.yml` jobs. Covers `TransferQueueStore`'s
save/load round trip and `TransferManager`'s restore/reclaim mechanism
end to end: a `LocalToLocal` item restores and dispatches immediately
with no reconnect needed; a `RemoteToLocal` item restores as
`PendingReconnect`; a reconnect to a *different* server leaves it alone;
a reconnect to the *matching* server claims it (and a second item
sharing that same connection, in the same call) and resumes it from the
persisted `bytesDone` — confirmed reaching `downloadFile()` as a real
resume offset, not just round-tripping through JSON, via a small custom
fake `RemoteBackend` (see the test's own header comment for why
`LocalBackend` can't prove this specifically: it ignores `resumeOffset`
entirely, since a real local copy is always a fresh, atomic
`QFile::copy()`, never a partial resume). Also confirms
`saveQueueForShutdown()`'s exclusions: a `Done` item and a
`RemoteToRemote` item are both absent from what actually gets written,
and a raw read of `queue.json`'s own text confirms no
password/passphrase-shaped key ever appears in it — same explicit
regression guard `site-store-test` already established for `sites.json`.
See `src/queue_persistence_test.cpp`'s own header comment and
ARCHITECTURE.md's `TransferQueueStore` entry for the full detail,
including the real `cancelItem()` gap this work found and fixed along
the way (a second, independent status gate that didn't originally
recognize the new `PendingReconnect` status). Run it locally the same
way:

```
cmake --build build --target queue-persistence-test
QT_QPA_PLATFORM=offscreen ./build/queue-persistence-test
```

Needs no fixtures or environment overrides beyond `QT_QPA_PLATFORM` — it
builds and wipes its own scratch directory (`/tmp/queue_persistence_test`)
at the top of `main()`, same convention `edit-session-test`/
`move-entry-test` use, and isolates `QStandardPaths::AppConfigLocation`
via `setTestModeEnabled(true)` the same way `site-store-test`/
`app-settings-test` already do (both `queue.json` and `sites.json` live
there). **Not covered, and documented as such rather than faked**:
losing the queue on an actual crash (only a clean `closeEvent()` is
exercised — the deliberate scope boundary itself, not a test gap) and
restoring a `RemoteToRemote` item (deliberately excluded from
persistence entirely, nothing to restore). Verify the reconnect-and-
resume flow manually against a real server: queue an upload/download,
quit mid-transfer, relaunch, confirm it shows "Waiting to reconnect",
reconnect to that same site, confirm it resumes and completes.

### `app-settings-test`

Same treatment as the three above — self-contained, `EXCLUDE_FROM_ALL`,
added to all four `build.yml` jobs. `AppSettings`' first-ever test
coverage of any kind:
the full setter/save/load round trip for every persisted field, the
documented fresh-start and corrupt-`settings.json` fallback-to-defaults
behavior, and that a same-value `set()` call is a genuine no-op rather
than silently re-writing defaults over an already-saved value. Also
regression coverage for a real bug a code review found: `save()` used
to write `settings.json` in place, so a crash/power-loss/full-disk
mid-write could corrupt it — and since `load()` treats any parse
failure as total corruption, a truncated file from interrupting the
save of just ONE preference could silently reset every OTHER
already-saved preference back to defaults too. Fixed with `QSaveFile`
(temp file + atomic replace on `commit()`) in place of `QFile` — see
ARCHITECTURE.md's `AppSettings` entry for why the crash-mid-write
recovery itself isn't exercised by this test specifically (no seam to
interrupt a private, always-committing `save()` through `AppSettings`'
own public API) even though it was verified with a standalone probe
before relying on it. Uses `site-store-test`'s same
`QStandardPaths::setTestModeEnabled(true)` isolation, for the same
reason — without it this would read/overwrite whatever `settings.json`
a real developer actually has saved. Run it locally the same way:

```
cmake --build build --target app-settings-test
QT_QPA_PLATFORM=offscreen ./build/app-settings-test
```

### `trust-prompt-test`

Same treatment as the four above — self-contained, `EXCLUDE_FROM_ALL`,
added to all four `build.yml` jobs. Drives the REAL `QMessageBox` that
`HostKeyVerifier::confirmHostKey()`/`CertificateVerifier::confirmCertificate()`
each pop, same live-dialog technique `conflict-resolution-test` uses
(`QApplication::activeModalWidget()` while `exec()` is still blocking).
Regression coverage for two real bugs a code review found in
`HostKeyVerifier.cpp`, fixed together with `CertificateVerifier.cpp`
(same pattern in both): the prompt used to show only the host, never
the port, even though the known-hosts/known-certs stores are both keyed
on host+port together — two saved sites sharing a hostname on different
ports got an identical, indistinguishable prompt; and the dialog was
shown with a `nullptr` parent instead of the main window, with no
guaranteed stacking/focus relationship to the app. Run it locally the
same way:

```
cmake --build build --target trust-prompt-test
QT_QPA_PLATFORM=offscreen ./build/trust-prompt-test
```

## Live-server verification (SFTP public-key auth, FTP/FTPS, cancel/pause/resume)

The targets above are all deliberately self-contained — no external
server needed. `SftpBackend`'s public-key auth path, its real
mid-transfer cancel/pause/resume, and all of `FtpBackend` used to be
genuinely unverified as a result (see ARCHITECTURE.md's Known gaps):
nothing in this environment could reach a real server that way.
`tools/local-test-servers/` closes that — throwaway local `sshd`/FTP/FTPS
servers (no root, no system config touched) plus seven harnesses that
drive the real backend classes (and, for the three Move/remote-to-remote
ones, real `TransferManager`/`FilePaneWidget` orchestration on top of
them, not just the backend directly) against them:

```
tools/local-test-servers/start-sftp-pubkey.sh
tools/local-test-servers/start-ftp.sh
tools/local-test-servers/start-ftps.sh
tools/local-test-servers/start-ftp-legacy-list.sh
tools/local-test-servers/start-ftps-trusted.sh
tools/local-test-servers/start-ftp-active-only.sh

cmake --build build --target verify-sftp-pubkey verify-ftp-live verify-sftp-pause-cancel verify-ftps-trust
QT_QPA_PLATFORM=offscreen SFTP_TEST_SCRATCH=/tmp/zephyrftp-local-test-servers/sftp ./build/verify-sftp-pubkey
QT_QPA_PLATFORM=offscreen ./build/verify-ftp-live
QT_QPA_PLATFORM=offscreen SFTP_TEST_SCRATCH=/tmp/zephyrftp-local-test-servers/sftp ./build/verify-sftp-pause-cancel
QT_QPA_PLATFORM=offscreen ./build/verify-ftps-trust

tools/local-test-servers/stop-all.sh
```

Three more, specifically for `TransferManager`'s server-side Move and
remote-to-remote features — see each `.cpp`'s own header comment for the
exact real-server gap each one closes, and ARCHITECTURE.md's
Verification status entries for both features. `verify-sftp-move`/
`verify-ftp-move` each need one server instance (two independent
connections *to* it — real cross-pane Move eligibility, not simulated);
`verify-remote-to-remote-live` needs two independent SFTP instances on
different ports (two genuinely different servers, matching
`TransferManager`'s remote-to-remote staging path):

```
tools/local-test-servers/start-sftp-pubkey.sh
cmake --build build --target verify-sftp-move
QT_QPA_PLATFORM=offscreen SFTP_TEST_SCRATCH=/tmp/zephyrftp-local-test-servers/sftp \
    ./build/verify-sftp-move
tools/local-test-servers/stop-all.sh

tools/local-test-servers/start-ftp.sh
cmake --build build --target verify-ftp-move
QT_QPA_PLATFORM=offscreen FTP_TEST_SCRATCH=/tmp/zephyrftp-local-test-servers/ftp \
    ./build/verify-ftp-move
tools/local-test-servers/stop-all.sh

SFTP_TEST_SCRATCH=/tmp/zephyrftp-local-test-servers/sftp-a SFTP_TEST_PORT=2222 \
    tools/local-test-servers/start-sftp-pubkey.sh
SFTP_TEST_SCRATCH=/tmp/zephyrftp-local-test-servers/sftp-b SFTP_TEST_PORT=2224 \
    tools/local-test-servers/start-sftp-pubkey.sh
cmake --build build --target verify-remote-to-remote-live
QT_QPA_PLATFORM=offscreen \
    SFTP_TEST_SCRATCH_A=/tmp/zephyrftp-local-test-servers/sftp-a SFTP_TEST_PORT_A=2222 \
    SFTP_TEST_SCRATCH_B=/tmp/zephyrftp-local-test-servers/sftp-b SFTP_TEST_PORT_B=2224 \
    ./build/verify-remote-to-remote-live
tools/local-test-servers/stop-all.sh
```

All three are safe to re-run against the same already-running server(s)
without a restart — each resets its own fixtures/leftover destination
files up front for exactly that reason (see each `.cpp`'s own header
comment for the real conflict-collision bug that not doing so produced
during development). `verify-ftp-move` needs `python3-pyftpdlib` on
`PATH` for `start-ftp.sh` the same as every other FTP local-test-server
target — see this file's own Fedora dependency list above; on a
distro without a packaged `pyftpdlib` (confirmed needed in this
project's own dev environment), a throwaway venv
(`python3 -m venv .venv && .venv/bin/pip install pyftpdlib pyOpenSSL`,
then run `start-ftp.sh` with that venv's `bin/` prepended to `PATH`)
works just as well — `ftp_server.py` has no other dependency.

`tools/local-test-servers/containers/` goes one step further: real
vsftpd/proftpd/Dropbear, not pyftpdlib — each built from its own
`Containerfile` and run in a throwaway `podman` container, for genuine
vendor/implementation diversity rather than a Python stand-in this
project fully controls. **`podman` is only needed for this specific
piece** (same scoping as `wine` above — not needed for the build or any
of the self-contained targets above):

```
tools/local-test-servers/start-vsftpd.sh
tools/local-test-servers/start-proftpd.sh
tools/local-test-servers/start-dropbear.sh

cmake --build build --target verify-ftp-vendors verify-sftp-vendors
QT_QPA_PLATFORM=offscreen ./build/verify-ftp-vendors
QT_QPA_PLATFORM=offscreen ./build/verify-sftp-vendors

tools/local-test-servers/stop-all.sh   # stops everything above too, containers included
```

`verify-ftp-vendors`'s two plain-FTP phases (vsftpd, proftpd — not
repeated over FTPS, see this file's own header comment for why) also
exercise `FtpBackend::setPermissions()` — a real `SITE CHMOD` against
each real vendor. It can't confirm the applied mode by re-listing
through `FtpBackend` itself (its `LIST`/`MLSD` parsers deliberately
never translate real permission bits — every FTP entry's
`RemoteEntry::permissions` is a `"-"` placeholder, a pre-existing,
disclosed limitation unrelated to chmod), so it shells out to `podman
exec <container> stat -c %a <path>` instead — ground truth straight
from the container's own filesystem, independent of the client's own
listing code entirely. `verify_sftp_pubkey.cpp` (above) similarly
exercises `SftpBackend::setPermissions()` — a real
`libssh2_sftp_setstat()` against the real local `sshd`, this time
confirmed the more direct way, by re-listing through the client itself
and checking the returned `RemoteEntry::permissions` string, since
`SftpBackend`'s own `LIST`/attribute parsing (unlike `FtpBackend`'s)
already reports real bits.

Proxy support (SOCKS5/HTTP CONNECT) gets its own two harnesses, each
proving `SftpBackend`, plain `FtpBackend`, and FTPS all genuinely
tunnel through a real proxy — not just that `ProxyConfig`/
`ConnectThroughProxy()` compiles. See `ARCHITECTURE.md`'s `ProxyConfig`
entry for why a hand-rolled SOCKS5/HTTP-CONNECT handshake was needed at
all (`QAbstractSocket::setProxy()` alone isn't enough for the two
backends that need the real socket descriptor) and for the real
containerization gotcha (`--network host`) getting the second harness
below working against loopback-bound test servers:

```
tools/local-test-servers/start-sftp-pubkey.sh
tools/local-test-servers/start-socks5-proxy.sh
tools/local-test-servers/start-ftp.sh
tools/local-test-servers/start-ftps-trusted.sh

cmake --build build --target verify-socks5-proxy
QT_QPA_PLATFORM=offscreen ./build/verify-socks5-proxy

tools/local-test-servers/stop-all.sh
```

```
tools/local-test-servers/start-sftp-pubkey.sh
tools/local-test-servers/start-http-connect-proxy.sh
tools/local-test-servers/start-ftp.sh
tools/local-test-servers/start-ftps-trusted.sh

cmake --build build --target verify-http-connect-proxy
QT_QPA_PLATFORM=offscreen ./build/verify-http-connect-proxy

tools/local-test-servers/stop-all.sh
```

`start-socks5-proxy.sh` needs no new dependency — it's OpenSSH's own
`ssh -D` dynamic port forwarding, tunneled through the sshd
`start-sftp-pubkey.sh` already starts, and `ssh` is already on `PATH`
wherever `ssh-keygen` (that same script's own dependency) is.
`start-http-connect-proxy.sh` needs `podman` (already a dependency for
the vsftpd/proftpd/Dropbear containers above) — tinyproxy itself
installs automatically inside its own throwaway container
(`containers/Containerfile.tinyproxy`), same as vsftpd/proftpd, no
separate host-side package needed. Both harnesses include a negative
control (`verify-socks5-proxy`: a deliberately wrong, nothing-listening
proxy port; `verify-http-connect-proxy`: deliberately wrong proxy
credentials) that must make the connection attempt genuinely fail —
without it, a silently-ignored proxy setting would make every other
check pass just as easily via an accidental direct connection.

One more harness goes a step further still: `verify-sftp-throughput`
needs a real, externally-provided, non-loopback server — nothing in
`tools/local-test-servers/` can substitute, since the whole point is a
real round-trip time, and every throwaway server above is loopback
(~0ms RTT). Env-var-configured rather than pointed at a fixed local
container:

```
cmake --build build --target verify-sftp-throughput
ZEPHYR_THROUGHPUT_HOST=<host> ZEPHYR_THROUGHPUT_USER=<user> \
    ZEPHYR_THROUGHPUT_PASSWORD=<password> \
    QT_QPA_PLATFORM=offscreen ./build/verify-sftp-throughput
```

One more harness needs no external server or script at all, just a real
local OS credential store — `verify-credential-store` exercises
`CredentialStore` (save/load/hasSecret/remove, including non-ASCII
content) against whatever Secret Service, Credential Manager, or
Keychain is already running on your machine (Linux/Windows/macOS
respectively — this is the one target that actually proves the macOS
Keychain Services backend round-trips against a real keychain, not
just that it compiled; run for real in the `build-macos` CI job).
Writes and unconditionally removes its own clearly-namespaced test
entries, so it's safe to run against a real, in-use keyring:

```
cmake --build build --target verify-credential-store
QT_QPA_PLATFORM=offscreen ./build/verify-credential-store
```

See `tools/local-test-servers/README.md` for the full picture (including
the real containerization gotchas — PAM/GDBM/foreground-mode quirks —
hit and fixed while building the vsftpd/proftpd/Dropbear containers) and
ARCHITECTURE.md's Known gaps entries for FTP/FTPS, certificate
trust-on-first-use, public-key authentication, cancel/pause/resume, and
vendor diversity for exactly what's confirmed and what still isn't,
even after this, plus Move/remote-to-remote's own entries there for what
`verify-sftp-move`/`verify-ftp-move`/`verify-remote-to-remote-live`
specifically confirm.
These seven targets are `EXCLUDE_FROM_ALL` like the required suite
above but intentionally **not** part of that suite or CI — an
external-server precondition is a different category from "always runs
the same way," which is the whole point of the required suite being
self-contained.

**Deliberately never added to CI, on purpose, not just left out for
lack of time**: unlike the required self-contained targets, these need
an external server (or, for the vendor containers, `podman`) already
running, and the vendor containers specifically add real per-run cost
(building/starting three containers) that isn't worth paying on every
push. The actual rule this project runs on: **local testing is the gate
before pushing, not CI** — if you've touched `FtpBackend`/`SftpBackend`,
run the relevant live-server and vendor-container harnesses locally
first, the same way you'd run the required suite, and only push once
those pass. CI (the required suite, on every push/PR/tag; the full
platform build on tags) is there to catch environment drift and confirm
what already passed locally still passes on GitHub's own runners — it
is not a substitute for having run the fuller local suite first when
the change touches something these harnesses cover.

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
- A regression test that simulated "an unreadable source file" via
  `chmod 000` passed everywhere it was ever run locally, then broke
  three of four release build jobs at once — because this project's own
  container-based CI jobs run their test suite as **root**, and root
  bypasses Unix permission bits entirely. Caught by reading the actual
  CI failure logs and reproducing the exact container as root (`podman
  run fedora:44`) rather than assuming "it passes locally" generalizes.
  Fixed by simulating the failure with a directory as the copy source
  instead — `QFile::copy()` refuses to open a directory regardless of
  privilege, verified both as a normal user and as root.
- `QFile::link()` does not create a real filesystem symlink on Windows
  the way it does on Unix — it writes a small Shell-Shortcut-style file
  instead, which `QFileInfo` reports as neither a directory nor a
  symlink. A regression test that assumed otherwise passed on every
  platform this project had actually run it on until `build-windows`'s
  CI job (which runs its test suite under `wine`) caught it. Confirmed
  with a standalone probe built and run under `wine` before touching
  the real test, not assumed from the Qt docs alone.

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
  (`python3 -c "import yaml,sys; yaml.safe_load(open('.github/workflows/build.yml'))"`)
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
