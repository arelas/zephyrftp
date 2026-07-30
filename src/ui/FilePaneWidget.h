#pragma once

#include <QWidget>
#include <QStandardItemModel>
#include <QPoint>
#include <QStringList>
#include <QIcon>
#include "../backends/RemoteBackend.h"

class FileTreeView;
class QLineEdit;
class QLabel;
class QThread;
class QAction;
class QToolButton;

// One side of the dual-pane layout. Owns a backend (local or remote) and
// renders its current directory listing. MainWindow instantiates two of
// these — deliberately dumb about which backend it holds, since the whole
// point of RemoteBackend is that this class can't tell the difference.
class FilePaneWidget : public QWidget {
    Q_OBJECT
public:
    explicit FilePaneWidget(RemoteBackend *backend, QWidget *parent = nullptr);

    RemoteBackend *backend() const { return m_backend; }
    QString selectedEntryName() const;
    QString currentDirectory() const { return m_backend->currentPath(); }

    // Selected rows that are files (not directories). Public because
    // FileTreeView (a different class, not a subwidget with special
    // access) calls this directly when starting a drag — see
    // FileTreeView::startDrag(). Also used internally by the context
    // menu's "Transfer Selected" action.
    QStringList selectedFileNames() const;

    void navigateTo(const QString &path);

    // Back/forward/up navigation, matching any ordinary file manager.
    // Back/forward walk a per-pane history list; Up computes the parent
    // of the current directory and navigates there — none of these
    // require backend support, they're built entirely on top of the
    // existing navigateTo()/currentDirectory().
    void goBack();
    void goForward();
    void goUp();
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

signals:
    // Emitted on double-click of a file (not directory) — MainWindow wires
    // this to "transfer to the other pane".
    void fileActivated(const QString &name);

    // Emitted from the right-click "Transfer Selected" context-menu action
    // — covers multi-select, unlike double-click which only ever acts on
    // the single row under the cursor. Directories are skipped (recursive
    // directory transfer isn't implemented); if the selection was only
    // directories, this fires with an empty list and MainWindow's handler
    // treats that as a no-op.
    void filesActivated(const QStringList &names);

    // Files dropped ONTO this pane FROM a different pane — forwarded
    // straight through from FileTreeView::filesDroppedFrom. MainWindow
    // connects both panes' filesDropped signals to one slot, using
    // sender() to identify which pane received the drop (the destination)
    // since sourcePane is already carried in the signal itself.
    void filesDropped(FilePaneWidget *sourcePane, const QStringList &names);

private slots:
    void onDirectoryListed(const QString &path, const QList<RemoteEntry> &entries);
    void onRowDoubleClicked(const QModelIndex &index);
    void onPathBarReturnPressed();
    void showContextMenu(const QPoint &pos);

private:
    void buildUi();

    // Sets/replaces the path bar's leading icon (device-laptop blue for
    // local, server green for remote) — called from setBackend() since
    // that's the only thing that can change which of the two applies.
    void updatePathBarIcon();

    // Per ICON-MAP.md's file/folder type table. Static since it only
    // depends on the entry's own data (name, isDir), not any pane state.
    static QIcon iconForEntry(const RemoteEntry &e);

    void updateNavigationButtonsEnabled();
    void resetHistory();   // called from setBackend() — a new backend means a fresh navigation context

    RemoteBackend *m_backend;
    QThread *m_backendThread = nullptr;   // null when backend has no thread of its own (e.g. LocalBackend)
    FileTreeView *m_view;
    QToolButton *m_backButton;
    QToolButton *m_forwardButton;
    QToolButton *m_upButton;
    QLineEdit *m_pathBar;
    QAction *m_pathBarLeadingIcon = nullptr;   // owned by m_pathBar once added; tracked so it can be replaced
    QLabel *m_statusLabel;
    QStandardItemModel *m_model;
    QList<RemoteEntry> m_currentEntries;

    // Navigation history. m_historyIndex points at the currently-displayed
    // entry; entries after it are "forward" history, cleared whenever a
    // fresh (non back/forward) navigation happens, same convention every
    // browser and file manager uses. m_navigatingHistory guards
    // onDirectoryListed() so a goBack()/goForward()-triggered listing
    // updates the index instead of pushing a new history entry.
    QStringList m_history;
    int m_historyIndex = -1;
    bool m_navigatingHistory = false;
};
