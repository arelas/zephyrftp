# CLAUDE.md

Non-negotiables for this project:

- **No passwords on disk.** `SavedSite`/`SiteStore` deliberately never
  persist a password or key passphrase — see ARCHITECTURE.md. Any change
  touching credential storage must preserve this; `site-store-test`'s
  raw-JSON key inspection is what actually enforces it, not code review.
- **Tests before claims.** "It compiles" is not "it works." All ten
  `EXCLUDE_FROM_ALL` test targets listed in CONTRIBUTING.md need to
  actually pass, not just build, before a change is done. Where a claim
  can't be tested in this environment, say so explicitly rather than
  letting it read as verified.
- **Verify rather than assume.** Read the actual header/API/YAML before
  trusting an assumption about its shape — this codebase has been bitten
  repeatedly by exactly that shortcut (see CONTRIBUTING.md's "core
  discipline" section for real examples).

## Current work: MSVC+vcpkg → MinGW cross-compilation from Fedora

Migrating Windows builds off MSVC+vcpkg to cross-compiling with MinGW on
Fedora. The libssh2 fix referenced in project history is already in.

Two known trouble spots, flagged in advance so they're recognized rather
than re-debugged from scratch:

- AUTOMOC / `.qrc` resource compilation may need the host-side Qt tools
  (moc, rcc, uic) pointed at explicitly under cross-compilation — the
  cross toolchain's own tools produce Windows binaries, not something
  that can run at build time on the host.
- The ten test targets build Windows `.exe`s that won't run natively on
  this Linux host. `wine ./build-win/<target>.exe` is the local check;
  a real Windows machine remains the source of truth for anything wine
  can't be fully trusted on.
