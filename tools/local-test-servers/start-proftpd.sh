#!/bin/bash
# Real proftpd in a throwaway podman container on 127.0.0.1:2127 — a
# second, independently-coded FTP server alongside start-vsftpd.sh,
# giving FtpBackend's legacy-LIST fallback a second genuine
# implementation to be exercised against (with its own LIST-format
# quirks), and deliberately configured to also deny MLSD (this build's
# mod_facts is compiled in, unlike vsftpd) — see
# containers/Containerfile.proftpd and containers/proftpd.conf for the
# real containerization gotchas those files document, including the
# discovery that a denied MLSD here replies 550, not 500/502, which
# FtpBackend.cpp's fallback trigger now also handles.
set -euo pipefail

PORT="${PROFTPD_TEST_PORT:-2127}"
PASSIVE_PORTS="${PROFTPD_TEST_PASSIVE_PORTS:-2201-2210}"
CONTAINER_NAME="zephyrftp-test-proftpd"
IMAGE_NAME="zephyrftp-test-proftpd-image"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

podman rm -f "$CONTAINER_NAME" >/dev/null 2>&1 || true
podman build -t "$IMAGE_NAME" -f "$SCRIPT_DIR/containers/Containerfile.proftpd" "$SCRIPT_DIR/containers"

podman run -d --name "$CONTAINER_NAME" \
    -p "$PORT:21" -p "$PASSIVE_PORTS:$PASSIVE_PORTS" \
    "$IMAGE_NAME" >/dev/null

sleep 0.5
if [ "$(podman inspect "$CONTAINER_NAME" --format '{{.State.Status}}' 2>/dev/null)" != "running" ]; then
    echo "proftpd test container failed to start — see below" >&2
    podman logs "$CONTAINER_NAME" >&2 || true
    exit 1
fi

echo "proftpd test server listening on 127.0.0.1:$PORT (container: $CONTAINER_NAME)"
echo "Username: proftpduser   Password: proftppass"
echo "Root dir: /srv/ftp inside the container (sample.txt there; writable uploads/ subdir)"
echo "MLSD explicitly denied (550) — a second, independent LIST-fallback example."
