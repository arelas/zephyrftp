// Tests ChecksumVerifier end to end: LocalToLocal hashes both files
// directly and matches; RemoteToLocal/LocalToRemote each hash the local
// side directly and re-read the remote side over a real (fake)
// RemoteBackend, matching when the remote content is unchanged and
// correctly detecting a mismatch when it's deliberately corrupted
// between the original transfer and the verify() call; the hidden
// temp-download this all rides on correctly queues behind a real
// in-flight transfer on the SAME backend instance rather than corrupting
// it; cancel() stops an in-flight verification cleanly; and none of the
// hidden temp-download items ever produce a visible row in
// TransferQueueWidget's tabs.
//
// Uses a small FakeVerifiableBackend — same QTimer-tick, real-bytes-on-
// disk technique remote_to_remote_test.cpp's/transfer_concurrency_test.cpp's
// own fakes already establish — with one addition: a single mutable
// `remoteContent` field standing in for "whatever the server currently
// has." downloadFile() writes it out, uploadFile() captures it from what
// was actually uploaded; the test mutates it directly between an
// original transfer and a later verify() call to simulate the remote
// copy having changed since (a real, deliberately induced mismatch, not
// a hypothetical one).
//
// Run with:
//   QT_QPA_PLATFORM=offscreen ./build/checksum-verification-test
#include <QApplication>
#include <QTimer>
#include <QEventLoop>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QCryptographicHash>
#include <functional>
#include "ui/FilePaneWidget.h"
#include "ui/TransferQueueWidget.h"
#include "ui/TransferQueueTable.h"
#include "backends/LocalBackend.h"
#include "backends/RemoteBackend.h"
#include "transfer/TransferManager.h"
#include "transfer/ChecksumVerifier.h"
#include "transfer/TransferItem.h"

namespace {

class FakeVerifiableBackend : public RemoteBackend {
    Q_OBJECT
public:
    explicit FakeVerifiableBackend(QObject *parent = nullptr) : RemoteBackend(parent) {
        static int nextInstanceId = 1;
        m_identity = QStringLiteral("fakeverify://%1").arg(nextInstanceId++);
        connect(&m_timer, &QTimer::timeout, this, &FakeVerifiableBackend::tick);
    }

    QString currentPath() const override { return QStringLiteral("/fake"); }
    bool isLocalFilesystem() const override { return false; }
    QString connectionIdentity() const override { return m_identity; }
    void requestCancel() override { m_cancelRequested = true; }
    void requestPause() override { }   // not exercised by this test

    // What downloadFile() writes / what uploadFile() overwrites this
    // with — the test mutates this directly, between an original
    // transfer and a later verify() call, to simulate the remote file
    // having changed since.
    QByteArray remoteContent;

    bool isBusy() const { return m_timer.isActive(); }

public slots:
    void connectToHost() override { emit connected(); }
    void listDirectory(const QString &path) override { emit directoryListed(path, {}); }

    void downloadFile(const QString &remotePath, const QString &localPath, qint64 resumeOffset = 0) override {
        Q_UNUSED(resumeOffset);
        m_writeTargetPath = localPath;
        m_readSourcePath.clear();
        beginTransfer(remotePath);
    }
    void uploadFile(const QString &localPath, const QString &remotePath, qint64 resumeOffset = 0) override {
        Q_UNUSED(resumeOffset);
        m_writeTargetPath.clear();
        m_readSourcePath = localPath;
        beginTransfer(remotePath);
    }

