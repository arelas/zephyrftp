#pragma once

#include <QString>

// Which wire protocol a connection speaks. Introduced when FTP/FTPS was
// wired up to the UI: before that, "remote" and "SFTP" were the same
// thing everywhere above the backend layer, and the distinction didn't
// need to exist outside FtpBackend's own files.
//
// Ftp, Ftps, and FtpsImplicit are separate enum values rather than one
// Ftp value plus flags because the user picks exactly one of four things
// in a combo box, and collapsing any of them into a flag would mean
// every call site reconstructing which was meant. FtpCredentials still
// carries its own FtpsMode — protocolToFtpsMode() below is the single
// place that translation happens, so the redundancy stays contained.
enum class Protocol {
    Sftp,
    Ftp,
    Ftps,
    // TLS from the very first byte of the control connection, on its
    // own conventional port (990) — as opposed to Ftps above, which
    // connects on the plain port and upgrades in place via "AUTH TLS".
    // Legacy, lower real-world demand than explicit FTPS (FileZilla
    // also defaults to explicit), but a real, still-encountered mode
    // some older/embedded FTP servers only offer. See FtpBackend.cpp's
    // ensureConnected() for exactly where the two modes' connection
    // sequences diverge.
    FtpsImplicit
};

// The port each protocol conventionally uses. Explicit FTPS returns 21
// (the normal FTP port, upgraded in place via "AUTH TLS"); implicit
// FTPS returns 990, its own historically separate, TLS-from-the-start
// port.
inline int defaultPortFor(Protocol protocol)
{
    switch (protocol) {
    case Protocol::Sftp:         return 22;
    case Protocol::Ftp:          return 21;
    case Protocol::Ftps:         return 21;
    case Protocol::FtpsImplicit: return 990;
    }
    return 22;
}

// Shown in the protocol combo box, and in the connection dialog's title.
inline QString displayNameFor(Protocol protocol)
{
    switch (protocol) {
    case Protocol::Sftp:         return QStringLiteral("SFTP (SSH File Transfer Protocol)");
    case Protocol::Ftp:          return QStringLiteral("FTP (unencrypted)");
    case Protocol::Ftps:         return QStringLiteral("FTPS (FTP with explicit TLS)");
    case Protocol::FtpsImplicit: return QStringLiteral("FTPS (Implicit)");
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
    case Protocol::Sftp:         return QStringLiteral("sftp");
    case Protocol::Ftp:          return QStringLiteral("ftp");
    case Protocol::Ftps:         return QStringLiteral("ftps");
    case Protocol::FtpsImplicit: return QStringLiteral("ftps-implicit");
    }
    return QStringLiteral("sftp");
}

// Anything unrecognized — including the key being absent entirely, which
// is exactly what every sites.json written before this field existed
// looks like — reads back as SFTP. That's the correct migration: every
// previously-saved site was necessarily an SFTP site, since nothing else
// could be saved. Also what happens if a NEWER file (containing
// "ftps-implicit") is ever read by an OLDER build that doesn't
// recognize that key yet — same graceful degradation, not a crash or
// data loss, just a silent downgrade to SFTP for that one site/entry.
inline Protocol protocolFromKey(const QString &key)
{
    if (key == QStringLiteral("ftp"))           return Protocol::Ftp;
    if (key == QStringLiteral("ftps"))          return Protocol::Ftps;
    if (key == QStringLiteral("ftps-implicit")) return Protocol::FtpsImplicit;
    return Protocol::Sftp;
}
