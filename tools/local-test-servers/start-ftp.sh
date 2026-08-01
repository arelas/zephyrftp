#!/bin/bash
# Throwaway plain-FTP server on 127.0.0.1:2121 (pyftpdlib), for exercising
# FtpBackend's control connection / PASV / transfer logic against a real
# server. See ftp_server.py for the shared implementation.
set -euo pipefail

SCRATCH="${FTP_TEST_SCRATCH:-/tmp/zephyrftp-local-test-servers/ftp}"
PORT="${FTP_TEST_PORT:-2121}"
PASSIVE_PORTS="${FTP_TEST_PASSIVE_PORTS:-2130-2140}"
USER="ftpuser"
PASSWORD="ftppass"

rm -rf "$SCRATCH"
mkdir -p "$SCRATCH/root/uploads"
echo "hello from the local ftp test server" > "$SCRATCH/root/sample.txt"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

nohup python3 "$SCRIPT_DIR/ftp_server.py" \
    --port "$PORT" \
    --root "$SCRATCH/root" \
    --user "$USER" \
    --password "$PASSWORD" \
    --pid-file "$SCRATCH/server.pid" \
    --passive-ports "$PASSIVE_PORTS" \
    > "$SCRATCH/server.log" 2>&1 &

disown
sleep 0.5
if [ ! -f "$SCRATCH/server.pid" ]; then
    echo "FTP test server failed to start — see $SCRATCH/server.log" >&2
    cat "$SCRATCH/server.log" >&2
    exit 1
fi

echo "FTP test server listening on 127.0.0.1:$PORT (pid $(cat "$SCRATCH/server.pid"))"
echo "Username: $USER   Password: $PASSWORD"
echo "Root dir: $SCRATCH/root (sample.txt there; writable uploads/ subdir)"
