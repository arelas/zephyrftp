#include "ScriptRunner.h"

#include <QThread>
#include <QMetaObject>
#include <QFileInfo>
#include <QSet>

#include "../ui/FilePaneWidget.h"
#include "../AppSettings.h"
#include "../backends/RemoteBackend.h"
#include "../backends/LocalBackend.h"
#include "../backends/SftpBackend.h"
#include "../backends/FtpBackend.h"
#include "../backends/SavedSite.h"
#include "../backends/CredentialStore.h"
#include "../backends/ConnectionRequest.h"
#include "../transfer/TransferManager.h"
#include "../transfer/TransferItem.h"
#include "../compare/DirectoryComparer.h"
#include "../compare/CompareSyncExecutor.h"
#include "../PathUtils.h"

QString ScriptRunner::resolvePath(FilePaneWidget *pane, const QString &path) const
{
    return path.startsWith(QLatin1Char('/')) ? path : joinPath(pane->currentDirectory(), path);
}

ScriptRunner::ScriptRunner(QObject *parent)
    : QObject(parent)
    , m_transferManager(new TransferManager(this))
    , m_settings(new AppSettings(this))
{
    // Both panes start on a fresh LocalBackend, exactly like MainWindow's
    // own two panes do — m_remotePane's backend gets swapped out by
    // `open` (or attachRemotePaneForTesting()) via the pane's existing
    // setBackend(), never by constructing a second FilePaneWidget.
    m_localPane = new FilePaneWidget(new LocalBackend(), nullptr, m_settings);
    m_remotePane = new FilePaneWidget(new LocalBackend(), nullptr, m_settings);
}

void ScriptRunner::attachRemotePaneForTesting(RemoteBackend *backend)
{
    m_remotePane->setBackend(backend, nullptr);
}

void ScriptRunner::run(const QList<ScriptCommand> &commands)
{
    m_commands = commands;
    m_index = 0;

    if (m_commands.isEmpty()) {
        succeed();
        return;
    }

    // Wait for BOTH panes' own initial navigation (to their starting
    // directory) before the first script command runs — same discipline
    // every other command already follows, never assumed-ready by
    // timing. m_remotePane needs this too even though `open` hasn't run
    // yet: it starts on a placeholder LocalBackend exactly like
    // m_localPane (see the constructor), which does the identical async
    // connect-then-navigate-home dance — a script command that touches
    // m_remotePane before `open` (or one that never calls `open` at
    // all, using the remote pane as a second local location) would
    // otherwise race that still-in-flight initial navigation.
    waitForNavigation(m_localPane, m_localPane->backend(), [this](bool ok, QString reason) {
        if (!ok) {
            fail(QStringLiteral("could not initialize local directory: %1").arg(reason));
            return;
        }
        waitForNavigation(m_remotePane, m_remotePane->backend(), [this](bool ok2, QString reason2) {
            if (!ok2) {
                fail(QStringLiteral("could not initialize remote pane: %1").arg(reason2));
                return;
            }
            executeNext();
        });
    });
}

void ScriptRunner::advance()
{
    ++m_index;
    executeNext();
}

void ScriptRunner::fail(const QString &message)
{
    if (m_finished)
        return;
    m_finished = true;
    emit output(QStringLiteral("error: %1").arg(message));
    emit finished(2);
}

void ScriptRunner::succeed()
{
    if (m_finished)
        return;
    m_finished = true;
    emit finished(0);
}

void ScriptRunner::executeNext()
{
    if (m_finished)
        return;
    if (m_index >= m_commands.size()) {
        succeed();
        return;
    }

    const ScriptCommand cmd = m_commands.at(m_index);
    switch (cmd.type) {
    case ScriptCommandType::Open:   runOpen(cmd); break;
    case ScriptCommandType::Cd:     runCd(cmd); break;
    case ScriptCommandType::Lcd:    runLcd(cmd); break;
    case ScriptCommandType::Get:    runGet(cmd); break;
    case ScriptCommandType::Put:    runPut(cmd); break;
    case ScriptCommandType::Ls:     runLs(cmd); break;
    case ScriptCommandType::Lls:    runLls(cmd); break;
    case ScriptCommandType::Rm:     runRm(cmd); break;
    case ScriptCommandType::Mkdir:  runMkdir(cmd); break;
    case ScriptCommandType::Mv:     runMv(cmd); break;
    case ScriptCommandType::Mirror: runMirror(cmd); break;
    case ScriptCommandType::Echo:   runEcho(cmd); break;
    case ScriptCommandType::Exit:   succeed(); break;
    }
}

