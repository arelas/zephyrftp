#pragma once

#include "RemoteBackend.h"

// Wraps the local filesystem behind the same RemoteBackend interface as
// SftpBackend. Runs on the GUI thread today since QDir/QFile listing is
// fast enough not to matter — flagged below if that assumption changes
// (e.g. slow network-mounted drives).
class LocalBackend : public RemoteBackend {
    Q_OBJECT
public:
    explicit LocalBackend(QObject *parent = nullptr);

    void connectToHost() override;   // immediately emits connected()
    void listDirectory(const QString &path) override;
    void downloadFile(const QString &remotePath, const QString &localPath, qint64 resumeOffset = 0) override;
    void uploadFile(const QString &localPath, const QString &remotePath, qint64 resumeOffset = 0) override;

    void deleteEntry(const QString &path, bool isDirectory) override;
    void renameEntry(const QString &oldPath, const QString &newPath) override;
    void createDirectory(const QString &path) override;
    void createFile(const QString &path) override;

    QString currentPath() const override;
    bool isLocalFilesystem() const override { return true; }

    // No-op: QFile::copy() is a single OS-level call with no loop of ours
    // to check a flag inside — there's nothing to interrupt mid-transfer.
    // Local copies are fast enough in practice that this hasn't mattered.
    void requestCancel() override {}

    // Same reasoning as requestCancel() above — see RemoteBackend's doc
    // comment on requestPause() for why local transfers never actually
    // reach this in practice (the UI doesn't offer Pause for them).
    void requestPause() override {}

private:
    QString m_currentPath;
};
