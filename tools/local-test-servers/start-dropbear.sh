#!/bin/bash
# Real Dropbear (a genuinely different SSH daemon from OpenSSH) in a
# throwaway podman container on 127.0.0.1:2223 — for SSH transport/
# session/auth-layer diversity alongside start-sftp-pubkey.sh's OpenSSH
# server. Password auth here (vs. that script's pubkey-only setup) adds
# auth-method diversity too. Real, checked-for-real limitation: Dropbear
# has no sftp-server of its own and shells out to OpenSSH's — so this is
# NOT proof of a genuinely different SFTP wire-protocol implementation,
# just a different SSH daemon fronting the likely-same one. See
# containers/Containerfile.dropbear and ARCHITECTURE.md's Known Gaps
# entry for the honest accounting.
set -euo pipefail

PORT="${DROPBEAR_TEST_PORT:-2223}"
CONTAINER_NAME="zephyrftp-test-dropbear"
IMAGE_NAME="zephyrftp-test-dropbear-image"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

podman rm -f "$CONTAINER_NAME" >/dev/null 2>&1 || true
podman build -t "$IMAGE_NAME" -f "$SCRIPT_DIR/containers/Containerfile.dropbear" "$SCRIPT_DIR/containers"

podman run -d --name "$CONTAINER_NAME" -p "$PORT:22" "$IMAGE_NAME" >/dev/null

sleep 0.5
if [ "$(podman inspect "$CONTAINER_NAME" --format '{{.State.Status}}' 2>/dev/null)" != "running" ]; then
    echo "Dropbear test container failed to start — see below" >&2
    podman logs "$CONTAINER_NAME" >&2 || true
    exit 1
fi

echo "Dropbear SFTP test server listening on 127.0.0.1:$PORT (container: $CONTAINER_NAME)"
echo "Username: dropbearuser   Password: dropbearpass"
echo "Home dir: /home/dropbearuser inside the container (sample.txt there; writable uploads/ subdir)"
echo "A different SSH daemon than start-sftp-pubkey.sh's OpenSSH — but almost certainly the same"
echo "underlying sftp-server binary (OpenSSH's own); see ARCHITECTURE.md for the honest accounting."
