#pragma once

#include <QObject>
#include <QString>

// FTPS equivalent of HostKeyVerifier — same lifetime, same cross-thread
// pattern: lives on the GUI thread for the app's whole lifetime
// (MainWindow owns one instance), and FtpBackend — running on its own
// worker thread — calls confirmCertificate() via
// QMetaObject::invokeMethod(..., Qt::BlockingQueuedConnection), which
// marshals the call onto this object's (GUI) thread, shows a modal
// dialog there, and blocks the worker thread until the person answers.
// See HostKeyVerifier.h's identical reasoning.
class CertificateVerifier : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;

    // isMismatch=false: first time seeing this host:port's certificate —
    // normal trust-on-first-use prompt. isMismatch=true: the certificate
    // CHANGED since a previously-trusted connection — shown as a strong
    // warning (possible interception), defaults to "no". details carries
    // the certificate's subject/issuer plus the actual reason(s)
    // QSslSocket couldn't verify it (self-signed, expired, ...), so the
    // prompt names the real problem rather than a generic warning. port
    // is shown alongside host — see HostKeyVerifier::confirmHostKey()'s
    // identical reasoning: the trusted-fingerprint store
    // (loadTrustedFingerprint()/saveTrustedFingerprint() in
    // FtpBackend.cpp) is keyed on host+port together, so this prompt
    // needs to show both to actually identify which service is being
    // verified.
    Q_INVOKABLE bool confirmCertificate(const QString &host, int port, const QString &fingerprint,
                                         const QString &details, bool isMismatch);
};
