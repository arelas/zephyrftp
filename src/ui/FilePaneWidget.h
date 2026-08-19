#pragma once

#include <QWidget>
#include <QStandardItemModel>
#include <QPoint>
#include <QStringList>
#include <QIcon>
#include <QPair>
#include <QSet>
#include <functional>
#include "../backends/RemoteBackend.h"
#include "../backends/ConnectionDescriptor.h"
#include "../backends/ConnectionHistory.h"
#include "../transfer/FolderEnumerator.h"

class FileTreeView;
class QLineEdit;
class QLabel;
class QThread;
class QAction;
class QToolButton;
class AppSettings;

// One side of the dual-pane layout. Owns a backend (local or remote) and
// renders its current directory listing. MainWindow instantiates two of
// these — deliberately dumb about which backend it holds, since the whole
// point of RemoteBackend is that this class can't tell the difference.
class FilePaneWidget : public QWidget {
    Q_OBJECT
public:
    // settings defaults to nullptr so every existing test-file call site
    // (constructing a pane with just a backend, no live preferences)
    // keeps compiling unchanged; a null settings pointer just means
    // "hide dotfiles, and don't react live to a toggle that can't exist."
    explicit FilePaneWidget(RemoteBackend *backend, QWidget *parent = nullptr,
                             AppSettings *settings = nullptr);

    RemoteBackend *backend() const { return m_backend; }
    QString selectedEntryName() const;
    QString currentDirectory() const { return m_backend->currentPath(); }

    // Identifying (never secret) fields for whichever connection
    // setBackend() most recently attached — set by MainWindow::
    // startConnection() right after it builds the backend, cleared back
    // to a default-constructed (empty) descriptor by
    // MainWindow::disconnectPane(). Used by TransferManager's queue-
    // persistence save/reclaim (see TransferManager::saveQueueForShutdown()/
    // tryReclaimPendingItems()) to describe/match "which server" a
    // pane's connection is, across a restart. Deliberately NOT derived
    // from RemoteBackend::connectionIdentity() after the fact — that
    // string has no savedSiteId component and isn't worth parsing back
    // apart.
    ConnectionDescriptor connectionDescriptor() const { return m_connectionDescriptor; }
    void setConnectionDescriptor(const ConnectionDescriptor &descriptor) { m_connectionDescriptor = descriptor; }

    // True from the moment setBackend() queues a thread-owning backend's
    // connectToHost() until that backend reports connected or
    // connectionFailed. A real bug this exists to let callers guard
    // against: setBackend() tears down the PREVIOUS backend with a
    // blocking QThread::quit()+wait() (see setBackend()'s own comment),
    // but quit() cannot interrupt a worker thread currently blocked
    // inside a synchronous connect()/SSH-handshake syscall — calling
    // setBackend() again on a pane that's still mid-connect (a second
    // Connect click, or Disconnect, before the first attempt resolves)
    // would freeze the whole GUI until that syscall times out on its
    // own. MainWindow checks this before ever calling setBackend() again
    // on the same pane, rather than fixing the underlying blocking I/O
    // itself, which would be a much larger change to SftpBackend/
    // FtpBackend's connection handling.
    bool isConnecting() const { return m_connecting; }

    // Selected rows that are files (not directories). A stale doc
    // comment here was a real bug found by code review: this used to be
    // what FileTreeView::startDrag() and the context menu's "Transfer
    // Selected" action called, but both were switched to
    // selectedEntries() below (to carry isDir, needed once folder drag/
    // transfer shipped) — this method has had zero production callers
    // since, and is kept only because test code still exercises it
    // directly. Still public for that reason, not because anything else
    // needs it live.
    QStringList selectedFileNames() const;

    // Selected rows regardless of type (files AND directories) — unlike
    // selectedFileNames() above. Used by Rename (single-selection only)
    // and Delete (works on any mix of files/folders) in the context
    // menu, where directories are meaningful targets even though they
    // never are for a transfer.
    QList<RemoteEntry> selectedEntries() const;

