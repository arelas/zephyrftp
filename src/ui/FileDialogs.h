#pragma once

#include <QFileDialog>
#include <QWidget>
#include <QString>

// Small shared helpers for file-picker dialogs used identically from more
// than one place in the UI layer — kept header-only since each is a
// one-line wrapper, not because a .cpp would be wrong.
namespace FileDialogs {

// Prompts for a private key file; returns the picked path, or an empty
// string if the user cancelled. Shared by ConnectionDialog and
// SiteManagerDialog's "Browse..." buttons — a real duplication found by
// code review. Returns the raw path rather than taking a QLineEdit* to
// fill in directly: the two callers' follow-up behavior had already
// started to drift (SiteManagerDialog also calls its own onFieldEdited()
// after setting the text, which ConnectionDialog has no equivalent of),
// so this only unifies the part that was genuinely identical — the
// QFileDialog call itself — and leaves each caller free to do its own
// thing with the result.
inline QString pickPrivateKeyFile(QWidget *parent)
{
    return QFileDialog::getOpenFileName(parent, QObject::tr("Select Private Key File"));
}

}
