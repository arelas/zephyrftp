#include "ConnectionDialog.h"

#include <QFormLayout>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QSpinBox>
#include <QAbstractSpinBox>
#include <QRadioButton>
#include <QButtonGroup>
#include <QStackedWidget>
#include <QPushButton>
#include <QFileDialog>
#include <QDialogButtonBox>
#include <QWidget>
#include <QComboBox>
#include <QLabel>

ConnectionDialog::ConnectionDialog(QWidget *parent)
    : QDialog(parent)
    , m_protocolCombo(new QComboBox(this))
    , m_hostEdit(new QLineEdit(this))
    , m_portSpin(new QSpinBox(this))
    , m_usernameEdit(new QLineEdit(this))
    , m_passwordAuthRadio(new QRadioButton(tr("Password"), this))
    , m_keyAuthRadio(new QRadioButton(tr("Private key"), this))
    , m_authFieldsStack(new QStackedWidget(this))
    , m_passwordEdit(new QLineEdit(this))
    , m_privateKeyPathEdit(new QLineEdit(this))
    , m_passphraseEdit(new QLineEdit(this))
    , m_authRowWidget(nullptr)
    , m_authRowLabel(nullptr)
{
    setWindowTitle(tr("Connect to Server"));

    // Order matters only for presentation; the stored data is the enum,
    // not the index, so reordering here can't corrupt anything.
    m_protocolCombo->addItem(displayNameFor(Protocol::Sftp), QVariant::fromValue(int(Protocol::Sftp)));
    m_protocolCombo->addItem(displayNameFor(Protocol::Ftp),  QVariant::fromValue(int(Protocol::Ftp)));
    m_protocolCombo->addItem(displayNameFor(Protocol::Ftps), QVariant::fromValue(int(Protocol::Ftps)));

    // Object names exist for one reason: tests locate these fields by
    // name rather than by position in findChildren()'s list, which would
    // silently start testing the wrong widget the moment a field is
    // added or reordered.
    m_protocolCombo->setObjectName(QStringLiteral("protocolCombo"));
    m_hostEdit->setObjectName(QStringLiteral("hostEdit"));
    m_portSpin->setObjectName(QStringLiteral("portSpin"));
    m_usernameEdit->setObjectName(QStringLiteral("usernameEdit"));
    m_passwordEdit->setObjectName(QStringLiteral("passwordEdit"));
    m_privateKeyPathEdit->setObjectName(QStringLiteral("privateKeyPathEdit"));
    m_passphraseEdit->setObjectName(QStringLiteral("passphraseEdit"));
    m_passwordAuthRadio->setObjectName(QStringLiteral("passwordAuthRadio"));
    m_keyAuthRadio->setObjectName(QStringLiteral("keyAuthRadio"));

    // No up/down arrows: a port is typed, not nudged one integer at a
    // time, so the buttons were pure visual noise. Kept as a QSpinBox
    // rather than swapped for a QLineEdit specifically to retain what
    // the widget is actually earning its place with — numeric-only
    // input and clamping to the valid 1-65535 range. A QLineEdit would
    // need a QIntValidator bolted on to get back to the same place.
    m_portSpin->setButtonSymbols(QAbstractSpinBox::NoButtons);
    m_portSpin->setRange(1, 65535);
    m_portSpin->setValue(defaultPortFor(Protocol::Sftp));
    m_passwordEdit->setEchoMode(QLineEdit::Password);
    m_passphraseEdit->setEchoMode(QLineEdit::Password);
    m_passphraseEdit->setPlaceholderText(tr("(leave blank if the key has no passphrase)"));

    auto *authGroup = new QButtonGroup(this);
    authGroup->addButton(m_passwordAuthRadio);
    authGroup->addButton(m_keyAuthRadio);
    m_passwordAuthRadio->setChecked(true);
    connect(m_passwordAuthRadio, &QRadioButton::toggled, this, &ConnectionDialog::updateAuthFieldsVisibility);

    // Wrapped in a plain QWidget rather than added as a bare layout so
    // the whole row can be hidden as a unit for FTP/FTPS.
    m_authRowWidget = new QWidget(this);
    m_authRowWidget->setObjectName(QStringLiteral("authRowWidget"));
    auto *authRadioLayout = new QHBoxLayout(m_authRowWidget);
    authRadioLayout->setContentsMargins(0, 0, 0, 0);
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
    form->addRow(tr("Protocol:"), m_protocolCombo);
    form->addRow(tr("Host:"), m_hostEdit);
    form->addRow(tr("Port:"), m_portSpin);
    form->addRow(tr("Username:"), m_usernameEdit);

    m_authRowLabel = new QLabel(tr("Authentication:"), this);
    form->addRow(m_authRowLabel, m_authRowWidget);

    connect(m_protocolCombo, &QComboBox::currentIndexChanged,
            this, &ConnectionDialog::onProtocolChanged);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto *layout = new QVBoxLayout(this);
    layout->addLayout(form);
    layout->addWidget(m_authFieldsStack);
    layout->addWidget(buttons);

    onProtocolChanged();   // establishes the initial (SFTP) field visibility
    m_hostEdit->setFocus();
}

