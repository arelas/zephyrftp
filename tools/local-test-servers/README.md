# tools/local-test-servers/

Throwaway local SFTP/FTP/FTPS servers for exercising `SftpBackend` and
`FtpBackend` against something real — not a mock, not just "the code
compiles." This is what closed five gaps ARCHITECTURE.md used to list
as "never verified": public-key SFTP auth, `checkExists()`,
`listDirectoryForEnumeration()`/the recursive folder-transfer walk, real
mid-transfer cancel/pause/resume, and FTP/FTPS touching a real server at
all. See ARCHITECTURE.md's "Known gaps" entries for FTP/FTPS,
public-key authentication (which also covers the two SFTP primitives),
and cancel/pause/resume for exactly what's now confirmed and what still
isn't.

Nothing here touches system config, needs root, or installs a system
service — everything lives under `/tmp/zephyrftp-local-test-servers/`
and runs as your own user on unprivileged ports. Safe to run repeatedly;
each `start-*.sh` stops any instance it previously started (so re-running
without stopping first doesn't orphan a process silently holding the
port) before wiping and regenerating its own scratch directory.

Dependencies (Fedora): `openssh-server` (for `sshd`/`ssh-keygen`, likely
already installed), `python3-pyftpdlib`, `python3-pyOpenSSL`
(`sudo dnf install -y python3-pyftpdlib python3-pyOpenSSL`).

## Starting the servers

```
tools/local-test-servers/start-sftp-pubkey.sh     # sshd, pubkey-only, 127.0.0.1:2222
tools/local-test-servers/start-ftp.sh             # plain FTP, 127.0.0.1:2121
tools/local-test-servers/start-ftps.sh            # explicit FTPS, self-signed cert, 127.0.0.1:2122
tools/local-test-servers/start-ftp-legacy-list.sh # plain FTP, MLSD disabled, 127.0.0.1:2123
tools/local-test-servers/start-ftps-trusted.sh    # explicit FTPS, CA-signed cert, 127.0.0.1:2124
tools/local-test-servers/start-ftp-active-only.sh # plain FTP, PASV/EPSV disabled, 127.0.0.1:2125
```

Each prints the connection details it just generated (private key path,
username/password, served directory). Self-signed on purpose for
`start-ftps.sh` — `FtpBackend` is supposed to reject it as untrusted
(without ever having seen it before) unless a person explicitly trusts
it; see the script's own header comment and `verify-ftps-trust` below.
`start-ftps-trusted.sh` is the opposite case: a leaf certificate signed
by a throwaway local CA, for proving a full encrypted transfer completes
when the certificate genuinely validates (the CA cert it prints has to
be explicitly trusted by whatever's connecting — see `verify-ftp-live`'s
CA-trusted-transfer phase for how). `start-ftp-legacy-list.sh` and
`start-ftp-active-only.sh` each disable one real server-side feature
(MLSD, PASV/EPSV) so `FtpBackend`'s fallback for that feature is
exercised against a server that genuinely lacks it, not just
unit-tested against crafted sample data.

## Running the verification harnesses

```
cmake --build build --target verify-sftp-pubkey verify-ftp-live verify-sftp-pause-cancel verify-ftps-trust

QT_QPA_PLATFORM=offscreen SFTP_TEST_SCRATCH=/tmp/zephyrftp-local-test-servers/sftp \
    ./build/verify-sftp-pubkey

QT_QPA_PLATFORM=offscreen FTP_TEST_SCRATCH=/tmp/zephyrftp-local-test-servers/ftp \
    ./build/verify-ftp-live

QT_QPA_PLATFORM=offscreen SFTP_TEST_SCRATCH=/tmp/zephyrftp-local-test-servers/sftp \
    ./build/verify-sftp-pause-cancel

QT_QPA_PLATFORM=offscreen ./build/verify-ftps-trust
```

All four drive the real backend classes directly
(`src/verify_sftp_pubkey.cpp`, `src/verify_ftp_live.cpp`,
`src/verify_sftp_pause_cancel.cpp`, `src/verify_ftps_trust.cpp`) — real
connect, list, download, upload, cancel, pause, and resume, with content
checked both client-side and by reading files back directly off the
server's own disk. `verify-ftp-live` needs `start-ftp.sh`, `start-ftps.sh`,
`start-ftp-legacy-list.sh`, `start-ftps-trusted.sh`, and
`start-ftp-active-only.sh` all running (five phases, one per server).
`verify-ftps-trust` needs `start-ftps.sh` and drives the real
`CertificateVerifier` trust-on-first-use prompt end to end — same
"auto-accept the real modal dialog" technique `verify-sftp-pubkey` uses
for the SSH host-key prompt, so it's fully automated despite popping a
real `QMessageBox`. `verify-sftp-pause-cancel` generates a real
(~300MB) local file on first use and reuses it afterward
(`/tmp/zephyrftp_verify_pause_source.bin`), needed so the transfer runs
long enough for a cancel/pause request to land reliably mid-flight
rather than the whole thing finishing before it can. Exit code reflects
pass/fail (see CONTRIBUTING.md's wine section for why exit code, not
console text, is the reliable signal in general — not specific to
these, but the same habit applies). These are deliberately **not** part
of the ten-target `EXCLUDE_FROM_ALL` test suite or CI: they need an
external server already running, which those ten are built specifically
to avoid depending on.

## Stopping the servers

```
tools/local-test-servers/stop-all.sh
```

Safe to run even if none are up.
