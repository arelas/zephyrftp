#pragma once

#include <QObject>
#include <QString>

// Lives on the GUI thread for the app's whole lifetime (MainWindow owns
// one instance). SftpBackend — running on its own worker thread — calls
// confirmHostKey() via QMetaObject::invokeMethod(..., Qt::BlockingQueuedConnection),
// which marshals the call onto this object's (GUI) thread, shows a modal
// dialog there (safe, since QMessageBox always runs on the GUI thread),
// and blocks the worker thread until the person answers. This is the
// standard Qt pattern for "a background thread needs a synchronous answer
// from the UI" — there's no other safe way to pop a modal dialog from a
// non-GUI thread.
class HostKeyVerifier : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;

    // isMismatch=false: first time seeing this host — normal "trust on
    // first use" prompt. isMismatch=true: the key has CHANGED since a
    // previous connection — shown as a strong warning (possible
    // man-in-the-middle), defaults to "no".
    Q_INVOKABLE bool confirmHostKey(const QString &host, const QString &fingerprint, bool isMismatch);
};
