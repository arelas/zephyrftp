#include "SiteManagerDialog.h"
#include "IconTheme.h"

#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QSignalBlocker>
#include <QLineEdit>
#include <QSpinBox>
#include <QRadioButton>
#include <QButtonGroup>
#include <QStackedWidget>
#include <QPushButton>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QSplitter>
#include <QLabel>
#include <QMessageBox>
#include <QInputDialog>
#include <QFileDialog>
#include <QUuid>
#include <QMap>

namespace {
constexpr int IdRole = Qt::UserRole;
}

SiteManagerDialog::SiteManagerDialog(QWidget *parent)
    : QDialog(parent)
    , m_tree(new QTreeWidget(this))
{
    setWindowTitle(tr("Site Manager"));
    resize(700, 440);

    m_sites = SiteStore::load();
    buildUi();
    rebuildTree();
    onTreeSelectionChanged();   // start with the (empty) selection state applied to the form
}

void SiteManagerDialog::buildUi()
{
    // --- Left: site tree + New/Duplicate/Delete ---
    m_tree->setHeaderHidden(true);
    m_tree->setIconSize(QSize(16, 16));
    connect(m_tree, &QTreeWidget::currentItemChanged, this, &SiteManagerDialog::onTreeSelectionChanged);

    auto *newSiteButton = new QPushButton(tr("New Site"), this);
    newSiteButton->setIcon(IconTheme::tintedIcon(":/icons/folder-plus.svg", IconTheme::Green));
    connect(newSiteButton, &QPushButton::clicked, this, &SiteManagerDialog::onNewSite);

    m_duplicateButton = new QPushButton(tr("Duplicate"), this);
    m_duplicateButton->setIcon(IconTheme::tintedIcon(":/icons/copy.svg", IconTheme::Gray));
    connect(m_duplicateButton, &QPushButton::clicked, this, &SiteManagerDialog::onDuplicateSite);

    m_deleteButton = new QPushButton(tr("Delete"), this);
    m_deleteButton->setIcon(IconTheme::tintedIcon(":/icons/trash.svg", IconTheme::Red));
    connect(m_deleteButton, &QPushButton::clicked, this, &SiteManagerDialog::onDeleteSite);

    auto *treeActionsRow = new QHBoxLayout;
    treeActionsRow->addWidget(m_duplicateButton);
    treeActionsRow->addWidget(m_deleteButton);

    auto *leftLayout = new QVBoxLayout;
    leftLayout->addWidget(newSiteButton);
    leftLayout->addWidget(m_tree, 1);
    leftLayout->addLayout(treeActionsRow);
    auto *leftWidget = new QWidget(this);
    leftWidget->setLayout(leftLayout);

    // --- Right: details form ---
    m_nameEdit = new QLineEdit(this);
    connect(m_nameEdit, &QLineEdit::editingFinished, this, &SiteManagerDialog::onFieldEdited);

    m_hostEdit = new QLineEdit(this);
    connect(m_hostEdit, &QLineEdit::editingFinished, this, &SiteManagerDialog::onFieldEdited);

    m_portSpin = new QSpinBox(this);
    m_portSpin->setRange(1, 65535);
    m_portSpin->setValue(22);
    connect(m_portSpin, &QSpinBox::editingFinished, this, &SiteManagerDialog::onFieldEdited);

    m_usernameEdit = new QLineEdit(this);
    connect(m_usernameEdit, &QLineEdit::editingFinished, this, &SiteManagerDialog::onFieldEdited);

    m_passwordAuthRadio = new QRadioButton(tr("Password"), this);
    m_keyAuthRadio = new QRadioButton(tr("Private key"), this);
    auto *authGroup = new QButtonGroup(this);
    authGroup->addButton(m_passwordAuthRadio);
    authGroup->addButton(m_keyAuthRadio);
    m_passwordAuthRadio->setChecked(true);
    connect(m_passwordAuthRadio, &QRadioButton::toggled, this, &SiteManagerDialog::updateAuthFieldsVisibility);

    auto *authRadioRow = new QHBoxLayout;
    authRadioRow->addWidget(m_passwordAuthRadio);
    authRadioRow->addWidget(m_keyAuthRadio);
    authRadioRow->addStretch();

    // Page 0 (password auth): deliberately no password field at all —
    // see the class doc comment. Just an honest explanation of why.
    auto *passwordPage = new QWidget(this);
    auto *passwordPageLayout = new QVBoxLayout(passwordPage);
    passwordPageLayout->setContentsMargins(0, 0, 0, 0);
    auto *passwordNote = new QLabel(
        tr("The password isn't saved. You'll be asked for it each time you connect to this site."), this);
    passwordNote->setWordWrap(true);
    passwordPageLayout->addWidget(passwordNote);

    // Page 1 (key auth): key path only — no passphrase field, same reason.
    auto *keyPage = new QWidget(this);
    m_privateKeyPathEdit = new QLineEdit(this);
    connect(m_privateKeyPathEdit, &QLineEdit::editingFinished, this, &SiteManagerDialog::onFieldEdited);
    auto *browseButton = new QPushButton(tr("Browse..."), keyPage);
    connect(browseButton, &QPushButton::clicked, this, [this]() {
        const QString path = QFileDialog::getOpenFileName(this, tr("Select Private Key File"));
        if (!path.isEmpty()) {
            m_privateKeyPathEdit->setText(path);
            onFieldEdited();
        }
    });
    auto *keyPathRow = new QHBoxLayout;
    keyPathRow->addWidget(m_privateKeyPathEdit);
    keyPathRow->addWidget(browseButton);
    auto *keyPageLayout = new QFormLayout(keyPage);
    keyPageLayout->setContentsMargins(0, 0, 0, 0);
    keyPageLayout->addRow(tr("Key file:"), keyPathRow);
    auto *keyNote = new QLabel(
        tr("If the key has a passphrase, you'll be asked for it each time you connect."), keyPage);
    keyNote->setWordWrap(true);
    keyPageLayout->addRow(keyNote);

    m_authFieldsStack = new QStackedWidget(this);
    m_authFieldsStack->addWidget(passwordPage);   // index 0
    m_authFieldsStack->addWidget(keyPage);         // index 1

    auto *form = new QFormLayout;
    form->addRow(tr("Site name:"), m_nameEdit);
    form->addRow(tr("Host:"), m_hostEdit);
    form->addRow(tr("Port:"), m_portSpin);
    form->addRow(tr("Username:"), m_usernameEdit);
    form->addRow(tr("Authentication:"), authRadioRow);

    auto *rightLayout = new QVBoxLayout;
    rightLayout->addLayout(form);
    rightLayout->addWidget(m_authFieldsStack);
    rightLayout->addStretch();
    auto *rightWidget = new QWidget(this);
    rightWidget->setLayout(rightLayout);

    auto *splitter = new QSplitter(this);
    splitter->addWidget(leftWidget);
    splitter->addWidget(rightWidget);
    splitter->setSizes({200, 500});

    // --- Bottom: Cancel / Connect ---
    auto *cancelButton = new QPushButton(tr("Cancel"), this);
    connect(cancelButton, &QPushButton::clicked, this, &QDialog::reject);

    auto *connectButton = new QPushButton(tr("Connect"), this);
    connectButton->setIcon(IconTheme::tintedIcon(":/icons/plug.svg", IconTheme::Green));
    connectButton->setProperty("primary", true);   // picks up theme.qss's .primary green treatment
    connectButton->setDefault(true);
    connect(connectButton, &QPushButton::clicked, this, &SiteManagerDialog::onConnectClicked);

    auto *buttonRow = new QHBoxLayout;
    buttonRow->addStretch();
    buttonRow->addWidget(cancelButton);
    buttonRow->addWidget(connectButton);

    auto *mainLayout = new QVBoxLayout(this);
    mainLayout->addWidget(splitter, 1);
    mainLayout->addLayout(buttonRow);

    updateAuthFieldsVisibility();
}

