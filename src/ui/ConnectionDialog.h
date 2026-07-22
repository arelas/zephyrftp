#pragma once

#include <QDialog>

class QLineEdit;
class QSpinBox;

// Minimal modal dialog collecting SFTP connection parameters. Deliberately
// dumb — no validation beyond "port is a number", no saved-site list yet.
// That's the next obvious feature once this path is proven out.
class ConnectionDialog : public QDialog {
    Q_OBJECT
public:
    explicit ConnectionDialog(QWidget *parent = nullptr);

    QString host() const;
    int port() const;
    QString username() const;
    QString password() const;

private:
    QLineEdit *m_hostEdit;
    QSpinBox *m_portSpin;
    QLineEdit *m_usernameEdit;
    QLineEdit *m_passwordEdit;
};
