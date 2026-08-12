#pragma once

#include <QString>
#include "Protocol.h"

// Result of parsing a quick-connect toolbar string. Deliberately mirrors
// only what MainWindow::onQuickConnectReturnPressed() needs to build a
// ConnectionRequest — always password auth (see this struct's own
// caller for why: no reasonable way to fit a private-key path into a
// one-line quick-connect string), so authMethod/privateKeyPath aren't
// represented here at all.
struct QuickConnectResult {
    bool ok = false;
    QString errorMessage;   // set only when ok == false
    Protocol protocol = Protocol::Sftp;
    QString host;
    int port = 0;
    QString username;       // may be empty — same as ConnectionDialog's own username field
};

// Parses "[protocol://][user@]host[:port]" — the toolbar quick-connect
// field's accepted syntax. protocol:// is one of sftp/ftp/ftps
// (case-insensitive); if absent, defaultProtocol is used (the caller
// passes AppSettings::defaultProtocol(), the same accessor
// MainWindow::connectViaDialog() already uses to preselect
// ConnectionDialog's own combo). user@ is split on the LAST '@' (the
// URI-userinfo convention) and is optional. :port is split on the LAST
// ':' and is optional, defaulting to defaultPortFor(protocol)
// (Protocol.h) — deliberately no IPv6 literal ([::1]:port) support, a
// documented scope boundary: a plain last-':' split is ambiguous with
// IPv6 and quick-connect isn't the place to solve that.
QuickConnectResult parseQuickConnectString(const QString &input, Protocol defaultProtocol);
