// Headless functional test of automation scripting — ScriptParser's
// parsing correctness, and ScriptRunner's command execution against a
// real LocalBackend-backed remote pane (via attachRemotePaneForTesting(),
// skipping the open command's SiteStore/CredentialStore/network path —
// see ScriptRunner.h's own doc comment for why: this sandbox can't
// reliably guarantee a real OS keyring session inside a CI container,
// the same reasoning verify-credential-store is a live-only target
// rather than part of the required suite). A separate,
// verify-script-runner-live (EXCLUDE_FROM_ALL, not part of this file)
// covers the real `open` path against a real local sshd.
//
// Three phases:
//  1. Parsing — comments/blank lines/quoted args/unknown commands/wrong
//     argument counts, entirely synchronous, no event loop needed.
//  2. Command execution — lcd/cd/put/get/ls/lls/rm/mkdir/mv against two
//     real local directories.
//  3. mirror --delete end-to-end, specifically proving the completion
//     barrier: every assertion below runs from ScriptRunner's own
//     finished() handler, so if the barrier didn't actually wait for
//     every transfer/delete to settle before emitting finished(), these
//     assertions would see a stale, incomplete destination tree and
//     fail — not "eventually pass after a delay this test doesn't wait
//     for."
#include <QApplication>
#include <QTimer>
#include <QDebug>
#include <QDir>
#include <QFile>
#include "cli/ScriptParser.h"
#include "cli/ScriptRunner.h"
#include "backends/LocalBackend.h"