    // Deletes an arbitrary set of absolute paths (each paired with whether
    // it's a directory), doing the exact same m_pendingFileOpRefreshes
    // bookkeeping + queued deleteEntry() dispatch confirmAndDelete()'s own
    // per-entry loop already does — extracted so a caller whose paths span
    // MANY different directories at once (Compare-and-Sync's delete-extras
    // action, driven by a diff tree rather than one right-clicked
    // directory's own entries) can still route through this pane's
    // "fire and refresh" bookkeeping correctly. Routing matters, not just
    // convenience: a caller invoking deleteEntry() directly on backend()
    // would have each delete's own resulting re-list misread by
    // onDirectoryListed() as a genuine navigation (see
    // m_pendingFileOpRefreshes's own doc comment), corrupting the path bar/
    // history and potentially re-driving the other pane if synchronized
    // browsing happens to be on. No confirmation dialog here — the caller
    // is expected to have already confirmed with the user; this is the
    // dispatch mechanism, not the confirmation UX (see confirmAndDelete(),
    // which now just builds the pairs and calls this after its own dialog).
    //
    // offerRecursiveDeleteOnFailure: when true, every DIRECTORY entry
    // dispatched here is also recorded in m_recursiveDeleteCandidates, so
    // a later "not empty" failure for it triggers offerRecursiveDelete()
    // instead of a plain failure warning. Default false preserves every
    // existing caller unchanged — right now, only CompareSyncExecutor,
    // which deliberately stays precise (it only ever deletes what its
    // own diff determined is actually "extra"; a directory that turns
    // out to still contain something identical on both sides should
    // fail plainly, not be offered a recursive escape hatch that could
    // delete more than the diff intended). confirmAndDelete() is the one
    // caller that opts in.
    void deleteEntriesAt(const QList<QPair<QString, bool>> &pathsAndIsDir,
                          bool offerRecursiveDeleteOnFailure = false);

    void navigateTo(const QString &path);

    // Back/forward/up/home navigation, matching any ordinary file manager.
    // Back/forward walk a per-pane history list; Up computes the parent
    // of the current directory and navigates there; Home returns to
    // wherever this pane's backend first landed after connecting (the
    // OS home directory for LocalBackend, the resolved starting/PWD
    // directory for SFTP/FTP) — none of these require backend support,
    // they're built entirely on top of the existing
    // navigateTo()/currentDirectory().
    void goBack();
    void goForward();
    void goUp();
    void goHome();
    bool canGoBack() const { return m_historyIndex > 0; }
    bool canGoForward() const { return m_historyIndex < m_history.size() - 1; }

    // Computes the parent of a path using '/' as the separator — correct
    // for SFTP paths always (the protocol mandates forward slashes
    // regardless of the server's OS) and for local paths too under Qt's
    // own convention (QDir/QFileInfo normalize to '/' even on Windows).
    // Public specifically so it can be tested directly and precisely —
    // this sandbox is Linux, so a genuine Windows drive path (e.g.
    // "C:/Users") can never be exercised through the full
    // navigateTo()->LocalBackend->onDirectoryListed() stack here (it
    // wouldn't resolve to a real directory); testing the pure string
    // logic directly is the only way to verify Windows drive-root
    // behavior in this environment.
    static QString parentOfPath(const QString &path);

    // Replaces the pane's backend, e.g. swapping the placeholder LocalBackend
    // for a live SftpBackend once a connection dialog succeeds. If `thread`
    // is non-null, ownership of both backend and thread becomes this pane's
    // responsibility: the old pair (if any) is torn down safely — deleteLater
    // on the backend so cleanup runs on its own thread, then quit()/wait() on
    // the thread — before the new pair is wired up and started. Pass
    // thread=nullptr for backends with no thread affinity of their own (e.g.
    // LocalBackend), matching the original constructor's behavior of
    // parenting the backend directly to this widget.
    void setBackend(RemoteBackend *backend, QThread *thread = nullptr);

    // A factory for building one more backend+thread pair identical to
    // whatever setBackend() was just given (same host/credentials/proxy/
    // bandwidth limit) — MainWindow::startConnection() builds this right
    // after its own setBackend() call, using the exact same construction
    // recipe. Returns null pointers on failure (never throws); the
    // caller (pickIdleTransferBackend() below) treats that the same as
    // "pool exhausted."
    using TransferBackendFactory = std::function<QPair<RemoteBackend *, QThread *>()>;

