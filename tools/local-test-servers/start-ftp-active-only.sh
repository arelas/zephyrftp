#!/bin/bash
# Throwaway plain-FTP server on 127.0.0.1:2125 (pyftpdlib) with PASV/EPSV
# disabled — genuinely, at the protocol level — so it forces FtpBackend's
# active/PORT fallback to trigger against a real server that actually
# refuses passive mode, closing the gap flagged in ARCHITECTURE.md ("no
# active/PORT fallback ... a server that requires active mode won't work
# at all"). See ftp_server.py's --no-pasv flag.
set -euo pipefail

SCRATCH="${FTP_ACTIVE_TEST_SCRATCH:-/tmp/zephyrftp-local-test-servers/ftp-active-only}"
PORT="${FTP_ACTIVE_TEST_PORT:-2125}"
# Unused when PASV is disabled, but ftp_server.py always requires the
# flag — pass a range that doesn't collide with any other test server.
PASSIVE_PORTS="${FTP_ACTIVE_TEST_PASSIVE_PORTS:-2161-2170}"
USER="ftpuser"
PASSWORD="ftppass"

if [ -f "$SCRATCH/server.pid" ]; then
    kill "$(cat "$SCRATCH/server.pid")" 2>/dev/null || true
    sleep 0.2
fi

rm -rf "$SCRATCH"
mkdir -p "$SCRATCH/root/uploads"
echo "hello from the local active-only ftp test server" > "$SCRATCH/root/sample.txt"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

nohup python3 "$SCRIPT_DIR/ftp_server.py" \
    --port "$PORT" \
    --root "$SCRATCH/root" \
    --user "$USER" \
    --password "$PASSWORD" \
    --pid-file "$SCRATCH/server.pid" \
    --passive-ports "$PASSIVE_PORTS" \
    --no-pasv \
    > "$SCRATCH/server.log" 2>&1 &

disown
sleep 0.5
if [ ! -f "$SCRATCH/server.pid" ]; then
    echo "Active-only FTP test server failed to start — see $SCRATCH/server.log" >&2
    cat "$SCRATCH/server.log" >&2
    exit 1
fi

echo "Active-only FTP test server listening on 127.0.0.1:$PORT (pid $(cat "$SCRATCH/server.pid"))"
echo "Username: $USER   Password: $PASSWORD"
echo "Root dir: $SCRATCH/root (sample.txt there; writable uploads/ subdir)"
echo "PASV/EPSV are disabled server-side — expect FtpBackend to fall back to active/PORT mode."