namespace {

bool g_allPass = true;

void check(const QString &label, bool condition)
{
    qDebug() << (condition ? "[PASS]" : "[FAIL]") << label;
    if (!condition)
        g_allPass = false;
}

void writeFile(const QString &path, const QString &content)
{
    QFile f(path);
    f.open(QIODevice::WriteOnly | QIODevice::Truncate);
    f.write(content.toUtf8());
}

QString readFile(const QString &path)
{
    QFile f(path);
    f.open(QIODevice::ReadOnly);
    return QString::fromUtf8(f.readAll());
}

void testParsing()
{
    // Comments, blank lines, and a normal command are all handled
    // correctly together — the common case a script actually looks like.
    {
        QString err;
        const auto commands = parseScript(
            "# a comment\n"
            "\n"
            "echo hello world\n"
            "  \n"
            "exit\n", &err);
        check("parsing: comments/blank lines skipped, 2 real commands remain", commands.size() == 2);
        check("parsing: no error on valid script", err.isEmpty());
        if (commands.size() == 2) {
            check("parsing: echo command type correct", commands.at(0).type == ScriptCommandType::Echo);
            check("parsing: echo joined multi-word args into one", commands.at(0).args.size() == 1
                  && commands.at(0).args.at(0) == "hello world");
            check("parsing: exit command type correct", commands.at(1).type == ScriptCommandType::Exit);
        }
    }

    // Quoted arguments with embedded spaces.
    {
        QString err;
        const auto commands = parseScript("lcd \"/path/with spaces/dir\"\n", &err);
        check("parsing: quoted arg with spaces kept as one token",
              commands.size() == 1 && commands.at(0).args.size() == 1
              && commands.at(0).args.at(0) == "/path/with spaces/dir");
    }

    // Unterminated quote is a parse error, not a silent misparse.
    {
        QString err;
        const auto commands = parseScript("lcd \"unterminated\n", &err);
        check("parsing: unterminated quote produces empty command list", commands.isEmpty());
        check("parsing: unterminated quote produces a non-empty error message", !err.isEmpty());
    }

    // Unknown command — nothing parses, not just the bad line skipped.
    {
        QString err;
        const auto commands = parseScript("cd /tmp\nbogus_command foo\nexit\n", &err);
        check("parsing: unknown command rejects the WHOLE script", commands.isEmpty());
        check("parsing: unknown command error message names it", err.contains("bogus_command"));
    }

    // Wrong argument count.
    {
        QString err;
        const auto commands = parseScript("mv onlyonearg\n", &err);
        check("parsing: wrong argument count is a parse error", commands.isEmpty());
        check("parsing: wrong argument count error mentions the command", err.contains("mv"));
    }

    // mirror's optional third argument must be exactly "--delete".
    {
        QString err;
        const auto commands = parseScript("mirror /a /b --wrong\n", &err);
        check("parsing: mirror's bad third argument is rejected at parse time", commands.isEmpty());
        check("parsing: mirror's bad third argument error mentions --delete", err.contains("--delete"));
    }
    {
        QString err;
        const auto commands = parseScript("mirror /a /b --delete\n", &err);
        check("parsing: mirror with a valid --delete parses", commands.size() == 1 && err.isEmpty());
    }
}

} // namespace

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);

    testParsing();

    const QString base = QStringLiteral("/tmp/script_runner_test");
    QDir(base).removeRecursively();

    // --- Phase 2 fixtures: lcd/cd/put/get/ls/lls/rm/mkdir/mv ---
    const QString localDir = base + "/p2_local";
    const QString remoteDir = base + "/p2_remote";
    QDir().mkpath(localDir);
    QDir().mkpath(remoteDir);
    writeFile(localDir + "/upload.txt", "put me");
    writeFile(remoteDir + "/download.txt", "get me");
    writeFile(remoteDir + "/tomove.txt", "move me");
    writeFile(remoteDir + "/todelete.txt", "delete me");

    QString parseErr;
    const auto phase2Commands = parseScript(
        QStringLiteral("lcd %1\n"
                        "cd %2\n"
                        "put upload.txt\n"
                        "get download.txt\n"
                        "mkdir newdir\n"
                        "mv tomove.txt renamed.txt\n"
                        "rm todelete.txt\n"
                        "ls\n"
                        "lls\n"
                        "exit\n").arg(localDir, remoteDir),
        &parseErr);
    check("phase 2 script parses cleanly", parseErr.isEmpty() && !phase2Commands.isEmpty());

    auto *phase2Runner = new ScriptRunner(&app);
    int phase2OutputLines = 0;
    QObject::connect(phase2Runner, &ScriptRunner::output, &app, [&](const QString &) {
        phase2OutputLines++;
    });
    phase2Runner->attachRemotePaneForTesting(new LocalBackend());

    // --- Phase 3 fixtures: mirror --delete ---
    const QString mirrorLocal = base + "/p3_local";
    const QString mirrorRemote = base + "/p3_remote";
    QDir().mkpath(mirrorLocal + "/sub");
    QDir().mkpath(mirrorRemote);
    writeFile(mirrorLocal + "/one.txt", "file one");
    writeFile(mirrorLocal + "/sub/two.txt", "file two");
    writeFile(mirrorRemote + "/extra.txt", "stale, should be deleted");

    const auto phase3Commands = parseScript(
        QStringLiteral("mirror %1 %2 --delete\nexit\n").arg(mirrorLocal, mirrorRemote), &parseErr);
    check("phase 3 script parses cleanly", parseErr.isEmpty() && !phase3Commands.isEmpty());

    QObject::connect(phase2Runner, &ScriptRunner::finished, &app, [&](int code) {
        check("phase 2: exit code 0 (all commands succeeded)", code == 0);
        check("phase 2: ls/lls produced at least some output", phase2OutputLines > 0);

        check("phase 2: put copied upload.txt to remote with correct content",
              readFile(remoteDir + "/upload.txt") == "put me");
        check("phase 2: get copied download.txt to local with correct content",
              readFile(localDir + "/download.txt") == "get me");
        check("phase 2: mkdir created newdir on remote", QDir(remoteDir + "/newdir").exists());
        check("phase 2: mv renamed tomove.txt to renamed.txt",
              !QFile::exists(remoteDir + "/tomove.txt") && QFile::exists(remoteDir + "/renamed.txt"));
        check("phase 2: rm deleted todelete.txt", !QFile::exists(remoteDir + "/todelete.txt"));

        auto *phase3Runner = new ScriptRunner(&app);
        phase3Runner->attachRemotePaneForTesting(new LocalBackend());
        QObject::connect(phase3Runner, &ScriptRunner::finished, &app, [&](int code3) {
            // Every check below runs synchronously inside this handler —
            // see this file's own header comment on why that's the actual
            // proof the completion barrier works, not an assumption.
            check("phase 3: exit code 0", code3 == 0);
            check("phase 3: mirror copied one.txt",
                  readFile(mirrorRemote + "/one.txt") == "file one");
            check("phase 3: mirror copied sub/two.txt (nested directory created first)",
                  readFile(mirrorRemote + "/sub/two.txt") == "file two");
            check("phase 3: mirror --delete removed extra.txt not present at source",
                  !QFile::exists(mirrorRemote + "/extra.txt"));

            qDebug() << (g_allPass ? "[test] ALL PASS" : "[test] AT LEAST ONE FAILURE");
            app.exit(g_allPass ? 0 : 1);
        });
        phase3Runner->run(phase3Commands);
    });

    QTimer::singleShot(0, &app, [&]() {
        phase2Runner->run(phase2Commands);
    });

    return app.exec();
}
