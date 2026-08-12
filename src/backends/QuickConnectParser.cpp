#include "QuickConnectParser.h"

namespace {

QuickConnectResult fail(const QString &message)
{
    QuickConnectResult result;
    result.ok = false;
    result.errorMessage = message;
    return result;
}

}

QuickConnectResult parseQuickConnectString(const QString &input, Protocol defaultProtocol)
{
    QString remainder = input.trimmed();
    if (remainder.isEmpty())
        return fail(QStringLiteral("Enter a host to connect to."));

    Protocol protocol = defaultProtocol;
    const int schemeIndex = remainder.indexOf(QStringLiteral("://"));
    if (schemeIndex >= 0) {
        const QString scheme = remainder.left(schemeIndex).toLower();
        if (scheme == QStringLiteral("sftp"))
            protocol = Protocol::Sftp;
        else if (scheme == QStringLiteral("ftp"))
            protocol = Protocol::Ftp;
        else if (scheme == QStringLiteral("ftps"))
            protocol = Protocol::Ftps;
        else
            return fail(QStringLiteral("Unknown protocol \"%1\" — use sftp://, ftp://, or ftps://.").arg(scheme));
        remainder = remainder.mid(schemeIndex + 3);
    }

    QString username;
    const int atIndex = remainder.lastIndexOf(QLatin1Char('@'));
    if (atIndex >= 0) {
        username = remainder.left(atIndex);
        remainder = remainder.mid(atIndex + 1);
    }

    QString hostPart = remainder;
    int port = defaultPortFor(protocol);
    const int colonIndex = remainder.lastIndexOf(QLatin1Char(':'));
    if (colonIndex >= 0) {
        hostPart = remainder.left(colonIndex);
        const QString portText = remainder.mid(colonIndex + 1);
        bool portOk = false;
        const int parsedPort = portText.toInt(&portOk);
        if (!portOk || parsedPort < 1 || parsedPort > 65535)
            return fail(QStringLiteral("Invalid port \"%1\".").arg(portText));
        port = parsedPort;
    }

    if (hostPart.isEmpty())
        return fail(QStringLiteral("Enter a host to connect to."));

    QuickConnectResult result;
    result.ok = true;
    result.protocol = protocol;
    result.host = hostPart;
    result.port = port;
    result.username = username;
    return result;
}
