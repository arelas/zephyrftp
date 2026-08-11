# CLAUDE.md

Non-negotiables for this project:

- **No secrets in `sites.json`, ever.** `SavedSite`/`SiteStore` never
  gain a plaintext-in-JSON password or passphrase field — see
  ARCHITECTURE.md. This is narrower than it used to be: a password/
  passphrase CAN now be persisted, but only opt-in (SiteManagerDialog's
  "Save password" checkbox, unchecked by default) and only via
  `CredentialStore`, which writes to the OS's own credential store
  (libsecret/Keychain-equivalent on Linux, the real Windows Credential
  Manager on Windows) — never to a file this app writes itself. Any
  change touching credential storage must preserve both halves of this:
  `site-store-test`'s raw-JSON key inspection (still enforces zero
  secrets in `sites.json`) and `CredentialStore` staying the only place
  a secret is ever written to disk.
- **Tests before claims.** "It compiles" is not "it works." Every
  self-contained `EXCLUDE_FROM_ALL` test target CONTRIBUTING.md's
  "Running the test suites" section documents as required needs to
  actually pass, not just build, before a change is done. Deliberately
  not naming a count here: it started at ten, has grown several times
  since (each addition documented in its own subsection there rather
  than triggering a rename sweep), and will keep growing — check
  CONTRIBUTING.md itself for the current, exact list rather than trusting
  a number that will go stale again. Where a claim can't be tested in
  this environment, say so explicitly rather than letting it read as
  verified.
- **Verify rather than assume.** Read the actual header/API/YAML before
  trusting an assumption about its shape — this codebase has been bitten
  repeatedly by exactly that shortcut (see CONTRIBUTING.md's "core
  discipline" section for real examples).
- **License is GPL-3.0-or-later, not MIT.** Relicensed from MIT — see
  [LICENSE](LICENSE), README.md's License section, and the About dialog
  (`MainWindow.cpp`) for the three places this has to stay consistent.
  Don't reintroduce MIT boilerplate or headers on this project's own
  code. The one deliberate exception: the vendored `resources/icons/`
  set (Tabler Icons) keeps its own original MIT license — that's a
  separate copyright holder's code, not this project's, and stays
  untouched.
