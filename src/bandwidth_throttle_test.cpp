// Verifies BandwidthThrottle's pacing math directly — no server, no
// SftpBackend/FtpBackend, just real wall-clock timing against a small,
// fast, deterministic-enough-within-generous-margins set of checks
// (same margin-based timing-assertion style this project's other
// real-timing tests already use, e.g. transfer_pause_test.cpp).
//
// What this does NOT cover: that a real SftpBackend transfer, once
// throttled, actually achieves close to the configured real-world
// MB/s over a real network — that needs a real server and is covered
// separately by verify-bandwidth-throttle-live (EXCLUDE_FROM_ALL,
// external precondition, not part of this required suite). This test
// is scoped to BandwidthThrottle's own pacing logic in isolation.
//
// Run with:
//   QT_QPA_PLATFORM=offscreen ./build/bandwidth-throttle-test
#include <QCoreApplication>
#include <QDebug>
#include <QElapsedTimer>
#include "backends/BandwidthThrottle.h"

namespace {
bool allPass = true;
void check(const char *label, bool condition)
{
    qDebug() << (condition ? "[PASS]" : "[FAIL]") << label;
    if (!condition) allPass = false;
}
}

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    // ---- Unlimited (limitKBps <= 0): pace() must be a true no-op, no
    // QElapsedTimer/sleep overhead at all — checked by calling it many
    // times with large, fast-growing byte counts and confirming the
    // whole batch stays near-instant. ----
    {
        BandwidthThrottle throttle(0);
        QElapsedTimer timer;
        timer.start();
        for (int i = 1; i <= 2000; ++i)
            throttle.pace(static_cast<qint64>(i) * 10 * 1024 * 1024, [] { return false; });
        const qint64 elapsedMs = timer.elapsed();
        qDebug() << "[test] unlimited: 2000 pace() calls with huge byte counts took" << elapsedMs << "ms";
        check("limitKBps <= 0 is a true no-op (2000 calls complete in well under 100ms)",
              elapsedMs < 100);
    }

    // ---- Limited: a real, measurable sleep. 100 KB/s limit, two calls
    // 50KB apart (simulating a producer that transfers instantly, far
    // faster than the configured rate) — each call should block for
    // roughly 500ms (50KB / 100KB/s), landing the pair around ~1000ms
    // total. Generous +-300ms tolerance for real scheduling jitter,
    // consistent with this project's other real-timing assertions
    // (e.g. transfer_pause_test.cpp's own margins). ----
    {
        BandwidthThrottle throttle(100);   // 100 KB/s
        QElapsedTimer timer;
        timer.start();
        throttle.pace(50 * 1024, [] { return false; });
        const qint64 afterFirstMs = timer.elapsed();
        throttle.pace(100 * 1024, [] { return false; });
        const qint64 afterSecondMs = timer.elapsed();
        qDebug() << "[test] 100 KB/s limit: after 50KB =" << afterFirstMs
                 << "ms, after 100KB =" << afterSecondMs << "ms (expected ~500ms / ~1000ms)";
        check("first 50KB chunk paced to roughly 500ms (300-900ms)",
              afterFirstMs >= 300 && afterFirstMs <= 900);
        check("second 50KB chunk paced to roughly another 500ms, ~1000ms total (700-1500ms)",
              afterSecondMs >= 700 && afterSecondMs <= 1500);
    }

    // ---- shouldStop cuts a long sleep short. A very low limit (1 KB/s)
    // asked to pace 50KB would "correctly" want to sleep ~50 real
    // seconds — but shouldStop() returning true immediately must cut
    // that off almost immediately instead of ever completing it. ----
    {
        BandwidthThrottle throttle(1);   // 1 KB/s — a 50KB ask would naively need ~50s
        QElapsedTimer timer;
        timer.start();
        throttle.pace(50 * 1024, [] { return true; });   // stop signaled from the very first check
        const qint64 elapsedMs = timer.elapsed();
        qDebug() << "[test] shouldStop==true immediately: pace() for a ~50s-implied sleep "
                    "returned after" << elapsedMs << "ms";
        check("shouldStop cuts the sleep short (returns in well under 1s, not ~50s)",
              elapsedMs < 1000);
    }

    // ---- shouldStop only becomes true partway through — confirms the
    // sleep is genuinely composed of short increments with real
    // re-checks between them, not one uninterruptible block. Stops
    // after ~2 increments' worth of real elapsed time. ----
    {
        BandwidthThrottle throttle(1);   // same ~50s-implied full sleep if never stopped
        QElapsedTimer timer;
        timer.start();
        throttle.pace(50 * 1024, [&timer] { return timer.elapsed() > 350; });
        const qint64 elapsedMs = timer.elapsed();
        qDebug() << "[test] shouldStop becomes true after ~350ms: pace() returned after"
                 << elapsedMs << "ms";
        check("a stop signaled partway through is honored promptly (350-800ms), "
              "not after the full implied ~50s sleep",
              elapsedMs >= 350 && elapsedMs < 800);
    }

    qDebug() << (allPass ? "[test] ALL PASS" : "[test] AT LEAST ONE FAILURE");
    return allPass ? 0 : 1;
}
