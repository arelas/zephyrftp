#pragma once

#include <QtGlobal>
#include <QElapsedTimer>
#include <functional>

// Per-transfer pacing — NOT a shared/global limiter. One instance is
// constructed fresh for each downloadFile()/uploadFile() call (see
// SftpBackend.cpp/FtpBackend.cpp), so a configured KB/s limit is applied
// independently to each transfer; two transfers running concurrently
// (see TransferManager's per-backend-instance concurrency) can together
// use up to roughly 2x the configured number, since neither knows about
// the other. Deliberate, simpler-of-two-options scope, not an oversight
// — see ARCHITECTURE.md's SftpBackend/FtpBackend entries for the
// per-transfer-vs-global tradeoff this was chosen over.
class BandwidthThrottle {
public:
    // limitKBps <= 0 means unlimited — pace() becomes a true no-op (no
    // QElapsedTimer overhead, no sleeping), the common case when the
    // user hasn't set a limit in Preferences.
    explicit BandwidthThrottle(int limitKBps);

    // Called after each chunk is transferred, with the CUMULATIVE bytes
    // transferred so far in THIS call (not counting a resumed offset —
    // the pacing clock restarts fresh on every dispatch, including a
    // resume after pause, which is harmless). Sleeps in short (~150ms)
    // increments, re-checking shouldStop() between each, until the
    // average rate since construction is back down to the configured
    // limit — never sleeps in one long block, so a cancel/pause request
    // is never delayed by more than one increment even at a very low
    // configured limit. No-op immediately if unlimited or if
    // shouldStop() is already true.
    void pace(qint64 bytesSoFar, const std::function<bool()> &shouldStop);

private:
    qint64 m_limitBytesPerSec = 0;   // 0 == unlimited
    QElapsedTimer m_timer;
};
