#include "ScriptParser.h"

#include <QHash>

namespace {

// Whitespace-split with "quoted argument" support. Returns false (via
// *ok) on an unterminated quote — the only tokenization-level failure.
QStringList tokenizeLine(const QString &line, bool *ok)
{
    QStringList tokens;
    const int n = line.size();
    int i = 0;
    while (i < n) {
        while (i < n && line[i].isSpace())
            ++i;
        if (i >= n)
            break;

        QString token;
        if (line[i] == QLatin1Char('"')) {
            ++i;
            const int start = i;
            while (i < n && line[i] != QLatin1Char('"'))
                ++i;
            if (i >= n) {
                *ok = false;
                return {};
            }
            token = line.mid(start, i - start);
            ++i;   // skip closing quote
        } else {
            const int start = i;
            while (i < n && !line[i].isSpace())
                ++i;
            token = line.mid(start, i - start);
        }
        tokens.append(token);
    }
    *ok = true;
    return tokens;
}

// Every command word this parser recognizes, and how many arguments
// (after the command word) it accepts. Echo is intentionally not
// listed here — it accepts any number of arguments, including zero,
// checked separately below.
struct CommandSpec {
    ScriptCommandType type;
    int minArgs;
    int maxArgs;
};

bool lookupCommand(const QString &word, CommandSpec *spec)
{
    static const QHash<QString, CommandSpec> table = {
        {QStringLiteral("open"),   {ScriptCommandType::Open, 1, 1}},
        {QStringLiteral("cd"),     {ScriptCommandType::Cd, 1, 1}},
        {QStringLiteral("lcd"),    {ScriptCommandType::Lcd, 1, 1}},
        {QStringLiteral("get"),    {ScriptCommandType::Get, 1, 1}},
        {QStringLiteral("put"),    {ScriptCommandType::Put, 1, 1}},
        {QStringLiteral("ls"),     {ScriptCommandType::Ls, 0, 0}},
        {QStringLiteral("lls"),    {ScriptCommandType::Lls, 0, 0}},
        {QStringLiteral("rm"),     {ScriptCommandType::Rm, 1, 1}},
        {QStringLiteral("mkdir"),  {ScriptCommandType::Mkdir, 1, 1}},
        {QStringLiteral("mv"),     {ScriptCommandType::Mv, 2, 2}},
        {QStringLiteral("mirror"), {ScriptCommandType::Mirror, 2, 3}},
        {QStringLiteral("exit"),   {ScriptCommandType::Exit, 0, 0}},
        {QStringLiteral("bye"),    {ScriptCommandType::Exit, 0, 0}},
    };
    const auto it = table.find(word);
    if (it == table.end())
        return false;
    *spec = it.value();
    return true;
}

} // namespace

QList<ScriptCommand> parseScript(const QString &text, QString *errorMessage)
{
    QList<ScriptCommand> commands;
    const QStringList lines = text.split(QLatin1Char('\n'));

    for (int i = 0; i < lines.size(); ++i) {
        const QString rawLine = lines.at(i);
        const QString trimmed = rawLine.trimmed();
        if (trimmed.isEmpty() || trimmed.startsWith(QLatin1Char('#')))
            continue;

        bool tokOk = false;
        QStringList tokens = tokenizeLine(trimmed, &tokOk);
        if (!tokOk) {
            if (errorMessage)
                *errorMessage = QStringLiteral("line %1: unterminated quoted argument").arg(i + 1);
            return {};
        }
        if (tokens.isEmpty())
            continue;

        const QString commandWord = tokens.takeFirst().toLower();

        if (commandWord == QStringLiteral("echo")) {
            ScriptCommand cmd;
            cmd.type = ScriptCommandType::Echo;
            cmd.args = {tokens.join(QLatin1Char(' '))};
            cmd.lineNumber = i + 1;
            cmd.rawLine = rawLine;
            commands.append(cmd);
            continue;
        }

        CommandSpec spec{};
        if (!lookupCommand(commandWord, &spec)) {
            if (errorMessage)
                *errorMessage = QStringLiteral("line %1: unknown command '%2'").arg(i + 1).arg(commandWord);
            return {};
        }
        if (tokens.size() < spec.minArgs || tokens.size() > spec.maxArgs) {
            if (errorMessage) {
                *errorMessage = QStringLiteral("line %1: '%2' takes %3 argument(s), got %4")
                    .arg(i + 1).arg(commandWord).arg(spec.minArgs).arg(tokens.size());
            }
            return {};
        }
        // mirror's optional 3rd argument is only ever meaningful as the
        // literal flag "--delete" — caught here at parse time rather
        // than left to surface as a confusing runtime error.
        if (spec.type == ScriptCommandType::Mirror && tokens.size() == 3
            && tokens.at(2) != QStringLiteral("--delete")) {
            if (errorMessage) {
                *errorMessage = QStringLiteral("line %1: mirror's third argument must be \"--delete\", got \"%2\"")
                    .arg(i + 1).arg(tokens.at(2));
            }
            return {};
        }

        ScriptCommand cmd;
        cmd.type = spec.type;
        cmd.args = tokens;
        cmd.lineNumber = i + 1;
        cmd.rawLine = rawLine;
        commands.append(cmd);
    }

    if (errorMessage)
        errorMessage->clear();
    return commands;
}
