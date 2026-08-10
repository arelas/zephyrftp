#include "HostKeyVerifier.h"
#include "TrustPromptDialog.h"

bool HostKeyVerifier::confirmHostKey(const QString &host, int port, const QString &fingerprint, bool isMismatch)
{
    // "[host]:port" — matches the known-hosts store's own key format
    // (SftpBackend::verifyHostKey()) exactly, so what's shown here always
    // identifies the actual service being verified.
    const QString hostLabel = QStringLiteral("%1:%2").arg(host).arg(port);

    // Parented to the main window (this object's own QObject parent —
    // MainWindow always constructs one as `new HostKeyVerifier(this)`),
    // not nullptr — a real bug found by code review: a parentless
    // QMessageBox has no guaranteed stacking/focus relationship to the
    // app, so on some window managers it could open unfocused, behind the
    // main window, or on a different virtual desktop, making the app
    // LOOK hung (the worker thread really is blocked, waiting on this
    // dialog) while the actual prompt is invisible. qobject_cast rather
    // than a stored QWidget* member: this class's only real construction
    // site already passes the main window as its QObject parent, so
    // there's no need for a second, redundant reference to it.
    auto *parentWidget = qobject_cast<QWidget *>(parent());

    if (isMismatch) {
        return TrustPromptDialog::confirm(parentWidget, tr("Host Key Changed"),
            tr("WARNING: the host key for %1 does not match the one seen previously.\n\n"
               "Fingerprint: %2\n\n"
               "This can mean the server was legitimately reinstalled or "
               "reconfigured — or it can mean someone is intercepting this "
               "connection (a man-in-the-middle attack). Only continue if "
               "you're certain this change is expected.\n\n"
               "Connect anyway?")
                .arg(hostLabel, fingerprint),
            true);
    }

    return TrustPromptDialog::confirm(parentWidget, tr("Unknown Host"),
        tr("The authenticity of host '%1' can't be established — this is "
           "the first time connecting to it.\n\n"
           "Fingerprint: %2\n\n"
           "Trust this host and continue? This will be remembered for "
           "future connections.")
            .arg(hostLabel, fingerprint),
        false);
}
