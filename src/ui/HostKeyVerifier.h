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
    // man-in-the-middle), defaults to "no". port is shown alongside host
    // in the dialog — a real bug found by code review: the known-hosts
    // store keys entries on "[host]:port" (see SftpBackend::verifyHostKey()'s
    // own comment on why — two different SSH services can share a
    // hostname on different ports), but this prompt used to show only the
    // host, so two saved sites on the same hostname but different ports
    // got an identical, indistinguishable "unknown/changed host key"
    // prompt with no way to tell which service was actually being
    // verified.
    Q_INVOKABLE bool confirmHostKey(const QString &host, int port, const QString &fingerprint, bool isMismatch);
};
