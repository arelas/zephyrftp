#include "ConnectionDialog.h"
#include "FileDialogs.h"

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
#include <QSignalBlocker>

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
    // See m_portManuallyEdited's own doc comment. Connected AFTER the
    // setValue() above (which has no subscriber yet regardless) so this
    // never fires for that one; every LATER programmatic setValue() call
    // (onProtocolChanged()'s own) is wrapped in a QSignalBlocker
    // specifically so only genuine user edits ever reach here.
    connect(m_portSpin, QOverload<int>::of(&QSpinBox::valueChanged), this, [this]() {
        m_portManuallyEdited = true;
    });
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

    m_form = new QFormLayout;
    m_form->addRow(tr("Protocol:"), m_protocolCombo);
    m_form->addRow(tr("Host:"), m_hostEdit);
    m_form->addRow(tr("Port:"), m_portSpin);
    m_form->addRow(tr("Username:"), m_usernameEdit);

    m_authRowLabel = new QLabel(tr("Authentication:"), this);
    m_form->addRow(m_authRowLabel, m_authRowWidget);

    connect(m_protocolCombo, &QComboBox::currentIndexChanged,
            this, &ConnectionDialog::onProtocolChanged);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto *layout = new QVBoxLayout(this);
    layout->addLayout(m_form);
    layout->addWidget(m_authFieldsStack);
    layout->addWidget(buttons);

    onProtocolChanged();   // establishes the initial (SFTP) field visibility
    m_hostEdit->setFocus();
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
    // m_portManuallyEdited's own doc comment. Blocked so this own
    // programmatic change can't be mistaken for a user edit.
    if (!m_portManuallyEdited) {
        const QSignalBlocker blocker(m_portSpin);
        m_portSpin->setValue(defaultPortFor(selected));
    }

    // FTP/FTPS have no key auth, so the choice is hidden rather than
    // shown-and-ignored. Force the selection back to Password on the way
    // out so credentials() can't produce a key-auth request for a
    // protocol that has no such concept, even if the user had picked
    // "Private key" under SFTP a moment earlier.
    const bool keyAuthAvailable = supportsKeyAuth(selected);
    if (!keyAuthAvailable) {
        // Blocked, matching SiteManagerDialog's identical forced-Password
        // reset — a real inconsistency found by code review. Harmless
        // today (updateAuthFieldsVisibility() below is idempotent), but
        // without this, setChecked(true) here actually changing state
        // synchronously emits toggled(true) and runs that slot a second
        // time via the signal, on top of the explicit call three lines
        // down — silent double-firing waiting to matter the moment either
        // slot stops being idempotent.
        const QSignalBlocker blocker(m_passwordAuthRadio);
        m_passwordAuthRadio->setChecked(true);
    }

    // A real simplification found by code review: this used to hide
    // m_authRowWidget/m_authRowLabel individually via plain setVisible(),
    // which doesn't shrink the row's own reserved space in the
    // surrounding QFormLayout — fixed at the time (see the coming
    // isVisible()/adjustSize() block's history) with a manual
    // layout()->activate() to force synchronous recomputation before
    // adjustSize() could read a stale size hint. QFormLayout::
    // setRowVisible() (Qt 5.15+, and this project requires Qt6) hides
    // BOTH the row's label and field as a unit and — confirmed directly
    // with a standalone probe — already updates the layout's size hint
    // synchronously, correctly resizing on the very first switch with
    // just an adjustSize() after it, no manual activate() needed.
    m_form->setRowVisible(m_authRowWidget, keyAuthAvailable);

    updateAuthFieldsVisibility();

    // Real bug, found during a systematic dialog-consistency screenshot
    // pass (not user-reported): a dialog that starts in FTP/FTPS mode
    // looks correctly compact, but switching the Protocol combo on an
    // ALREADY-OPEN dialog (the actual common case: it always opens on
    // SFTP first) left dead space where the Authentication row used to
    // be, with no resize. adjustSize() re-fits the window to its layout's
    // current sizeHint — smaller when switching away from SFTP, larger
    // when switching back — matching what a freshly-opened dialog in
    // that same protocol already looked like.
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
    if (isVisible())
        adjustSize();
}

void ConnectionDialog::updateAuthFieldsVisibility()
{
    m_authFieldsStack->setCurrentIndex(m_passwordAuthRadio->isChecked() ? 0 : 1);
}

void ConnectionDialog::browseForPrivateKey()
{
    const QString path = FileDialogs::pickPrivateKeyFile(this);
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
        // Trimmed like host above — a real bug found by code review:
        // pasting a username with invisible leading/trailing whitespace
        // (common when copying from a credentials email or spreadsheet)
        // used to connect fine (host was already trimmed) but fail auth
        // with a generic wrong-username/password error, nothing hinting
        // the username field itself carried the stray whitespace.
        creds.username = m_usernameEdit->text().trimmed();

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
        // Trimmed like host above — a real bug found by code review:
        // pasting a username with invisible leading/trailing whitespace
        // (common when copying from a credentials email or spreadsheet)
        // used to connect fine (host was already trimmed) but fail auth
        // with a generic wrong-username/password error, nothing hinting
        // the username field itself carried the stray whitespace.
        creds.username = m_usernameEdit->text().trimmed();
        creds.password = m_passwordEdit->text();
        creds.ftpsMode = protocolToFtpsMode(request.protocol);
    }

    return request;
}