void SiteManagerDialog::rebuildTree()
{
    const QString previouslySelected = m_selectedId;
    m_tree->clear();

    QMap<QString, QTreeWidgetItem *> groupItems;   // group name -> folder item, created on demand

    for (const SavedSite &site : m_sites) {
        QTreeWidgetItem *parent = nullptr;
        if (!site.group.isEmpty()) {
            auto it = groupItems.find(site.group);
            if (it == groupItems.end()) {
                auto *folderItem = new QTreeWidgetItem(m_tree, {site.group});
                folderItem->setIcon(0, IconTheme::tintedIcon(":/icons/folder.svg", IconTheme::Gray));
                groupItems.insert(site.group, folderItem);
                parent = folderItem;
            } else {
                parent = it.value();
            }
        }

        auto *siteItem = parent
            ? new QTreeWidgetItem(parent, {site.name})
            : new QTreeWidgetItem(m_tree, {site.name});
        siteItem->setIcon(0, IconTheme::tintedIcon(":/icons/server.svg", IconTheme::Gray));
        siteItem->setData(0, IdRole, site.id);
    }

    m_tree->expandAll();

    // Re-select whatever was selected before the rebuild, if it still exists.
    if (!previouslySelected.isEmpty()) {
        const QTreeWidgetItemIterator it(m_tree);
        for (auto iter = it; *iter; ++iter) {
            if ((*iter)->data(0, IdRole).toString() == previouslySelected) {
                m_tree->setCurrentItem(*iter);
                return;
            }
        }
    }
}