    // Configures this pane's transfer-connection pool: up to maxSize
    // total connections (the primary set by setBackend() counts as one
    // of them) may exist at once, grown lazily via factory only as
    // TransferManager actually asks for one — see
    // pickIdleTransferBackend()'s own doc comment. maxSize <= 1 means
    // "no pooling," today's exact behavior, and the factory is never
    // invoked. Called once per connection, right after setBackend();
    // setBackend() itself clears any previously configured pool (see its
    // own doc comment), since a torn-down connection's factory would
    // just rebuild stale credentials.
    void configureTransferPool(int maxSize, TransferBackendFactory factory);

    // Returns whichever backend among {primary, existing pool members}
    // isClaimed() reports as free, growing the pool by one (via the
    // factory configureTransferPool() was given, up to its maxSize) if
    // every existing member is currently claimed and there's still room
    // to grow. Returns nullptr if the pool is already at its configured
    // cap and every member is busy — the caller (TransferManager)
    // already treats a null/no-available-backend result as "stays
    // Queued," the exact same handling a single busy backend gets today.
    // isClaimed is TransferManager's own isBackendClaimed(), passed in
    // rather than called back into directly, so this pane doesn't need
    // to know anything about TransferManager's internals beyond "can you
    // tell me if a given backend is currently busy."
    RemoteBackend *pickIdleTransferBackend(const std::function<bool(RemoteBackend *)> &isClaimed);

    // True once configureTransferPool() has been given a maxSize above
    // 1 — used by TransferManager::startNextIfLikelyToDispatch()'s own
    // "is a real scan even worth it" pre-check: that heuristic's
    // documented safety invariant (skipping is safe because whatever
    // WOULD make an item dispatchable already re-triggers a real scan
    // on its own) doesn't hold for a pooled pane — growing the pool is
    // a new event type only reachable from inside an actual startNext()
    // scan, not something a backend-release elsewhere re-triggers. This
    // is deliberately a cheap, imprecise "is pooling enabled at all"
    // check, not "is the pool actually not-yet-exhausted right now" —
    // the latter would need calling pickIdleTransferBackend() itself
    // (which has real side effects: it can construct a new backend),
    // wrong to do from a pure pre-check. An occasional real scan that
    // finds the pool genuinely saturated is cheap insurance, not a
    // performance problem worth a more precise (and side-effect-prone)
    // query for.
    bool hasTransferPool() const { return m_maxPoolSize > 1; }

    // Shows/hides the filename-filter row — called by MainWindow's View
    // menu toggle on both panes at once (see AppSettings::
    // filenameFilterVisible's own doc comment). Hiding also clears the
    // filter text (which itself triggers rebuildModel() via the
    // existing textChanged connection) — a hidden field must not leave
    // a stale, invisible filter silently narrowing the list with no
    // visible control left to clear it.
    void setFilterFieldVisible(bool visible);

signals:
    // Emitted on double-click of a file (not directory) — MainWindow wires
    // this to "transfer to the other pane".
    void fileActivated(const QString &name);

    // Emitted from the right-click "Transfer Selected" context-menu
    // action — covers multi-select, unlike double-click which only ever
    // acts on the single row under the cursor. Carries full RemoteEntry
    // (not just names) specifically so isDir survives to MainWindow,
    // which routes files to TransferManager::enqueue() and directories
    // to TransferManager::enqueueFolder() — a mixed selection of both is
    // valid and handled entry-by-entry.
    void filesActivated(const QList<RemoteEntry> &entries);

    // Emitted from the right-click "Move Selected" context-menu action —
    // always emitted when there's a selection, regardless of whether the
    // other pane is actually move-eligible (see
    // TransferManager::moveEligible()); this pane has no way to know
    // what the OTHER pane's backend is, by the same design that keeps
    // filesActivated()/filesDropped() dumb about panes other than
    // itself. MainWindow checks eligibility once the signal arrives and
    // shows its own explanatory message if it isn't met.
    void moveRequested(const QList<RemoteEntry> &entries);

