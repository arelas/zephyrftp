// Exercises FtpBackend's certificate trust-on-first-use flow against a
// REAL FTPS server — closes the gap flagged in ARCHITECTURE.md: FTPS
// used to fail closed on any unverifiable certificate with no way to
// proceed, unlike SSH host keys, which get a real trust-on-first-use
// prompt. Needs tools/local-test-servers/start-ftps.sh already running
// (self-signed cert — the exact case this whole feature exists for);
// this is NOT one of the ten self-contained EXCLUDE_FROM_ALL test
// targets (it has an external precondition those deliberately don't),
// so it isn't part of that suite or CI.
//
// Three phases, each a fresh FtpBackend/CertificateVerifier pair on
// their own worker thread (same reasoning as verify_sftp_pubkey.cpp:
// CertificateVerifier's blocking cross-thread call needs FtpBackend on
// a DIFFERENT thread than the one servicing that call, or it deadlocks):
// (1) no stored fingerprint yet — the trust prompt fires, auto-accepted
// (same "poll for the active modal, click its button" technique
// verify_sftp_pubkey.cpp already uses for the SSH host-key prompt), and
// the connection + a real listing succeed. (2) a fresh connection to
// the same host:port — must succeed with NO prompt at all, proving the
// accepted fingerprint was actually persisted and reused. (3) the
// stored fingerprint is directly overwritten with a deliberately wrong
// value (simpler and fully deterministic than regenerating the
// server's actual certificate mid-test) — the mismatch warning must
// fire, and declining it (auto-clicked here) must leave the connection
// failed closed, same as a declined SSH host-key change.
//
// Run with:
//   tools/local-test-servers/start-ftps.sh
//   cmake --build build --target verify-ftps-trust
//   QT_QPA_PLATFORM=offscreen ./build/verify-ftps-trust
#include <QApplication>
#include <QTimer>
#include <QThread>
#include <QEventLoop>
#include <QMessageBox>
#include <QAbstractButton>
#include <QFile>
#include <QStandardPaths>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QSslConfiguration>
#include <cstdio>
#include <memory>
#include "ui/CertificateVerifier.h"
#include "backends/FtpBackend.h"
#include "backends/FtpCredentials.h"

namespace {
bool allPass = true;

void check(const char *label, bool condition)
{
    fprintf(stderr, "[%s] %s\n", condition ? "PASS" : "FAIL", label);
    if (!condition) allPass = false;
}

const char *kHost = "127.0.0.1";
constexpr int kPort = 2122;

QString knownCertsPath()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation)
        + QStringLiteral("/known_certs.json");
}

// Removes any stored fingerprint for kHost:kPort, so phase 1 genuinely
// starts from "never seen this certificate before" rather than
// depending on whatever a previous run (or the real app) left behind.
void clearStoredFingerprint()
{
    QJsonArray kept;
    QFile file(knownCertsPath());
    if (file.open(QIODevice::ReadOnly)) {
        const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
        file.close();
        if (doc.isArray()) {
            for (const QJsonValue &value : doc.array()) {
                const QJsonObject obj = value.toObject();
                if (obj.value(QStringLiteral("host")).toString() == QLatin1String(kHost)
                    && obj.value(QStringLiteral("port")).toInt() == kPort) {
                    continue;
                }
                kept.append(obj);
            }
        }
    }
    QFile writeFile(knownCertsPath());
    if (writeFile.open(QIODevice::WriteOnly | QIODevice::Truncate))
        writeFile.write(QJsonDocument(kept).toJson());
}