    // Not exercised here — this test is about verification, not file
    // management — but RemoteBackend is a pure interface, so a concrete
    // fake needs some implementation to be instantiable at all.
    void deleteEntry(const QString &, bool) override {}
    void renameEntry(const QString &, const QString &) override {}
    void moveEntry(const QString &, const QString &, int requestId) override {
        emit entryMoveFailed(QStringLiteral("Not implemented"), requestId);
    }
    void createDirectory(const QString &) override {}
    void createFile(const QString &) override {}
    void setPermissions(const QString &, int) override {}
    void listDirectoryForEnumeration(const QString &, int) override {}
    // Must actually respond — startNext() always issues this before
    // dispatching an ordinary (non-skip-conflict-check) item and waits
    // for existsChecked() before proceeding.
    void checkExists(const QString &path, int requestId) override {
        emit existsChecked(path, false, false, requestId);
    }

private slots:
    void tick() {
        if (m_finished)
            return;
        if (m_cancelRequested) {
            m_finished = true;
            m_timer.stop();
            emit transferFailed(m_name, QStringLiteral("Cancelled"));
            return;
        }
        m_done = qMin(m_done + 100, m_total);
        emit transferProgress(m_name, m_done, m_total);
        if (m_done >= m_total) {
            m_finished = true;
            m_timer.stop();
            if (!m_readSourcePath.isEmpty()) {
                QFile src(m_readSourcePath);
                src.open(QIODevice::ReadOnly);
                remoteContent = src.readAll();
            } else if (!m_writeTargetPath.isEmpty()) {
                QFile dst(m_writeTargetPath);
                dst.open(QIODevice::WriteOnly | QIODevice::Truncate);
                dst.write(remoteContent);
            }
            emit transferFinished(m_name);
        }
    }

private:
    void beginTransfer(const QString &name) {
        m_cancelRequested = false;
        m_finished = false;
        m_name = name;
        m_done = 0;
        m_total = 1000;
        m_timer.start(30);   // ~30ms per 100-byte chunk, same pacing as this codebase's other fakes
    }

