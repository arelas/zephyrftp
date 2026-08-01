#include "SiteManagerDialog.h"
#include "IconTheme.h"
#include "../backends/CredentialStore.h"

#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QSignalBlocker>
#include <QLineEdit>
#include <QSpinBox>
#include <QAbstractSpinBox>
#include <QRadioButton>
#include <QButtonGroup>
#include <QStackedWidget>
#include <QPushButton>
#include <QComboBox>
#include <QCheckBox>
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

    // Editable combo: pick an existing group from the dropdown, or type a
    // new name to create one on the spot. There's no separate "groups"
    // collection to manage — a group exists precisely when at least one
    // site references it, so typing a new name here is how one gets
    // created; leaving it blank means ungrouped.
    m_groupCombo = new QComboBox(this);
    m_groupCombo->setEditable(true);
    m_groupCombo->setInsertPolicy(QComboBox::NoInsert);   // don't auto-add typed text to the dropdown list itself
    m_groupCombo->lineEdit()->setPlaceholderText(tr("(none)"));
    connect(m_groupCombo->lineEdit(), &QLineEdit::editingFinished, this, &SiteManagerDialog::onFieldEdited);
    connect(m_groupCombo, &QComboBox::currentIndexChanged, this, &SiteManagerDialog::onFieldEdited);

    m_protocolCombo = new QComboBox(this);
    m_protocolCombo->addItem(displayNameFor(Protocol::Sftp), int(Protocol::Sftp));
    m_protocolCombo->addItem(displayNameFor(Protocol::Ftp),  int(Protocol::Ftp));
    m_protocolCombo->addItem(displayNameFor(Protocol::Ftps), int(Protocol::Ftps));
    // Two connections, deliberately: onProtocolChanged() updates which
    // fields are visible, onFieldEdited() persists the change. Folding
    // both into one slot would tangle "what the form looks like" with
    // "what's written to disk".
    connect(m_protocolCombo, &QComboBox::currentIndexChanged, this, &SiteManagerDialog::onProtocolChanged);
    connect(m_protocolCombo, &QComboBox::currentIndexChanged, this, &SiteManagerDialog::onFieldEdited);

    m_hostEdit = new QLineEdit(this);
    connect(m_hostEdit, &QLineEdit::editingFinished, this, &SiteManagerDialog::onFieldEdited);

    m_portSpin = new QSpinBox(this);
    // No up/down arrows: a port is typed, not nudged one integer at a
    // time, so the buttons were pure visual noise. Kept as a QSpinBox
    // rather than swapped for a QLineEdit specifically to retain what
    // the widget is actually earning its place with — numeric-only
    // input and clamping to the valid 1-65535 range. A QLineEdit would
    // need a QIntValidator bolted on to get back to the same place.
    m_portSpin->setButtonSymbols(QAbstractSpinBox::NoButtons);
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

    m_authRowWidget = new QWidget(this);
    auto *authRadioRow = new QHBoxLayout(m_authRowWidget);
    authRadioRow->setContentsMargins(0, 0, 0, 0);
    authRadioRow->addWidget(m_passwordAuthRadio);
    authRadioRow->addWidget(m_keyAuthRadio);
    authRadioRow->addStretch();

    // Page 0 (password auth): deliberately no password field at all —
    // see the class doc comment. Just an honest explanation of why:
    // still always prompted at connect time even if "Save password"
    // (below) is checked — that prompt is just pre-filled in that case.
    auto *passwordPage = new QWidget(this);
    auto *passwordPageLayout = new QVBoxLayout(passwordPage);
    passwordPageLayout->setContentsMargins(0, 0, 0, 0);
    auto *passwordNote = new QLabel(
        tr("You'll be asked for the password each time you connect — pre-filled "
           "if you check \"Save password\" below, but always shown, never silent."), this);
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
        tr("If the key has a passphrase, you'll be asked for it each time you connect — "
           "pre-filled if you check \"Save passphrase\" below."), keyPage);
    keyNote->setWordWrap(true);
    keyPageLayout->addRow(keyNote);

    m_authFieldsStack = new QStackedWidget(this);
    m_authFieldsStack->addWidget(passwordPage);   // index 0
    m_authFieldsStack->addWidget(keyPage);         // index 1

    // Below the stack, not inside either page — one checkbox, shared
    // across both auth methods (see the header's doc comment on why).
    // Unchecked by default: this is opt-in, not opt-out. Text updated
    // per auth method in updateAuthFieldsVisibility().
    m_savePasswordCheck = new QCheckBox(tr("Save password"), this);
    connect(m_savePasswordCheck, &QCheckBox::toggled, this, [this](bool checked) {
        // Unchecking clears any already-stored secret immediately,
        // rather than waiting for the next Connect — consistent with
        // every other field on this dialog persisting the instant it
        // changes, not on some later "Save" step. Checking it doesn't
        // itself store anything yet: there's nothing to store until the
        // Connect prompt actually collects a secret.
        if (!checked) {
            if (SavedSite *site = selectedSite())
                CredentialStore::remove(site->id);
        }
    });

    // --- Starting directory: home (default) or a specific path ---
    m_homeDirRadio = new QRadioButton(tr("Home directory"), this);
    m_specificDirRadio = new QRadioButton(tr("Specific directory:"), this);
    auto *dirGroup = new QButtonGroup(this);
    dirGroup->addButton(m_homeDirRadio);
    dirGroup->addButton(m_specificDirRadio);
    m_homeDirRadio->setChecked(true);
    connect(m_homeDirRadio, &QRadioButton::toggled, this, &SiteManagerDialog::updateStartingDirVisibility);

    m_startingDirEdit = new QLineEdit(this);
    m_startingDirEdit->setPlaceholderText(tr("/path/on/server"));
    connect(m_startingDirEdit, &QLineEdit::editingFinished, this, &SiteManagerDialog::onFieldEdited);

    auto *dirRadioRow = new QHBoxLayout;
    dirRadioRow->addWidget(m_homeDirRadio);
    dirRadioRow->addWidget(m_specificDirRadio);
    dirRadioRow->addStretch();

    auto *dirFieldColumn = new QVBoxLayout;
    dirFieldColumn->addLayout(dirRadioRow);
    dirFieldColumn->addWidget(m_startingDirEdit);

    auto *form = new QFormLayout;
    form->addRow(tr("Site name:"), m_nameEdit);
    form->addRow(tr("Group:"), m_groupCombo);
    form->addRow(tr("Protocol:"), m_protocolCombo);
    form->addRow(tr("Host:"), m_hostEdit);
    form->addRow(tr("Port:"), m_portSpin);
    form->addRow(tr("Username:"), m_usernameEdit);
    m_authRowLabel = new QLabel(tr("Authentication:"), this);
    form->addRow(m_authRowLabel, m_authRowWidget);
    form->addRow(tr("Starting directory:"), dirFieldColumn);

    auto *rightLayout = new QVBoxLayout;
    rightLayout->addLayout(form);
    rightLayout->addWidget(m_authFieldsStack);
    // A sibling of m_authFieldsStack, not inside it — deliberately stays
    // visible even when that stack collapses for FTP/FTPS (no auth
    // *method* choice there, but a saved FTP/FTPS site still has exactly
    // one password worth being able to opt into saving).
    rightLayout->addWidget(m_savePasswordCheck);
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

    onProtocolChanged();   // establishes initial field visibility for the default protocol
    updateStartingDirVisibility();
}

