#pragma once

#include <QString>
#include "ProxyConfig.h"

enum class SftpAuthMethod {
    Password,
    PublicKey
};

struct SftpCredentials {
    QString host;
    int port = 22;
    QString username;

    SftpAuthMethod authMethod = SftpAuthMethod::Password;

    QString password;        // used when authMethod == Password
    QString privateKeyPath;  // used when authMethod == PublicKey
    QString passphrase;      // optional, used when authMethod == PublicKey

    // Where the remote pane lands right after connecting. Defaults to
    // true/empty, meaning "resolve the server's home directory" — the
    // existing behavior, unchanged for anything that doesn't set these
    // explicitly (e.g. ConnectionDialog's one-off connections).
    bool useHomeDirectory = true;
    QString startingDirectory;   // only meaningful when useHomeDirectory == false

    // Default-constructed (ProxyType::None) means "connect directly" —
    // every existing construction site is unaffected. Populated from
    // AppSettings::resolvedProxyConfig() by MainWindow::startConnection()
    // right before the backend is constructed; see SftpBackend::
    // ensureSession() for where it's actually applied.
    ProxyConfig proxy;
};
