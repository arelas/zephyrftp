#pragma once

#include <QString>
#include "Protocol.h"

// Identifying (never secret) fields describing which connection a
// FilePaneWidget currently has open, or which one a persisted transfer
// queue item is waiting to reconnect to (see TransferQueueStore). Never
// carries a password/passphrase — those live only in SftpCredentials/
// FtpCredentials, which this struct deliberately has no relationship to.
//
// savedSiteId is empty for a connection made through the plain
// ConnectionDialog (no SavedSite involved) — protocol/host/port/username
// are always populated either way and are what TransferManager actually
// matches a reconnected pane against (see
// TransferManager::tryReclaimPendingItems()); savedSiteId is used only
// when present, to show a saved site's friendly name instead of a bare
// host in the UI.
struct ConnectionDescriptor {
    QString savedSiteId;
    Protocol protocol = Protocol::Sftp;
    QString host;
    int port = 0;
    QString username;

    bool isEmpty() const { return host.isEmpty(); }
};
