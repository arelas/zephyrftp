#!/bin/bash
# Real vsftpd in a throwaway podman container on 127.0.0.1:2126 — closes
# the ARCHITECTURE.md gap that FtpBackend's legacy-LIST fallback had only
# ever been exercised against pyftpdlib with MLSD manually disabled via a
# flag, never a genuinely different, independently-implemented server.
# vsftpd never implemented MLSD/RFC 3659 at all, in any version, so this
# is a real server hitting the real fallback trigger. See
# containers/Containerfile.vsftpd and containers/vsftpd.conf for exactly
# how it's built and configured, including the real containerization
# gotchas (seccomp, foreground mode, PAM virtual users) those files
# document.
#
# Self-contained image (test content baked in at build time) — unlike
# the native-process scripts above, there's no host-side scratch
# directory to wipe; "starting fresh" is just recreating the container.
# The one exception: the FTPS CA certificate (see vsftpd.conf/
# Containerfile.vsftpd) is copied out to a small host-side path below —
# public information, same status this project already gives SSH
# host-key fingerprints and FTPS leaf-cert fingerprints, not a departure
# from "self-contained" in the way a bind-mounted volume would be.
set -euo pipefail

PORT="${VSFTPD_TEST_PORT:-2126}"
PASSIVE_PORTS="${VSFTPD_TEST_PASSIVE_PORTS:-2191-2200}"
CA_SCRATCH="${VSFTPD_TEST_CA_SCRATCH:-/tmp/zephyrftp-local-test-servers/vsftpd-ftps-ca}"
CONTAINER_NAME="zephyrftp-test-vsftpd"
IMAGE_NAME="zephyrftp-test-vsftpd-image"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

podman rm -f "$CONTAINER_NAME" >/dev/null 2>&1 || true
podman build -t "$IMAGE_NAME" -f "$SCRIPT_DIR/containers/Containerfile.vsftpd" "$SCRIPT_DIR/containers"

podman run -d --name "$CONTAINER_NAME" \
    -p "$PORT:21" -p "$PASSIVE_PORTS:$PASSIVE_PORTS" \
    "$IMAGE_NAME" >/dev/null

sleep 0.5
if [ "$(podman inspect "$CONTAINER_NAME" --format '{{.State.Status}}' 2>/dev/null)" != "running" ]; then
    echo "vsftpd test container failed to start — see below" >&2
    podman logs "$CONTAINER_NAME" >&2 || true
    exit 1
fi

rm -rf "$CA_SCRATCH"
mkdir -p "$CA_SCRATCH"
podman cp "$CONTAINER_NAME:/etc/vsftpd/ftps-ca-cert.pem" "$CA_SCRATCH/ca-cert.pem"

echo "vsftpd test server listening on 127.0.0.1:$PORT (container: $CONTAINER_NAME)"
echo "Username: vsftpuser   Password: vsftppass"
echo "Root dir: /srv/ftp inside the container (sample.txt there; writable uploads/ subdir)"
echo "Real vsftpd — does NOT support MLSD at all; exercises FtpBackend's genuine LIST fallback."
echo "FTPS also available (AUTH TLS). require_ssl_reuse left off — see vsftpd.conf's"
echo "own comment for the real, environment-specific stall that setting causes here."
echo "CA cert: $CA_SCRATCH/ca-cert.pem — trust this to validate the FTPS handshake."
