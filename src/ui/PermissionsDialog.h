#pragma once

#include <QDialog>
#include "../backends/RemoteEntry.h"

class QCheckBox;
class QLabel;

// Parses a RemoteEntry::permissions display string ("rwxr-xr-x") back
// into raw POSIX permission bits — the first time anywhere in this
// codebase that string has ever been parsed rather than just shown
// (RemoteEntry.h documents it as "display only, not parsed back").
// Malformed input (wrong length, unrecognized characters — including
// FTP's own "-" placeholder, which every FtpBackend listing produces
// today since neither its MLSD nor LIST parser translates real bits)
// returns 0 (all bits unset) rather than guessing: an honest "we don't
// know the current mode" starting point for the dialog below, since
// chmod is an absolute set, not an incremental change, so there's
// nothing lost by starting from nothing when the real value isn't
// known.
//
// Exposed as free functions, not private PermissionsDialog internals,
// specifically so they're testable without constructing a real QDialog.
int permissionsStringToMode(const QString &nineChars);

// The inverse — a live "0755"-style octal readout as the dialog's
// checkboxes change. Always exactly 4 digits (a leading 0, then the
// three octal digits for owner/group/other), matching the conventional
// chmod-dialog display every mainstream GUI client uses.
QString modeToPermissionsString(int mode);

// A small modal dialog for setting a single entry's Unix permission
// bits — the standard 9 rwxrwxrwx bits only, no setuid/setgid/sticky
// (a rare, advanced, high-consequence-if-fat-fingered tier every
// mainstream GUI chmod dialog also keeps separate). Same convention
// ConnectionDialog already establishes: a real QDialog subclass with a
// public getter (mode()) the caller reads after exec() == Accepted,
// not a signal-based result.
class PermissionsDialog : public QDialog {
    Q_OBJECT
public:
    explicit PermissionsDialog(const RemoteEntry &entry, QWidget *parent = nullptr);

    int mode() const;

private:
    void updateModeLabel();

    QCheckBox *m_ownerRead;
    QCheckBox *m_ownerWrite;
    QCheckBox *m_ownerExecute;
    QCheckBox *m_groupRead;
    QCheckBox *m_groupWrite;
    QCheckBox *m_groupExecute;
    QCheckBox *m_otherRead;
    QCheckBox *m_otherWrite;
    QCheckBox *m_otherExecute;
    QLabel *m_modeLabel;
};
