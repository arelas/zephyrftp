#!/bin/bash
# Real vsftpd running IMPLICIT FTPS (TLS from the very first byte of the
# control connection — no AUTH TLS command, no plaintext phase at all)
# in a throwaway podman container on 127.0.0.1:2128 — closes the gap
# that every other FTPS test server in this project (pyftpdlib, the
# other vsftpd container, proftpd) only ever exercises EXPLICIT FTPS.
# See containers/Containerfile.vsftpd-implicit and
# containers/vsftpd-implicit.conf for exactly how it's built and
# configured, including why this needed a genuinely separate
# container/config rather than a flag on the existing vsftpd one
# (vsftpd's own man page: a single running instance can't offer both
# explicit and implicit on the same listener).
#
# Same self-contained-image approach as start-vsftpd.sh: test content
# baked in at build time, "starting fresh" is just recreating the
# container. Same one exception: the FTPS CA certificate is copied out
# to a small host-side path below — public information, not a departure
# from "self-contained" the way a bind-mounted volume would be.
set -euo pipefail

PORT="${VSFTPD_IMPLICIT_TEST_PORT:-2128}"
PASSIVE_PORTS="${VSFTPD_IMPLICIT_TEST_PASSIVE_PORTS:-2201-2210}"
CA_SCRATCH="${VSFTPD_IMPLICIT_TEST_CA_SCRATCH:-/tmp/zephyrftp-local-test-servers/vsftpd-implicit-ftps-ca}"
CONTAINER_NAME="zephyrftp-test-vsftpd-implicit"
IMAGE_NAME="zephyrftp-test-vsftpd-implicit-image"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

podman rm -f "$CONTAINER_NAME" >/dev/null 2>&1 || true
podman build -t "$IMAGE_NAME" -f "$SCRIPT_DIR/containers/Containerfile.vsftpd-implicit" "$SCRIPT_DIR/containers"

podman run -d --name "$CONTAINER_NAME" \
    -p "$PORT:990" -p "$PASSIVE_PORTS:$PASSIVE_PORTS" \
    "$IMAGE_NAME" >/dev/null

sleep 0.5
if [ "$(podman inspect "$CONTAINER_NAME" --format '{{.State.Status}}' 2>/dev/null)" != "running" ]; then
    echo "implicit-FTPS vsftpd test container failed to start — see below" >&2
    podman logs "$CONTAINER_NAME" >&2 || true
    exit 1
fi

rm -rf "$CA_SCRATCH"
mkdir -p "$CA_SCRATCH"
podman cp "$CONTAINER_NAME:/etc/vsftpd/ftps-ca-cert.pem" "$CA_SCRATCH/ca-cert.pem"

echo "Implicit-FTPS vsftpd test server listening on 127.0.0.1:$PORT (container: $CONTAINER_NAME)"
echo "Username: vsftpuser   Password: vsftppass"
echo "Root dir: /srv/ftp inside the container (sample.txt there; writable uploads/ subdir)"
echo "TLS from the very first byte — no AUTH TLS command, no plaintext phase at all."
echo "CA cert: $CA_SCRATCH/ca-cert.pem — trust this to validate the TLS handshake."
