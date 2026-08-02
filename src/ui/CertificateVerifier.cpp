#include "CertificateVerifier.h"

#include <QMessageBox>

bool CertificateVerifier::confirmCertificate(const QString &host, const QString &fingerprint,
                                              const QString &details, bool isMismatch)
{
    if (isMismatch) {
        const auto reply = QMessageBox::warning(
            nullptr, tr("Certificate Changed"),
            tr("WARNING: the certificate for %1 does not match the one seen previously.\n\n"
               "Fingerprint (SHA-256): %2\n\n%3\n\n"
               "This can mean the server's certificate was legitimately renewed or "
               "reissued — or it can mean someone is intercepting this connection "
               "(a man-in-the-middle attack). Only continue if you're certain this "
               "change is expected.\n\n"
               "Connect anyway?")
                .arg(host, fingerprint, details),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        return reply == QMessageBox::Yes;
    }

    const auto reply = QMessageBox::question(
        nullptr, tr("Untrusted Certificate"),
        tr("The certificate presented by '%1' can't be verified — this is the first "
           "time connecting to it.\n\n"
           "Fingerprint (SHA-256): %2\n\n%3\n\n"
           "Trust this certificate and continue? This will be remembered for future "
           "connections.")
            .arg(host, fingerprint, details),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    return reply == QMessageBox::Yes;
}
