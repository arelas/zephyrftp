#pragma once

#include <QString>
#include <QStringList>

// One parsed line of an automation script (see ScriptParser.h). Plain
// data — ScriptRunner is the only thing that interprets `type`/`args`;
// this struct doesn't know what any command actually does.
enum class ScriptCommandType {
    Open,    // open <site-name>
    Cd,      // cd <remote-path>
    Lcd,     // lcd <local-path>
    // TransferManager::enqueue() uses ONE filename for both source and
    // destination (see its own doc comment) — no rename-during-transfer
    // support exists anywhere in this codebase today, so neither does
    // this: the file lands under the same name in the other side's
    // current directory. Use `lcd`/`cd` to control which directory, and
    // `mv` after the fact for a rename.
    Get,     // get <remote-file>
    Put,     // put <local-file>
    Ls,      // ls
    Lls,     // lls
    Rm,      // rm <path>
    Mkdir,   // mkdir <path>
    Mv,      // mv <old> <new>
    Mirror,  // mirror <local-dir> <remote-dir> [--delete]
    Echo,    // echo <text...>
    Exit,    // exit | bye
};

struct ScriptCommand {
    ScriptCommandType type;
    QStringList args;

    // For actionable parse/runtime error messages ("line 7: ...").
    int lineNumber = 0;
    QString rawLine;
};
