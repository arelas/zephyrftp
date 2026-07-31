#pragma once

#include <QString>

// Which wire protocol a connection speaks. Introduced when FTP/FTPS was
// wired up to the UI: before that, "remote" and "SFTP" were the same
// thing everywhere above the backend layer, and the distinction didn't
// need to exist outside FtpBackend's own files.
//
// Ftp and Ftps are separate enum values rather than one Ftp value plus a
// bool because the user picks exactly one of three things in a combo
// box, and collapsing two of them into a flag would mean every call site
// reconstructing which of the three was meant. FtpCredentials still
// carries its own FtpsMode — protocolToFtpsMode() below is the single
// place that translation happens, so the redundancy stays contained.
enum class Protocol {
    Sftp,
    Ftp,
    Ftps
};

// The port each protocol conventionally uses. Explicit FTPS deliberately
// returns 21, not 990: 990 is *implicit* FTPS (TLS from the first byte),
// which this app doesn't implement — see FtpCredentials.h. Explicit FTPS
// connects on the normal FTP port and upgrades in place.
inline int defaultPortFor(Protocol protocol)
{
    switch (protocol) {
    case Protocol::Sftp: return 22;
    case Protocol::Ftp:  return 21;
    case Protocol::Ftps: return 21;
    }
    return 22;
}

// Shown in the protocol combo box, and in the connection dialog's title.
inline QString displayNameFor(Protocol protocol)
{
    switch (protocol) {
    case Protocol::Sftp: return QStringLiteral("SFTP (SSH File Transfer Protocol)");
    case Protocol::Ftp:  return QStringLiteral("FTP (unencrypted)");
    case Protocol::Ftps: return QStringLiteral("FTPS (FTP with explicit TLS)");
    }
    return QStringLiteral("SFTP (SSH File Transfer Protocol)");
}

// Only SFTP supports key-based authentication; FTP and FTPS authenticate
// with a username and password over the control connection, full stop.
// The UI uses this to hide the auth-method choice entirely rather than
// offering a key option that would silently do nothing.
inline bool supportsKeyAuth(Protocol protocol)
{
    return protocol == Protocol::Sftp;
}

// Stable string keys for JSON persistence — deliberately not the raw
// enum's integer values, which would silently change meaning if anyone
// ever reordered the enum.
inline QString protocolToKey(Protocol protocol)
{
    switch (protocol) {
    case Protocol::Sftp: return QStringLiteral("sftp");
    case Protocol::Ftp:  return QStringLiteral("ftp");
    case Protocol::Ftps: return QStringLiteral("ftps");
    }
    return QStringLiteral("sftp");
}

// Anything unrecognized — including the key being absent entirely, which
// is exactly what every sites.json written before this field existed
// looks like — reads back as SFTP. That's the correct migration: every
// previously-saved site was necessarily an SFTP site, since nothing else
// could be saved.
inline Protocol protocolFromKey(const QString &key)
{
    if (key == QStringLiteral("ftp"))  return Protocol::Ftp;
    if (key == QStringLiteral("ftps")) return Protocol::Ftps;
    return Protocol::Sftp;
}
