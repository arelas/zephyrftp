#pragma once

#include <QObject>
#include <QHash>
#include <QString>

class TransferManager;
class FilePaneWidget;
struct TransferItem;

// Confirms a specific COMPLETED transfer's file actually matches on both
// ends, on demand — never automatic/background (see TransferQueueTable's
// "Verify Checksum..." context-menu action, the sole trigger). SHA-256,
// hardcoded — matches this project's existing use of
// QCryptographicHash::Sha256 for host-key/cert fingerprints and in its own
// verify_sftp_throughput.cpp/verify_bandwidth_throttle_live.cpp probes.
//
// The reason this exists at all rather than a simpler "just hash both
// paths": for anything except LocalToLocal, one side is remote, and there
// is no cheaper way to independently confirm its bytes than reading it a
// second time over the network (grepped the installed libssh2_sftp.h
// directly — no check-file/hash extension binding, no generic "send an
// arbitrary SFTP extended request" escape hatch either; FTP-side, neither
// this project's own test servers nor FtpBackend send any hash command).
// That re-read is modeled as an ordinary, hidden TransferItem (see
// TransferManager::startVerificationDownload() and TransferItem::
// isVerificationTask's own doc comments) so it automatically gets
// busy-backend queuing, pool support, and cancellation for free, instead
// of a second, parallel claim mechanism.
//
// Constructed with a TransferManager*, used only through its *public*
// surface (items(), startVerificationDownload(), cancelItem(),
// itemUpdated) — no new coupling into TransferManager's internals. Same
// "focused subsystem, not piled onto TransferManager" shape as
// DirectoryComparer/CompareSyncExecutor for compare-and-sync.
class ChecksumVerifier : public QObject {
    Q_OBJECT
public:
    explicit ChecksumVerifier(TransferManager *manager, QObject *parent = nullptr);

    // Starts verifying the given, already-Done, non-RemoteToRemote
    // item's transfer. A no-op (nothing emitted) if originalItemId is
    // already being verified — see isVerifying(). Reports
    // verificationFailed immediately, synchronously, for anything that
    // can't be verified at all (item no longer found, not Done,
    // RemoteToRemote/Move/Unsupported direction) rather than silently
    // doing nothing — TransferQueueTable's own enablement check should
    // normally prevent these, but this is the authoritative check, not a
    // belt-and-braces duplicate of it.
    void verify(int originalItemId);

    bool isVerifying(int originalItemId) const { return m_pending.contains(originalItemId); }

    // Cancels an in-flight verification immediately, from THIS class's
    // own bookkeeping — regardless of whether the remote re-download
    // ever started, is mid-flight, or this is a pure local-to-local hash
    // pass with no TransferManager involvement at all (which, lacking
    // any cancellable primitive of its own, simply has its eventual
    // result discarded once forgotten here — a real, accepted gap, not
    // worth a QPromise-based cancellation mechanism for how rarely a
    // local hash pass is slow enough to matter). Emits
    // verificationFailed("Verification cancelled.") — safe to call for
    // an id that isn't currently verifying (silent no-op).
    void cancel(int originalItemId);

signals:
    // matched is a case-insensitive hex compare of localHex/remoteHex —
    // QCryptographicHash::result().toHex() is already consistently
    // lowercase, but compared case-insensitively anyway for robustness.
    // "local"/"remote" here always means "the side hashed directly from
    // disk" / "the side that needed a fresh network re-read" — for
    // LocalToLocal specifically, both sides are hashed directly and
    // "remoteHex" just means the second (destination) path's hash, not
    // an actual network read.
    void verificationFinished(int originalItemId, bool matched, const QString &localHex,
                               const QString &remoteHex);
    void verificationFailed(int originalItemId, const QString &reason);

    // Forwarded, filtered progress for the remote re-download half only
    // — the local hash pass has no meaningful chunked progress of its
    // own (QCryptographicHash::addData(QIODevice*) reads a whole file in
    // one call). TransferQueueWidget's QProgressDialog listens to this
    // instead of reaching into TransferManager::itemUpdated() itself.
    void verificationProgress(int originalItemId, qint64 bytesDone, qint64 bytesTotal);

private slots:
    void onManagerItemUpdated(const TransferItem &item);

private:
    struct PendingVerification {
        QString localHex;
        bool localOk = false;
        bool localFinished = false;
        QString localError;

        QString otherHex;
        bool otherOk = false;
        bool otherFinished = false;
        QString otherError;

        // -1 for LocalToLocal (both sides hashed directly, no
        // TransferManager involvement for either) — otherwise the id
        // TransferManager::startVerificationDownload() returned, used to
        // route itemUpdated() deliveries via m_verifyItemIdToOriginalId
        // and to cancel the in-flight download from cancel() above.
        int verifyItemId = -1;
    };

    void startLocalHash(int originalItemId, const QString &path);
    void startOtherLocalHash(int originalItemId, const QString &path);   // LocalToLocal's second file
    void startRemoteReread(int originalItemId, FilePaneWidget *remotePane, const QString &remotePath,
                            const QString &fileName);
    void finishWithRemoteFile(int originalItemId, const QString &tempPath);
    void onLocalHashFinished(int originalItemId, bool ok, const QString &hex, const QString &errorString);
    void onOtherHashFinished(int originalItemId, bool ok, const QString &hex, const QString &errorString);
    void maybeFinish(int originalItemId);

    TransferManager *m_manager;
    QHash<int, PendingVerification> m_pending;         // keyed by originalItemId
    QHash<int, int> m_verifyItemIdToOriginalId;         // temp-download item id -> originalItemId
};
