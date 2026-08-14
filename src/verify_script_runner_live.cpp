// Live-server verification of the ONE thing script-runner-test
// deliberately can't cover: the real `open <site-name>` path — a real
// SavedSite (via SiteStore), a real host-key trust decision, and a real
// SftpBackend connection, run against an actual local sshd
// (tools/local-test-servers/start-sftp-pubkey.sh, same server every
// other verify_sftp_*.cpp harness in this project already uses).
// EXCLUDE_FROM_ALL, not part of the required suite — same reasoning
// verify-credential-store isn't either: a live server (and, in spirit,
// an OS keyring) is an external precondition this required suite
// deliberately avoids depending on.
//
// Scope: public-key auth with NO passphrase — deliberately chosen so
// this harness needs no real OS keyring dependency at all (ScriptRunner
// ::runOpen() only ever calls CredentialStore::load() for a passphrase
// when the key actually has one; an unprotected key's lookup failing is
// the ordinary, expected case, not an error — see ScriptRunner.cpp's own
// comment). What this harness actually proves, for real, on a real
// connection: ARCHITECTURE.md's "Scripting/automation" entry claims
// `open` constructs SftpBackend/FtpBackend with a null HostKeyVerifier*,
// and that an ALREADY-TRUSTED host connects with zero verifier
// interaction (confirmed by reading SftpBackend::verifyHostKey()'s
// CHECK_MATCH branch, not previously proven against a real server). This
// harness establishes trust for real first (a real host-key TOFU prompt,
// auto-accepted — same technique verify_sftp_pubkey.cpp already uses),
// then runs a REAL ScriptRunner script against that now-trusted host and
// confirms it connects silently — no prompt, no hang, real directory
// listing back.
#include <QApplication>
#include <QTimer>
#include <QDebug>
#include <QFile>
#include <QThread>
#include <QMessageBox>
#include <QAbstractButton>
#include <QStandardPaths>
#include "ui/HostKeyVerifier.h"
#include "backends/SftpBackend.h"
#include "backends/SftpCredentials.h"
#include "backends/SavedSite.h"
#include "cli/ScriptParser.h"
#include "cli/ScriptRunner.h"
#include "ui/FilePaneWidget.h"

namespace {

void autoAcceptHostKeyPrompt()
{
    QTimer::singleShot(50, qApp, [] {
        auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
        if (box) {
            for (QAbstractButton *button : box->buttons()) {
                if (box->buttonRole(button) == QMessageBox::YesRole) {
                    button->click();
                    return;
                }
            }
        }
        QTimer::singleShot(50, qApp, &autoAcceptHostKeyPrompt);
    });
}

} // namespace

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);   // see verify_sftp_pubkey.cpp's identical comment

    // Isolated sites.json — this harness writes a real SavedSite and
    // should never touch whatever the person running it actually has
    // saved. known_hosts is DELIBERATELY NOT isolated the same way —
    // this harness needs the SAME trust store both the setup phase (a
    // real HostKeyVerifier) and the real `open` command (a null one)
    // actually share, exactly as they would in real use.
    QStandardPaths::setTestModeEnabled(true);

    bool allPass = true;
    auto check = [&](const char *label, bool condition) {
        fprintf(stderr, "[%s] %s\n", condition ? "PASS" : "FAIL", label);
        if (!condition) allPass = false;
    };

    const QString scratch = qEnvironmentVariable(
        "SFTP_TEST_SCRATCH", "/tmp/zephyrftp-local-test-servers/sftp");

    SftpCredentials creds;
    creds.host = "127.0.0.1";
    creds.port = 2222;
    creds.username = qEnvironmentVariable("USER", "test");
    creds.authMethod = SftpAuthMethod::PublicKey;
    creds.privateKeyPath = scratch + "/client_key";

    // --- Phase 1: establish real host-key trust (real verifier, auto-accepted) ---
    auto *hostKeyVerifier = new HostKeyVerifier(&app);
    auto *setupBackend = new SftpBackend(creds, hostKeyVerifier);
    auto *setupThread = new QThread(&app);
    setupBackend->moveToThread(setupThread);
    setupThread->start();

    bool setupConnected = false;
    QString setupFailure;
    QObject::connect(setupBackend, &RemoteBackend::connectionFailed, &app, [&](const QString &reason) {
        setupFailure = reason;
        qApp->quit();
    });
    QObject::connect(setupBackend, &RemoteBackend::connected, &app, [&]() {
        setupConnected = true;
        qApp->quit();
    });

    autoAcceptHostKeyPrompt();
    QMetaObject::invokeMethod(setupBackend, "connectToHost", Qt::QueuedConnection);
    QTimer::singleShot(15000, &app, [&]() { qApp->quit(); });   // safety net
    app.exec();

    check("phase 1: trust setup connected (real host-key prompt, auto-accepted)", setupConnected);
    if (!setupConnected)
        qWarning() << "phase 1 failure reason:" << setupFailure;

    setupThread->quit();
    setupThread->wait();

    // --- Phase 2: write a real SavedSite, no stored credential needed
    // (unprotected key) ---
    SavedSite site;
    site.id = QStringLiteral("verify-script-runner-live-site");
    site.name = QStringLiteral("verify-script-runner-live-site");
    site.protocol = Protocol::Sftp;
    site.host = creds.host;
    site.port = creds.port;
    site.username = creds.username;
    site.authMethod = SftpAuthMethod::PublicKey;
    site.privateKeyPath = creds.privateKeyPath;
    check("phase 2: SavedSite written", SiteStore::save({site}));

    // --- Phase 3: a real ScriptRunner, driven through the real `open`
    // command against the now-trusted site ---
    QString parseErr;
    const auto commands = parseScript(
        QStringLiteral("open %1\nls\nexit\n").arg(site.name), &parseErr);
    check("phase 3: script parses", parseErr.isEmpty() && !commands.isEmpty());

    auto *runner = new ScriptRunner(&app);
    int outputLines = 0;
    QObject::connect(runner, &ScriptRunner::output, &app, [&](const QString &line) {
        outputLines++;
        qDebug() << "[script output]" << line;
    });

    int exitCode = -1;
    QObject::connect(runner, &ScriptRunner::finished, &app, [&](int code) {
        exitCode = code;
        qApp->quit();
    });

    QTimer::singleShot(0, &app, [&]() { runner->run(commands); });
    QTimer::singleShot(15000, &app, [&]() { qApp->quit(); });   // safety net
    app.exec();

    check("phase 3: open (nullptr HostKeyVerifier) connected to an ALREADY-TRUSTED host "
          "with no prompt and no hang", exitCode == 0);
    check("phase 3: ls produced real directory output over the real connection", outputLines > 0);

    // `open` leaves its backend running on its own worker thread —
    // FilePaneWidget's own destructor normally tears this down when a
    // real MainWindow closes, but this harness never constructs one.
    // Without an explicit quit()+wait() here, QThread's destructor
    // aborts the process ("Destroyed while thread is still running") as
    // the QApplication tears everything down at exit — a real cleanup
    // gap in this harness, not in ScriptRunner/the shipped app.
    if (FilePaneWidget *remotePane = runner->remotePaneForTesting()) {
        if (RemoteBackend *remoteBackend = remotePane->backend()) {
            if (QThread *workerThread = remoteBackend->thread(); workerThread != qApp->thread()) {
                workerThread->quit();
                workerThread->wait();
            }
        }
    }

    qDebug() << (allPass ? "[test] ALL PASS" : "[test] AT LEAST ONE FAILURE");
    return allPass ? 0 : 1;
}