SavedSite *SiteManagerDialog::selectedSite()
{
    if (m_selectedId.isEmpty())
        return nullptr;
    for (SavedSite &site : m_sites) {
        if (site.id == m_selectedId)
            return &site;
    }
    return nullptr;
}

void SiteManagerDialog::onTreeSelectionChanged()
{
    QTreeWidgetItem *current = m_tree->currentItem();
    const QString id = current ? current->data(0, IdRole).toString() : QString();

    m_selectedId = id;
    const bool hasSelection = !id.isEmpty();
    m_duplicateButton->setEnabled(hasSelection);
    m_deleteButton->setEnabled(hasSelection);

    if (SavedSite *site = selectedSite()) {
        loadSiteIntoForm(*site);
    } else {
        // Folder selected, or nothing at all — clear the form to a blank
        // state ready for a fresh one-off connection (Connect still works
        // without a saved site selected, same as the original
        // ConnectionDialog's behavior).
        loadSiteIntoForm(SavedSite{});
    }
}

void SiteManagerDialog::loadSiteIntoForm(const SavedSite &site)
{
    const QSignalBlocker b1(m_nameEdit);
    const QSignalBlocker b2(m_hostEdit);
    const QSignalBlocker b3(m_portSpin);
    const QSignalBlocker b4(m_usernameEdit);
    const QSignalBlocker b5(m_passwordAuthRadio);
    const QSignalBlocker b6(m_keyAuthRadio);
    const QSignalBlocker b7(m_privateKeyPathEdit);

    m_nameEdit->setText(site.name);
    m_hostEdit->setText(site.host);
    m_portSpin->setValue(site.port > 0 ? site.port : 22);
    m_usernameEdit->setText(site.username);
    if (site.authMethod == SftpAuthMethod::PublicKey)
        m_keyAuthRadio->setChecked(true);
    else
        m_passwordAuthRadio->setChecked(true);
    m_privateKeyPathEdit->setText(site.privateKeyPath);

    updateAuthFieldsVisibility();
}

