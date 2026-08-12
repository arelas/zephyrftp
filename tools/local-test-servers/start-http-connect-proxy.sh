#!/bin/bash
# Real tinyproxy in a throwaway podman container on 127.0.0.1:8888 — a
# genuine, independent HTTP CONNECT proxy implementation, for exercising
# ProxyConnect.cpp's manual HTTP CONNECT handshake (see that file's own
# header comment for why it's hand-rolled rather than
# QAbstractSocket::setProxy()) against real proxy software, not just
# this project's own test code. See containers/Containerfile.tinyproxy
# and containers/tinyproxy.conf for exactly how it's built/configured —
# BasicAuth is enabled there so the Proxy-Authorization header path gets
# real coverage too, not just the no-auth case
# start-socks5-proxy.sh's proxy already covers.
#
# Self-contained image, same as start-vsftpd.sh/start-proftpd.sh — no
# host-side scratch directory, "starting fresh" is just recreating the
# container.
set -euo pipefail

PORT="${TINYPROXY_TEST_PORT:-8888}"
CONTAINER_NAME="zephyrftp-test-tinyproxy"
IMAGE_NAME="zephyrftp-test-tinyproxy-image"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

podman rm -f "$CONTAINER_NAME" >/dev/null 2>&1 || true
podman build -t "$IMAGE_NAME" -f "$SCRIPT_DIR/containers/Containerfile.tinyproxy" "$SCRIPT_DIR/containers"

# --network host, not -p port mapping: this proxy needs to reach OTHER
# throwaway test servers that are themselves bound to 127.0.0.1 only
# (start-sftp-pubkey.sh/start-ftp.sh/start-ftps-trusted.sh) — a real gap
# found by actually running this, not assumed: slirp4netns's default
# bridged networking gives the container its OWN loopback, so
# "CONNECT 127.0.0.1:2222" from inside an ordinarily-networked container
# never reaches the host's sshd at all ("500 Unable to connect", not a
# proxy bug). Host networking makes the container share the host's
# network namespace directly, the same model start-socks5-proxy.sh's
# native (non-containerized) ssh -D process already has for the exact
# same reason. PORT is then just which host port tinyproxy.conf's own
# `Port` directive binds, not a mapping — the two need to agree (both
# default to 8888).
podman run -d --name "$CONTAINER_NAME" --network host \
    "$IMAGE_NAME" >/dev/null

sleep 0.5
if [ "$(podman inspect "$CONTAINER_NAME" --format '{{.State.Status}}' 2>/dev/null)" != "running" ]; then
    echo "tinyproxy test container failed to start — see below" >&2
    podman logs "$CONTAINER_NAME" >&2 || true
    exit 1
fi

echo "HTTP CONNECT proxy (tinyproxy) listening on 127.0.0.1:$PORT (container: $CONTAINER_NAME)"
echo "Username: proxyuser   Password: proxypass   (BasicAuth, required — see tinyproxy.conf)"
