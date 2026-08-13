#pragma once

#include <QString>

// Joins a directory and a (possibly multi-segment, e.g. "sub/dir/file")
// relative name into one path — was independently duplicated,
// byte-for-byte identical, in FolderEnumerator.cpp, EditSessionManager.cpp,
// TransferManager.cpp, and FilePaneWidget.cpp before being extracted here;
// a code review flagged that duplication as a real risk (a future fix to
// leading/trailing-slash handling applied to one copy silently wouldn't
// reach the others). name is expected to have no leading separator of its
// own — every call site already guarantees that.
inline QString joinPath(const QString &dir, const QString &name)
{
    return dir.endsWith('/') ? dir + name : dir + '/' + name;
}
