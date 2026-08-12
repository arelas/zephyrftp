#include "PermissionsDialog.h"

#include <QGridLayout>
#include <QVBoxLayout>
#include <QCheckBox>
#include <QLabel>
#include <QDialogButtonBox>

int permissionsStringToMode(const QString &nineChars)
{
    if (nineChars.size() != 9)
        return 0;

    int mode = 0;
    if (nineChars[0] == QLatin1Char('r')) mode |= 0400;
    if (nineChars[1] == QLatin1Char('w')) mode |= 0200;
    if (nineChars[2] == QLatin1Char('x')) mode |= 0100;
    if (nineChars[3] == QLatin1Char('r')) mode |= 0040;
    if (nineChars[4] == QLatin1Char('w')) mode |= 0020;
    if (nineChars[5] == QLatin1Char('x')) mode |= 0010;
    if (nineChars[6] == QLatin1Char('r')) mode |= 0004;
    if (nineChars[7] == QLatin1Char('w')) mode |= 0002;
    if (nineChars[8] == QLatin1Char('x')) mode |= 0001;
    return mode;
}

QString modeToPermissionsString(int mode)
{
    // Always exactly 4 digits (a leading 0, then owner/group/other) —
    // matching the conventional chmod-dialog octal readout every
    // mainstream GUI client shows.
    return QStringLiteral("0%1").arg(QString::number(mode & 0777, 8), 3, QLatin1Char('0'));
}

PermissionsDialog::PermissionsDialog(const RemoteEntry &entry, QWidget *parent)
    : QDialog(parent)
    , m_ownerRead(new QCheckBox(this))
    , m_ownerWrite(new QCheckBox(this))
    , m_ownerExecute(new QCheckBox(this))
    , m_groupRead(new QCheckBox(this))
    , m_groupWrite(new QCheckBox(this))
    , m_groupExecute(new QCheckBox(this))
    , m_otherRead(new QCheckBox(this))
    , m_otherWrite(new QCheckBox(this))
    , m_otherExecute(new QCheckBox(this))
    , m_modeLabel(new QLabel(this))
{
    setWindowTitle(tr("Permissions — %1").arg(entry.name));

    const int initialMode = permissionsStringToMode(entry.permissions);
    m_ownerRead->setChecked(initialMode & 0400);
    m_ownerWrite->setChecked(initialMode & 0200);
    m_ownerExecute->setChecked(initialMode & 0100);
    m_groupRead->setChecked(initialMode & 0040);
    m_groupWrite->setChecked(initialMode & 0020);
    m_groupExecute->setChecked(initialMode & 0010);
    m_otherRead->setChecked(initialMode & 0004);
    m_otherWrite->setChecked(initialMode & 0002);
    m_otherExecute->setChecked(initialMode & 0001);

    // A directory's "execute" bit actually means "can list/traverse
    // into it", not "run it" — same real-world meaning, different
    // enough from a file's that it's worth a one-line hint rather than
    // a separate label scheme for the whole dialog.
    if (entry.isDir) {
        const QString hint = tr("For a folder, this means \"can list/enter it\", not \"run it\".");
        m_ownerExecute->setToolTip(hint);
        m_groupExecute->setToolTip(hint);
        m_otherExecute->setToolTip(hint);
    }

    auto *grid = new QGridLayout;
    grid->addWidget(new QLabel(tr("Read"), this), 0, 1);
    grid->addWidget(new QLabel(tr("Write"), this), 0, 2);
    grid->addWidget(new QLabel(entry.isDir ? tr("List/Enter") : tr("Execute"), this), 0, 3);

    grid->addWidget(new QLabel(tr("Owner"), this), 1, 0);
    grid->addWidget(m_ownerRead, 1, 1);
    grid->addWidget(m_ownerWrite, 1, 2);
    grid->addWidget(m_ownerExecute, 1, 3);

    grid->addWidget(new QLabel(tr("Group"), this), 2, 0);
    grid->addWidget(m_groupRead, 2, 1);
    grid->addWidget(m_groupWrite, 2, 2);
    grid->addWidget(m_groupExecute, 2, 3);

    grid->addWidget(new QLabel(tr("Other"), this), 3, 0);
    grid->addWidget(m_otherRead, 3, 1);
    grid->addWidget(m_otherWrite, 3, 2);
    grid->addWidget(m_otherExecute, 3, 3);

    // Live octal readout, same convention WinSCP/FileZilla's own
    // permissions dialogs already show — updated on every checkbox
    // toggle rather than only when the dialog is accepted, so it's
    // never stale while it's on screen.
    for (QCheckBox *box : {m_ownerRead, m_ownerWrite, m_ownerExecute, m_groupRead, m_groupWrite,
                            m_groupExecute, m_otherRead, m_otherWrite, m_otherExecute}) {
        connect(box, &QCheckBox::toggled, this, &PermissionsDialog::updateModeLabel);
    }
    updateModeLabel();

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto *layout = new QVBoxLayout(this);
    layout->addLayout(grid);
    layout->addWidget(m_modeLabel);
    layout->addWidget(buttons);
}

int PermissionsDialog::mode() const
{
    int mode = 0;
    if (m_ownerRead->isChecked())    mode |= 0400;
    if (m_ownerWrite->isChecked())   mode |= 0200;
    if (m_ownerExecute->isChecked()) mode |= 0100;
    if (m_groupRead->isChecked())    mode |= 0040;
    if (m_groupWrite->isChecked())   mode |= 0020;
    if (m_groupExecute->isChecked()) mode |= 0010;
    if (m_otherRead->isChecked())    mode |= 0004;
    if (m_otherWrite->isChecked())   mode |= 0002;
    if (m_otherExecute->isChecked()) mode |= 0001;
    return mode;
}

void PermissionsDialog::updateModeLabel()
{
    m_modeLabel->setText(tr("Mode: %1").arg(modeToPermissionsString(mode())));
}
