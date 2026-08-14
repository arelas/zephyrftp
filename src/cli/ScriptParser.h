#pragma once

#include <QList>
#include <QString>
#include "ScriptCommand.h"

// Parses a line-oriented automation script into a flat command list —
// a pure, free function, deliberately not touching Qt GUI/Network at
// all, so it's testable in isolation from ScriptRunner's execution.
//
// Format: one command per line, '#' starts a comment (whole line
// ignored), blank lines ignored. Arguments are whitespace-separated;
// "double-quoted arguments" may contain spaces. No variable expansion,
// no globbing, no line continuation, no escape sequences inside quotes
// — deliberately minimal, matching this feature's v1 scope.
//
// Returns an empty list and sets *errorMessage (if non-null) on any
// parse failure (unknown command, wrong argument count, unterminated
// quote) — nothing partially parses and runs; a script either fully
// parses or nothing in it ever executes. errorMessage is cleared (set
// to an empty string) on success, only ever null when the caller has
// no use for a message at all.
QList<ScriptCommand> parseScript(const QString &text, QString *errorMessage);
