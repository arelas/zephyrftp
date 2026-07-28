#pragma once

#include <QDialog>
#include <QList>
#include "../backends/SavedSite.h"

class QTreeWidget;
class QTreeWidgetItem;
class QLineEdit;
class QSpinBox;
class QRadioButton;
class QStackedWidget;
class QPushButton;

// Saved-connections manager — a tree of sites (optionally grouped into
// folders) on the left, a details form on the right, matching the design
// package's site-manager.html mockup. Persists via SiteStore (JSON on
// disk) on every structural or field change, so there's no separate
// "Save" step to forget.
//
// SECURITY: consistent with SavedSite's own doc comment — neither a
// saved password NOR a saved key passphrase ever touches disk. Both are
// prompted for fresh at connect time regardless of what's saved, every
// time. This is stricter than strictly necessary for the passphrase
// (which protects a key file, not a bare credential) but keeps the rule
// simple and uniform rather than having two different secret-handling
// paths to reason about.
class SiteManagerDialog : public QDialog {
    Q_OBJECT
public:
    explicit SiteManagerDialog(QWidget *parent = nullptr);

    // Valid only after exec() returns QDialog::Accepted (i.e. Connect was
    // clicked and any required password/passphrase prompt was completed,
    // not cancelled). Fully populated, ready to hand to SftpBackend.
    SftpCredentials credentialsToConnect() const { return m_pendingCredentials; }

private slots:
    void onTreeSelectionChanged();
    void onNewSite();
    void onDuplicateSite();
    void onDeleteSite();
    void onFieldEdited();
    void onConnectClicked();
    void updateAuthFieldsVisibility();

private:
    void buildUi();
    void rebuildTree();
    void loadSiteIntoForm(const SavedSite &site);
    void commitFormToSelectedSite();   // writes current form values back into m_sites, then persists
    SavedSite *selectedSite();         // nullptr if nothing selected, or selection is a group folder

    QList<SavedSite> m_sites;
    QString m_selectedId;   // empty if nothing/a folder is selected

    QTreeWidget *m_tree;
    QPushButton *m_duplicateButton;
    QPushButton *m_deleteButton;

    QLineEdit *m_nameEdit;
    QLineEdit *m_hostEdit;
    QSpinBox *m_portSpin;
    QLineEdit *m_usernameEdit;
    QRadioButton *m_passwordAuthRadio;
    QRadioButton *m_keyAuthRadio;
    QStackedWidget *m_authFieldsStack;
    QLineEdit *m_privateKeyPathEdit;   // page 1 — no passphrase field here; prompted at connect time instead

    SftpCredentials m_pendingCredentials;
};
