#!/bin/bash
# Throwaway explicit-FTPS server on 127.0.0.1:2122 (pyftpdlib +
# TLS_FTPHandler, self-signed cert), for exercising FtpBackend's AUTH
# TLS upgrade against a real server — never tried against a live server
# before this existed (see ARCHITECTURE.md's "Known gaps").
#
# Self-signed on purpose: FtpBackend validates the server certificate
# against the system trust store and fails closed on anything it can't
# verify (see ARCHITECTURE.md's FtpBackend entry) — a self-signed cert
# is exactly the case that's supposed to fail that way, and is also the
# realistic case for testing against a server you stood up yourself
# rather than a CA-issued one.
set -euo pipefail

SCRATCH="${FTPS_TEST_SCRATCH:-/tmp/zephyrftp-local-test-servers/ftps}"
PORT="${FTPS_TEST_PORT:-2122}"
PASSIVE_PORTS="${FTPS_TEST_PASSIVE_PORTS:-2150-2160}"
USER="ftpsuser"
PASSWORD="ftpspass"

# See start-sftp-pubkey.sh's matching comment: stop a prior instance
# THIS script started before its pid file gets wiped below, or a re-run
# orphans it holding the port.
if [ -f "$SCRATCH/server.pid" ]; then
    kill "$(cat "$SCRATCH/server.pid")" 2>/dev/null || true
    sleep 0.2
fi

rm -rf "$SCRATCH"
mkdir -p "$SCRATCH/root/uploads"
echo "hello from the local ftps test server" > "$SCRATCH/root/sample.txt"

openssl req -x509 -newkey rsa:2048 -nodes \
    -keyout "$SCRATCH/cert.pem" -out "$SCRATCH/cert.pem" \
    -days 1 -subj "/CN=127.0.0.1" 2>/dev/null
chmod 600 "$SCRATCH/cert.pem"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

nohup python3 "$SCRIPT_DIR/ftp_server.py" \
    --port "$PORT" \
    --root "$SCRATCH/root" \
    --user "$USER" \
    --password "$PASSWORD" \
    --pid-file "$SCRATCH/server.pid" \
    --passive-ports "$PASSIVE_PORTS" \
    --tls --certfile "$SCRATCH/cert.pem" \
    > "$SCRATCH/server.log" 2>&1 &

disown
sleep 0.5
if [ ! -f "$SCRATCH/server.pid" ]; then
    echo "FTPS test server failed to start — see $SCRATCH/server.log" >&2
    cat "$SCRATCH/server.log" >&2
    exit 1
fi

echo "FTPS test server listening on 127.0.0.1:$PORT (pid $(cat "$SCRATCH/server.pid"))"
echo "Username: $USER   Password: $PASSWORD"
echo "Root dir: $SCRATCH/root (sample.txt there; writable uploads/ subdir)"
echo "Self-signed cert — expect FtpBackend to reject it as untrusted (see script header); that's the correct, verified behavior."
