#include <QApplication>
#include <QFile>
#include <QTextStream>
#include <QDebug>
#include <QIcon>
#include <QCommandLineParser>
#include <QCommandLineOption>
#include <QTimer>
#include "ui/MainWindow.h"
#include "cli/ScriptParser.h"
#include "cli/ScriptRunner.h"

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QApplication::setApplicationName("ZephyrFTP");
    QApplication::setOrganizationName("Bad Cluster");

    QCommandLineParser parser;
    parser.setApplicationDescription(
        "ZephyrFTP - dual-pane SFTP/FTP/FTPS client. Run with no options to launch the GUI.");
    parser.addHelpOption();
    const QCommandLineOption scriptOption(
        QStringList{"script"},
        "Run an automation script non-interactively and exit — see CONTRIBUTING.md/README.md's "
        "\"Scripting\" section for the command set. A site referenced via 'open <name>' must "
        "already be trusted (connected once via the GUI) and have a stored credential "
        "(Site Manager's \"Save password\" checkbox) — scripts never prompt.",
        "path");
    parser.addOption(scriptOption);
    parser.process(app);

    // Script mode: no window is ever shown, output goes to stdout/stderr,
    // and the process exits with the script's own result code (0 = ran
    // to completion, 1 = couldn't read/parse the script file, 2 = a
    // command failed at runtime — see ScriptRunner::finished's own doc
    // comment). Everything below this branch is the ordinary GUI launch,
    // byte-identical to before this flag existed.
    if (parser.isSet(scriptOption)) {
        const QString scriptPath = parser.value(scriptOption);
        QFile scriptFile(scriptPath);
        if (!scriptFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
            QTextStream(stderr) << "error: could not open script file: " << scriptPath << Qt::endl;
            return 1;
        }
        const QString scriptText = QTextStream(&scriptFile).readAll();

        QString parseError;
        const QList<ScriptCommand> commands = parseScript(scriptText, &parseError);
        if (!parseError.isEmpty()) {
            QTextStream(stderr) << "error: " << parseError << Qt::endl;
            return 1;
        }

        auto *runner = new ScriptRunner(&app);
        QObject::connect(runner, &ScriptRunner::output, &app, [](const QString &line) {
            QTextStream(stdout) << line << Qt::endl;
        });
        QObject::connect(runner, &ScriptRunner::finished, &app, [](int code) {
            qApp->exit(code);
        });
        // Kicked off once the event loop is actually running — every
        // async signal ScriptRunner waits on needs a live event loop to
        // ever fire, the same "start after exec(), not before" pattern
        // this project's own test suite already relies on throughout.
        QTimer::singleShot(0, &app, [runner, commands]() {
            runner->run(commands);
        });

        return QApplication::exec();
    }

    // Multi-resolution window/taskbar icon — Qt picks whichever size fits
    // the context (title bar, Alt-Tab, taskbar) rather than scaling one
    // image. The Windows .exe's own native icon (File Explorer, taskbar
    // before launch) is separate — see resources/app-icon.rc.
    QIcon appIcon;
    for (int size : {16, 24, 32, 48, 64, 128, 256}) {
        appIcon.addFile(QStringLiteral(":/icons/app-icon-%1.png").arg(size), QSize(size, size));
    }
    QApplication::setWindowIcon(appIcon);

    // Dark theme ported from the design package's assets/zephyr-theme.css
    // (see resources/theme.qss for the token-by-token mapping notes).
    // Failure here is non-fatal — the app just falls back to Qt's default
    // platform style — so this doesn't need to be more than a warning.
    QFile themeFile(":/theme/theme.qss");
    if (themeFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        app.setStyleSheet(QTextStream(&themeFile).readAll());
    } else {
        qWarning("Could not load theme.qss from resources — falling back to default style");
    }

    MainWindow window;
    window.show();

    return QApplication::exec();
}
