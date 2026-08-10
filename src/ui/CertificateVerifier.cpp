#include "CertificateVerifier.h"
#include "TrustPromptDialog.h"

bool CertificateVerifier::confirmCertificate(const QString &host, int port, const QString &fingerprint,
                                              const QString &details, bool isMismatch)
{
    // "host:port" — matches the trusted-fingerprint store's own
    // host+port key (FtpBackend.cpp's loadTrustedFingerprint()/
    // saveTrustedFingerprint()) exactly. See HostKeyVerifier.cpp's
    // identical reasoning — a real bug found by code review, fixed
    // together with its sibling class.
    const QString hostLabel = QStringLiteral("%1:%2").arg(host).arg(port);

    // Parented to the main window, not nullptr — see
    // HostKeyVerifier.cpp's identical reasoning; a real bug found by
    // code review, fixed together with its sibling class.
    auto *parentWidget = qobject_cast<QWidget *>(parent());

    if (isMismatch) {
        return TrustPromptDialog::confirm(parentWidget, tr("Certificate Changed"),
            tr("WARNING: the certificate for %1 does not match the one seen previously.\n\n"
               "Fingerprint (SHA-256): %2\n\n%3\n\n"
               "This can mean the server's certificate was legitimately renewed or "
               "reissued — or it can mean someone is intercepting this connection "
               "(a man-in-the-middle attack). Only continue if you're certain this "
               "change is expected.\n\n"
               "Connect anyway?")
                .arg(hostLabel, fingerprint, details),
            true);
    }

    return TrustPromptDialog::confirm(parentWidget, tr("Untrusted Certificate"),
        tr("The certificate presented by '%1' can't be verified — this is the first "
           "time connecting to it.\n\n"
           "Fingerprint (SHA-256): %2\n\n%3\n\n"
           "Trust this certificate and continue? This will be remembered for future "
           "connections.")
            .arg(hostLabel, fingerprint, details),
        false);
}
