#pragma once

#include <QString>
#include <QDateTime>

// A single directory-listing row. Deliberately protocol-agnostic:
// LocalBackend and SftpBackend both produce these, so FilePaneWidget's
// model never needs to know which backend it's looking at.
struct RemoteEntry {
    QString name;
    bool isDir = false;
    bool isSymlink = false;
    qint64 size = 0;
    QDateTime modified;
    QString permissions;   // e.g. "rwxr-xr-x" — display only, not parsed back
};
