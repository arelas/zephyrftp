#!/bin/bash
# Spins up a real, local SOCKS5 proxy via `ssh -D` (OpenSSH's own dynamic
# port forwarding), tunneled through the already-running local sshd from
# start-sftp-pubkey.sh — a genuine, independent, well-tested SOCKS5
# implementation (OpenSSH's own), not a hand-rolled stand-in, and needs
# no new package: openssh-clients (the `ssh` binary) is already a
# dependency of start-sftp-pubkey.sh itself (which uses ssh-keygen from
# the same package).
#
# Requires start-sftp-pubkey.sh to already be running — this proxies
# THROUGH that SSH connection rather than standing up its own server,
# reusing the exact same client key/user/port it already set up.
set -euo pipefail

SFTP_SCRATCH="${SFTP_TEST_SCRATCH:-/tmp/zephyrftp-local-test-servers/sftp}"
SFTP_PORT="${SFTP_TEST_PORT:-2222}"
SCRATCH="${SOCKS5_PROXY_SCRATCH:-/tmp/zephyrftp-local-test-servers/socks5-proxy}"
PROXY_PORT="${SOCKS5_PROXY_PORT:-1080}"

if [ ! -f "$SFTP_SCRATCH/sshd.pid" ]; then
    echo "start-sftp-pubkey.sh isn't running (no $SFTP_SCRATCH/sshd.pid) — start that first; this proxy tunnels through that same sshd, it doesn't stand up its own server." >&2
    exit 1
fi

# See start-sftp-pubkey.sh's matching comment: stop a prior instance
# THIS script started before its pid file gets wiped below.
if [ -f "$SCRATCH/ssh.pid" ]; then
    kill "$(cat "$SCRATCH/ssh.pid")" 2>/dev/null || true
    sleep 0.2
fi
rm -rf "$SCRATCH"
mkdir -p "$SCRATCH"

# -N: no remote command, just port forwarding. -D: dynamic (SOCKS5)
# forward. StrictHostKeyChecking/UserKnownHostsFile disabled — this is
# the exact same throwaway, freshly-generated host key
# start-sftp-pubkey.sh already wrote and prints, not worth a known_hosts
# entry for a per-run ephemeral test server. ExitOnForwardFailure makes
# a bind failure (e.g. PROXY_PORT already in use) fail loudly at
# connect time rather than silently leaving a useless SSH session up.
nohup ssh -D "127.0.0.1:$PROXY_PORT" -N \
    -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
    -o ExitOnForwardFailure=yes \
    -i "$SFTP_SCRATCH/client_key" \
    -p "$SFTP_PORT" "$(whoami)@127.0.0.1" \
    > "$SCRATCH/ssh.log" 2>&1 &
echo $! > "$SCRATCH/ssh.pid"

sleep 0.5
if ! kill -0 "$(cat "$SCRATCH/ssh.pid")" 2>/dev/null; then
    echo "ssh -D failed to start — see $SCRATCH/ssh.log" >&2
    cat "$SCRATCH/ssh.log" >&2
    exit 1
fi

echo "SOCKS5 proxy listening on 127.0.0.1:$PROXY_PORT (pid $(cat "$SCRATCH/ssh.pid"), tunneled through the sshd on port $SFTP_PORT)"
echo "Log: $SCRATCH/ssh.log"
