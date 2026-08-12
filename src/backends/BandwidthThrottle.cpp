#include "BandwidthThrottle.h"
#include <QThread>

namespace {
// Longest single sleep before re-checking shouldStop() — short enough
// that a cancel/pause at a low configured limit (where the "correct"
// sleep to hit the target rate could otherwise be many seconds long)
// is still honored promptly, long enough that this doesn't turn into a
// busy-loop of tiny sleeps at a high configured limit either.
constexpr qint64 kMaxSleepIncrementMs = 150;
}

BandwidthThrottle::BandwidthThrottle(int limitKBps)
    : m_limitBytesPerSec(limitKBps > 0 ? static_cast<qint64>(limitKBps) * 1024 : 0)
{
    if (m_limitBytesPerSec > 0)
        m_timer.start();
}

void BandwidthThrottle::pace(qint64 bytesSoFar, const std::function<bool()> &shouldStop)
{
    if (m_limitBytesPerSec <= 0)
        return;   // unlimited — the common case, kept a true no-op

    // How long bytesSoFar SHOULD have taken at the configured rate, vs.
    // how long it actually took — sleep off the difference. Computed
    // against the cumulative total (not per-chunk deltas) so small
    // rounding/scheduling jitter in any one chunk doesn't accumulate
    // drift across a long transfer.
    for (;;) {
        const qint64 expectedMs = (bytesSoFar * 1000) / m_limitBytesPerSec;
        const qint64 actualMs = m_timer.elapsed();
        const qint64 remainingMs = expectedMs - actualMs;
        if (remainingMs <= 0)
            return;   // already at or behind the target pace — nothing to wait for

        if (shouldStop && shouldStop())
            return;

        QThread::msleep(static_cast<unsigned long>(qMin(remainingMs, kMaxSleepIncrementMs)));
    }
}
