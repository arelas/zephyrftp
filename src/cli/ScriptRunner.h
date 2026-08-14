#pragma once

#include <QObject>
#include <QList>
#include <functional>
#include "ScriptCommand.h"

class FilePaneWidget;
class RemoteBackend;
class TransferManager;
class AppSettings;
class CompareSyncExecutor;
struct CompareEntry;

// Executes a parsed script (see ScriptParser.h) sequentially — one
// command dispatches its async work and waits for its own completion
// signal(s) before the next one starts, the same "chain to the next
// step only once the previous one's callback actually fires" discipline
// this codebase's own tests already use (never independent fixed-delay
// timers).
//
// Owns two FilePaneWidgets internally exactly the way MainWindow does
// (both start on a fresh LocalBackend; TransferManager::enqueue() and
// friends require real FilePaneWidget* — not incidentally, see
// TransferManager.h's own doc comments — so this reuses that existing,
// already-tested machinery rather than adding new backend-only overloads).
// m_localPane always stays local; m_remotePane gets its real backend
// swapped in by `open` (SftpBackend/FtpBackend, exactly like
// MainWindow::startConnection()'s own switch/moveToThread/setBackend
// sequence) or by attachRemotePaneForTesting() in tests.
//
// Trust verification: `open` constructs SftpBackend/FtpBackend with a
// null HostKeyVerifier*/CertificateVerifier* — confirmed by reading
// SftpBackend::verifyHostKey()/askUserToTrustHostKey() and FtpBackend::
// askUserToTrustCertificate() directly: a host/cert already trusted
// (present and matching in the same known_hosts/known_certs.json the
// GUI populates) never calls into the verifier at all, so it connects
// silently; anything unknown or changed fails safe (verifier returns
// false when null), producing a clean connectionFailed rather than a
// hang or a silent trust. No new verifier class needed — this is the
// existing, already-tested contract, just exercised with a null
// verifier the same way it already handles one being absent.
class ScriptRunner : public QObject {
    Q_OBJECT
public:
    explicit ScriptRunner(QObject *parent = nullptr);

    void run(const QList<ScriptCommand> &commands);

    // Test-only: skips `open`'s SiteStore/CredentialStore/network path
    // and directly attaches an already-constructed backend (e.g. a
    // second LocalBackend) to the remote pane — so the required test
    // suite never depends on a real OS keyring or network connection,
    // the same reasoning verify-credential-store is a live-only target
    // rather than part of the required suite. Safe to call immediately
    // before run() with no manual waiting — run() itself waits for both
    // panes' own initial navigation (see its own comment) before the
    // first script command executes, exactly like it would need to for
    // a real `open` command's remote pane too.
    void attachRemotePaneForTesting(RemoteBackend *backend);

    FilePaneWidget *remotePaneForTesting() const { return m_remotePane; }
    FilePaneWidget *localPaneForTesting() const { return m_localPane; }

signals:
    // 0 = ran to completion (explicit exit/bye or end of script).
    // 1 = reserved for a script parse failure (set by the caller before
    // run() is ever invoked — ScriptRunner itself only ever emits 0 or 2).
    // 2 = a command failed at runtime; the script aborts at the first
    // failure (no --continue-on-error in v1).
    void finished(int exitCode);

    // One line of human-readable progress/output (echo text, ls/lls
    // listings, error messages) — kept separate from qDebug() so a
    // script's own output isn't tangled with this app's existing
    // debug-log conventions. The CLI entry point connects this straight
    // to stdout/stderr.
    void output(const QString &line);

private:
    void executeNext();
    void advance();                        // ++m_index, then executeNext()
    void fail(const QString &message);     // emits output() + finished(2), once
    void succeed();                        // emits finished(0), once

    // A script's rm/mkdir/mv/cd/lcd arguments are typed relative to
    // whatever "current directory" cd/lcd last established (the natural
    // CLI-tool expectation), same as a bare filename FilePaneWidget's own
    // right-click actions already resolve against `directory` before
    // ever reaching a backend call — the backend API itself has no
    // concept of "current directory" of its own, everything it's given
    // must already be absolute. An argument starting with '/' is passed
    // through unchanged (already absolute).
    QString resolvePath(FilePaneWidget *pane, const QString &path) const;

    void runOpen(const ScriptCommand &cmd);
    void runCd(const ScriptCommand &cmd);
    void runLcd(const ScriptCommand &cmd);
    void runGet(const ScriptCommand &cmd);
    void runPut(const ScriptCommand &cmd);
    void runLs(const ScriptCommand &cmd);
    void runLls(const ScriptCommand &cmd);
    void runRm(const ScriptCommand &cmd);
    void runMkdir(const ScriptCommand &cmd);
    void runMv(const ScriptCommand &cmd);
    void runMirror(const ScriptCommand &cmd);
    void onMirrorCompareFinished(const QList<CompareEntry> &results, bool withDelete);
    // Runs the delete phase (if any) and only then advances — see its
    // own .cpp comment for why this needs its own completion barrier,
    // separate from the copy phase's.
    void runMirrorDeletes(CompareSyncExecutor *executor, const QList<CompareEntry> &toDelete);
    void runEcho(const ScriptCommand &cmd);

    // One-shot wait for `pane`'s own directoryChanged (success) or
    // backendToWatch's connectionFailed (failure) — used by open (where
    // backendToWatch is the NEW backend being attached, not yet
    // pane->backend() at the time this is called) and cd/lcd (where
    // backendToWatch is simply pane->backend(), unchanged). Both
    // connections are torn down the moment either fires.
    void waitForNavigation(FilePaneWidget *pane, RemoteBackend *backendToWatch,
                            const std::function<void(bool ok, QString reason)> &onDone);

    // One-shot wait for backend's fire-and-refresh contract
    // (directoryListed = success, fileOperationFailed = failure) — used
    // by rm/mkdir/mv, the same contract FilePaneWidget's own
    // confirmAndDelete()/promptAndCreateFolder()/promptAndRename() rely
    // on, just without a UI element to update afterward.
    void waitForFileOp(RemoteBackend *backend, const std::function<void(bool ok, QString reason)> &onDone);

    // One-shot wait for a specific checkExists() request.
    void checkPathExists(RemoteBackend *backend, const QString &path,
                          const std::function<void(bool exists, bool isDir)> &onDone);

    // One-shot wait for a specific TransferItem (by id) to reach a
    // terminal status.
    void waitForTransferItem(int itemId, const std::function<void(bool success, QString reason)> &onDone);

    void printEntries(RemoteBackend *backend, const QString &path, bool isLocal);

    QList<ScriptCommand> m_commands;
    int m_index = 0;
    bool m_finished = false;

    FilePaneWidget *m_localPane = nullptr;
    FilePaneWidget *m_remotePane = nullptr;
    TransferManager *m_transferManager = nullptr;
    AppSettings *m_settings = nullptr;

    int m_nextRequestId = 1;   // for checkExists() correlation
};
