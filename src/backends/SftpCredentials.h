#pragma once

#include <QString>

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
};