bool ConnectionDialog::portIsUntouchedDefault() const
{
    // "Untouched" means the current value is the default for *any* of the
    // protocols, not just the currently selected one. Checking only the
    // current protocol's default would mean switching SFTP->FTP->SFTP
    // leaves the port stuck at 21, since 21 isn't 22 and would look
    // deliberately chosen on the way back.
    const int current = m_portSpin->value();
    return current == defaultPortFor(Protocol::Sftp)
        || current == defaultPortFor(Protocol::Ftp)
        || current == defaultPortFor(Protocol::Ftps);
}

Protocol ConnectionDialog::protocol() const
{
    return Protocol(m_protocolCombo->currentData().toInt());
}

void ConnectionDialog::setProtocol(Protocol protocol)
{
    const int index = m_protocolCombo->findData(int(protocol));
    if (index >= 0)
        m_protocolCombo->setCurrentIndex(index);
}

void ConnectionDialog::onProtocolChanged()
{
    const Protocol selected = protocol();

    // Only adjust a port the user hasn't deliberately set — see
    // portIsUntouchedDefault().
    if (portIsUntouchedDefault())
        m_portSpin->setValue(defaultPortFor(selected));

    // FTP/FTPS have no key auth, so the choice is hidden rather than
    // shown-and-ignored. Force the selection back to Password on the way
    // out so credentials() can't produce a key-auth request for a
    // protocol that has no such concept, even if the user had picked
    // "Private key" under SFTP a moment earlier.
    const bool keyAuthAvailable = supportsKeyAuth(selected);
    if (!keyAuthAvailable)
        m_passwordAuthRadio->setChecked(true);

    m_authRowWidget->setVisible(keyAuthAvailable);
    m_authRowLabel->setVisible(keyAuthAvailable);

    updateAuthFieldsVisibility();

    // Real bug, found during a systematic dialog-consistency screenshot
    // pass (not user-reported): hiding a QFormLayout row's widgets doesn't
    // shrink the row's own reserved space — a dialog that starts in
    // FTP/FTPS mode looks correctly compact, but switching the Protocol
    // combo on an ALREADY-OPEN dialog (the actual common case: it always
    // opens on SFTP first) left dead space where the Authentication row
    // used to be, with no resize. adjustSize() re-fits the window to its
    // layout's current sizeHint — smaller when switching away from SFTP,
    // larger when switching back — matching what a freshly-opened dialog
    // in that same protocol already looked like.
    //
    // Guarded on isVisible(): this same slot can also run before the
    // dialog's first show() (setProtocol() called by a caller up front,
    // or protocol-selection-test driving it directly) — calling
    // adjustSize() on a not-yet-shown top-level widget computes its
    // sizeHint from not-yet-fully-resolved style metrics and can produce
    // a wrong (larger) size that then sticks, since it also marks the
    // window as already explicitly sized and skips Qt's normal
    // auto-size-on-first-show. Letting the first real show() do that
    // initial sizing itself, untouched, avoids that.
    if (isVisible()) {
        // layout()->activate() forces the outer layout to recompute its
        // cached size hint immediately. Without it, the setVisible() calls
        // above only *invalidate* the layout and post a deferred
        // LayoutRequest event to actually recompute it — so adjustSize()
        // right after, in the same call, would read a stale (pre-hide)
        // size hint. Confirmed directly: the first live protocol switch
        // after opening the dialog silently used the previous size, and
        // only the *next* switch (now reading a hint stale by one step)
        // resized — layout()->activate() makes the very first switch
        // correct too, not just eventually-consistent.
        layout()->activate();
        adjustSize();
    }
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

ConnectionRequest ConnectionDialog::connectionRequest() const
{
    ConnectionRequest request;
    request.protocol = protocol();

    if (request.protocol == Protocol::Sftp) {
        SftpCredentials &creds = request.sftp;
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
    } else {
        FtpCredentials &creds = request.ftp;
        creds.host = m_hostEdit->text().trimmed();
        creds.port = m_portSpin->value();
        creds.username = m_usernameEdit->text();
        creds.password = m_passwordEdit->text();
        creds.ftpsMode = protocolToFtpsMode(request.protocol);
    }

    return request;
}
