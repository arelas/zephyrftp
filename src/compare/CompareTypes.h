#pragma once

#include <QString>
#include <QDateTime>

// Shared plain data between DirectoryComparer (produces these) and
// CompareDialog/CompareSyncExecutor (consume these) — kept in their own
// header so neither the UI nor the executor needs to reach into
// DirectoryComparer's internals just to know the shape of a result row.

enum class CompareStatus {
    OnlyLeft,    // present under the left root, absent under the right
    OnlyRight,   // present under the right root, absent under the left
    Differs,     // present on both sides but size and/or modified differ
                 // (or one side is a file and the other a directory of
                 // the same relative path — a known edge case, not a
                 // silently-wrong classification: see DirectoryComparer's
                 // own class comment)
    Identical,   // present on both sides with matching size and modified,
                 // or a directory present on both sides (a directory's
                 // own "content" is its children, which get their own
                 // rows — the directory entry itself has nothing to
                 // compare beyond existing on both sides)
};

struct CompareEntry {
    // Relative to each side's own compared root, '/'-separated, no
    // leading slash (DirectoryComparer strips FolderEnumerator's leading
    // slash before this struct is ever built — see its own comment on
    // why one would otherwise appear).
    QString relativePath;
    bool isDir = false;
    CompareStatus status = CompareStatus::Identical;

    // -1 / invalid QDateTime means "absent on that side" — meaningful
    // only when status is OnlyLeft/OnlyRight/Differs; both sides are
    // populated for Differs and for a both-sides Identical file.
    qint64 leftSize = -1;
    qint64 rightSize = -1;
    QDateTime leftModified;
    QDateTime rightModified;
};
