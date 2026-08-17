// Verifies the protocol-selection wiring added when FTP/FTPS was
// connected to the UI: that picking a protocol in ConnectionDialog
// actually produces the right ConnectionRequest, that the port and
// auth-field rules behave, and that SavedSite persists the choice
// (including reading back files written before the field existed).
//
// WHY THIS TEST EXISTS: the FTP backend was already written and its
// parsers already tested, but none of it was reachable — nothing
// constructed an FtpBackend. The wiring is where the actual risk moved:
// a protocol tag that silently defaults to SFTP, a password that lands
// in the wrong struct, or a saved FTP site that reads back as SFTP would
// all compile perfectly and fail only against a live server, which this
// environment doesn't have. So the wiring itself is what's tested here.
//
// WHAT THIS TEST DOES NOT COVER: any actual network behavior. It never
// opens a socket. Constructing an FtpBackend is verified only to the
// extent that MainWindow's switch would pick it — the connection,
// AUTH TLS, and every FTP command remain unverified against a real
// server, exactly as ARCHITECTURE.md's Known gaps says.
#include <QApplication>
#include <QDebug>
#include <QSpinBox>
#include <QAbstractSpinBox>
#include <QRadioButton>
#include <QLineEdit>
#include <QWidget>
#include <QSize>
#include <QFile>
#include <QDir>
#include <QStandardPaths>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>