void ScriptRunner::waitForNavigation(FilePaneWidget *pane, RemoteBackend *backendToWatch,
                                      const std::function<void(bool, QString)> &onDone)
{
    auto successConn = std::make_shared<QMetaObject::Connection>();
    auto failureConn = std::make_shared<QMetaObject::Connection>();

    *successConn = connect(pane, &FilePaneWidget::directoryChanged, this,
                            [onDone, successConn, failureConn](const QString &) {
        QObject::disconnect(*successConn);
        QObject::disconnect(*failureConn);
        onDone(true, QString());
    });
    *failureConn = connect(backendToWatch, &RemoteBackend::connectionFailed, this,
                            [onDone, successConn, failureConn](const QString &reason) {
        QObject::disconnect(*successConn);
        QObject::disconnect(*failureConn);
        onDone(false, reason);
    });
}

void ScriptRunner::waitForFileOp(RemoteBackend *backend, const std::function<void(bool, QString)> &onDone)
{
    auto successConn = std::make_shared<QMetaObject::Connection>();
    auto failureConn = std::make_shared<QMetaObject::Connection>();

    *successConn = connect(backend, &RemoteBackend::directoryListed, this,
                            [onDone, successConn, failureConn](const QString &, const QList<RemoteEntry> &) {
        QObject::disconnect(*successConn);
        QObject::disconnect(*failureConn);
        onDone(true, QString());
    });
    *failureConn = connect(backend, &RemoteBackend::fileOperationFailed, this,
                            [onDone, successConn, failureConn](const QString &, const QString &, const QString &reason) {
        QObject::disconnect(*successConn);
        QObject::disconnect(*failureConn);
        onDone(false, reason);
    });
}

void ScriptRunner::checkPathExists(RemoteBackend *backend, const QString &path,
                                    const std::function<void(bool, bool)> &onDone)
{
    const int requestId = m_nextRequestId++;
    auto conn = std::make_shared<QMetaObject::Connection>();
    *conn = connect(backend, &RemoteBackend::existsChecked, this,
                     [requestId, onDone, conn](const QString &, bool exists, bool isDir, int id) {
        if (id != requestId)
            return;
        QObject::disconnect(*conn);
        onDone(exists, isDir);
    });
    QMetaObject::invokeMethod(backend, "checkExists", Qt::QueuedConnection,
                               Q_ARG(QString, path), Q_ARG(int, requestId));
}

void ScriptRunner::waitForTransferItem(int itemId, const std::function<void(bool, QString)> &onDone)
{
    auto conn = std::make_shared<QMetaObject::Connection>();
    *conn = connect(m_transferManager, &TransferManager::itemUpdated, this,
                     [itemId, onDone, conn](const TransferItem &item) {
        if (item.id != itemId)
            return;
        switch (item.status) {
        case TransferStatus::Done:
            QObject::disconnect(*conn);
            onDone(true, QString());
            break;
        case TransferStatus::Failed:
        case TransferStatus::Cancelled:
        case TransferStatus::Skipped:
            QObject::disconnect(*conn);
            onDone(false, item.errorMessage);
            break;
        default:
            break;   // still in flight (Queued/InProgress/Paused/PendingReconnect) — keep waiting
        }
    });
}

