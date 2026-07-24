#include "HostKeyVerifier.h"

#include <QMessageBox>

bool HostKeyVerifier::confirmHostKey(const QString &host, const QString &fingerprint, bool isMismatch)
{
    if (isMismatch) {
        const auto reply = QMessageBox::warning(
            nullptr, tr("Host Key Changed"),
            tr("WARNING: the host key for %1 does not match the one seen previously.\n\n"
               "Fingerprint: %2\n\n"
               "This can mean the server was legitimately reinstalled or "
               "reconfigured — or it can mean someone is intercepting this "
               "connection (a man-in-the-middle attack). Only continue if "
               "you're certain this change is expected.\n\n"
               "Connect anyway?")
                .arg(host, fingerprint),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        return reply == QMessageBox::Yes;
    }

    const auto reply = QMessageBox::question(
        nullptr, tr("Unknown Host"),
        tr("The authenticity of host '%1' can't be established — this is "
           "the first time connecting to it.\n\n"
           "Fingerprint: %2\n\n"
           "Trust this host and continue? This will be remembered for "
           "future connections.")
            .arg(host, fingerprint),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    return reply == QMessageBox::Yes;
}