    // Files (and/or folders) dropped ONTO this pane FROM a different
    // pane — forwarded straight through from FileTreeView::filesDroppedFrom.
    // MainWindow connects both panes' filesDropped signals to one slot,
    // using sender() to identify which pane received the drop (the
    // destination) since sourcePane is already carried in the signal
    // itself.
    void filesDropped(FilePaneWidget *sourcePane, const QList<RemoteEntry> &entries);

    // Emitted from onDirectoryListed() on a genuine fresh navigation
    // (double-click, Up, Home, path bar, Back/Forward — every one of
    // them funnels through navigateTo()) — NOT for a delete/rename/
    // create's own same-directory refresh (see
    // m_pendingFileOpRefreshes's own doc comment), which isn't a
    // navigation at all. Used by MainWindow to drive synchronized
    // browsing (see AppSettings::synchronizedBrowsingEnabled()); stays a
    // stable signal across backend swaps, same reasoning as
    // commandLogged below.
    void directoryChanged(const QString &path);

    // Straight passthrough of whatever backend is currently set (see
    // setBackend()) — stays a stable signal across backend swaps
    // (Connect/Disconnect) so MainWindow only has to wire it up once per
    // pane, to the Commands pane's single log, rather than re-wiring it
    // every time a pane's backend changes.
    void commandLogged(const QString &line);

    // "Connect...", "Sites...", "Disconnect" chosen from this pane's own
    // path-bar icon menu (see onPathBarIconClicked()) — this pane's own
    // UI-intent signals, mirroring how fileActivated/filesActivated etc.
    // already work: FilePaneWidget prompts/menus locally but never
    // constructs a concrete backend itself, MainWindow does that. Carries
    // `this` explicitly (rather than relying on sender()) so MainWindow's
    // slots can take the target pane directly as a parameter.
    void connectRequested(FilePaneWidget *pane);
    void siteManagerRequested(FilePaneWidget *pane);
    void disconnectRequested(FilePaneWidget *pane);

    // A "Recent Connections" submenu entry was chosen from the same
    // path-bar icon menu — same "prompt/menu locally, never construct a
    // backend" shape as the three signals above. Carries the full entry
    // (not just an index into the store) so MainWindow doesn't need to
    // re-load/re-derive anything from it.
    void recentConnectionRequested(FilePaneWidget *pane, const ConnectionHistoryEntry &entry);

    // "Edit" chosen from the right-click context menu for a single,
    // non-directory entry on a REMOTE pane (see showContextMenu()'s own
    // enabling logic — never emitted for a local pane or a multi-select).
    // Carries `this` explicitly, same reasoning as connectRequested()
    // above: MainWindow owns the EditSessionManager this routes to, and
    // that manager's API takes the pane directly rather than relying on
    // sender().
    void editRequested(FilePaneWidget *pane, const RemoteEntry &entry);

private slots:
    void onDirectoryListed(const QString &path, const QList<RemoteEntry> &entries);
    void onRowDoubleClicked(const QModelIndex &index);
    void onPathBarReturnPressed();
    void showContextMenu(const QPoint &pos);

    // Opens a small menu (Connect.../Sites.../Disconnect) anchored under
    // the path bar's leading icon — the per-pane equivalent of the global
    // toolbar's Connect/Sites/Disconnect, letting EITHER pane connect, not
    // just whichever one the toolbar happens to target. Deliberately reuses
    // this already-present icon rather than adding a new button/menu —
    // see updatePathBarIcon()'s own comment for why it's a natural fit.
    void onPathBarIconClicked();

    // Connected to the backend's fileOperationFailed signal in
    // setBackend() — surfaces delete/rename/create-file/create-folder
    // failures as a message box, since there's no queue or persistent
    // UI element for these one-shot operations the way there is for
    // transfers.
    void onFileOperationFailed(const QString &operation, const QString &path, const QString &reason);

private:
    void buildUi();

    // Rebuilds the model from m_currentEntries, applying the current
    // showHiddenFiles filter. Separate from onDirectoryListed() so a live
    // preference toggle can re-render instantly without a fresh backend
    // round-trip — m_currentEntries already holds everything the last
    // listDirectory() returned, filtered or not.
    void rebuildModel();