void ScriptRunner::runOpen(const ScriptCommand &cmd)
{
    const QString siteName = cmd.args.at(0);
    const QList<SavedSite> sites = SiteStore::load();
    QList<SavedSite> matches;
    for (const SavedSite &site : sites) {
        if (site.name.compare(siteName, Qt::CaseInsensitive) == 0)
            matches.append(site);
    }
    if (matches.isEmpty()) {
        fail(QStringLiteral("line %1: no saved site named '%2'").arg(cmd.lineNumber).arg(siteName));
        return;
    }
    if (matches.size() > 1) {
        fail(QStringLiteral("line %1: multiple saved sites named '%2' — rename one to disambiguate")
             .arg(cmd.lineNumber).arg(siteName));
        return;
    }

    const SavedSite &site = matches.first();
    ConnectionRequest request = site.toConnectionRequest();

    // Password auth (SFTP or FTP/FTPS) always needs a stored secret — a
    // script cannot prompt. Key auth only needs one if the key is
    // itself passphrase-protected; CredentialStore having nothing stored
    // is the ordinary, expected state for an unprotected key, not an
    // error — SftpBackend's own auth call fails on its own, with a
    // clear connectionFailed reason, if a passphrase actually was needed.
    if (request.protocol == Protocol::Sftp && request.sftp.authMethod == SftpAuthMethod::PublicKey) {
        QString passphrase;
        if (CredentialStore::load(site.id, &passphrase))
            request.sftp.passphrase = passphrase;
    } else {
        QString password;
        if (!CredentialStore::load(site.id, &password)) {
            fail(QStringLiteral("line %1: no stored password for site '%2' — enable \"Save password\" "
                                 "in Site Manager first (scripts cannot prompt)").arg(cmd.lineNumber).arg(siteName));
            return;
        }
        if (request.protocol == Protocol::Sftp)
            request.sftp.password = password;
        else
            request.ftp.password = password;
    }

    // Same construction sequence as MainWindow::startConnection(), minus
    // the interactive verifiers (see this class's own header comment for
    // why passing null here is safe and correct, not a shortcut) and
    // status-bar messages.
    RemoteBackend *backend = nullptr;
    switch (request.protocol) {
    case Protocol::Sftp: {
        SftpCredentials creds = request.sftp;
        creds.proxy = m_settings->resolvedProxyConfig();
        creds.bandwidthLimitKBps = m_settings->bandwidthLimitKBps();
        backend = new SftpBackend(creds, nullptr);
        break;
    }
    case Protocol::Ftp:
    case Protocol::Ftps:
    case Protocol::FtpsImplicit: {
        FtpCredentials creds = request.ftp;
        creds.proxy = m_settings->resolvedProxyConfig();
        creds.bandwidthLimitKBps = m_settings->bandwidthLimitKBps();
        backend = new FtpBackend(creds, nullptr);
        break;
    }
    }

    auto *thread = new QThread(this);
    backend->moveToThread(thread);

    // Connected to the NEW backend directly (not m_remotePane->backend(),
    // which is still the OLD one until setBackend() below actually swaps
    // it) — see waitForNavigation()'s own doc comment.
    waitForNavigation(m_remotePane, backend, [this](bool ok, QString reason) {
        if (!ok) {
            fail(QStringLiteral("open failed: %1").arg(reason));
            return;
        }
        advance();
    });
    m_remotePane->setBackend(backend, thread);
}

void ScriptRunner::runCd(const ScriptCommand &cmd)
{
    waitForNavigation(m_remotePane, m_remotePane->backend(), [this](bool ok, QString reason) {
        if (!ok) {
            fail(QStringLiteral("cd failed: %1").arg(reason));
            return;
        }
        advance();
    });
    m_remotePane->navigateTo(resolvePath(m_remotePane, cmd.args.at(0)));
}

void ScriptRunner::runLcd(const ScriptCommand &cmd)
{
    waitForNavigation(m_localPane, m_localPane->backend(), [this](bool ok, QString reason) {
        if (!ok) {
            fail(QStringLiteral("lcd failed: %1").arg(reason));
            return;
        }
        advance();
    });
    m_localPane->navigateTo(resolvePath(m_localPane, cmd.args.at(0)));
}

void ScriptRunner::runGet(const ScriptCommand &cmd)
{
    const QString fileName = cmd.args.at(0);
    auto conn = std::make_shared<QMetaObject::Connection>();
    *conn = connect(m_transferManager, &TransferManager::itemAdded, this,
                     [this, conn](const TransferItem &item) {
        QObject::disconnect(*conn);
        waitForTransferItem(item.id, [this](bool ok, QString reason) {
            if (!ok) {
                fail(QStringLiteral("get failed: %1").arg(reason));
                return;
            }
            advance();
        });
    });
    m_transferManager->enqueue(m_remotePane, m_localPane, fileName);
}

void ScriptRunner::runPut(const ScriptCommand &cmd)
{
    const QString fileName = cmd.args.at(0);
    auto conn = std::make_shared<QMetaObject::Connection>();
    *conn = connect(m_transferManager, &TransferManager::itemAdded, this,
                     [this, conn](const TransferItem &item) {
        QObject::disconnect(*conn);
        waitForTransferItem(item.id, [this](bool ok, QString reason) {
            if (!ok) {
                fail(QStringLiteral("put failed: %1").arg(reason));
                return;
            }
            advance();
        });
    });
    m_transferManager->enqueue(m_localPane, m_remotePane, fileName);
}

