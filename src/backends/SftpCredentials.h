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

    // 0 (default) means unlimited — every existing construction site is
    // unaffected. Populated from AppSettings::bandwidthLimitKBps() by
    // MainWindow::startConnection(), same as proxy above. Applied
    // PER-TRANSFER, not as one shared/global cap — see
    // BandwidthThrottle.h's own class doc comment for why, and
    // SftpBackend::downloadFile()/uploadFile() for where it's actually
    // used. Fixed for the connection's whole lifetime, same as proxy —
    // changing it needs a reconnect, not a live-apply mechanism.
    int bandwidthLimitKBps = 0;
};
