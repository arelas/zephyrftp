#pragma once

#include <QDialog>

class QCheckBox;
class QComboBox;
class QLineEdit;
class QSpinBox;
class AppSettings;

// A small preferences dialog — deliberately no OK/Cancel/Apply: every
// field persists to AppSettings the instant it changes (same
// immediate-persist convention SiteManagerDialog already uses, "no
// separate Save step to forget"), so the only button is Close. Whatever
// is checked/selected when this dialog opens already reflects real,
// current, on-disk state.
class PreferencesDialog : public QDialog {
    Q_OBJECT
public:
    explicit PreferencesDialog(AppSettings *settings, QWidget *parent = nullptr);

private:
    void updateProxyFieldsEnabled();

    AppSettings *m_settings;
    QCheckBox *m_showHiddenFilesCheck;
    QComboBox *m_defaultProtocolCombo;
    QLineEdit *m_externalEditorCommandEdit;

    // A single global proxy applied to every SFTP/FTP/FTPS connection —
    // see AppSettings::resolvedProxyConfig(). Host/port/username/
    // password are disabled while type is None (updateProxyFieldsEnabled()),
    // same "grey out what doesn't apply" idea ConnectionDialog's own
    // auth fields already use.
    QComboBox *m_proxyTypeCombo;
    QLineEdit *m_proxyHostEdit;
    QSpinBox *m_proxyPortSpin;
    QLineEdit *m_proxyUsernameEdit;
    QLineEdit *m_proxyPasswordEdit;
};
