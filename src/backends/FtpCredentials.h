#pragma once

#include <QString>

// Passive mode always tried first — matches what essentially every
// modern FTP client defaults to; active mode routinely fails behind
// NAT/firewalls since it requires the SERVER to open a connection back
// to the client. FtpBackend falls back to active/PORT only if the
// server outright refuses PASV (see FtpBackend.h's class doc comment) —
// a fallback for the rare server that requires it, not a competing mode
// this struct needs to expose a choice between.
//
// None: plain FTP, nothing encrypted — control connection and data both
// travel in the clear.
// Explicit: connects on the plain port (21 by default) and issues
// "AUTH TLS" to upgrade the existing control connection to TLS before
// logging in, then "PBSZ 0" + "PROT P" to also encrypt data connections.
// This is what "FTPS" means in most modern clients (FileZilla defaults
// to this) and is the only encrypted mode this app supports for now —
// implicit FTPS (an entirely separate TLS-from-the-start port,
// historically 990) is legacy and not implemented.
enum class FtpsMode {
    None,
    Explicit
};

struct FtpCredentials {
    QString host;
    int port = 21;
    QString username;
    QString password;   // FTP has no key-based auth — always a password
    FtpsMode ftpsMode = FtpsMode::None;

    // Same meaning and same default as SftpCredentials' identical fields
    // — see that struct's comment.
    bool useHomeDirectory = true;
    QString startingDirectory;
};
