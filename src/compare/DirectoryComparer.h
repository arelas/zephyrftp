#pragma once

#include <QObject>
#include <QString>
#include <QList>
#include "CompareTypes.h"
#include "../transfer/FolderEnumerator.h"

class RemoteBackend;

// Recursively diffs two directory trees — one per side, each potentially
// on a different backend/server entirely — by reusing FolderEnumerator
// (already built for folder-transfer's own recursive walk) against both
// roots at once, then classifying every relative path found on either
// side into OnlyLeft/OnlyRight/Differs/Identical (see CompareTypes.h).
//
// One-shot: construct, call start(), wait for finished() or failed().
//
// Deliberately does NOT compare file content (no hashing) — classification
// is size + modified time only. Either one differing is treated as real,
// observable evidence the files aren't the same content; using both
// (rather than either alone) avoids two false-negative failure modes a
// single-field comparison would have: same-size-different-content edits,
// and a byte-identical re-upload that only changed the timestamp.
//
// Symlinks get no special handling — they classify by whatever
// isDir/size/modified the existing RemoteEntry -> EnumeratedItem pipeline
// already reports for them, same as an ordinary directory listing already
// treats them elsewhere in this codebase. Not a gap being silently missed,
// just genuinely out of scope for this feature.
class DirectoryComparer : public QObject {
    Q_OBJECT
public:
    // Backends are not owned — same convention as FolderEnumerator. Roots
    // are absolute paths on their respective backend.
    DirectoryComparer(RemoteBackend *leftBackend, const QString &leftRoot,
                       RemoteBackend *rightBackend, const QString &rightRoot,
                       QObject *parent = nullptr);

    void start();

signals:
    void finished(const QList<CompareEntry> &results);
    // Emitted once, identifying which side failed and why. If the other
    // side is still enumerating when this fires, it's left to finish
    // naturally (FolderEnumerator has no cancel()) but its result is
    // discarded — nothing waits on it, and classify() never runs.
    void failed(const QString &reason);

private:
    void onLeftFinished(const QList<EnumeratedItem> &items);
    void onRightFinished(const QList<EnumeratedItem> &items);
    void onLeftFailed(const QString &reason);
    void onRightFailed(const QString &reason);
    void maybeClassify();
    void classify();

    // FolderEnumerator seeds every result's relativePath from a
    // caller-supplied rootName; passing an empty one here (so both sides'
    // paths are comparable as "relative to each side's own root", unlike
    // folder-transfer's own use where rootName seeds the manifest with the
    // folder's own name) has two side effects, both handled in classify():
    // the synthetic first result (relativePath == rootName == "") must be
    // dropped, and PathUtils.h's joinPath("", name) — which only
    // special-cases a *trailing* slash, not an empty dir argument —
    // produces a leading '/' on every path that must be stripped.
    RemoteBackend *m_leftBackend;
    QString m_leftRoot;
    RemoteBackend *m_rightBackend;
    QString m_rightRoot;

    FolderEnumerator *m_leftEnumerator = nullptr;
    FolderEnumerator *m_rightEnumerator = nullptr;

    bool m_leftDone = false;
    bool m_rightDone = false;
    bool m_failed = false;
    QList<EnumeratedItem> m_leftItems;
    QList<EnumeratedItem> m_rightItems;
};
