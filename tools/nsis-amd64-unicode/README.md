# NSIS amd64-unicode stubs and plugins

Vendored, not built from source. Upstream NSIS (both `nsis-3.12-setup.exe`
and `nsis-3.12.zip` from https://nsis.sourceforge.io/) ships **zero**
64-bit (`amd64-unicode`) stub support at all — confirmed directly by
extracting the official zip and finding only `x86-ansi`/`x86-unicode`
variants in its `Stubs/` directory. `tools/windows-installer.nsi`'s own
`Target amd64-unicode` directive needs these to build at all.

The files here are extracted from Fedora's `mingw64-nsis` package
(`mingw64-nsis-3.11-3.fc44.noarch.rpm`, license `Zlib AND CPL-1.0`,
https://bugz.fedoraproject.org/mingw-nsis) — the same package
`.github/workflows/build.yml`'s `build-windows` job already depends on
for the exact same reason, cross-compiling from a Fedora container. This
directory exists so `build-windows-native` (native MSVC build on a
self-hosted Windows runner, no Fedora container involved) can use the
same amd64 stub/plugin support without needing network access to
Fedora's package mirrors from that machine at build time.

- `Stubs/*-amd64-unicode` — the 6 compressor variants (`zlib`, `bzip2`,
  `lzma`, each plain and `_solid`) `makensis`'s `Target amd64-unicode`
  needs to link against.
- `Plugins/amd64-unicode/*.dll` — the amd64-unicode builds of NSIS's own
  bundled plugins (`StartMenu.dll` etc.) that `MUI2.nsh`'s
  `MUI_PAGE_STARTMENU` macro needs; `windows-installer.nsi` uses that
  macro for its Start Menu folder page.

`build.yml`'s "Build Windows installer" step copies these into the
winget-installed NSIS's own `Stubs/`/`Plugins/amd64-unicode/`
directories before invoking `makensis` — that installer only ships the
x86 set winget's silent install selected.
