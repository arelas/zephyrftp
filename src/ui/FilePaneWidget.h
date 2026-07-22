#pragma once

#include <QWidget>
#include <QStandardItemModel>
#include "../backends/RemoteBackend.h"

class QTreeView;
class QLineEdit;
class QLabel;
class QThread;

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

    void navigateTo(const QString &path);

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

private slots:
    void onDirectoryListed(const QString &path, const QList<RemoteEntry> &entries);
    void onRowDoubleClicked(const QModelIndex &index);
    void onPathBarReturnPressed();

private:
    void buildUi();

    RemoteBackend *m_backend;
    QThread *m_backendThread = nullptr;   // null when backend has no thread of its own (e.g. LocalBackend)
    QTreeView *m_view;
    QLineEdit *m_pathBar;
    QLabel *m_statusLabel;
    QStandardItemModel *m_model;
    QList<RemoteEntry> m_currentEntries;
};