// Overwrites (or creates) the stored fingerprint for kHost:kPort with a
// value that can never match a real certificate — deterministically
// forces the "certificate changed" mismatch path without needing to
// regenerate the server's actual certificate mid-test.
void corruptStoredFingerprint()
{
    QJsonArray updated;
    QFile readFile(knownCertsPath());
    if (readFile.open(QIODevice::ReadOnly)) {
        const QJsonDocument doc = QJsonDocument::fromJson(readFile.readAll());
        if (doc.isArray()) {
            for (const QJsonValue &value : doc.array()) {
                const QJsonObject obj = value.toObject();
                if (obj.value(QStringLiteral("host")).toString() == QLatin1String(kHost)
                    && obj.value(QStringLiteral("port")).toInt() == kPort) {
                    continue;
                }
                updated.append(obj);
            }
        }
    }
    QJsonObject bogus;
    bogus[QStringLiteral("host")] = QLatin1String(kHost);
    bogus[QStringLiteral("port")] = kPort;
    bogus[QStringLiteral("fingerprint")] =
        QStringLiteral("0000000000000000000000000000000000000000000000000000000000000000");
    updated.append(bogus);

    QFile writeFile(knownCertsPath());
    if (writeFile.open(QIODevice::WriteOnly | QIODevice::Truncate))
        writeFile.write(QJsonDocument(updated).toJson());
}

// Polls for the active modal QMessageBox and clicks the button with the
// given role — same technique as verify_sftp_pubkey.cpp's
// autoAcceptHostKeyPrompt(), generalized to also support declining
// (phase 3's mismatch-decline case) and BOUNDED to one phase's lifetime:
// phase 2 legitimately shows no prompt at all, which means the original
// unbounded version of this (reschedule forever until a box appears)
// would keep polling — and keep being "the" active poller — well past
// that phase's own runConnectionAttempt() returning, using a role meant
// for a phase that had already finished. That stale, still-armed poller
// then raced the NEXT phase's own (correctly configured) one and could
// win, clicking the wrong button on a completely different phase's
// prompt. Caught by actually running all three phases together, not by
// reasoning about the polling loop in isolation. `active` is flipped
// false by runConnectionAttempt() the moment ITS OWN loop.exec()
// returns, so any leftover poll tied to a finished phase becomes a
// harmless no-op instead of reaching across into the next one.
struct PromptWatcher {
    QMessageBox::ButtonRole role = QMessageBox::YesRole;
    bool sawPrompt = false;
    bool active = true;
};

void pollForCertificatePrompt(const std::shared_ptr<PromptWatcher> &watcher)
{
    QTimer::singleShot(50, qApp, [watcher] {
        if (!watcher->active)
            return;   // owning phase already finished — nothing left to answer
        auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
        if (box) {
            watcher->sawPrompt = true;
            for (QAbstractButton *button : box->buttons()) {
                if (box->buttonRole(button) == watcher->role) {
                    button->click();
                    return;
                }
            }
            return;
        }
        pollForCertificatePrompt(watcher);
    });
}

struct PhaseResult {
    bool connected = false;
    bool listed = false;
    QString failureReason;
    bool sawPrompt = false;
};

