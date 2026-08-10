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
class QFormLayout;

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
    QComboBox *m_protocolCombo;
    QLineEdit *m_hostEdit;
    QSpinBox *m_portSpin;
    QLineEdit *m_usernameEdit;

    // Kept as a member (rather than a local in the constructor) so
    // onProtocolChanged() can call setRowVisible() on it directly — see
    // that method's own comment.
    QFormLayout *m_form = nullptr;

    QRadioButton *m_passwordAuthRadio;
    QRadioButton *m_keyAuthRadio;
    // The whole "Authentication:" form row, kept as members so it can be
    // hidden wholesale for FTP/FTPS via m_form->setRowVisible() — hiding
    // the radios alone would leave a stranded label behind.
    QWidget *m_authRowWidget;
    QLabel *m_authRowLabel;

    QStackedWidget *m_authFieldsStack;
    QLineEdit *m_passwordEdit;                 // page 0
    QLineEdit *m_privateKeyPathEdit;            // page 1
    QLineEdit *m_passphraseEdit;                // page 1

    // True once the user has genuinely typed their own port — only then
    // does onProtocolChanged() leave the port box alone on a protocol
    // switch; overwriting a deliberately-chosen port would be a small but
    // real data-loss bug. A real bug found by code review: this used to
    // be inferred after the fact via portIsUntouchedDefault() — "is the
    // current value equal to ANY of the three protocols' defaults?" —
    // which can't tell a deliberately-typed 21 (a real, valid port choice
    // under SFTP) apart from an auto-set 21 (FTP/FTPS's shared default),
    // since both look identical once typed. Switching SFTP -> FTP (both
    // land on 21, no visible change) -> SFTP then silently reset the
    // deliberately-typed 21 back to 22. Tracking the real user action
    // directly, via QSpinBox::valueChanged connected here (see the .cpp),
    // removes the ambiguity entirely — every OWN programmatic setValue()
    // call is wrapped in a QSignalBlocker specifically so it can't be
    // mistaken for one.
    bool m_portManuallyEdited = false;
};
