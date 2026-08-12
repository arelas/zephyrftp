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

// Export/import destination pickers for SiteManagerDialog's Export.../
// Import... buttons — the first callers of getSaveFileName() in this
// codebase. Deliberately given a filename filter and (for export) a
// suggested default name, unlike pickPrivateKeyFile() above's bare
// filter-less call: a key file can be anything, but an exported site
// list is always this app's own JSON schema, so a filter genuinely
// helps here.
inline QString pickSitesExportFile(QWidget *parent)
{
    return QFileDialog::getSaveFileName(parent, QObject::tr("Export Sites"),
        QStringLiteral("zephyrftp-sites.json"), QObject::tr("JSON files (*.json);;All files (*)"));
}

inline QString pickSitesImportFile(QWidget *parent)
{
    return QFileDialog::getOpenFileName(parent, QObject::tr("Import Sites"),
        QString(), QObject::tr("JSON files (*.json);;All files (*)"));
}

}