// Runs one full connect (+ a real listing on success) against the
// self-signed FTPS test server, with a FRESH FtpBackend/CertificateVerifier
// pair each time — deliberately not reusing state between phases except
// through known_certs.json itself, the same way two separate real app
// launches would only share state through that file.
PhaseResult runConnectionAttempt(QMessageBox::ButtonRole promptResponse)
{
    PhaseResult result;

    FtpCredentials creds;
    creds.host = QLatin1String(kHost);
    creds.port = kPort;
    creds.username = QStringLiteral("ftpsuser");
    creds.password = QStringLiteral("ftpspass");
    creds.ftpsMode = FtpsMode::Explicit;

    auto *verifier = new CertificateVerifier();
    auto *backend = new FtpBackend(creds, verifier);
    auto *thread = new QThread();
    backend->moveToThread(thread);

    QEventLoop loop;
    QObject::connect(backend, &RemoteBackend::connectionFailed, &loop,
        [&](const QString &reason) { result.failureReason = reason; loop.quit(); });
    QObject::connect(backend, &RemoteBackend::connected, &loop, [&]() {
        result.connected = true;
        QMetaObject::invokeMethod(backend, "listDirectory", Qt::QueuedConnection,
                                   Q_ARG(QString, QStringLiteral(".")));
    });
    QObject::connect(backend, &RemoteBackend::directoryListed, &loop,
        [&](const QString &, const QList<RemoteEntry> &) {
            result.listed = true;
            loop.quit();
        });

    QTimer::singleShot(15000, &loop, [&]() {
        if (result.failureReason.isEmpty())
            result.failureReason = QStringLiteral("timed out after 15s");
        loop.quit();
    });

    auto watcher = std::make_shared<PromptWatcher>();
    watcher->role = promptResponse;

    thread->start();
    pollForCertificatePrompt(watcher);
    QMetaObject::invokeMethod(backend, "connectToHost", Qt::QueuedConnection);
    loop.exec();

    // Done with this phase — any poll still in flight (there legitimately
    // is one whenever no prompt ever appears, e.g. phase 2) must not go
    // on answering prompts that belong to a LATER phase. See
    // PromptWatcher's own comment for why this matters.
    watcher->active = false;
    result.sawPrompt = watcher->sawPrompt;

    backend->deleteLater();
    thread->quit();
    thread->wait();
    delete thread;
    delete verifier;

    return result;
}
}

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    // Same reasoning as verify_sftp_pubkey.cpp: no QMainWindow is ever
    // shown, the trust dialog is the only top-level window this process
    // displays, and quitOnLastWindowClosed's default would quit the
    // instant it's answered — well before the connection actually
    // finishes.
    app.setQuitOnLastWindowClosed(false);

    // Each phase below reconnects to the SAME host:port within this one
    // process — without this, Qt/OpenSSL's normal TLS session
    // resumption silently reuses an earlier phase's already-validated
    // session on a later connection, which means the certificate is
    // never freshly re-validated and sslErrors() never fires at all —
    // caught by actually running phase 3 and finding it silently
    // "succeeded" with no prompt, not by reasoning about it in advance.
    // This is purely how a real client's session cache behaves and
    // isn't specific to FtpBackend; disabling it here is test-harness
    // determinism (so phase 3 gets the fresh handshake a genuinely
    // later reconnection would also get, once any real session ticket
    // has long since expired), not a change to how the app itself
    // negotiates TLS.
    QSslConfiguration noResumeConfig = QSslConfiguration::defaultConfiguration();
    noResumeConfig.setSslOption(QSsl::SslOptionDisableSessionTickets, true);
    noResumeConfig.setSslOption(QSsl::SslOptionDisableSessionSharing, true);
    noResumeConfig.setSslOption(QSsl::SslOptionDisableSessionPersistence, true);
    QSslConfiguration::setDefaultConfiguration(noResumeConfig);

    clearStoredFingerprint();

    // --- Phase 1: first-ever sighting, accept the prompt ---
    const PhaseResult phase1 = runConnectionAttempt(QMessageBox::YesRole);
    check("[phase 1] trust prompt actually appeared (first-ever sighting of this certificate)",
          phase1.sawPrompt);
    check("[phase 1] accepting the prompt connects successfully", phase1.connected);
    check("[phase 1] a real directory listing succeeds after accepting", phase1.listed);
    if (!phase1.failureReason.isEmpty())
        fprintf(stderr, "[phase 1] failure reason: %s\n", qPrintable(phase1.failureReason));

    // --- Phase 2: reconnect, fingerprint already trusted — no prompt ---
    const PhaseResult phase2 = runConnectionAttempt(QMessageBox::YesRole);
    check("[phase 2] NO prompt on a second connection to the same, unchanged certificate",
          !phase2.sawPrompt);
    check("[phase 2] connects silently using the persisted trust decision", phase2.connected);
    check("[phase 2] a real directory listing succeeds", phase2.listed);

    // --- Phase 3: stored fingerprint corrupted — mismatch warning, decline it ---
    corruptStoredFingerprint();
    const PhaseResult phase3 = runConnectionAttempt(QMessageBox::NoRole);
    check("[phase 3] mismatch warning fires when the stored fingerprint no longer matches",
          phase3.sawPrompt);
    check("[phase 3] declining the mismatch warning fails the connection closed",
          !phase3.connected && !phase3.failureReason.isEmpty());
    if (!phase3.failureReason.isEmpty())
        fprintf(stderr, "[phase 3] failure reason: %s\n", qPrintable(phase3.failureReason));

    fprintf(stderr, "%s\n", allPass ? "ALL PASS" : "AT LEAST ONE FAILURE");
    return allPass ? 0 : 1;
}
