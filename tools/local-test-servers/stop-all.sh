#!/bin/bash
# Kills whichever of the ephemeral SFTP/FTP/FTPS test servers are
# currently running, via the pid files each start-*.sh script writes.
# Safe to run even if none are up — missing pid files are skipped, not
# an error.
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
