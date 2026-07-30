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

    int m_nextRequestId = 1;
    int m_activeRequestId = -1;
    QList<EnumeratedItem> m_results;
};
