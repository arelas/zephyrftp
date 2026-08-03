#include "PreferencesDialog.h"
#include "../AppSettings.h"
#include "../backends/Protocol.h"

#include <QFormLayout>
#include <QVBoxLayout>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QLabel>

PreferencesDialog::PreferencesDialog(AppSettings *settings, QWidget *parent)
    : QDialog(parent)
    , m_settings(settings)
    , m_showHiddenFilesCheck(new QCheckBox(tr("Show hidden files (dotfiles)"), this))
    , m_defaultProtocolCombo(new QComboBox(this))
{
    setWindowTitle(tr("Preferences"));

    m_showHiddenFilesCheck->setChecked(m_settings->showHiddenFiles());
    connect(m_showHiddenFilesCheck, &QCheckBox::toggled, this, [this](bool checked) {
        m_settings->setShowHiddenFiles(checked);
    });

    // Same population pattern ConnectionDialog's protocol combo uses —
    // display text from Protocol.h's shared displayNameFor(), the enum
    // itself stashed as the item data rather than relying on index order.
    m_defaultProtocolCombo->addItem(displayNameFor(Protocol::Sftp), QVariant::fromValue(int(Protocol::Sftp)));
    m_defaultProtocolCombo->addItem(displayNameFor(Protocol::Ftp),  QVariant::fromValue(int(Protocol::Ftp)));
    m_defaultProtocolCombo->addItem(displayNameFor(Protocol::Ftps), QVariant::fromValue(int(Protocol::Ftps)));
    const int currentIndex = m_defaultProtocolCombo->findData(QVariant::fromValue(int(m_settings->defaultProtocol())));
    if (currentIndex >= 0)
        m_defaultProtocolCombo->setCurrentIndex(currentIndex);
    connect(m_defaultProtocolCombo, &QComboBox::currentIndexChanged, this, [this](int index) {
        const Protocol protocol = static_cast<Protocol>(m_defaultProtocolCombo->itemData(index).toInt());
        m_settings->setDefaultProtocol(protocol);
    });

    auto *form = new QFormLayout;
    form->addRow(tr("Local && remote panes:"), m_showHiddenFilesCheck);
    form->addRow(tr("Default protocol for new connections:"), m_defaultProtocolCombo);

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
