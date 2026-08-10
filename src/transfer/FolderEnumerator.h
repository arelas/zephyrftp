#pragma once

#include <QObject>
#include <QString>
#include <QList>
#include "../backends/RemoteEntry.h"

class RemoteBackend;

// A single discovered item within an enumerated folder tree, relative to
// the folder's own root — the caller combines this with wherever the
// folder is actually being transferred TO, so relativePath already reads
// as "the path this item should have under the destination folder",
// starting with the root folder's own name.
struct EnumeratedItem {
    QString relativePath;   // e.g. "photos/subdir/photo.jpg", or "photos/subdir" for a directory itself
    bool isDir = false;
    qint64 size = 0;        // 0 for directories
};

// Recursively walks a folder via a backend's listDirectoryForEnumeration()
// — never listDirectory() (see RemoteBackend's doc comment on why that
// would corrupt the owning pane's own navigation/display mid-walk) —
// collecting a flat manifest of everything found. One-shot: construct,
// call start(), wait for finished() or failed().
//
// Processes one directory at a time, strictly serially, rather than
// firing off many concurrent listDirectoryForEnumeration() calls: a real
// SftpBackend has exactly one session, so concurrent calls would just
// serialize through libssh2 anyway, and LocalBackend gains nothing from
// parallel QDir reads either. Simple and strictly ordered beats a
// concurrency scheme that wouldn't actually buy anything here.
//
// Ordering guarantee: items in finished()'s list are ordered so every
// directory appears before anything inside it — a caller building a
// mirrored directory structure on a destination can create directories
// strictly in list order without needing to sort or resolve dependencies
// itself.
class FolderEnumerator : public QObject {
    Q_OBJECT
public:
    // backend must already be connected/usable; not owned — this class
    // doesn't manage the backend's lifetime, just talks to it. rootPath
    // is the absolute path of the folder being walked; rootName seeds
    // relativePath so results already read as paths relative to
    // wherever the caller intends to recreate this folder, not relative
    // to rootPath's own parent directory.
    FolderEnumerator(RemoteBackend *backend, const QString &rootPath, const QString &rootName,
                      QObject *parent = nullptr);

    void start();

signals:
    void finished(const QList<EnumeratedItem> &items);
    // Any enumeration failure aborts the whole walk rather than skipping
    // the failed subdirectory and continuing — silently completing with
    // missing content would look successful while actually being wrong,
    // which is worse than clearly failing.
    void failed(const QString &reason);

private slots:
    void onDirectoryEnumerated(const QString &path, const QList<RemoteEntry> &entries, int requestId);
    void onEnumerationFailed(const QString &path, const QString &reason, int requestId);

private:
    void enumerateNext();

    RemoteBackend *m_backend;
    QString m_rootPath;
    QString m_rootName;

    struct PendingDir {
        QString absolutePath;
        QString relativePrefix;
    };
    QList<PendingDir> m_pending;
    PendingDir m_activeDir;   // the one currently being listed, popped from m_pending in enumerateNext()

    // A real bug found by testing: this used to be a per-INSTANCE counter
    // starting at 1. Each instance connects directly to the shared
    // backend's directoryEnumerated signal (see the constructor), so two
    // FolderEnumerators walking concurrently against the same backend
    // (e.g. two folders enqueued back to back onto the same source pane)
    // could both be waiting on request id 1 at the same time — Qt fans a
    // signal out to every connected slot, so each instance's
    // onDirectoryEnumerated() would see BOTH responses, and a colliding id
    // meant each incorrectly accepted the OTHER instance's response as its
    // own (requestId == m_activeRequestId matching for the wrong reason),
    // corrupting both enumerations' results together. A `static` counter,
    // shared across every instance ever created, makes that collision
    // impossible between two concurrently-ACTIVE enumerators — the actual
    // scenario the bug above was about. It's a plain (non-atomic) int, not
    // an atomic, because both of this class's real construction sites
    // (TransferManager.cpp, which — per TransferManager.h's own comment —
    // always lives on the GUI thread; verify-sftp-pubkey.cpp's manual test
    // harness) only ever construct one on the GUI thread; a future caller
    // constructing one from a worker thread would reintroduce a genuine
    // data race here, since nothing enforces that today.
    //
    // NOT covered by "impossible" above, found by review rather than
    // testing: a FINISHED enumerator stays connected to the backend's
    // signals until its deleteLater() (issued by the caller right after
    // finished()/failed(), see TransferManager.cpp) actually runs on the
    // next event-loop pass — if a backend ever re-emitted
    // directoryEnumerated for that enumerator's last-consumed requestId
    // during that narrow window, this class would silently reprocess it.
    // Not fixed: no real backend (LocalBackend/SftpBackend/FtpBackend)
    // double-emits for an already-answered request id, so this has no
    // known way to actually happen — noted here rather than guarded
    // against in code for a scenario nothing currently triggers.
    static int s_nextRequestId;
    int m_activeRequestId = -1;
    QList<EnumeratedItem> m_results;
};