void ScriptRunner::printEntries(RemoteBackend *backend, const QString &path, bool isLocal)
{
    auto conn = std::make_shared<QMetaObject::Connection>();
    *conn = connect(backend, &RemoteBackend::directoryListed, this,
                     [this, conn, isLocal](const QString &, const QList<RemoteEntry> &entries) {
        QObject::disconnect(*conn);
        for (const RemoteEntry &entry : entries) {
            emit output(QStringLiteral("%1\t%2\t%3")
                         .arg(entry.isDir ? QStringLiteral("<dir>") : QString::number(entry.size))
                         .arg(entry.modified.toString(Qt::ISODate))
                         .arg(entry.name));
        }
        advance();
    });
    Q_UNUSED(isLocal);
    QMetaObject::invokeMethod(backend, "listDirectory", Qt::QueuedConnection, Q_ARG(QString, path));
}

void ScriptRunner::runLs(const ScriptCommand &)
{
    printEntries(m_remotePane->backend(), m_remotePane->currentDirectory(), false);
}

void ScriptRunner::runLls(const ScriptCommand &)
{
    printEntries(m_localPane->backend(), m_localPane->currentDirectory(), true);
}

void ScriptRunner::runRm(const ScriptCommand &cmd)
{
    RemoteBackend *backend = m_remotePane->backend();
    const QString path = resolvePath(m_remotePane, cmd.args.at(0));
    checkPathExists(backend, path, [this, backend, path](bool exists, bool isDir) {
        if (!exists) {
            fail(QStringLiteral("rm failed: '%1' does not exist").arg(path));
            return;
        }
        waitForFileOp(backend, [this](bool ok, QString reason) {
            if (!ok) {
                fail(QStringLiteral("rm failed: %1").arg(reason));
                return;
            }
            advance();
        });
        QMetaObject::invokeMethod(backend, "deleteEntry", Qt::QueuedConnection,
                                   Q_ARG(QString, path), Q_ARG(bool, isDir));
    });
}

void ScriptRunner::runMkdir(const ScriptCommand &cmd)
{
    RemoteBackend *backend = m_remotePane->backend();
    waitForFileOp(backend, [this](bool ok, QString reason) {
        if (!ok) {
            fail(QStringLiteral("mkdir failed: %1").arg(reason));
            return;
        }
        advance();
    });
    QMetaObject::invokeMethod(backend, "createDirectory", Qt::QueuedConnection,
                               Q_ARG(QString, resolvePath(m_remotePane, cmd.args.at(0))));
}

void ScriptRunner::runMv(const ScriptCommand &cmd)
{
    RemoteBackend *backend = m_remotePane->backend();
    waitForFileOp(backend, [this](bool ok, QString reason) {
        if (!ok) {
            fail(QStringLiteral("mv failed: %1").arg(reason));
            return;
        }
        advance();
    });
    QMetaObject::invokeMethod(backend, "renameEntry", Qt::QueuedConnection,
                               Q_ARG(QString, resolvePath(m_remotePane, cmd.args.at(0))),
                               Q_ARG(QString, resolvePath(m_remotePane, cmd.args.at(1))));
}

void ScriptRunner::runEcho(const ScriptCommand &cmd)
{
    emit output(cmd.args.isEmpty() ? QString() : cmd.args.at(0));
    advance();
}

void ScriptRunner::runMirrorDeletes(CompareSyncExecutor *executor, const QList<CompareEntry> &toDelete)
{
    if (toDelete.isEmpty()) {
        executor->deleteLater();
        advance();
        return;
    }

    // deleteSelected() -> FilePaneWidget::deleteEntriesAt() issues one
    // QUEUED deleteEntry() call per item (files first, then empty
    // directories bottom-up — see CompareSyncExecutor's own doc comment)
    // — fire-and-forget from the caller's side, same as the GUI's own
    // delete path, which just lets the pane's model refresh whenever
    // each one completes. A script has no UI to eventually refresh: it
    // needs to know for certain every one of those queued deletes has
    // actually run before considering `mirror --delete` done (and
    // definitely before the process might exit on the very next line).
    // Each deleteEntry() resolves via the SAME fire-and-refresh contract
    // as rm/mkdir/mv (directoryListed = success, fileOperationFailed =
    // failure) — counting exactly toDelete.size() of either down to zero
    // is the complete, correct signal that the whole batch has settled.
    RemoteBackend *backend = m_remotePane->backend();
    auto remaining = std::make_shared<int>(toDelete.size());
    auto successConn = std::make_shared<QMetaObject::Connection>();
    auto failureConn = std::make_shared<QMetaObject::Connection>();

    auto settleOne = [this, executor, remaining, successConn, failureConn]() {
        if (--(*remaining) > 0)
            return;
        QObject::disconnect(*successConn);
        QObject::disconnect(*failureConn);
        executor->deleteLater();
        advance();
    };

    *successConn = connect(backend, &RemoteBackend::directoryListed, this,
                            [settleOne](const QString &, const QList<RemoteEntry> &) { settleOne(); });
    *failureConn = connect(backend, &RemoteBackend::fileOperationFailed, this,
                            [settleOne](const QString &, const QString &, const QString &) { settleOne(); });

    executor->deleteSelected(toDelete, CompareSyncExecutor::DeleteSide::Right);
}