    // Live Dark/Light switching — re-sets the 4 nav buttons' icons
    // (set once in buildUi(), never touched again otherwise) via fresh
    // IconTheme::tintedIcon() calls, then calls rebuildModel() so every
    // currently-displayed row's per-file icon (computed fresh inside it)
    // also picks up the new color immediately. Connected to
    // m_settings->themeChanged in the constructor — this pane doesn't
    // need MainWindow to know to call it.
    void retintIcons();

    // Sets/replaces the path bar's leading icon (device-laptop blue for
    // local, server green for remote) — called from setBackend() since
    // that's the only thing that can change which of the two applies.
    void updatePathBarIcon();

    // Per ICON-MAP.md's file/folder type table. Static since it only
    // depends on the entry's own data (name, isDir), not any pane state.
    static QIcon iconForEntry(const RemoteEntry &e);

    void updateNavigationButtonsEnabled();
    void resetHistory();   // called from setBackend() — a new backend means a fresh navigation context

    // Looks up the RemoteEntry a view row currently displays by matching
    // the name stashed on that row's Name item, rather than indexing
    // m_currentEntries by row position — sorting (see buildUi()'s
    // setSortingEnabled(true)) physically reorders the model's rows, so
    // row position alone no longer identifies which entry is which.
    // Returns nullptr for an out-of-range row or one whose stashed name
    // has no match (shouldn't happen; m_currentEntries and the model are
    // always rebuilt together in rebuildModel()).
    const RemoteEntry *entryForRow(int row) const;

    // Each prompts (or confirms, for delete) then dispatches to the
    // backend via QMetaObject::invokeMethod(..., Qt::QueuedConnection) —
    // same pattern as every other cross-thread-safe backend call in this
    // class. All four are no-ops if the person cancels the prompt/dialog,
    // or (rename/delete) if nothing appropriate is selected. `directory`
    // is always showContextMenu()'s own snapshot, taken before its
    // menu.exec() — see that function's own doc comment for why these
    // must NOT call currentDirectory() themselves after their own modal
    // dialog closes.
    void promptAndCreateFile(const QString &directory);
    void promptAndCreateFolder(const QString &directory);
    void promptAndRename(const RemoteEntry &entry, const QString &directory);
    void promptAndChmod(const RemoteEntry &entry, const QString &directory);
    void confirmAndDelete(const QList<RemoteEntry> &entries, const QString &directory);

    RemoteBackend *m_backend;
    QThread *m_backendThread = nullptr;   // null when backend has no thread of its own (e.g. LocalBackend)

    // Extra connections to the SAME server as m_backend, for
    // TransferManager to dispatch transfers through in parallel — see
    // pickIdleTransferBackend()'s own doc comment. m_backend itself is
    // NOT included in these lists; it's always the first backend
    // pickIdleTransferBackend() tries. Empty/1 for every pane that
    // hasn't opted into pooling (the default), which is the whole point
    // — zero extra state for the common case.
    QList<RemoteBackend *> m_poolBackends;
    QList<QThread *> m_poolThreads;
    int m_maxPoolSize = 1;
    TransferBackendFactory m_poolBackendFactory;
    bool m_connecting = false;   // see isConnecting()'s own doc comment
    // Default-constructed (isEmpty() == true) until MainWindow's
    // startConnection() sets a real one — see connectionDescriptor()'s
    // own doc comment above.
    ConnectionDescriptor m_connectionDescriptor;
    FileTreeView *m_view;
    QToolButton *m_backButton;
    QToolButton *m_forwardButton;
    QToolButton *m_upButton;
    QToolButton *m_homeButton;
    QToolButton *m_refreshButton;
    QLineEdit *m_pathBar;
    QAction *m_pathBarLeadingIcon = nullptr;   // owned by m_pathBar once added; tracked so it can be replaced
    // Case-insensitive substring filter by name — composes with
    // showHiddenFiles in rebuildModel()'s own filter loop, not a
    // separate pass (see that method's own comment). Own per-pane text;
    // visibility is shared across both panes via setFilterFieldVisible().
    QLineEdit *m_filterEdit;
    QLabel *m_statusLabel;
    QStandardItemModel *m_model;
    // The filtered set currently rendered in m_model — NOT necessarily in
    // the same row order the view is displaying once a column has been
    // sorted (see setSortingEnabled(true) in buildUi()), so lookups by
    // view row go through entryForRow() rather than indexing this
    // directly by row position.
    QList<RemoteEntry> m_currentEntries;
    // Everything the last listDirectory() call actually returned, before
    // the showHiddenFiles filter — kept separately so a live preference
    // toggle can re-filter and redraw instantly via rebuildModel(),
    // without a fresh (and pointless) round-trip back to the backend.
    QList<RemoteEntry> m_lastRawEntries;

