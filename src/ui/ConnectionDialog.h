#pragma once

#include <QDialog>
#include "../backends/ConnectionRequest.h"

class QLineEdit;
class QSpinBox;
class QRadioButton;
class QStackedWidget;
class QComboBox;
class QLabel;
class QWidget;

// Minimal modal dialog collecting connection parameters for any of the
// three supported protocols (SFTP, FTP, FTPS). Deliberately dumb — no
// validation beyond "port is a number" and "host isn't empty".
//
// For SFTP, supports password or public-key auth, toggled via radio
// buttons; the two credential field sets (password vs. key path +
// passphrase) are swapped via a QStackedWidget so only the relevant
// fields are ever visible. For FTP and FTPS the auth choice is hidden
// entirely rather than shown-and-ignored, because neither has key-based
// auth — see supportsKeyAuth() in Protocol.h.
class ConnectionDialog : public QDialog {
    Q_OBJECT
public:
    explicit ConnectionDialog(QWidget *parent = nullptr);

    // Bundles the protocol and its matching credentials into one struct
    // rather than exposing each field as its own getter — MainWindow
    // switches on the protocol once and hands the right half straight to
    // the right backend, with no unpacking and repacking at the call site.
    ConnectionRequest connectionRequest() const;

    // Exposed for testing: protocol-dependent UI behavior (which fields
    // are visible, what the port resets to) is real logic worth
    // verifying, and driving it through the public setter is far more
    // honest than re-implementing the rules in the test.
    void setProtocol(Protocol protocol);
    Protocol protocol() const;

private slots:
    void browseForPrivateKey();
    void updateAuthFieldsVisibility();
    void onProtocolChanged();

private:
    // True when the port box still holds the default for the protocol
    // that was selected a moment ago — i.e. the user hasn't typed their
    // own port. Only then is it safe to auto-update the port on a
    // protocol switch; overwriting a deliberately-chosen port would be a
    // small but real data-loss bug.
    bool portIsUntouchedDefault() const;

    QComboBox *m_protocolCombo;
    QLineEdit *m_hostEdit;
    QSpinBox *m_portSpin;
    QLineEdit *m_usernameEdit;

    QRadioButton *m_passwordAuthRadio;
    QRadioButton *m_keyAuthRadio;
    // The whole "Authentication:" form row, kept as members so it can be
    // hidden wholesale for FTP/FTPS. Hiding the radios alone would leave
    // a stranded label behind.
    QWidget *m_authRowWidget;
    QLabel *m_authRowLabel;

    QStackedWidget *m_authFieldsStack;
    QLineEdit *m_passwordEdit;                 // page 0
    QLineEdit *m_privateKeyPathEdit;            // page 1
    QLineEdit *m_passphraseEdit;                // page 1
};
