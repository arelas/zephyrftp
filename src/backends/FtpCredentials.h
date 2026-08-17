#pragma once

#include <QString>
#include "ProxyConfig.h"

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
// to this) and is the mode most servers actually offer.
// Implicit: TLS from the very first byte of the control connection, on
// its own conventional port (990 by default) — no "AUTH TLS" command at
// all, since the server never expects a plaintext phase. Legacy, lower
// real-world demand than Explicit, but still genuinely offered by some
// older/embedded FTP servers. "PBSZ 0"/"PROT P" for the data channel
// are still required identically to Explicit — RFC 4217 doesn't
// distinguish there. See FtpBackend::ensureConnected() for exactly
// where the two modes' connection sequences diverge, and
// FtpBackend::usesTls() for the "either encrypted mode" check every
// TLS-vs-plain decision in that class goes through.
enum class FtpsMode {
    None,
    Explicit,
    Implicit
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

    // Same meaning as SftpCredentials::proxy — see that struct's
    // comment. Applied to both the control connection and PASV data
    // connections (see FtpBackend::ensureConnected()/openDataChannel());
    // active/PORT data connections can't be proxied (they're a listen
    // socket waiting for the server to dial back), a pre-existing
    // limitation of active mode, not a new one.
    ProxyConfig proxy;

    // Same meaning as SftpCredentials::bandwidthLimitKBps — see that
    // struct's comment. Applied per-transfer in
    // FtpBackend::downloadFile()/uploadFile().
    int bandwidthLimitKBps = 0;

    // Same meaning as SftpCredentials::simultaneousConnections — see
    // that struct's comment.
    int simultaneousConnections = 1;
};
