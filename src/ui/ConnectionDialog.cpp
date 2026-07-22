#include "ConnectionDialog.h"

#include <QFormLayout>
#include <QVBoxLayout>
#include <QLineEdit>
#include <QSpinBox>
#include <QDialogButtonBox>

ConnectionDialog::ConnectionDialog(QWidget *parent)
    : QDialog(parent)
    , m_hostEdit(new QLineEdit(this))
    , m_portSpin(new QSpinBox(this))
    , m_usernameEdit(new QLineEdit(this))
    , m_passwordEdit(new QLineEdit(this))
{
    setWindowTitle(tr("Connect to SFTP Server"));

    m_portSpin->setRange(1, 65535);
    m_portSpin->setValue(22);
    m_passwordEdit->setEchoMode(QLineEdit::Password);

    auto *form = new QFormLayout;
    form->addRow(tr("Host:"), m_hostEdit);
    form->addRow(tr("Port:"), m_portSpin);
    form->addRow(tr("Username:"), m_usernameEdit);
    form->addRow(tr("Password:"), m_passwordEdit);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto *layout = new QVBoxLayout(this);
    layout->addLayout(form);
    layout->addWidget(buttons);

    m_hostEdit->setFocus();
}

QString ConnectionDialog::host() const { return m_hostEdit->text().trimmed(); }
int ConnectionDialog::port() const { return m_portSpin->value(); }
QString ConnectionDialog::username() const { return m_usernameEdit->text(); }
QString ConnectionDialog::password() const { return m_passwordEdit->text(); }
