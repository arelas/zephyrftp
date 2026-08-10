// Headless regression test for HostKeyVerifier::confirmHostKey() and
// CertificateVerifier::confirmCertificate() — drives the REAL QMessageBox
// each pops, not a mock, using conflict-resolution-test.cpp's own proven
// technique: a QTimer::singleShot() call that (from the GUI thread, no
// cross-thread invokeMethod needed here — that plumbing is SftpBackend/
// FtpBackend's job, already covered by their own live-server verify-*
// harnesses) invokes the blocking confirm*() call directly, while a
// second, slightly-later QTimer::singleShot() finds the live modal via
// QApplication::activeModalWidget() (exec() pumps events internally, the
// same mechanism this project's other live-dialog tests rely on) and
// answers it.
//
// Regression coverage for two real bugs found by code review of
// HostKeyVerifier.cpp (fixed together with its sibling CertificateVerifier,
// same pattern in both):
//   1. The dialog text used to show only the host, never the port, even
//      though the underlying known-hosts/known-certs stores are keyed on
//      host+port together — two saved sites sharing a hostname but on
//      different ports got an identical, indistinguishable prompt.
//   2. QMessageBox::warning()/question() used to be called with a nullptr
//      parent instead of the main window, with no guaranteed stacking/
//      focus relationship to the app.
//
// Run with:
//   QT_QPA_PLATFORM=offscreen ./build/trust-prompt-test
#include <QApplication>
#include <QTimer>
#include <QDebug>
#include <QMessageBox>
#include <QAbstractButton>
#include <QMainWindow>
#include "ui/HostKeyVerifier.h"
#include "ui/CertificateVerifier.h"

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);

    bool allPass = true;
    auto check = [&](const QString &label, bool condition) {
        qDebug() << (condition ? "[PASS]" : "[FAIL]") << label;
        if (!condition) allPass = false;
    };

    // Real QMainWindow, standing in for MainWindow — all that matters
    // here is that it's a real QWidget these verifiers get parented to,
    // matching MainWindow.cpp's own `new HostKeyVerifier(this)` wiring.
    auto *window = new QMainWindow();
    auto *hostKeyVerifier = new HostKeyVerifier(window);
    auto *certificateVerifier = new CertificateVerifier(window);

    bool hostKeyResult = false;
    bool certificateResult = false;

    QTimer::singleShot(100, &app, [&]() {
        hostKeyResult = hostKeyVerifier->confirmHostKey(
            QStringLiteral("sftp.example.com"), 2222, QStringLiteral("SHA256:deadbeef"), false);
    });

    QTimer::singleShot(300, &app, [&]() {
        auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
        check("confirmHostKey() opened a real QMessageBox", box != nullptr);
        if (box) {
            check("host key dialog text includes the PORT alongside the host "
                  "(matches the known-hosts store's own host:port key)",
                  box->text().contains(QStringLiteral("sftp.example.com:2222")));
            check("host key dialog is parented to the main window, not nullptr",
                  box->parentWidget() == window);
            box->button(QMessageBox::Yes)->click();
        }
    });

    QTimer::singleShot(500, &app, [&]() {
        check("confirmHostKey() returned true after clicking Yes", hostKeyResult);

        certificateResult = certificateVerifier->confirmCertificate(
            QStringLiteral("ftps.example.com"), 990, QStringLiteral("SHA256:cafef00d"),
            QStringLiteral("self-signed certificate"), true);
    });

    QTimer::singleShot(700, &app, [&]() {
        auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
        check("confirmCertificate() opened a real QMessageBox", box != nullptr);
        if (box) {
            check("certificate dialog text includes the PORT alongside the host "
                  "(matches the trusted-fingerprint store's own host+port key)",
                  box->text().contains(QStringLiteral("ftps.example.com:990")));
            check("certificate dialog is parented to the main window, not nullptr",
                  box->parentWidget() == window);
            box->button(QMessageBox::No)->click();
        }
    });

    QTimer::singleShot(900, &app, [&]() {
        check("confirmCertificate() returned false after clicking No", !certificateResult);
        qDebug() << (allPass ? "[test] ALL PASS" : "[test] AT LEAST ONE FAILURE");
        app.exit(allPass ? 0 : 1);
    });

    return app.exec();
}