#include "ui/ConnectionDialog.h"
#include "backends/SavedSite.h"
#include "backends/ConnectionRequest.h"
#include "backends/QuickConnectParser.h"

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    // Redirects AppConfigLocation to a throwaway `qttest` subdirectory so
    // the SavedSite persistence checks below can never touch a real
    // user's actual sites.json — Qt's own cross-platform mechanism for
    // this, not the XDG_CONFIG_HOME env var this used to rely on. A real
    // bug found running on native Windows (not just CI's wine
    // emulation): XDG_CONFIG_HOME is Linux/XDG-only and Qt's native
    // Win32 QStandardPaths implementation never consults it, so that
    // "isolation" silently did nothing there — see site-store-test's own
    // header comment for the full detail.
    QStandardPaths::setTestModeEnabled(true);

    bool allPass = true;
    auto check = [&](const QString &label, bool condition) {
        qDebug() << (condition ? "[PASS]" : "[FAIL]") << label;
        if (!condition) allPass = false;
    };

    // ===================================================================
    // ConnectionRequest::missingRequiredPrivateKeyPath() — shared by
    // MainWindow::connectViaDialog() and SiteManagerDialog::
    // onConnectClicked(), extracted after code review found the identical
    // three-part condition duplicated at both call sites. Checked
    // directly against the struct rather than through either dialog,
    // since it's a pure function of ConnectionRequest's own fields.
    // ===================================================================
    {
        ConnectionRequest sftpKeyNoPath;
        sftpKeyNoPath.protocol = Protocol::Sftp;
        sftpKeyNoPath.sftp.authMethod = SftpAuthMethod::PublicKey;
        check("SFTP + key auth + empty path: missing",
              sftpKeyNoPath.missingRequiredPrivateKeyPath());

        ConnectionRequest sftpKeyWithPath;
        sftpKeyWithPath.protocol = Protocol::Sftp;
        sftpKeyWithPath.sftp.authMethod = SftpAuthMethod::PublicKey;
        sftpKeyWithPath.sftp.privateKeyPath = QStringLiteral("/home/dave/.ssh/id_ed25519");
        check("SFTP + key auth + a real path: not missing",
              !sftpKeyWithPath.missingRequiredPrivateKeyPath());

        ConnectionRequest sftpPassword;
        sftpPassword.protocol = Protocol::Sftp;
        sftpPassword.sftp.authMethod = SftpAuthMethod::Password;
        check("SFTP + password auth (no key involved at all): not missing",
              !sftpPassword.missingRequiredPrivateKeyPath());

        ConnectionRequest ftpRequest;
        ftpRequest.protocol = Protocol::Ftp;
        check("plain FTP (privateKeyPath meaningless for this protocol): not missing",
              !ftpRequest.missingRequiredPrivateKeyPath());
    }

    // ===================================================================
    // ConnectionDialog: protocol drives port, auth visibility, and the
    // shape of the resulting ConnectionRequest.
    // ===================================================================
    {
        ConnectionDialog dialog;
        check("default protocol is SFTP", dialog.protocol() == Protocol::Sftp);

        auto *portSpin = dialog.findChild<QSpinBox *>(QStringLiteral("portSpin"));
        check("port spin box found", portSpin != nullptr);
        if (portSpin) {
            check("default port is 22 for SFTP", portSpin->value() == 22);
            // Deliberate: a port is typed, not nudged. Guards against a
            // silent revert, which nothing else in this suite would catch.
            check("port field has no up/down arrows",
                  portSpin->buttonSymbols() == QAbstractSpinBox::NoButtons);
        }

        // --- switching to FTP ---
        dialog.setProtocol(Protocol::Ftp);
        check("protocol reads back as FTP after setProtocol", dialog.protocol() == Protocol::Ftp);
        if (portSpin)
            check("port auto-updates to 21 on switch to FTP", portSpin->value() == 21);

        // --- switching to FTPS: still port 21 (explicit FTPS, not 990) ---
        dialog.setProtocol(Protocol::Ftps);
        if (portSpin)
            check("port is 21 for explicit FTPS (not implicit-FTPS 990)", portSpin->value() == 21);

        // --- switching to implicit FTPS: 990, its own conventional port ---
        dialog.setProtocol(Protocol::FtpsImplicit);
        check("protocol reads back as FtpsImplicit after setProtocol",
              dialog.protocol() == Protocol::FtpsImplicit);
        if (portSpin)
            check("port auto-updates to 990 on switch to implicit FTPS", portSpin->value() == 990);

        // --- and back to SFTP: the port must follow, not stay stuck at 990 ---
        dialog.setProtocol(Protocol::Sftp);
        if (portSpin)
            check("port returns to 22 when switching back to SFTP", portSpin->value() == 22);
    }

    // ===================================================================
    // A deliberately-chosen port must survive a protocol switch. This is
    // the case that would be a real, silent data-loss bug: typing 2222
    // and then changing protocol must not quietly reset it.
    // ===================================================================
    {
        ConnectionDialog dialog;
        auto *portSpin = dialog.findChild<QSpinBox *>(QStringLiteral("portSpin"));
        if (portSpin) {
            portSpin->setValue(2222);
            dialog.setProtocol(Protocol::Ftp);
            check("a user-typed port (2222) is NOT overwritten by a protocol switch",
                  portSpin->value() == 2222);
        }
    }

    // ===================================================================
    // Regression: a deliberately-typed port that happens to ALIAS with
    // FTP/FTPS's shared default (21) must also survive a round-trip
    // switch — a real data-loss bug found by code review.
    // portIsUntouchedDefault() used to infer "the user hasn't typed
    // their own port" by checking the current value against ALL THREE
    // protocols' defaults (22/21/21), so a genuinely user-typed 21 under
    // SFTP was indistinguishable from FTP/FTPS's own auto-set 21:
    // SFTP(22) -> type 21 -> switch to FTP (still 21, no visible change,
    // "confirmed" as untouched) -> switch back to SFTP silently reset it
    // to 22, discarding the deliberate choice. Fixed by tracking real
    // user edits directly via a dirty flag instead of guessing from the
    // value alone.
    // ===================================================================
    {
        ConnectionDialog dialog;
        auto *portSpin = dialog.findChild<QSpinBox *>(QStringLiteral("portSpin"));
        if (portSpin) {
            check("starts on SFTP's default port (22), matching FTP/FTPS's later aliasing setup",
                  portSpin->value() == 22);
            portSpin->setValue(21);   // deliberately typed — happens to equal FTP/FTPS's own default
            dialog.setProtocol(Protocol::Ftp);
            check("still 21 after switching to FTP (no visible change either way at this step)",
                  portSpin->value() == 21);
            dialog.setProtocol(Protocol::Sftp);
            check("a deliberately-typed port that ALIASES with FTP/FTPS's shared default (21) "
                  "still survives switching back to SFTP, not silently reset to 22",
                  portSpin->value() == 21);
        }
    }

    // ===================================================================
    // Regression: switching Protocol on an ALREADY-SHOWN dialog must
    // actually resize the window when the Authentication row appears/
    // disappears, not just correctly hide the fields while leaving dead
    // space behind — the original real bug (found via a screenshot pass,
    // not caught by any automated test before this). Re-verified here
    // since code review replaced the fix's mechanism (QFormLayout::
    // setRowVisible() instead of the original manual setVisible() +
    // layout()->activate() + adjustSize() dance) — this confirms the
    // simplification didn't quietly regress what it replaced.
    // ===================================================================
    {
        ConnectionDialog dialog;
        dialog.show();
        const QSize sftpSize = dialog.size();

        dialog.setProtocol(Protocol::Ftp);
        const QSize ftpSize = dialog.size();
        check("switching to FTP on an already-shown dialog actually shrinks it "
              "(the Authentication row disappears) on the very first switch",
              ftpSize.height() < sftpSize.height());

        dialog.setProtocol(Protocol::Sftp);
        const QSize backToSftpSize = dialog.size();
        check("switching back to SFTP restores the original (taller) size",
              backToSftpSize.height() == sftpSize.height());
    }

    // ===================================================================
    // The request produced for each protocol carries the right payload in
    // the right struct — the mistake that would compile fine and fail
    // only at connect time.
    // ===================================================================
    {
        ConnectionDialog dialog;
        auto *hostEdit = dialog.findChild<QLineEdit *>(QStringLiteral("hostEdit"));
        check("host field found", hostEdit != nullptr);

        if (hostEdit) {
            hostEdit->setText(QStringLiteral("ftp.example.com"));

            dialog.setProtocol(Protocol::Ftps);
            const ConnectionRequest ftpsRequest = dialog.connectionRequest();
            check("FTPS request is tagged Ftps", ftpsRequest.protocol == Protocol::Ftps);
            check("FTPS request populates the FTP credential struct, not the SFTP one",
                  ftpsRequest.ftp.host == "ftp.example.com" && ftpsRequest.sftp.host.isEmpty());
            check("FTPS request sets FtpsMode::Explicit",
                  ftpsRequest.ftp.ftpsMode == FtpsMode::Explicit);
            check("host() accessor reads through to the FTP struct",
                  ftpsRequest.host() == "ftp.example.com");

            dialog.setProtocol(Protocol::Ftp);
            const ConnectionRequest ftpRequest = dialog.connectionRequest();
            check("plain FTP sets FtpsMode::None (no accidental TLS claim)",
                  ftpRequest.ftp.ftpsMode == FtpsMode::None);

            dialog.setProtocol(Protocol::FtpsImplicit);
            const ConnectionRequest implicitRequest = dialog.connectionRequest();
            check("implicit FTPS request is tagged FtpsImplicit",
                  implicitRequest.protocol == Protocol::FtpsImplicit);
            check("implicit FTPS request populates the FTP credential struct, not the SFTP one",
                  implicitRequest.ftp.host == "ftp.example.com" && implicitRequest.sftp.host.isEmpty());
            check("implicit FTPS request sets FtpsMode::Implicit (not Explicit)",
                  implicitRequest.ftp.ftpsMode == FtpsMode::Implicit);

            dialog.setProtocol(Protocol::Sftp);
            const ConnectionRequest sftpRequest = dialog.connectionRequest();
            check("SFTP request populates the SFTP credential struct, not the FTP one",
                  sftpRequest.sftp.host == "ftp.example.com" && sftpRequest.ftp.host.isEmpty());
        }
    }

    // ===================================================================
    // Key auth is SFTP-only. Selecting "Private key" and then switching
    // to FTP must not leave a key-auth request behind for a protocol that
    // has no such concept.
    //
    // NOTE ON THE VISIBILITY ASSERTIONS: isVisible() is useless here — on
    // a dialog that is never shown it returns false for every widget, so
    // a "hidden for FTP" check written with it passes even if the code
    // hides nothing at all. That exact vacuous assertion was in the first
    // draft of this test and was caught by probing both branches and
    // finding they returned the same value. isVisibleTo(&dialog) answers
    // the question actually being asked — "would this be visible if the
    // dialog were shown" — and genuinely differs between the two cases.
    // Both the positive and negative case are asserted deliberately: a
    // one-sided check can't distinguish "correctly hidden" from
    // "always hidden".
    // ===================================================================
    {
        ConnectionDialog dialog;
        auto *keyRadio = dialog.findChild<QRadioButton *>(QStringLiteral("keyAuthRadio"));
        auto *keyPathEdit = dialog.findChild<QLineEdit *>(QStringLiteral("privateKeyPathEdit"));
        auto *hostEdit = dialog.findChild<QLineEdit *>(QStringLiteral("hostEdit"));
        check("private-key radio found", keyRadio != nullptr);
        check("private-key path field found", keyPathEdit != nullptr);

        if (keyRadio && keyPathEdit && hostEdit) {
            hostEdit->setText(QStringLiteral("sftp.example.com"));
            keyRadio->setChecked(true);
            keyPathEdit->setText(QStringLiteral("/home/dave/.ssh/id_ed25519"));

            check("key auth selectable under SFTP", keyRadio->isChecked());
            check("the auth row IS visible under SFTP", keyRadio->isVisibleTo(&dialog));

            // Confirm the key path really is carried while still on SFTP,
            // so the post-switch assertion below can't pass merely because
            // the field was never populated.
            const ConnectionRequest sftpRequest = dialog.connectionRequest();
            check("SFTP request carries the private key path",
                  sftpRequest.sftp.privateKeyPath == "/home/dave/.ssh/id_ed25519");

            dialog.setProtocol(Protocol::Ftp);
            check("switching to FTP clears the key-auth selection", !keyRadio->isChecked());
            check("the auth row is NOT visible under FTP", !keyRadio->isVisibleTo(&dialog));

            const ConnectionRequest ftpRequest = dialog.connectionRequest();
            check("an FTP request carries no private key path anywhere",
                  ftpRequest.sftp.privateKeyPath.isEmpty());
            check("an FTP request is not tagged as SFTP", ftpRequest.protocol != Protocol::Sftp);
            check("supportsKeyAuth() agrees FTP has no key auth", !supportsKeyAuth(Protocol::Ftp));
        }
    }

    // ===================================================================
    // SavedSite: protocol survives a real JSON round-trip through
    // SiteStore, and toConnectionRequest() maps it to the right struct.
    // ===================================================================
    {
        SavedSite ftpsSite;
        ftpsSite.id = QStringLiteral("id-ftps");
        ftpsSite.name = QStringLiteral("An FTPS site");
        ftpsSite.protocol = Protocol::Ftps;
        ftpsSite.host = QStringLiteral("ftps.example.com");
        ftpsSite.port = 21;
        ftpsSite.username = QStringLiteral("alice");

        SavedSite implicitSite;
        implicitSite.id = QStringLiteral("id-ftps-implicit");
        implicitSite.name = QStringLiteral("An implicit-FTPS site");
        implicitSite.protocol = Protocol::FtpsImplicit;
        implicitSite.host = QStringLiteral("implicit.example.com");
        implicitSite.port = 990;
        implicitSite.username = QStringLiteral("eve");

        SavedSite sftpSite;
        sftpSite.id = QStringLiteral("id-sftp");
        sftpSite.name = QStringLiteral("An SFTP site");
        sftpSite.protocol = Protocol::Sftp;
        sftpSite.host = QStringLiteral("sftp.example.com");
        sftpSite.port = 22;
        sftpSite.username = QStringLiteral("bob");

        const bool saved = SiteStore::save({ftpsSite, implicitSite, sftpSite});
        check("SiteStore::save() succeeded", saved);

        const QList<SavedSite> loaded = SiteStore::load();
        check("all three sites loaded back", loaded.size() == 3);
        if (loaded.size() == 3) {
            check("FTPS site's protocol survived the round-trip",
                  loaded[0].protocol == Protocol::Ftps);
            check("implicit-FTPS site's protocol survived the round-trip (not misread as explicit)",
                  loaded[1].protocol == Protocol::FtpsImplicit);
            check("SFTP site's protocol survived the round-trip",
                  loaded[2].protocol == Protocol::Sftp);
        }

        // The mapping into a ConnectionRequest.
        const ConnectionRequest ftpsRequest = ftpsSite.toConnectionRequest();
        check("a saved FTPS site maps into the FTP credential struct",
              ftpsRequest.protocol == Protocol::Ftps
              && ftpsRequest.ftp.host == "ftps.example.com"
              && ftpsRequest.ftp.ftpsMode == FtpsMode::Explicit);
        check("a saved FTPS site's password is still empty after mapping "
              "(no-secrets-on-disk rule holds for FTP too)",
              ftpsRequest.ftp.password.isEmpty());

        const ConnectionRequest implicitRequest = implicitSite.toConnectionRequest();
        check("a saved implicit-FTPS site maps into the FTP credential struct with FtpsMode::Implicit",
              implicitRequest.protocol == Protocol::FtpsImplicit
              && implicitRequest.ftp.host == "implicit.example.com"
              && implicitRequest.ftp.ftpsMode == FtpsMode::Implicit);
        check("a saved implicit-FTPS site's password is still empty after mapping",
              implicitRequest.ftp.password.isEmpty());
    }

    // ===================================================================
    // Backward compatibility: a sites.json written before `protocol`
    // existed has no such key. It must read back as SFTP — every site
    // saved then was necessarily an SFTP site.
    // ===================================================================
    {
        const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
        QDir().mkpath(dir);
        const QString path = dir + QStringLiteral("/sites.json");

        // Hand-written to match the OLD schema exactly — no "protocol" key.
        QJsonObject legacy;
        legacy[QStringLiteral("id")] = QStringLiteral("legacy-1");
        legacy[QStringLiteral("name")] = QStringLiteral("Pre-FTP site");
        legacy[QStringLiteral("host")] = QStringLiteral("old.example.com");
        legacy[QStringLiteral("port")] = 22;
        legacy[QStringLiteral("username")] = QStringLiteral("carol");
        legacy[QStringLiteral("authMethod")] = QStringLiteral("password");

        QJsonArray array;
        array.append(legacy);

        QFile file(path);
        const bool opened = file.open(QIODevice::WriteOnly | QIODevice::Truncate);
        check("legacy sites.json written for the migration check", opened);
        if (opened) {
            file.write(QJsonDocument(array).toJson());
            file.close();

            const QList<SavedSite> loaded = SiteStore::load();
            check("legacy site still loads", loaded.size() == 1);
            if (loaded.size() == 1) {
                check("a site saved before `protocol` existed reads back as SFTP",
                      loaded[0].protocol == Protocol::Sftp);
                check("the rest of the legacy site is intact",
                      loaded[0].host == "old.example.com" && loaded[0].username == "carol");
            }
        }
    }

    // ===================================================================
    // The security property, restated for FTP specifically: an FTP site
    // written to disk must contain no password key. FTP has no key-auth
    // alternative, so this is the only protection there is.
    // ===================================================================
    {
        SavedSite ftpSite;
        ftpSite.id = QStringLiteral("id-ftp-secret");
        ftpSite.name = QStringLiteral("FTP site");
        ftpSite.protocol = Protocol::Ftp;
        ftpSite.host = QStringLiteral("ftp.example.com");
        ftpSite.username = QStringLiteral("dave");
        SiteStore::save({ftpSite});

        const QString path = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation)
            + QStringLiteral("/sites.json");
        QFile file(path);
        if (file.open(QIODevice::ReadOnly)) {
            const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
            bool anySecretKey = false;
            for (const QJsonValue &value : doc.array()) {
                const QJsonObject obj = value.toObject();
                for (const QString &key : obj.keys()) {
                    const QString lowered = key.toLower();
                    if (lowered.contains(QStringLiteral("password"))
                        || lowered.contains(QStringLiteral("passphrase"))
                        || lowered.contains(QStringLiteral("secret"))) {
                        anySecretKey = true;
                    }
                }
            }
            check("a saved FTP site writes no password/passphrase/secret key to disk",
                  !anySecretKey);
        }
    }

    // ===================================================================
    // parseQuickConnectString() — the toolbar quick-connect field's
    // "[protocol://][user@]host[:port]" parser. Pure function, no
    // dialog/widget involved.
    // ===================================================================
    {
        const auto bare = parseQuickConnectString(QStringLiteral("example.com"), Protocol::Sftp);
        check("bare host: ok", bare.ok);
        check("bare host: protocol falls back to the passed-in default",
              bare.protocol == Protocol::Sftp);
        check("bare host: host parsed correctly", bare.host == "example.com");
        check("bare host: port falls back to defaultPortFor(Sftp) == 22", bare.port == 22);
        check("bare host: username is empty", bare.username.isEmpty());

        const auto userHost = parseQuickConnectString(QStringLiteral("alice@example.com"), Protocol::Sftp);
        check("user@host: ok", userHost.ok);
        check("user@host: username parsed", userHost.username == "alice");
        check("user@host: host parsed", userHost.host == "example.com");

        const auto userHostPort = parseQuickConnectString(
            QStringLiteral("alice@example.com:2222"), Protocol::Sftp);
        check("user@host:port: ok", userHostPort.ok);
        check("user@host:port: explicit port used, not the default",
              userHostPort.port == 2222);

        const auto sftpScheme = parseQuickConnectString(
            QStringLiteral("sftp://alice@example.com"), Protocol::Ftp);
        check("sftp:// scheme: ok", sftpScheme.ok);
        check("sftp:// scheme: overrides the passed-in default protocol",
              sftpScheme.protocol == Protocol::Sftp);
        check("sftp:// scheme: default port follows the SCHEME's protocol (22), "
              "not the caller's default (Ftp's 21)", sftpScheme.port == 22);

        const auto ftpScheme = parseQuickConnectString(QStringLiteral("ftp://example.com"), Protocol::Sftp);
        check("ftp:// scheme: ok and protocol overridden", ftpScheme.ok && ftpScheme.protocol == Protocol::Ftp);
        check("ftp:// scheme: default port is 21", ftpScheme.port == 21);

        const auto ftpsScheme = parseQuickConnectString(QStringLiteral("ftps://example.com"), Protocol::Sftp);
        check("ftps:// scheme: ok and protocol overridden",
              ftpsScheme.ok && ftpsScheme.protocol == Protocol::Ftps);
        check("ftps:// scheme: default port is 21 (explicit FTPS, not implicit 990)",
              ftpsScheme.port == 21);

        const auto schemeCaseInsensitive = parseQuickConnectString(
            QStringLiteral("SFTP://example.com"), Protocol::Ftp);
        check("scheme matching is case-insensitive",
              schemeCaseInsensitive.ok && schemeCaseInsensitive.protocol == Protocol::Sftp);

        const auto unknownScheme = parseQuickConnectString(
            QStringLiteral("gopher://example.com"), Protocol::Sftp);
        check("unknown scheme: NOT ok (a clear error, not a silent fallback)", !unknownScheme.ok);
        check("unknown scheme: errorMessage set", !unknownScheme.errorMessage.isEmpty());

        const auto empty = parseQuickConnectString(QString(), Protocol::Sftp);
        check("empty input: not ok", !empty.ok);

        const auto whitespaceOnly = parseQuickConnectString(QStringLiteral("   "), Protocol::Sftp);
        check("whitespace-only input: not ok (trimmed first)", !whitespaceOnly.ok);

        const auto emptyHost = parseQuickConnectString(QStringLiteral("alice@"), Protocol::Sftp);
        check("username with no host: not ok", !emptyHost.ok);

        const auto badPort = parseQuickConnectString(QStringLiteral("example.com:notaport"), Protocol::Sftp);
        check("non-numeric port: not ok", !badPort.ok);

        const auto outOfRangePort = parseQuickConnectString(QStringLiteral("example.com:99999"), Protocol::Sftp);
        check("out-of-range port (99999): not ok", !outOfRangePort.ok);

        const auto zeroPort = parseQuickConnectString(QStringLiteral("example.com:0"), Protocol::Sftp);
        check("port 0: not ok (1-65535 only)", !zeroPort.ok);
    }

    qDebug() << (allPass ? "[test] ALL PASS" : "[test] AT LEAST ONE FAILURE");
    return allPass ? 0 : 1;
}
