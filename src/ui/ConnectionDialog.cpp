#include "ConnectionDialog.h"

#include <QFormLayout>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QSpinBox>
#include <QRadioButton>
#include <QButtonGroup>
#include <QStackedWidget>
#include <QPushButton>
#include <QFileDialog>
#include <QDialogButtonBox>
#include <QWidget>

ConnectionDialog::ConnectionDialog(QWidget *parent)
    : QDialog(parent)
    , m_hostEdit(new QLineEdit(this))
    , m_portSpin(new QSpinBox(this))
    , m_usernameEdit(new QLineEdit(this))
    , m_passwordAuthRadio(new QRadioButton(tr("Password"), this))
    , m_keyAuthRadio(new QRadioButton(tr("Private key"), this))
    , m_authFieldsStack(new QStackedWidget(this))
    , m_passwordEdit(new QLineEdit(this))
    , m_privateKeyPathEdit(new QLineEdit(this))
    , m_passphraseEdit(new QLineEdit(this))
{
    setWindowTitle(tr("Connect to SFTP Server"));

    m_portSpin->setRange(1, 65535);
    m_portSpin->setValue(22);
    m_passwordEdit->setEchoMode(QLineEdit::Password);
    m_passphraseEdit->setEchoMode(QLineEdit::Password);
    m_passphraseEdit->setPlaceholderText(tr("(leave blank if the key has no passphrase)"));

    auto *authGroup = new QButtonGroup(this);
    authGroup->addButton(m_passwordAuthRadio);
    authGroup->addButton(m_keyAuthRadio);
    m_passwordAuthRadio->setChecked(true);
    connect(m_passwordAuthRadio, &QRadioButton::toggled, this, &ConnectionDialog::updateAuthFieldsVisibility);

    auto *authRadioLayout = new QHBoxLayout;
    authRadioLayout->addWidget(m_passwordAuthRadio);
    authRadioLayout->addWidget(m_keyAuthRadio);
    authRadioLayout->addStretch();

    // Page 0: password field alone.
    auto *passwordPage = new QWidget(this);
    auto *passwordPageLayout = new QFormLayout(passwordPage);
    passwordPageLayout->setContentsMargins(0, 0, 0, 0);
    passwordPageLayout->addRow(tr("Password:"), m_passwordEdit);

    // Page 1: private key path (with Browse…) + optional passphrase.
    auto *keyPage = new QWidget(this);
    auto *keyPathRow = new QHBoxLayout;
    auto *browseButton = new QPushButton(tr("Browse..."), keyPage);
    keyPathRow->addWidget(m_privateKeyPathEdit);
    keyPathRow->addWidget(browseButton);
    connect(browseButton, &QPushButton::clicked, this, &ConnectionDialog::browseForPrivateKey);

    auto *keyPageLayout = new QFormLayout(keyPage);
    keyPageLayout->setContentsMargins(0, 0, 0, 0);
    keyPageLayout->addRow(tr("Private key file:"), keyPathRow);
    keyPageLayout->addRow(tr("Passphrase:"), m_passphraseEdit);

    m_authFieldsStack->addWidget(passwordPage);   // index 0
    m_authFieldsStack->addWidget(keyPage);         // index 1

    auto *form = new QFormLayout;
    form->addRow(tr("Host:"), m_hostEdit);
    form->addRow(tr("Port:"), m_portSpin);
    form->addRow(tr("Username:"), m_usernameEdit);
    form->addRow(tr("Authentication:"), authRadioLayout);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto *layout = new QVBoxLayout(this);
    layout->addLayout(form);
    layout->addWidget(m_authFieldsStack);
    layout->addWidget(buttons);

    updateAuthFieldsVisibility();
    m_hostEdit->setFocus();
}

void ConnectionDialog::updateAuthFieldsVisibility()
{
    m_authFieldsStack->setCurrentIndex(m_passwordAuthRadio->isChecked() ? 0 : 1);
}

void ConnectionDialog::browseForPrivateKey()
{
    const QString path = QFileDialog::getOpenFileName(this, tr("Select Private Key File"));
    if (!path.isEmpty())
        m_privateKeyPathEdit->setText(path);
}

SftpCredentials ConnectionDialog::credentials() const
{
    SftpCredentials creds;
    creds.host = m_hostEdit->text().trimmed();
    creds.port = m_portSpin->value();
    creds.username = m_usernameEdit->text();

    if (m_passwordAuthRadio->isChecked()) {
        creds.authMethod = SftpAuthMethod::Password;
        creds.password = m_passwordEdit->text();
    } else {
        creds.authMethod = SftpAuthMethod::PublicKey;
        creds.privateKeyPath = m_privateKeyPathEdit->text().trimmed();
        creds.passphrase = m_passphraseEdit->text();
    }

    return creds;
}