void SiteManagerDialog::commitFormToSelectedSite()
{
    SavedSite *site = selectedSite();
    if (!site)
        return;   // nothing selected — form is just a scratch area for a one-off connect

    site->name = m_nameEdit->text().trimmed().isEmpty() ? tr("Untitled Site") : m_nameEdit->text().trimmed();
    site->host = m_hostEdit->text().trimmed();
    site->port = m_portSpin->value();
    site->username = m_usernameEdit->text();
    site->authMethod = m_keyAuthRadio->isChecked() ? SftpAuthMethod::PublicKey : SftpAuthMethod::Password;
    site->privateKeyPath = m_privateKeyPathEdit->text().trimmed();

    SiteStore::save(m_sites);

    // Tree item text needs to track the name in real time; everything
    // else about hierarchy doesn't change from field edits in this
    // version (no in-UI group editing yet — see class doc comment).
    if (QTreeWidgetItem *item = m_tree->currentItem())
        item->setText(0, site->name);
}

void SiteManagerDialog::onFieldEdited()
{
    commitFormToSelectedSite();
}

void SiteManagerDialog::updateAuthFieldsVisibility()
{
    m_authFieldsStack->setCurrentIndex(m_passwordAuthRadio->isChecked() ? 0 : 1);
    onFieldEdited();   // auth-method choice is itself an edit worth persisting immediately
}

void SiteManagerDialog::onNewSite()
{
    SavedSite site;
    site.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    site.name = tr("New Site");
    site.port = 22;
    m_sites.append(site);
    SiteStore::save(m_sites);

    m_selectedId = site.id;
    rebuildTree();
}

void SiteManagerDialog::onDuplicateSite()
{
    SavedSite *source = selectedSite();
    if (!source)
        return;

    SavedSite copy = *source;
    copy.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    copy.name = tr("%1 (Copy)").arg(source->name);
    m_sites.append(copy);
    SiteStore::save(m_sites);

    m_selectedId = copy.id;
    rebuildTree();
}

void SiteManagerDialog::onDeleteSite()
{
    SavedSite *site = selectedSite();
    if (!site)
        return;

    const auto reply = QMessageBox::question(
        this, tr("Delete Site"),
        tr("Delete \"%1\"? This can't be undone.").arg(site->name),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (reply != QMessageBox::Yes)
        return;

    const QString idToRemove = m_selectedId;
    for (int i = 0; i < m_sites.size(); ++i) {
        if (m_sites[i].id == idToRemove) {
            m_sites.removeAt(i);
            break;
        }
    }
    SiteStore::save(m_sites);

    m_selectedId.clear();
    rebuildTree();
}

void SiteManagerDialog::onConnectClicked()
{
    SftpCredentials creds;

    if (SavedSite *site = selectedSite()) {
        commitFormToSelectedSite();   // capture any not-yet-committed edits first
        creds = site->toCredentials();
    } else {
        // No saved site selected — the form is being used as a one-off,
        // unsaved connection, same as the original ConnectionDialog.
        creds.host = m_hostEdit->text().trimmed();
        creds.port = m_portSpin->value();
        creds.username = m_usernameEdit->text();
        creds.authMethod = m_passwordAuthRadio->isChecked() ? SftpAuthMethod::Password : SftpAuthMethod::PublicKey;
        creds.privateKeyPath = m_privateKeyPathEdit->text().trimmed();
    }

    if (creds.host.isEmpty()) {
        QMessageBox::warning(this, tr("Connect"), tr("Host cannot be empty."));
        return;
    }

    if (creds.authMethod == SftpAuthMethod::Password) {
        bool ok = false;
        const QString password = QInputDialog::getText(
            this, tr("Password"),
            tr("Password for %1@%2:").arg(creds.username, creds.host),
            QLineEdit::Password, QString(), &ok);
        if (!ok)
            return;   // cancelled the prompt — stay in the dialog rather than connecting with no password
        creds.password = password;
    } else {
        if (creds.privateKeyPath.isEmpty()) {
            QMessageBox::warning(this, tr("Connect"), tr("Select a private key file."));
            return;
        }
        bool ok = false;
        const QString passphrase = QInputDialog::getText(
            this, tr("Key Passphrase"),
            tr("Passphrase for the private key (leave blank if none):"),
            QLineEdit::Password, QString(), &ok);
        if (!ok)
            return;
        creds.passphrase = passphrase;
    }

    m_pendingCredentials = creds;
    accept();
}
