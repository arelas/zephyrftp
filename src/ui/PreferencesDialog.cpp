#include "PreferencesDialog.h"
#include "../AppSettings.h"
#include "../backends/Protocol.h"
#include "../backends/ProxyConfig.h"
#include "ProtocolCombo.h"
#include "ThemeCombo.h"

#include <QFormLayout>
#include <QVBoxLayout>
#include <QCheckBox>
#include <QComboBox>
#include <QLineEdit>
#include <QSpinBox>
#include <QAbstractSpinBox>
#include <QLabel>
#include <QFrame>
#include <QDialogButtonBox>

PreferencesDialog::PreferencesDialog(AppSettings *settings, QWidget *parent)
    : QDialog(parent)
    , m_settings(settings)
    , m_showHiddenFilesCheck(new QCheckBox(tr("Show hidden files (dotfiles)"), this))
    , m_defaultProtocolCombo(new QComboBox(this))
    , m_themeCombo(new QComboBox(this))
    , m_externalEditorCommandEdit(new QLineEdit(this))
    , m_proxyTypeCombo(new QComboBox(this))
    , m_proxyHostEdit(new QLineEdit(this))
    , m_proxyPortSpin(new QSpinBox(this))
    , m_proxyUsernameEdit(new QLineEdit(this))
    , m_proxyPasswordEdit(new QLineEdit(this))
    , m_bandwidthLimitSpin(new QSpinBox(this))
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

    ThemeCombo::populate(m_themeCombo);
    const int themeIndex = m_themeCombo->findData(QVariant::fromValue(int(m_settings->theme())));
    if (themeIndex >= 0)
        m_themeCombo->setCurrentIndex(themeIndex);
    connect(m_themeCombo, &QComboBox::currentIndexChanged, this, [this](int index) {
        const Theme theme = static_cast<Theme>(m_themeCombo->itemData(index).toInt());
        m_settings->setTheme(theme);
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

    // No up/down arrows: a port is typed, not nudged one integer at a
    // time — same reasoning and precedent as SiteManagerDialog's own
    // port field. QAbstractSpinBox's own sizeHint() still reserves space
    // for the (now invisible) spin-button area, a real height mismatch
    // against every sibling QLineEdit in this form — matched to
    // m_proxyHostEdit's actual sizeHint rather than a hardcoded number,
    // same fix SiteManagerDialog already applies for the identical
    // widget/rule combination.
    m_proxyPortSpin->setButtonSymbols(QAbstractSpinBox::NoButtons);
    m_proxyPortSpin->setRange(1, 65535);
    m_proxyPortSpin->setValue(m_settings->proxyPort());
    m_proxyPortSpin->setFixedHeight(m_proxyHostEdit->sizeHint().height());
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

    // One global per-transfer limit — see AppSettings::bandwidthLimitKBps()'s
    // own doc comment for why per-transfer, not one combined cap across
    // every transfer at once. 0 (the default, this QSpinBox's own
    // minimum) means unlimited; setSpecialValueText() shows that in
    // words rather than a bare "0", which would otherwise read as a
    // suspiciously-broken zero-speed limit. Only takes effect on
    // connections made AFTER this changes — an already-open connection
    // keeps whatever limit was in effect when it connected, same as the
    // proxy setting above.
    // No up/down arrows, same reasoning as m_proxyPortSpin above — a
    // bandwidth limit is typed, not nudged one KB/s at a time.
    m_bandwidthLimitSpin->setButtonSymbols(QAbstractSpinBox::NoButtons);
    m_bandwidthLimitSpin->setRange(0, 1000000);
    m_bandwidthLimitSpin->setSpecialValueText(tr("Unlimited"));
    m_bandwidthLimitSpin->setSuffix(tr(" KB/s"));
    m_bandwidthLimitSpin->setValue(m_settings->bandwidthLimitKBps());
    m_bandwidthLimitSpin->setFixedHeight(m_proxyHostEdit->sizeHint().height());
    connect(m_bandwidthLimitSpin, &QSpinBox::valueChanged, this, [this](int value) {
        m_settings->setBandwidthLimitKBps(value);
    });

    // Single-arg addRow(), not a separate row label — m_showHiddenFilesCheck's
    // own text ("Show hidden files (dotfiles)") already fully describes
    // what it does; the old paired label here ("Local && remote panes:")
    // read as an unrelated, non-interactive control sitting next to a
    // checkbox that "didn't seem to do anything" — a real reported
    // confusion, not a functional bug (the setting itself always worked;
    // see FilePaneWidget's own showHiddenFilesChanged handling).
    auto *form = new QFormLayout;
    form->addRow(m_showHiddenFilesCheck);
    form->addRow(tr("Default protocol for new connections:"), m_defaultProtocolCombo);
    form->addRow(tr("Theme:"), m_themeCombo);
    form->addRow(tr("External editor command:"), m_externalEditorCommandEdit);

    // Real QFrame::HLine dividers ahead of each section, not just a bold
    // label — the bold text alone read as visually cluttered/flat
    // against the general fields above it.
    auto *proxyDivider = new QFrame(this);
    proxyDivider->setFrameShape(QFrame::HLine);
    proxyDivider->setFrameShadow(QFrame::Sunken);
    form->addRow(proxyDivider);
    auto *proxyLabel = new QLabel(tr("<b>Proxy</b>"), this);
    form->addRow(proxyLabel);
    form->addRow(tr("Type:"), m_proxyTypeCombo);
    form->addRow(tr("Host:"), m_proxyHostEdit);
    form->addRow(tr("Port:"), m_proxyPortSpin);
    form->addRow(tr("Username:"), m_proxyUsernameEdit);
    form->addRow(tr("Password:"), m_proxyPasswordEdit);

    auto *bandwidthDivider = new QFrame(this);
    bandwidthDivider->setFrameShape(QFrame::HLine);
    bandwidthDivider->setFrameShadow(QFrame::Sunken);
    form->addRow(bandwidthDivider);
    auto *bandwidthLabel = new QLabel(tr("<b>Bandwidth</b>"), this);
    form->addRow(bandwidthLabel);
    form->addRow(tr("Limit per transfer:"), m_bandwidthLimitSpin);

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