void SiteManagerDialog::onProtocolChanged()
{
    const Protocol selected = Protocol(m_protocolCombo->currentData().toInt());
    const bool keyAuthAvailable = supportsKeyAuth(selected);

    // Same rule as ConnectionDialog: force Password for protocols with no
    // key auth, so a stale "Private key" selection can't be committed to
    // a saved FTP site where it would be meaningless.
    if (!keyAuthAvailable) {
        const QSignalBlocker blocker(m_passwordAuthRadio);
        m_passwordAuthRadio->setChecked(true);
    }

    m_authRowWidget->setVisible(keyAuthAvailable);
    m_authRowLabel->setVisible(keyAuthAvailable);
    m_authFieldsStack->setVisible(keyAuthAvailable);

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
    refreshGroupChoices();

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

void SiteManagerDialog::refreshGroupChoices()
{
    // Preserve whatever's currently typed/selected — this runs after
    // every tree rebuild, including ones triggered by an in-progress
    // group edit, so it must not clobber what the person just typed.
    const QString currentText = m_groupCombo->currentText();

    QStringList groups;
    for (const SavedSite &site : m_sites) {
        if (!site.group.isEmpty() && !groups.contains(site.group))
            groups.append(site.group);
    }
    groups.sort(Qt::CaseInsensitive);

    const QSignalBlocker blocker(m_groupCombo);
    m_groupCombo->clear();
    m_groupCombo->addItem(QString());   // "(none)" via placeholder text, not a literal empty-string row label
    m_groupCombo->addItems(groups);
    m_groupCombo->setCurrentText(currentText);
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
    const QSignalBlocker b8(m_homeDirRadio);
    const QSignalBlocker b9(m_specificDirRadio);
    const QSignalBlocker b10(m_startingDirEdit);
    const QSignalBlocker b11(m_groupCombo);
    const QSignalBlocker b12(m_groupCombo->lineEdit());
    const QSignalBlocker b13(m_protocolCombo);
    // Blocked for the same reason as every other field here — setting
    // this programmatically must not trigger the toggled handler's
    // CredentialStore::remove() side effect, which is only meant to
    // fire on a real, deliberate uncheck by the person using the dialog.
    const QSignalBlocker b14(m_savePasswordCheck);

    m_nameEdit->setText(site.name);
    m_groupCombo->setCurrentText(site.group);
    const int protocolIndex = m_protocolCombo->findData(int(site.protocol));
    if (protocolIndex >= 0)
        m_protocolCombo->setCurrentIndex(protocolIndex);
    m_hostEdit->setText(site.host);
    // Fall back to the default for THIS site's protocol, not a hardcoded
    // 22 — a saved FTP site with a missing/zero port should land on 21.
    m_portSpin->setValue(site.port > 0 ? site.port : defaultPortFor(site.protocol));
    m_usernameEdit->setText(site.username);
    if (site.authMethod == SftpAuthMethod::PublicKey)
        m_keyAuthRadio->setChecked(true);
    else
        m_passwordAuthRadio->setChecked(true);
    m_privateKeyPathEdit->setText(site.privateKeyPath);

    if (site.useHomeDirectory)
        m_homeDirRadio->setChecked(true);
    else
        m_specificDirRadio->setChecked(true);
    m_startingDirEdit->setText(site.startingDirectory);

    // Reflects whatever's actually in the OS credential store right
    // now, not a flag persisted in sites.json (there isn't one) — an
    // empty id means "no real site selected, form is a blank scratch
    // area," which never has a stored secret to reflect.
    m_savePasswordCheck->setChecked(!site.id.isEmpty() && CredentialStore::hasSecret(site.id));

    // Covers updateAuthFieldsVisibility() too — and unlike calling that
    // alone, it also applies the protocol's own show/hide rules, which
    // is what makes selecting a saved FTP site hide the auth row.
    onProtocolChanged();
    updateStartingDirVisibility();
}

void SiteManagerDialog::commitFormToSelectedSite()
{
    SavedSite *site = selectedSite();
    if (!site)
        return;   // nothing selected — form is just a scratch area for a one-off connect

    const QString newGroup = m_groupCombo->currentText().trimmed();
    const bool groupChanged = (newGroup != site->group);

    site->name = m_nameEdit->text().trimmed().isEmpty() ? tr("Untitled Site") : m_nameEdit->text().trimmed();
    site->group = newGroup;
    site->protocol = Protocol(m_protocolCombo->currentData().toInt());
    site->host = m_hostEdit->text().trimmed();
    site->port = m_portSpin->value();
    site->username = m_usernameEdit->text();
    site->authMethod = m_keyAuthRadio->isChecked() ? SftpAuthMethod::PublicKey : SftpAuthMethod::Password;
    site->privateKeyPath = m_privateKeyPathEdit->text().trimmed();
    site->useHomeDirectory = m_homeDirRadio->isChecked();
    site->startingDirectory = m_startingDirEdit->text().trimmed();

    SiteStore::save(m_sites);

    if (groupChanged) {
        // Hierarchy itself changed (site may need to move to a different
        // folder, or a folder may now be empty and should disappear) —
        // a full rebuild is the simplest correct way to reflect that,
        // unlike a plain rename which only needs the item's own text
        // updated in place.
        rebuildTree();
    } else if (QTreeWidgetItem *item = m_tree->currentItem()) {
        item->setText(0, site->name);
    }
}

void SiteManagerDialog::onFieldEdited()
{
    commitFormToSelectedSite();
}

void SiteManagerDialog::updateAuthFieldsVisibility()
{
    m_authFieldsStack->setCurrentIndex(m_passwordAuthRadio->isChecked() ? 0 : 1);
    m_savePasswordCheck->setText(m_passwordAuthRadio->isChecked() ? tr("Save password") : tr("Save passphrase"));
    onFieldEdited();   // auth-method choice is itself an edit worth persisting immediately
}

void SiteManagerDialog::updateStartingDirVisibility()
{
    m_startingDirEdit->setEnabled(m_specificDirRadio->isChecked());
    onFieldEdited();   // this choice is itself an edit worth persisting immediately, same as auth method above
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
    ConnectionRequest request;
    // Kept beyond the if/else below (unlike before this feature existed)
    // so the secrets step can look up/save/remove a stored credential —
    // stays nullptr for the one-off/unsaved-scratch path, which has no
    // site id to key a stored secret on and isn't offered this at all.
    SavedSite *site = selectedSite();

    if (site) {
        commitFormToSelectedSite();   // capture any not-yet-committed edits first
        request = site->toConnectionRequest();
    } else {
        // No saved site selected — the form is being used as a one-off,
        // unsaved connection. Built as a scratch SavedSite rather than
        // assembling credentials by hand so that BOTH paths go through
        // the same toConnectionRequest() conversion; when that mapping
        // changes, it can't drift between the saved and unsaved cases.
        SavedSite scratch;
        scratch.protocol = Protocol(m_protocolCombo->currentData().toInt());
        scratch.host = m_hostEdit->text().trimmed();
        scratch.port = m_portSpin->value();
        scratch.username = m_usernameEdit->text();
        scratch.authMethod = m_keyAuthRadio->isChecked() ? SftpAuthMethod::PublicKey : SftpAuthMethod::Password;
        scratch.privateKeyPath = m_privateKeyPathEdit->text().trimmed();
        scratch.useHomeDirectory = m_homeDirRadio->isChecked();
        scratch.startingDirectory = m_startingDirEdit->text().trimmed();
        request = scratch.toConnectionRequest();
    }

    if (request.host().isEmpty()) {
        QMessageBox::warning(this, tr("Connect"), tr("Host cannot be empty."));
        return;
    }
    if (!request.useHomeDirectory() && request.startingDirectory().isEmpty()) {
        QMessageBox::warning(this, tr("Connect"), tr("Enter a starting directory, or choose Home directory."));
        return;
    }

    // The prompt-for-secrets step, which is where SavedSite's
    // no-passwords-on-disk rule actually gets honored — toConnectionRequest()
    // deliberately leaves every secret empty, so without this the
    // connection would go out with a blank password. Pre-filled from
    // CredentialStore when this site has one saved — the prompt itself
    // never disappears (still shown, still requires a click), it's just
    // not blank in that case. Whatever's actually typed here (accepted
    // as-is, or edited) is also what gets saved/removed afterward, so
    // this one prompt does triple duty: reuse, update, and initial save.
    QString storedSecret;
    const bool hasStoredSecret = site && CredentialStore::hasSecret(site->id)
        && CredentialStore::load(site->id, &storedSecret);

    if (request.protocol == Protocol::Sftp
        && request.sftp.authMethod == SftpAuthMethod::PublicKey) {
        if (request.sftp.privateKeyPath.isEmpty()) {
            QMessageBox::warning(this, tr("Connect"), tr("Select a private key file."));
            return;
        }
        bool ok = false;
        const QString passphrase = QInputDialog::getText(
            this, tr("Key Passphrase"),
            tr("Passphrase for the private key (leave blank if none):"),
            QLineEdit::Password, hasStoredSecret ? storedSecret : QString(), &ok);
        if (!ok)
            return;
        request.sftp.passphrase = passphrase;
    } else {
        // Password auth — SFTP with a password, or FTP/FTPS, which have
        // no other option. Same prompt either way; only the struct the
        // result lands in differs.
        bool ok = false;
        const QString password = QInputDialog::getText(
            this, tr("Password"),
            tr("Password for %1@%2:").arg(
                request.protocol == Protocol::Sftp ? request.sftp.username : request.ftp.username,
                request.host()),
            QLineEdit::Password, hasStoredSecret ? storedSecret : QString(), &ok);
        if (!ok)
            return;   // cancelled the prompt — stay in the dialog rather than connecting with no password

        if (request.protocol == Protocol::Sftp)
            request.sftp.password = password;
        else
            request.ftp.password = password;
    }

    // Sync the credential store to match the checkbox + whatever was
    // just entered — covers all three real cases: newly checked (save
    // for the first time), already saved and still checked (overwrite,
    // picking up an edited value), and unchecked (remove, whether or
    // not anything was actually stored — CredentialStore::remove() is a
    // harmless no-op either way).
    if (site) {
        const QString &enteredSecret =
            (request.protocol == Protocol::Sftp && request.sftp.authMethod == SftpAuthMethod::PublicKey)
                ? request.sftp.passphrase
                : (request.protocol == Protocol::Sftp ? request.sftp.password : request.ftp.password);
        if (m_savePasswordCheck->isChecked())
            CredentialStore::save(site->id, enteredSecret);
        else
            CredentialStore::remove(site->id);
    }

    m_pendingRequest = request;
    accept();
}