    // Which directory m_lastRawEntries/m_currentEntries currently
    // represent — set in rebuildModel() itself, compared against
    // currentDirectory() each time it runs. A real bug found by code
    // review: rebuildModel()'s selection-restore-by-name (see its own
    // doc comment on why that's needed at all) matched purely by name
    // with no directory check, on the assumption that an old name
    // "essentially never" matches an unrelated directory's listing — but
    // common filenames (README.md, .gitignore, index.js, __init__.py)
    // genuinely do, silently auto-selecting a same-named file in a
    // directory the user just navigated into, with no user action.
    // Restoring the selection now requires the directory to be
    // unchanged since the previous rebuild (a settings toggle, a
    // file-op's own refresh, or a Refresh/re-navigation to the SAME
    // path all correctly still restore it — only landing on a
    // DIFFERENT directory doesn't).
    QString m_entriesDirectory;

    // Navigation history. m_historyIndex points at the currently-displayed
    // entry; entries after it are "forward" history, cleared whenever a
    // fresh (non back/forward) navigation happens, same convention every
    // browser and file manager uses. m_navigatingHistory guards
    // onDirectoryListed() so a goBack()/goForward()-triggered listing
    // updates the index instead of pushing a new history entry.
    QStringList m_history;
    int m_historyIndex = -1;
    bool m_navigatingHistory = false;

    // A real bug found by testing: onDirectoryListed() decides whether to
    // push a new history entry using the single shared m_navigatingHistory
    // flag above, set by whichever navigateTo() call most recently ran —
    // but listDirectory()/directoryListed() carry no per-request id to
    // correlate a response back to the specific call that triggered it.
    // Clicking Back twice quickly (or Back then typing a new path before
    // the first response lands) let a second, overlapping navigateTo()
    // call's setup silently stomp the first's still-pending state, or
    // vice versa, corrupting m_history/m_historyIndex. Since there's no
    // way to correlate responses to requests without changing
    // RemoteBackend's interface, navigateTo() instead refuses to issue a
    // second request while one is still outstanding — set true right
    // before dispatching, cleared once a response (success via
    // onDirectoryListed(), or failure via the connectionFailed handler in
    // setBackend() — listDirectory() reports a bad path that way, not via
    // directoryListed()) or a backend swap (resetHistory()) resolves it.
    bool m_navigationInFlight = false;

    // A real bug found by code review: deleteEntry()/renameEntry()/
    // createFile()/createDirectory() are all "fire and refresh" (see
    // RemoteBackend.h's doc comment) — they reuse the exact same
    // directoryListed signal a genuine navigateTo() response uses, with
    // no per-request id to tell the two apart. onDirectoryListed() used
    // to treat every arrival as THE outstanding navigation's response,
    // so an unrelated file operation's refresh (queued and processed
    // before a concurrently in-flight navigateTo()'s own response, since
    // the backend's worker thread processes queued calls strictly in
    // order) could clear m_navigationInFlight and run history bookkeeping
    // for the wrong request while the real navigation was still pending.
    // Incremented right before each of the four dispatches (one entry in
    // confirmAndDelete()'s loop counts as one), decremented in
    // onDirectoryListed() when consumed by a refresh, or in
    // onFileOperationFailed() when that dispatch fails instead (no
    // directoryListed ever arrives for it in that case). A count rather
    // than a bool since confirmAndDelete() can dispatch several deletes
    // at once, each producing its own refresh.
    int m_pendingFileOpRefreshes = 0;

