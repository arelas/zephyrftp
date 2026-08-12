#include "PreferencesDialog.h"
#include "../AppSettings.h"
#include "../backends/Protocol.h"
#include "../backends/ProxyConfig.h"
#include "ProtocolCombo.h"

#include <QFormLayout>
#include <QVBoxLayout>
#include <QCheckBox>
#include <QComboBox>
#include <QLineEdit>
#include <QSpinBox>
#include <QLabel>
#include <QDialogButtonBox>

PreferencesDialog::PreferencesDialog(AppSettings *settings, QWidget *parent)
    : QDialog(parent)
    , m_settings(settings)
    , m_showHiddenFilesCheck(new QCheckBox(tr("Show hidden files (dotfiles)"), this))
    , m_defaultProtocolCombo(new QComboBox(this))
    , m_externalEditorCommandEdit(new QLineEdit(this))
    , m_proxyTypeCombo(new QComboBox(this))
    , m_proxyHostEdit(new QLineEdit(this))
    , m_proxyPortSpin(new QSpinBox(this))
    , m_proxyUsernameEdit(new QLineEdit(this))
    , m_proxyPasswordEdit(new QLineEdit(this))
{
    setWindowTitle(tr("Preferences"));

    m_showHiddenFilesCheck->setChecked(m_settings->showHiddenFiles());
    connect(m_showHiddenFilesCheck, &QCheckBox::toggled, this, [this](bool checked) {
        m_settings->setShowHiddenFiles(checked);
    });

    ProtocolCombo::populate(m_defaultProtocolCombo);
    const int currentIndex = m_defaultProtocolCombo->findData(QVariant::fromValue(int(m_settings->defaultProtocol())));
    if (currentIndex >= 0)
        m_defaultProtocolCombo->setCurrentIndex(currentIndex);
    connect(m_defaultProtocolCombo, &QComboBox::currentIndexChanged, this, [this](int index) {
        const Protocol protocol = static_cast<Protocol>(m_defaultProtocolCombo->itemData(index).toInt());
        m_settings->setDefaultProtocol(protocol);
    });

    // No {file}-style template substitution — a single command string,
    // the file path is always just appended as its sole argument (see
    // EditSessionManager::launchEditor()). A documented v1 limitation,
    // not a half-finished feature.
    m_externalEditorCommandEdit->setText(m_settings->externalEditorCommand());
    m_externalEditorCommandEdit->setPlaceholderText(
        tr("Leave blank to use your system's default application"));
    m_externalEditorCommandEdit->setToolTip(
        tr("The file path is appended to this command as its only argument "
           "(e.g. \"code\", \"gedit\", \"notepad++\")."));
    // editingFinished, not textChanged — this is the first free-text
    // field in this dialog (every other field is a checkbox/combo, whose
    // "changed" signal only ever fires on a genuine, complete choice);
    // persisting on every keystroke would mean a settings.json write per
    // character typed.
    connect(m_externalEditorCommandEdit, &QLineEdit::editingFinished, this, [this]() {
        m_settings->setExternalEditorCommand(m_externalEditorCommandEdit->text());
    });

    // One global proxy, applied to every SFTP/FTP/FTPS connection — see
    // AppSettings::resolvedProxyConfig() and MainWindow::startConnection().
    // No per-site override exists; this is the only place it's configured.
    m_proxyTypeCombo->addItem(tr("None (direct connection)"), int(ProxyType::None));
    m_proxyTypeCombo->addItem(tr("SOCKS5"), int(ProxyType::Socks5));
    m_proxyTypeCombo->addItem(tr("HTTP (CONNECT)"), int(ProxyType::Http));
    const int proxyTypeIndex = m_proxyTypeCombo->findData(int(m_settings->proxyType()));
    if (proxyTypeIndex >= 0)
        m_proxyTypeCombo->setCurrentIndex(proxyTypeIndex);
    connect(m_proxyTypeCombo, &QComboBox::currentIndexChanged, this, [this](int index) {
        m_settings->setProxyType(static_cast<ProxyType>(m_proxyTypeCombo->itemData(index).toInt()));
        updateProxyFieldsEnabled();
    });

    m_proxyHostEdit->setText(m_settings->proxyHost());
    connect(m_proxyHostEdit, &QLineEdit::editingFinished, this, [this]() {
        m_settings->setProxyHost(m_proxyHostEdit->text());
    });

    m_proxyPortSpin->setRange(1, 65535);
    m_proxyPortSpin->setValue(m_settings->proxyPort());
    connect(m_proxyPortSpin, &QSpinBox::valueChanged, this, [this](int value) {
        m_settings->setProxyPort(value);
    });

    m_proxyUsernameEdit->setPlaceholderText(tr("Optional"));
    m_proxyUsernameEdit->setText(m_settings->proxyUsername());
    connect(m_proxyUsernameEdit, &QLineEdit::editingFinished, this, [this]() {
        m_settings->setProxyUsername(m_proxyUsernameEdit->text());
    });

    // Loaded from CredentialStore (the OS's own credential store), same
    // as SiteManagerDialog's saved-site passwords — never a settings.json
    // field. See AppSettings::proxyPassword()'s own doc comment.
    m_proxyPasswordEdit->setEchoMode(QLineEdit::Password);
    m_proxyPasswordEdit->setPlaceholderText(tr("Optional"));
    m_proxyPasswordEdit->setText(m_settings->proxyPassword());
    connect(m_proxyPasswordEdit, &QLineEdit::editingFinished, this, [this]() {
        m_settings->setProxyPassword(m_proxyPasswordEdit->text());
    });

    updateProxyFieldsEnabled();

    auto *form = new QFormLayout;
    form->addRow(tr("Local && remote panes:"), m_showHiddenFilesCheck);
    form->addRow(tr("Default protocol for new connections:"), m_defaultProtocolCombo);
    form->addRow(tr("External editor command:"), m_externalEditorCommandEdit);

    auto *proxyLabel = new QLabel(tr("<b>Proxy</b>"), this);
    form->addRow(proxyLabel);
    form->addRow(tr("Type:"), m_proxyTypeCombo);
    form->addRow(tr("Host:"), m_proxyHostEdit);
    form->addRow(tr("Port:"), m_proxyPortSpin);
    form->addRow(tr("Username:"), m_proxyUsernameEdit);
    form->addRow(tr("Password:"), m_proxyPasswordEdit);

    // QDialogButtonBox::Close is wired to reject() by Qt's own convention
    // (it's a RejectRole button) — accept() vs. reject() is meaningless
    // here anyway, since every field already persisted itself the moment
    // it changed, above.
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::close);

    auto *layout = new QVBoxLayout(this);
    layout->addLayout(form);
    layout->addWidget(buttons);
}

void PreferencesDialog::updateProxyFieldsEnabled()
{
    const bool enabled = m_proxyTypeCombo->currentData().toInt() != int(ProxyType::None);
    m_proxyHostEdit->setEnabled(enabled);
    m_proxyPortSpin->setEnabled(enabled);
    m_proxyUsernameEdit->setEnabled(enabled);
    m_proxyPasswordEdit->setEnabled(enabled);
}
