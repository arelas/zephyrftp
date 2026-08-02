#!/bin/bash
# Kills whichever of the ephemeral SFTP/FTP/FTPS test servers are
# currently running, via the pid files each native-process start-*.sh
# script writes, plus podman rm -f for the container-based ones
# (start-vsftpd.sh/start-proftpd.sh/start-dropbear.sh — see
# containers/README.md). Safe to run even if none are up — missing pid
# files and nonexistent containers are both skipped, not an error.
set -uo pipefail

for pidfile in \
    "${SFTP_TEST_SCRATCH:-/tmp/zephyrftp-local-test-servers/sftp}/sshd.pid" \
    "${FTP_TEST_SCRATCH:-/tmp/zephyrftp-local-test-servers/ftp}/server.pid" \
    "${FTPS_TEST_SCRATCH:-/tmp/zephyrftp-local-test-servers/ftps}/server.pid" \
    "${FTP_LEGACY_TEST_SCRATCH:-/tmp/zephyrftp-local-test-servers/ftp-legacy-list}/server.pid" \
    "${FTPS_TRUSTED_TEST_SCRATCH:-/tmp/zephyrftp-local-test-servers/ftps-trusted}/server.pid" \
    "${FTP_ACTIVE_TEST_SCRATCH:-/tmp/zephyrftp-local-test-servers/ftp-active-only}/server.pid"
do
    if [ -f "$pidfile" ]; then
        pid=$(cat "$pidfile")
        if kill "$pid" 2>/dev/null; then
            echo "Stopped $pidfile (pid $pid)"
        else
            echo "$pidfile (pid $pid) — already gone"
        fi
        rm -f "$pidfile"
    fi
done

if command -v podman >/dev/null 2>&1; then
    for container in zephyrftp-test-vsftpd zephyrftp-test-proftpd zephyrftp-test-dropbear; do
        if podman container exists "$container" 2>/dev/null; then
            podman rm -f "$container" >/dev/null && echo "Stopped container $container"
        fi
    done
fi
