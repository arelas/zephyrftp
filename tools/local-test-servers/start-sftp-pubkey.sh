#!/bin/bash
# Spins up a throwaway sshd instance on 127.0.0.1:2222, restricted to
# SFTP-only, public-key-only auth for the current user — for exercising
# SftpBackend's PublicKey auth path against a real server, which
# (per ARCHITECTURE.md's "Known gaps") had never actually been tried
# against a real key file before this existed.
#
# Deliberately NOT the system sshd: everything lives under a scratch
# directory this script owns outright (host key, client key,
# authorized_keys, the served directory, the pid file), started as the
# current user on an unprivileged port, so this never touches real SSH
# config or needs root. StrictModes no sidesteps sshd's usual
# home-directory permission checks, which don't apply cleanly to a
# scratch dir under /tmp; ForceCommand/Subsystem restrict every session
# on this throwaway server to SFTP only, nothing else.
set -euo pipefail

SCRATCH="${SFTP_TEST_SCRATCH:-/tmp/zephyrftp-local-test-servers/sftp}"
PORT="${SFTP_TEST_PORT:-2222}"

# Stop any instance THIS script previously started before wiping its pid
# file below — otherwise a re-run while an old one is still alive leaves
# it orphaned, silently holding the port (its own config/pid file just
# vanished under it, but the process itself keeps running), and the new
# sshd then fails to bind with a confusing "already in use" error that
# doesn't obviously point back here.
if [ -f "$SCRATCH/sshd.pid" ]; then
    kill "$(cat "$SCRATCH/sshd.pid")" 2>/dev/null || true
    sleep 0.2
fi

# Restore traversal permission on a previous run's chmod-000 fixture (see
# "restricted" below) before wiping — confirmed directly, not assumed:
# plain `rm -rf` on this system does NOT auto-recover from that on its
# own (no chmod-and-retry fallback), it just fails outright and — with
# `set -euo pipefail` above — aborts the whole script on a second run.
chmod -R u+rwx "$SCRATCH" 2>/dev/null || true
rm -rf "$SCRATCH"
mkdir -p "$SCRATCH/root" "$SCRATCH/root/uploads" "$SCRATCH/root/pause_test"
echo "hello from the local sftp test server" > "$SCRATCH/root/sample.txt"

# A real nested tree — mirrors folder-transfer-test.cpp's local fixture
# (multi-level nesting, a genuinely empty leaf directory) so the same
# shape can be walked via listDirectoryForEnumeration()/FolderEnumerator
# against a real server, not just LocalBackend.
mkdir -p "$SCRATCH/root/testfolder/subdir1" \
         "$SCRATCH/root/testfolder/subdir2/nested" \
         "$SCRATCH/root/testfolder/emptydir"
echo "hi from a" > "$SCRATCH/root/testfolder/a.txt"
echo "hi from b" > "$SCRATCH/root/testfolder/subdir1/b.txt"
echo "hi from c" > "$SCRATCH/root/testfolder/subdir1/c.txt"
echo "hi from d" > "$SCRATCH/root/testfolder/subdir2/nested/d.txt"

# A real permission-denied fixture for checkExists()'s ambiguous-stat-
# failure case — see SftpBackend::checkExists()'s own comment. chmod 000
# denies EVEN THE OWNER traversal (execute) permission into this
# directory, which is enough on its own: stat()-ing a path needs
# execute permission on every ancestor directory, not read/write
# permission on the target itself, so this genuinely reproduces
# EACCES from libssh2_sftp_stat() without needing a second test user —
# this server already runs as the current user (see this script's own
# header comment on why: AllowUsers $(whoami), no privilege drop).
mkdir -p "$SCRATCH/root/restricted"
echo "never readable by design" > "$SCRATCH/root/restricted/secret.txt"
chmod 000 "$SCRATCH/root/restricted"

ssh-keygen -t ed25519 -f "$SCRATCH/host_key" -N "" -q -C "zephyrftp-test-host"
ssh-keygen -t ed25519 -f "$SCRATCH/client_key" -N "" -q -C "zephyrftp-test-client"
cp "$SCRATCH/client_key.pub" "$SCRATCH/authorized_keys"
chmod 600 "$SCRATCH/host_key" "$SCRATCH/client_key" "$SCRATCH/authorized_keys"

cat > "$SCRATCH/sshd_config" <<EOF
Port $PORT
ListenAddress 127.0.0.1
HostKey $SCRATCH/host_key
PidFile $SCRATCH/sshd.pid
AuthorizedKeysFile $SCRATCH/authorized_keys
PasswordAuthentication no
PubkeyAuthentication yes
KbdInteractiveAuthentication no
UsePAM no
StrictModes no
AllowUsers $(whoami)
LogLevel VERBOSE
Subsystem sftp internal-sftp
# No ChrootDirectory: sshd requires every path component up to the
# chroot root to be root-owned and not group/other-writable — a real
# requirement this throwaway, non-root scratch directory under /tmp
# can't satisfy (confirmed against sshd_config(5) before assuming
# StrictModes would cover it — it doesn't; that only affects home-dir
# checks). Not chrooting means the client can technically cd anywhere
# on the real filesystem it has permission to read, which is a
# non-issue for a local, throwaway, this-user-only test server — -d
# just sets the starting directory instead.
ForceCommand internal-sftp -d $SCRATCH/root
EOF

/usr/sbin/sshd -t -f "$SCRATCH/sshd_config"
/usr/sbin/sshd -f "$SCRATCH/sshd_config"

sleep 0.3
if [ ! -f "$SCRATCH/sshd.pid" ]; then
    echo "sshd failed to start — no pid file at $SCRATCH/sshd.pid" >&2
    exit 1
fi

echo "SFTP test server listening on 127.0.0.1:$PORT (pid $(cat "$SCRATCH/sshd.pid"))"
echo "Private key: $SCRATCH/client_key"
echo "Username:    $(whoami)"
echo "Starting directory: $SCRATCH/root (sample.txt there; writable uploads/ subdir; not chrooted)"
