#!/bin/bash
# Throwaway explicit-FTPS server on 127.0.0.1:2124 (pyftpdlib +
# TLS_FTPHandler), presenting a leaf certificate signed by a throwaway
# local CA — unlike start-ftps.sh's self-signed cert (which FtpBackend
# is SUPPOSED to reject), this is for proving the opposite case: a full
# encrypted transfer completes when the certificate genuinely validates.
# See ARCHITECTURE.md's Known gaps: "no full authenticated FTPS transfer
# has been verified — FtpBackend has no override to trust a self-signed
# cert, by design, so this needs a CA-trusted cert to test."
#
# Prints the CA certificate's path — the verify-ftp-live harness loads
# it and adds it to QSslConfiguration's process-wide default trust set
# (pure test-harness code; FtpBackend itself is never told about any
# extra CA) so the handshake validates for real, the same way it would
# against a genuine publicly-trusted certificate.
set -euo pipefail

SCRATCH="${FTPS_TRUSTED_TEST_SCRATCH:-/tmp/zephyrftp-local-test-servers/ftps-trusted}"
PORT="${FTPS_TRUSTED_TEST_PORT:-2124}"
PASSIVE_PORTS="${FTPS_TRUSTED_TEST_PASSIVE_PORTS:-2151-2160}"
USER="ftpsuser"
PASSWORD="ftpspass"

if [ -f "$SCRATCH/server.pid" ]; then
    kill "$(cat "$SCRATCH/server.pid")" 2>/dev/null || true
    sleep 0.2
fi

rm -rf "$SCRATCH"
mkdir -p "$SCRATCH/root/uploads"
echo "hello from the local ca-trusted ftps test server" > "$SCRATCH/root/sample.txt"

# --- throwaway CA ---
openssl genrsa -out "$SCRATCH/ca-key.pem" 2048 2>/dev/null
openssl req -x509 -new -key "$SCRATCH/ca-key.pem" -days 1 \
    -out "$SCRATCH/ca-cert.pem" -subj "/CN=ZephyrFTP Test CA" 2>/dev/null

# --- leaf cert for 127.0.0.1, signed by that CA ---
openssl genrsa -out "$SCRATCH/server-key.pem" 2048 2>/dev/null
openssl req -new -key "$SCRATCH/server-key.pem" \
    -out "$SCRATCH/server.csr" -subj "/CN=127.0.0.1" 2>/dev/null
openssl x509 -req -in "$SCRATCH/server.csr" \
    -CA "$SCRATCH/ca-cert.pem" -CAkey "$SCRATCH/ca-key.pem" -CAcreateserial \
    -out "$SCRATCH/server-cert.pem" -days 1 \
    -extfile <(printf "subjectAltName=IP:127.0.0.1") 2>/dev/null

# pyftpdlib's TLS_FTPHandler.certfile wants key + cert in one file, same
# as start-ftps.sh's self-signed combined file.
cat "$SCRATCH/server-key.pem" "$SCRATCH/server-cert.pem" > "$SCRATCH/combined-cert.pem"
chmod 600 "$SCRATCH/combined-cert.pem"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

nohup python3 "$SCRIPT_DIR/ftp_server.py" \
    --port "$PORT" \
    --root "$SCRATCH/root" \
    --user "$USER" \
    --password "$PASSWORD" \
    --pid-file "$SCRATCH/server.pid" \
    --passive-ports "$PASSIVE_PORTS" \
    --tls --certfile "$SCRATCH/combined-cert.pem" \
    > "$SCRATCH/server.log" 2>&1 &

disown
sleep 0.5
if [ ! -f "$SCRATCH/server.pid" ]; then
    echo "CA-trusted FTPS test server failed to start — see $SCRATCH/server.log" >&2
    cat "$SCRATCH/server.log" >&2
    exit 1
fi

echo "CA-trusted FTPS test server listening on 127.0.0.1:$PORT (pid $(cat "$SCRATCH/server.pid"))"
echo "Username: $USER   Password: $PASSWORD"
echo "Root dir: $SCRATCH/root (sample.txt there; writable uploads/ subdir)"
echo "CA cert: $SCRATCH/ca-cert.pem — trust this (not the server's own leaf cert) to validate the handshake."