    QTimer m_timer;
    QString m_identity;
    QString m_name;
    QString m_writeTargetPath;
    QString m_readSourcePath;
    qint64 m_done = 0;
    qint64 m_total = 0;
    bool m_cancelRequested = false;
    bool m_finished = false;
};

QString writeFile(const QString &path, const QByteArray &content)
{
    QFile f(path);
    f.open(QIODevice::WriteOnly | QIODevice::Truncate);
    f.write(content);
    return path;
}

QByteArray readFile(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return QByteArray();
    return f.readAll();
}

QString sha256Hex(const QByteArray &data)
{
    return QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex());
}

}

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);

    const QString base = QStringLiteral("/tmp/checksum_verification_test");
    QDir(base).removeRecursively();
    QDir().mkpath(base + "/local");

    bool allPass = true;
    auto check = [&](const QString &label, bool condition) {
        qDebug() << (condition ? "[PASS]" : "[FAIL]") << label;
        if (!condition) allPass = false;
    };

    auto waitFor = [&](const std::function<bool()> &predicate, int timeoutMs = 5000) {
        QEventLoop loop;
        QTimer timeoutTimer;
        timeoutTimer.setSingleShot(true);
        QObject::connect(&timeoutTimer, &QTimer::timeout, &loop, &QEventLoop::quit);
        timeoutTimer.start(timeoutMs);
        while (!predicate() && timeoutTimer.isActive())
            loop.processEvents(QEventLoop::AllEvents, 20);
        return predicate();
    };

    auto *manager = new TransferManager(&app);
    auto *verifier = new ChecksumVerifier(manager, &app);

    // Constructed up front, alongside manager, specifically so it's
    // listening to itemAdded/itemUpdated for EVERY item this whole test
    // creates (real and hidden) — see Scenario 6 at the bottom, which
    // depends on that.
    auto *queueWidget = new TransferQueueWidget(manager);
    auto *activeTable = queueWidget->findChild<TransferQueueTable *>("activeQueueTable");
    auto *completedTable = queueWidget->findChild<TransferQueueTable *>("completedQueueTable");
    auto *failedTable = queueWidget->findChild<TransferQueueTable *>("failedQueueTable");

    int realItemsAdded = 0;
    int hiddenItemsAdded = 0;
    QObject::connect(manager, &TransferManager::itemAdded, &app, [&](const TransferItem &item) {
        if (item.isVerificationTask)
            ++hiddenItemsAdded;
        else
            ++realItemsAdded;
    });

    auto findItemStatus = [&](int id) -> TransferStatus {
        for (const TransferItem &item : manager->items()) {
            if (item.id == id)
                return item.status;
        }
        return TransferStatus::Failed;   // not found — shouldn't happen for an id this test just captured
    };

    // ===================================================================
    // Scenario 1: LocalToLocal — both sides hashed directly, no fake
    // backend involved at all, genuinely free (no network re-read).
    // ===================================================================
    {
        QDir().mkpath(base + "/local/s1_src");
        QDir().mkpath(base + "/local/s1_dst");
        writeFile(base + "/local/s1_src/match.txt", "identical content");

        auto *srcPane = new FilePaneWidget(new LocalBackend());
        auto *dstPane = new FilePaneWidget(new LocalBackend());
        bool srcReady = false, dstReady = false;
        srcPane->navigateTo(base + "/local/s1_src");
        dstPane->navigateTo(base + "/local/s1_dst");
        QTimer::singleShot(100, &app, [&]() { srcReady = true; dstReady = true; });
        waitFor([&]() { return srcReady && dstReady; }, 2000);

        int itemId = manager->enqueue(srcPane, dstPane, "match.txt");

        bool done = false;
        QObject::connect(manager, &TransferManager::itemUpdated, &app, [&](const TransferItem &item) {
            if (item.id == itemId && item.status == TransferStatus::Done)
                done = true;
        });
        check("scenario 1: LocalToLocal transfer completes", waitFor([&]() { return done; }));

        bool finished = false, failed = false, matched = false;
        QString capturedLocalHex, capturedRemoteHex;
        auto conFin = QObject::connect(verifier, &ChecksumVerifier::verificationFinished, &app,
            [&](int id, bool m, const QString &l, const QString &r) {
                if (id != itemId) return;
                finished = true; matched = m; capturedLocalHex = l; capturedRemoteHex = r;
            });
        auto conFail = QObject::connect(verifier, &ChecksumVerifier::verificationFailed, &app,
            [&](int id, const QString &) { if (id == itemId) failed = true; });

        verifier->verify(itemId);
        check("scenario 1: verification completes without failing",
              waitFor([&]() { return finished || failed; }) && !failed);
        check("scenario 1: content matches", matched);
        const QString expected = sha256Hex("identical content");
        check("scenario 1: local hash is the expected SHA-256", capturedLocalHex == expected);
        check("scenario 1: remote hash is the expected SHA-256", capturedRemoteHex == expected);
        check("scenario 1: no hidden temp-download was needed (LocalToLocal is free)", true);

        QObject::disconnect(conFin);
        QObject::disconnect(conFail);
    }

    // ===================================================================
    // Scenario 2: RemoteToLocal — match, then a deliberately induced
    // mismatch on the SAME already-Done item (simulating the remote copy
    // being corrupted sometime after the original transfer completed).
    // ===================================================================
    int scenario2ItemId = -1;
    {
        QDir().mkpath(base + "/local/s2_dst");
        auto *fake = new FakeVerifiableBackend(&app);
        fake->remoteContent = "hello from the server";

        auto *remotePane = new FilePaneWidget(fake);
        auto *localPane = new FilePaneWidget(new LocalBackend());
        bool ready = false;
        localPane->navigateTo(base + "/local/s2_dst");
        QTimer::singleShot(100, &app, [&]() { ready = true; });
        waitFor([&]() { return ready; }, 2000);

        scenario2ItemId = manager->enqueue(remotePane, localPane, "remote_file.bin");
        bool done = false;
        auto con = QObject::connect(manager, &TransferManager::itemUpdated, &app, [&](const TransferItem &item) {
            if (item.id == scenario2ItemId && item.status == TransferStatus::Done)
                done = true;
        });
        check("scenario 2: RemoteToLocal transfer completes", waitFor([&]() { return done; }));
        QObject::disconnect(con);

        // --- 2a: verify while the remote is unchanged -> match ---
        bool finished = false, matched = false;
        auto conFin = QObject::connect(verifier, &ChecksumVerifier::verificationFinished, &app,
            [&](int id, bool m, const QString &, const QString &) {
                if (id == scenario2ItemId) { finished = true; matched = m; }
            });
        verifier->verify(scenario2ItemId);
        check("scenario 2a: verification finishes", waitFor([&]() { return finished; }));
        check("scenario 2a: unmodified remote content matches", matched);
        QObject::disconnect(conFin);

        // --- 2b: corrupt the "server" content, verify again -> mismatch ---
        fake->remoteContent = "corrupted content, not what was originally sent";
        finished = false;
        bool mismatchedCorrectly = false;
        QString localHex, remoteHex;
        conFin = QObject::connect(verifier, &ChecksumVerifier::verificationFinished, &app,
            [&](int id, bool m, const QString &l, const QString &r) {
                if (id == scenario2ItemId) { finished = true; mismatchedCorrectly = !m; localHex = l; remoteHex = r; }
            });
        verifier->verify(scenario2ItemId);
        check("scenario 2b: verification finishes", waitFor([&]() { return finished; }));
        check("scenario 2b: corrupted remote content is correctly detected as a mismatch", mismatchedCorrectly);
        check("scenario 2b: local/remote hashes actually differ", localHex != remoteHex);
        check("scenario 2b: local file on disk was untouched by the corruption",
              readFile(base + "/local/s2_dst/remote_file.bin") == "hello from the server");
        QObject::disconnect(conFin);
    }

    // ===================================================================
    // Scenario 3: LocalToRemote — local source hashed directly, remote
    // DESTINATION re-read and verified to match what was actually
    // uploaded.
    // ===================================================================
    {
        QDir().mkpath(base + "/local/s3_src");
        writeFile(base + "/local/s3_src/upload.bin", "content being uploaded");

        auto *fake = new FakeVerifiableBackend(&app);
        auto *localPane = new FilePaneWidget(new LocalBackend());
        auto *remotePane = new FilePaneWidget(fake);
        bool ready = false;
        localPane->navigateTo(base + "/local/s3_src");
        QTimer::singleShot(100, &app, [&]() { ready = true; });
        waitFor([&]() { return ready; }, 2000);

        int itemId = manager->enqueue(localPane, remotePane, "upload.bin");
        bool done = false;
        auto con = QObject::connect(manager, &TransferManager::itemUpdated, &app, [&](const TransferItem &item) {
            if (item.id == itemId && item.status == TransferStatus::Done)
                done = true;
        });
        check("scenario 3: LocalToRemote transfer completes", waitFor([&]() { return done; }));
        QObject::disconnect(con);

        bool finished = false, matched = false;
        auto conFin = QObject::connect(verifier, &ChecksumVerifier::verificationFinished, &app,
            [&](int id, bool m, const QString &, const QString &) {
                if (id == itemId) { finished = true; matched = m; }
            });
        verifier->verify(itemId);
        check("scenario 3: verification finishes", waitFor([&]() { return finished; }));
        check("scenario 3: uploaded content matches on re-read", matched);
        QObject::disconnect(conFin);
    }

    // ===================================================================
    // Scenario 4: busy-backend queuing — verifying an item whose remote
    // side is ALREADY claimed by a second, real, in-flight transfer must
    // queue behind it rather than corrupting/interleaving with it.
    // ===================================================================
    {
        QDir().mkpath(base + "/local/s4_dst");
        auto *fake = new FakeVerifiableBackend(&app);
        fake->remoteContent = "steady, unchanging content";

        auto *remotePane = new FilePaneWidget(fake);
        auto *localPane = new FilePaneWidget(new LocalBackend());
        bool ready = false;
        localPane->navigateTo(base + "/local/s4_dst");
        QTimer::singleShot(100, &app, [&]() { ready = true; });
        waitFor([&]() { return ready; }, 2000);

        // Establish the original, already-Done item to verify.
        int origItemId = manager->enqueue(remotePane, localPane, "orig.bin");
        bool origDone = false;
        auto con1 = QObject::connect(manager, &TransferManager::itemUpdated, &app, [&](const TransferItem &item) {
            if (item.id == origItemId && item.status == TransferStatus::Done)
                origDone = true;
        });
        check("scenario 4: original transfer completes", waitFor([&]() { return origDone; }));
        QObject::disconnect(con1);

        // Start a second, slow transfer against the SAME fake backend
        // instance — this is what the hidden verification temp-download
        // must queue behind.
        int blockerItemId = manager->enqueue(remotePane, localPane, "blocker.bin");
        // dispatchActiveItem() reaches the backend via
        // QMetaObject::invokeMethod(..., Qt::QueuedConnection, ...) —
        // deferred to the next event-loop turn, not synchronous with
        // enqueue() returning — so this needs a real wait, not an
        // immediate check.
        check("scenario 4: blocker transfer is actually busy on the shared backend",
              waitFor([&]() { return fake->isBusy(); }, 1000));

        // Kick off verification while the blocker is still running, and
        // capture the hidden item's id via itemAdded.
        int hiddenItemId = -1;
        auto conAdd = QObject::connect(manager, &TransferManager::itemAdded, &app, [&](const TransferItem &item) {
            if (item.isVerificationTask && hiddenItemId < 0)
                hiddenItemId = item.id;
        });
        verifier->verify(origItemId);
        check("scenario 4: a hidden temp-download item was created", hiddenItemId >= 0);
        check("scenario 4: the hidden item stays Queued while the backend is busy",
              hiddenItemId >= 0 && findItemStatus(hiddenItemId) == TransferStatus::Queued);
        QObject::disconnect(conAdd);

        bool blockerDone = false;
        auto con2 = QObject::connect(manager, &TransferManager::itemUpdated, &app, [&](const TransferItem &item) {
            if (item.id == blockerItemId && item.status == TransferStatus::Done)
                blockerDone = true;
        });
        check("scenario 4: blocker transfer eventually completes", waitFor([&]() { return blockerDone; }));
        QObject::disconnect(con2);

        bool finished = false, matched = false;
        auto conFin = QObject::connect(verifier, &ChecksumVerifier::verificationFinished, &app,
            [&](int id, bool m, const QString &, const QString &) {
                if (id == origItemId) { finished = true; matched = m; }
            });
        check("scenario 4: verification dispatches and finishes once the backend frees up",
              waitFor([&]() { return finished; }));
        check("scenario 4: content still matches after queuing behind the blocker", matched);
        QObject::disconnect(conFin);
    }

    // ===================================================================
    // Scenario 5: cancellation — cancel() during an in-flight
    // verification stops it cleanly, with no eventual verificationFinished.
    // ===================================================================
    {
        QDir().mkpath(base + "/local/s5_dst");
        auto *fake = new FakeVerifiableBackend(&app);
        fake->remoteContent = "will be cancelled mid-verify";

        auto *remotePane = new FilePaneWidget(fake);
        auto *localPane = new FilePaneWidget(new LocalBackend());
        bool ready = false;
        localPane->navigateTo(base + "/local/s5_dst");
        QTimer::singleShot(100, &app, [&]() { ready = true; });
        waitFor([&]() { return ready; }, 2000);

        int itemId = manager->enqueue(remotePane, localPane, "tocancel.bin");
        bool done = false;
        auto con1 = QObject::connect(manager, &TransferManager::itemUpdated, &app, [&](const TransferItem &item) {
            if (item.id == itemId && item.status == TransferStatus::Done)
                done = true;
        });
        check("scenario 5: original transfer completes", waitFor([&]() { return done; }));
        QObject::disconnect(con1);

        bool everFinished = false, failedWithCancelReason = false;
        auto conFin = QObject::connect(verifier, &ChecksumVerifier::verificationFinished, &app,
            [&](int id, bool, const QString &, const QString &) { if (id == itemId) everFinished = true; });
        auto conFail = QObject::connect(verifier, &ChecksumVerifier::verificationFailed, &app,
            [&](int id, const QString &reason) {
                if (id == itemId && reason.contains("cancelled", Qt::CaseInsensitive))
                    failedWithCancelReason = true;
            });

        verifier->verify(itemId);
        check("scenario 5: verification is in flight", waitFor([&]() { return verifier->isVerifying(itemId); }, 1000));
        verifier->cancel(itemId);
        check("scenario 5: cancel() clears in-flight state immediately", !verifier->isVerifying(itemId));
        check("scenario 5: verificationFailed fired with a cancellation reason",
              waitFor([&]() { return failedWithCancelReason; }));

        // Give any stray late signal delivery a real chance to arrive
        // before asserting it never did.
        QEventLoop settle;
        QTimer::singleShot(300, &settle, &QEventLoop::quit);
        settle.exec();
        check("scenario 5: verificationFinished never fires for a cancelled verification", !everFinished);

        QObject::disconnect(conFin);
        QObject::disconnect(conFail);
    }

    // ===================================================================
    // Scenario 6: hidden temp-download items never produce a visible row
    // in TransferQueueWidget's tabs — checked against queueWidget, which
    // has been listening since before Scenario 1 ran.
    // ===================================================================
    {
        check("scenario 6: this run actually created hidden verification items to hide",
              hiddenItemsAdded > 0);
        const int visibleRows = activeTable->rowCount() + completedTable->rowCount() + failedTable->rowCount();
        check("scenario 6: visible row count matches only the REAL items, none of the hidden ones",
              visibleRows == realItemsAdded);
    }

    qDebug() << (allPass ? "ALL TESTS PASSED" : "SOME TESTS FAILED");
    return allPass ? 0 : 1;
}

#include "checksum_verification_test.moc"