void ScriptRunner::onMirrorCompareFinished(const QList<CompareEntry> &results, bool withDelete)
{
    QList<CompareEntry> toCopy;
    QList<CompareEntry> toDelete;
    for (const CompareEntry &entry : results) {
        if (entry.status == CompareStatus::OnlyLeft || entry.status == CompareStatus::Differs)
            toCopy.append(entry);
        else if (withDelete && entry.status == CompareStatus::OnlyRight)
            toDelete.append(entry);
    }

    auto *executor = new CompareSyncExecutor(m_localPane, m_remotePane, m_transferManager, this);

    if (toCopy.isEmpty()) {
        runMirrorDeletes(executor, toDelete);
        return;
    }

    // Completion barrier: capture every item id this copy batch produces
    // (connecting to itemAdded BEFORE calling copySelected(), so nothing
    // is missed), then only proceed once every one of them has reached a
    // terminal status. Unlike the GUI's CompareDialog (where nothing else
    // waits on the transfer dock's own async progress), the NEXT script
    // line must not run until mirror's transfers have genuinely finished.
    auto pending = std::make_shared<QSet<int>>();
    auto addConn = std::make_shared<QMetaObject::Connection>();
    *addConn = connect(m_transferManager, &TransferManager::itemAdded, this,
                        [pending](const TransferItem &item) {
        pending->insert(item.id);
    });

    auto updateConn = std::make_shared<QMetaObject::Connection>();
    *updateConn = connect(m_transferManager, &TransferManager::itemUpdated, this,
                           [this, pending, addConn, updateConn, executor, toDelete]
                           (const TransferItem &item) {
        if (!pending->contains(item.id))
            return;
        switch (item.status) {
        case TransferStatus::Done:
        case TransferStatus::Failed:
        case TransferStatus::Cancelled:
        case TransferStatus::Skipped:
            pending->remove(item.id);
            break;
        default:
            return;   // still in flight
        }
        if (!pending->isEmpty())
            return;

        QObject::disconnect(*addConn);
        QObject::disconnect(*updateConn);
        runMirrorDeletes(executor, toDelete);
    });

    executor->copySelected(toCopy, CompareSyncExecutor::CopyDirection::ToRight);
}

void ScriptRunner::runMirror(const ScriptCommand &cmd)
{
    const QString localDir = resolvePath(m_localPane, cmd.args.at(0));
    const QString remoteDir = resolvePath(m_remotePane, cmd.args.at(1));
    const bool withDelete = (cmd.args.size() == 3);   // parser already validated it's "--delete"

    waitForNavigation(m_localPane, m_localPane->backend(), [this, remoteDir, withDelete](bool ok, QString reason) {
        if (!ok) {
            fail(QStringLiteral("mirror failed (lcd): %1").arg(reason));
            return;
        }
        waitForNavigation(m_remotePane, m_remotePane->backend(), [this, withDelete](bool ok2, QString reason2) {
            if (!ok2) {
                fail(QStringLiteral("mirror failed (cd): %1").arg(reason2));
                return;
            }

            auto *comparer = new DirectoryComparer(m_localPane->backend(), m_localPane->currentDirectory(),
                                                     m_remotePane->backend(), m_remotePane->currentDirectory(),
                                                     this);
            connect(comparer, &DirectoryComparer::failed, this, [this, comparer](const QString &compareReason) {
                comparer->deleteLater();
                fail(QStringLiteral("mirror failed (compare): %1").arg(compareReason));
            });
            connect(comparer, &DirectoryComparer::finished, this,
                    [this, comparer, withDelete](const QList<CompareEntry> &results) {
                comparer->deleteLater();
                onMirrorCompareFinished(results, withDelete);
            });
            comparer->start();
        });
        m_remotePane->navigateTo(remoteDir);
    });
    m_localPane->navigateTo(localDir);
}