    // Guards against a real, live-reported crash: QMessageBox::warning()/
    // QMessageBox::question() both pump the whole app's event queue while
    // their own dialog is open, same as TransferManager::askConflict()'s
    // own QMessageBox::exec() already did before its own identical fix.
    // A burst of many failed createDirectory() calls (e.g. TransferManager's
    // own Write Into merge dispatching one per nested directory, several
    // of which fail for genuinely different reasons than "already
    // exists" — a permission error, a full disk, a dropped connection
    // partway through) could deliver a SECOND, already-queued failure
    // while the first one's dialog was still open, opening its own
    // dialog nested inside the first, and so on — an unbounded,
    // genuinely nested (not sequential) call stack that crashed with a
    // real Windows stack overflow on a real user's machine.
    //
    // Originally scoped narrowly around just onFileOperationFailed()'s
    // own plain warning (m_warningDialogInProgress/
    // m_deferredFailureWarnings, a QList of a fixed 3-string struct) —
    // widened here to guard ANY modal dialog this class shows as a
    // result of an async backend event, since the recursive-delete
    // confirmation below (offerRecursiveDelete()) is a SECOND, distinct
    // kind of modal with the identical hazard, and — with two dialog
    // kinds now live — they could reenter EACH OTHER, which two
    // separate, uncoordinated guards would not have caught. Set for the
    // duration of every such dialog; any action that arrives while it's
    // set is stashed in m_deferredDialogActions instead of running
    // immediately, then replayed once the in-progress one closes — see
    // runOrDeferModalAction()'s own body, which drains the queue in a
    // loop rather than recursively so the drain itself can't become
    // unbounded stack depth either. The specific "already exists during
    // a Write Into merge" trigger is ALSO fixed at its source — see
    // RemoteBackend::createDirectory()'s own ignoreAlreadyExists
    // parameter — but this guard closes the same crash risk for any
    // OTHER bulk-failure scenario too, not just that one trigger.
    bool m_modalDialogInProgress = false;
    QList<std::function<void()>> m_deferredDialogActions;

    // Runs action() immediately if no modal dialog from this class is
    // currently open, draining anything that had queued up while a
    // PREVIOUS action was running (a loop, not recursion — see
    // m_modalDialogInProgress's own doc comment for why). If a dialog
    // IS currently open, action() is stashed instead of run — it will
    // run later, from inside whichever call eventually finds the queue
    // non-empty after its own dialog closes.
    void runOrDeferModalAction(std::function<void()> action);

    // Shows one real QMessageBox::warning() for a file-operation
    // failure. Callers must route through runOrDeferModalAction(), not
    // call this directly — it does not itself check/set the guard.
    void showFailureWarning(const QString &operation, const QString &path, const QString &reason);

    // Paths most recently dispatched as a DIRECTORY delete via
    // deleteEntriesAt(..., /*offerRecursiveDeleteOnFailure=*/true) —
    // i.e. only from confirmAndDelete(), never from CompareSyncExecutor's
    // own calls (which don't pass that flag; see deleteEntriesAt()'s own
    // doc comment for why that's deliberate, not an oversight). Consulted
    // by onFileOperationFailed(): a "Delete" failure for a path found
    // (and removed) here is offered the recursive-delete flow instead of
    // a plain failure warning.
    QSet<QString> m_recursiveDeleteCandidates;

    // Enumerates the folder at path (already known to be non-empty — its
    // own plain, non-recursive deleteEntry() just failed) and, once the
    // walk finishes, offers to delete it and everything inside via a
    // content-aware warning. See FilePaneWidget.cpp's own implementation
    // comment for the full flow (mirrors TransferManager::
    // startFolderEnumeration()'s own FolderEnumerator lifecycle).
    void offerRecursiveDelete(const QString &path, const QString &reason);

    // Called once offerRecursiveDelete()'s own warning is answered Yes —
    // builds the deepest-first bottom-up delete order from the already-
    // enumerated items (mirroring CompareSyncExecutor::deleteSelected()'s
    // own sort exactly) and dispatches it via two deleteEntriesAt() calls.
    void runRecursiveDelete(const QString &path, const QList<EnumeratedItem> &items);

    // Non-owning — outlives this pane (MainWindow owns it for the app's
    // whole lifetime). Null in every test that constructs a pane directly.
    AppSettings *m_settings;
};
