#pragma once

#include "Protocol.h"
#include "SftpCredentials.h"
#include "FtpCredentials.h"

// What the UI hands to MainWindow::startConnection(): a protocol tag plus
// the credential struct that protocol actually needs.
//
// This is a tagged union in spirit but a plain struct in practice — both
// credential members always exist, only the one matching `protocol` is
// populated. A std::variant would express the invariant more precisely,
// but every consumer here already switches on the protocol anyway, and
// the two structs are small value types; the extra strictness wouldn't
// buy anything a reader doesn't already get from the switch. What it
// WOULD cost is readability at the call sites, which is the thing this
// codebase optimizes for.
//
// Why not one merged credentials struct with every field: SftpCredentials
// and FtpCredentials genuinely diverge (key path, passphrase, and auth
// method are meaningless for FTP; FtpsMode is meaningless for SFTP), and
// merging them would produce a struct where roughly half the fields are
// dead depending on a value elsewhere in the same struct — precisely the
// shape that invites someone to read privateKeyPath on an FTP connection
// and get an empty string that means nothing.
struct ConnectionRequest {
    Protocol protocol = Protocol::Sftp;

    SftpCredentials sftp;   // populated when protocol == Sftp
    FtpCredentials ftp;     // populated when protocol == Ftp or Ftps

    // Which SavedSite this request was built from, if any — empty for a
    // one-off connection through the plain ConnectionDialog. Populated by
    // SiteManagerDialog::connectionRequestToConnect() (the only place
    // that already knows which SavedSite the user picked); consumed by
    // MainWindow::startConnection() to build the pane's own
    // ConnectionDescriptor (see TransferManager's queue-persistence
    // reclaim mechanism, which uses it to show a saved site's friendly
    // name instead of a bare host for a pending item still waiting to
    // reconnect).
    QString sourceSiteId;

    // Convenience for status-bar messages and error text, which don't
    // care which protocol produced the host name.
    QString host() const
    {
        return protocol == Protocol::Sftp ? sftp.host : ftp.host;
    }

    // Same convenience as host() above — used by
    // MainWindow::startConnection() to build a ConnectionDescriptor.
    int port() const
    {
        return protocol == Protocol::Sftp ? sftp.port : ftp.port;
    }

    QString username() const
    {
        return protocol == Protocol::Sftp ? sftp.username : ftp.username;
    }

    // The starting-directory pair exists identically in both credential
    // structs, and every caller that validates it does so without caring
    // about the protocol. Exposed here so those call sites don't each
    // have to re-branch on the tag to read a field that means exactly the
    // same thing either way.
    bool useHomeDirectory() const
    {
        return protocol == Protocol::Sftp ? sftp.useHomeDirectory : ftp.useHomeDirectory;
    }

    QString startingDirectory() const
    {
        return protocol == Protocol::Sftp ? sftp.startingDirectory : ftp.startingDirectory;
    }

    // Same convenience as host()/port()/username() above — used by
    // MainWindow::startConnection() to configure the target pane's
    // transfer-connection pool without re-branching on protocol there.
    int simultaneousConnections() const
    {
        return protocol == Protocol::Sftp ? sftp.simultaneousConnections : ftp.simultaneousConnections;
    }

    // True when this request asked for SFTP public-key auth but never
    // actually got a key file — both MainWindow::connectViaDialog() and
    // SiteManagerDialog::onConnectClicked() need to reject exactly this
    // before dispatching a connection attempt that would otherwise fail
    // deep inside SftpBackend with a far less obvious error. A real
    // duplication found by code review had this exact three-part
    // condition copy-pasted at both call sites, with no shared helper —
    // risking one of the two silently drifting if the rule ever changed.
    bool missingRequiredPrivateKeyPath() const
    {
        return protocol == Protocol::Sftp
            && sftp.authMethod == SftpAuthMethod::PublicKey
            && sftp.privateKeyPath.isEmpty();
    }
};

// Explicit TLS for FTPS, none for plain FTP. The single place this
// translation lives — see Protocol.h's comment on why the enum keeps
// them separate.
inline FtpsMode protocolToFtpsMode(Protocol protocol)
{
    return protocol == Protocol::Ftps ? FtpsMode::Explicit : FtpsMode::None;
}
