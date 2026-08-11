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
(`sudo dnf install -y python3-pyftpdlib python3-pyOpenSSL`). The
`containers/` subdirectory (real vsftpd/proftpd/Dropbear, see below)
additionally needs `podman` — nothing else here does.

## Real vendor servers (`containers/`)

`start-ftp.sh` and friends above are all pyftpdlib — a Python stand-in
this project fully controls, useful but not a genuinely different,
independently-implemented server. `containers/` closes that gap for
real: actual vsftpd, proftpd, and Dropbear, each built from its own
`Containerfile` (`fedora:44` base, the same image `build-windows`'s CI
job already uses) and run in a throwaway `podman` container — no
third-party pre-built images, no host system packages installed, fully
start/stop at will.

```
tools/local-test-servers/start-vsftpd.sh   # real vsftpd, 127.0.0.1:2126 — never implements MLSD at all
tools/local-test-servers/start-proftpd.sh  # real proftpd, 127.0.0.1:2127 — MLSD explicitly denied (550)
tools/local-test-servers/start-dropbear.sh # real Dropbear SFTP, 127.0.0.1:2223 — password auth
```

`start-vsftpd.sh` is the single highest-value addition: vsftpd has never
implemented MLSD/RFC 3659 in any version, so it exercises `FtpBackend`'s
real legacy-`LIST` fallback trigger against a real, unmodified,
widely-deployed server — not pyftpdlib with a flag forcing MLSD off.
`start-proftpd.sh` is a second, independently-coded implementation with
its own `LIST`-format quirks; this build's `mod_facts` is compiled in
(Fedora doesn't ship a variant without it), so MLSD is denied via a
`<Limit>` block instead — which replies `550`, not the `500`/`502` a
genuinely-unimplemented command gets. That discovery is real, not
theoretical: `FtpBackend.cpp`'s fallback trigger now also treats `550`
as a fallback signal, exactly because this container found a real,
plausible server configuration (MLSD present but administratively
denied) the trigger didn't originally cover.

Both containers also serve FTPS (explicit `AUTH TLS`, a throwaway
CA-signed cert baked into each image at build time, whose CA
certificate each `start-*.sh` script copies out via `podman cp` and
prints the path to). `proftpd.conf`'s `mod_tls` is left at its own
default **strict TLS-session-reuse enforcement** (see that file's
comment on why omitting `TLSOptions NoSessionReuseRequired` is what
keeps it on) — the real, honest test of whether `FtpBackend`'s TLS
session reuse actually satisfies a strict server. It now genuinely
does: `FtpTlsSocket` (raw OpenSSL, forced to TLS 1.2 — see
`ARCHITECTURE.md`'s Known gaps entry) replaced the earlier
`QSslSocket`-based attempt, which this same container's `TLSLog`
first proved didn't satisfy `mod_tls`'s check at all. This container is
what `verify-ftp-vendors`' `proftpd-ftps` phase runs a full
list/download/upload round trip against, unmodified strict config and
all.

`vsftpd.conf` ships with `require_ssl_reuse=NO`, and that's a
deliberate, documented trade, not an oversight: turning it on
reproduces a genuine deadlock inside vsftpd's own privilege-separated
architecture in this specific container environment (confirmed via
strace — attaching `ptrace` to the process tree reliably unsticks it
within seconds; left undisturbed, it never resolves even after 4+
minutes), not a `FtpBackend` bug, and not something further client-side
changes can fix since the server itself never responds. With it off,
FTPS against vsftpd now completes a full real round trip — connect,
list, download, upload, content verified both ways — closing the
vendor-diversity gap for real. See ARCHITECTURE.md's Known Gaps for the
complete investigation, including three real client-side bugs this
same testing found and fixed along the way (none specific to either
vendor).

`start-dropbear.sh` is a genuinely different SSH daemon from the
`start-sftp-pubkey.sh`/OpenSSH server above, with password auth instead
of that one's pubkey-only setup — real transport/session/auth-layer
diversity. Stated plainly, not glossed over: Dropbear's Fedora package
ships no `sftp-server` binary of its own (confirmed:
`rpm -ql dropbear` has none) — it shells out to an external one,
configured here to be OpenSSH's own
(`/usr/libexec/openssh/sftp-server`). So this is real diversity at the
SSH transport/auth layer, not proof of a genuinely different
SFTP-wire-protocol implementation — see ARCHITECTURE.md's Known Gaps
entry for the full accounting.

Real containerization gotchas hit and fixed while building these — worth
knowing if you ever touch the `Containerfile`s/configs — are documented
directly in `containers/vsftpd.conf`, `containers/Containerfile.vsftpd`,
`containers/pam-vsftpd-virtual`, and `containers/Containerfile.dropbear`:
vsftpd's and proftpd's standalone modes both daemonize by default and
need an explicit foreground flag/config (`background=NO`, `-n`) or a
container's PID 1 sees an instant, silent exit; vsftpd's PAM stack
rejects a real system account unless its shell is listed in
`/etc/shells`; and — the one that took the most digging — Fedora's
`pam_userdb.so` links against `libgdbm`, not Berkeley DB, uses the `db=`
path completely literally with no `.db` suffix ever appended, and
defaults to expecting a `crypt()`-hashed value unless told `crypt=none`.
None of this was assumed from documentation; each was confirmed directly
against a real failure (`strace`, a tiny standalone PAM test program)
before being fixed.

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
cmake --build build --target verify-sftp-pubkey verify-ftp-live verify-sftp-pause-cancel \
    verify-ftps-trust verify-ftp-vendors verify-sftp-vendors

QT_QPA_PLATFORM=offscreen SFTP_TEST_SCRATCH=/tmp/zephyrftp-local-test-servers/sftp \
    ./build/verify-sftp-pubkey

QT_QPA_PLATFORM=offscreen FTP_TEST_SCRATCH=/tmp/zephyrftp-local-test-servers/ftp \
    ./build/verify-ftp-live

QT_QPA_PLATFORM=offscreen SFTP_TEST_SCRATCH=/tmp/zephyrftp-local-test-servers/sftp \
    ./build/verify-sftp-pause-cancel

QT_QPA_PLATFORM=offscreen ./build/verify-ftps-trust

QT_QPA_PLATFORM=offscreen ./build/verify-ftp-vendors    # needs start-vsftpd.sh + start-proftpd.sh
QT_QPA_PLATFORM=offscreen ./build/verify-sftp-vendors   # needs start-dropbear.sh
```

`verify-ftp-vendors` now passes cleanly end to end, all four phases —
plain FTP and FTPS against both vsftpd and proftpd, each completing a
full real round trip (connect, list, download, upload, content
verified both ways) over the real vendor server, including proftpd's
own strict TLS-session-reuse enforcement (unrelaxed, default config).
That used to be a documented, expected failure — see
`ARCHITECTURE.md`'s Known gaps entry for the fix (`FtpTlsSocket`, raw
OpenSSL) and the investigation that found it.

All six drive the real backend classes directly
(`src/verify_sftp_pubkey.cpp`, `src/verify_ftp_live.cpp`,
`src/verify_sftp_pause_cancel.cpp`, `src/verify_ftps_trust.cpp`,
`src/verify_ftp_vendors.cpp`, `src/verify_sftp_vendors.cpp`) — real
connect, list, download, upload, cancel, pause, and resume, with content
checked both client-side and (where the server is a native process with
a host-visible scratch directory) by reading files back directly off
the server's own disk. `verify-ftp-live` needs `start-ftp.sh`, `start-ftps.sh`,
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
rather than the whole thing finishing before it can. `verify-ftp-vendors`
and `verify-sftp-vendors` are the real-vendor harnesses for the
`containers/` servers above; since those containers are self-contained
(test content baked into the image, no host-mounted scratch directory),
their round-trip upload is verified by downloading it back through the
same protocol session instead of reading the container's filesystem
directly. Exit code reflects pass/fail (see CONTRIBUTING.md's wine
section for why exit code, not console text, is the reliable signal in
general — not specific to these, but the same habit applies). These are
deliberately **not** part of the required, self-contained
`EXCLUDE_FROM_ALL` test suite or CI (see CONTRIBUTING.md's "Running the
test suites" for that list's current count): they need an external
server already running, which that suite is built specifically to
avoid depending on.

### `verify-sftp-throughput`: a real, non-loopback server only

Unlike every harness above, `verify-sftp-throughput`
(`src/verify_sftp_throughput.cpp`) can't be pointed at a container this
project spins up for itself — the whole point is a real round-trip time,
and every local container here is loopback (~0ms RTT). It reads
connection details entirely from the environment and refuses to run
without them:

```
ZEPHYR_THROUGHPUT_HOST=<host> ZEPHYR_THROUGHPUT_USER=<user> \
    ZEPHYR_THROUGHPUT_PASSWORD=<password> [ZEPHYR_THROUGHPUT_PORT=22] \
    [ZEPHYR_THROUGHPUT_FILE_SIZE_MB=100] \
    cmake --build build --target verify-sftp-throughput && \
    QT_QPA_PLATFORM=offscreen ./build/verify-sftp-throughput
```

It uploads and downloads a freshly-generated file of the requested size,
verifies it byte-for-byte, and reports `SftpBackend`'s own MB/s
alongside an independent `scp`/`ssh` baseline against the same server
(non-interactive password auth via `SSH_ASKPASS_REQUIRE=force`, no extra
package needed) — see ARCHITECTURE.md's throughput Known Gaps entry for
the real result this produced.

## Stopping the servers

```
tools/local-test-servers/stop-all.sh
```

Safe to run even if none are up.
