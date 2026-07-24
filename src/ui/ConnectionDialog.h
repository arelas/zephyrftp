#pragma once

#include <QDialog>
#include "../backends/SftpCredentials.h"

class QLineEdit;
class QSpinBox;
class QRadioButton;
class QStackedWidget;

// Minimal modal dialog collecting SFTP connection parameters. Deliberately
// dumb — no validation beyond "port is a number", no saved-site list yet.
// That's the next obvious feature once this path is proven out.
//
// Supports password or public-key auth, toggled via radio buttons; the two
// credential fields (password vs. key-file-path + passphrase) are swapped
// via a QStackedWidget so only the relevant fields are ever visible.
class ConnectionDialog : public QDialog {
    Q_OBJECT
public:
    explicit ConnectionDialog(QWidget *parent = nullptr);

    // Bundles every field into one struct rather than exposing each field
    // as its own getter — SftpBackend and this dialog now share the same
    // shape (SftpCredentials), so there's no reason to unpack and repack
    // it field by field at the call site.
    SftpCredentials credentials() const;

private slots:
    void browseForPrivateKey();
    void updateAuthFieldsVisibility();

private:
    QLineEdit *m_hostEdit;
    QSpinBox *m_portSpin;
    QLineEdit *m_usernameEdit;

    QRadioButton *m_passwordAuthRadio;
    QRadioButton *m_keyAuthRadio;

    QStackedWidget *m_authFieldsStack;
    QLineEdit *m_passwordEdit;                 // page 0
    QLineEdit *m_privateKeyPathEdit;            // page 1
    QLineEdit *m_passphraseEdit;                // page 1
};
