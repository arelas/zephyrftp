#pragma once

#include <QTreeView>
#include <QStringList>
#include <QUrl>
#include "../backends/RemoteEntry.h"

class FilePaneWidget;

// Minimal QTreeView subclass adding cross-pane drag-and-drop. Qt's default
// item-view DnD machinery builds its MIME data from the MODEL
// (QAbstractItemView::startDrag() calls model()->mimeData(...)), which is
// oriented around reordering within a single model — it doesn't fit "drag
// a file from one pane's tree onto a different pane's tree", so drag
// start and drop handling are both done here manually instead.
//
// Meant for same-process use only: the dragged MIME data embeds a raw
// FilePaneWidget* (as a quintptr) identifying the source pane, resolved
// back to a pointer on drop — a standard, legitimate technique for
// internal Qt drag-and-drop, not a hack. It does mean the source pane
// must still exist when the drop lands; in practice that's guaranteed
// here since a drag's nested event loop (QDrag::exec()) keeps running
// until the drop completes, and nothing in this app destroys a
// FilePaneWidget while that's happening.
// **A real bug found by code review: "same-process only" was an
// assumption, not something ever enforced.** The underlying OS
// drag-and-drop transport (XDND on X11, the Wayland data-device
// protocol, OLE DnD on Windows) is inherently cross-PROCESS by design on
// every platform this app targets, and Qt adds no same-process
// restriction for a custom MIME type — so dragging a file from one
// running ZephyrFTP instance onto a SECOND instance's window delivered
// the first instance's raw pane pointer to the second instance, which
// genuinely dereferenced it (TransferManager::enqueue() calls
// sourcePane->currentDirectory()/backend() directly): a wild-pointer
// dereference into memory this process never allocated. Closed with a
// random per-process token (see FileTreeView.cpp's kProcessDragToken)
// embedded alongside the pointer and checked BEFORE the pointer bytes
// are ever even reconstructed, let alone dereferenced — a cross-process
// drop carries a token this process never generated, so it's rejected
// outright.
class FileTreeView : public QTreeView {
    Q_OBJECT
public:
    explicit FileTreeView(FilePaneWidget *owningPane, QWidget *parent = nullptr);

    // Only reached for a REMOTE pane's drag with a pure file selection
    // (no folders — see startDrag()'s own doc comment on why folders
    // aren't attempted) and a non-null FilePaneWidget::transferManager().
    // Downloads every entry to a real local temp path via
    // TransferManager::startEditDownload() (the exact same primitive
    // Edit-in-place uses — a fresh, guaranteed-unique temp path, no
    // destPane), blocking via a nested QEventLoop with a visible
    // QProgressDialog (Cancel aborts cleanly) until every download
    // finishes or fails. Returns real file:// QUrls on full success, an
    // empty list on any failure or cancellation — the caller falls back
    // to internal-only drag data either way, never partially.
    // tempPathsOut collects the real temp paths so the caller can clean
    // them up once the drag itself finishes. Public rather than private
    // — the only actual caller is startDrag() below, but a real native
    // drag gesture can't be reliably triggered headlessly to test that
    // call site directly (this project's test suite has no existing
    // precedent for simulating one, and QDrag::exec() has no real drop
    // target to negotiate with under the offscreen platform), so this is
    // exposed specifically to let external_drop_test.cpp exercise the
    // actual download-then-URLs logic directly — the real, novel part —
    // without needing the thin mouse-gesture layer on top of it.
    QList<QUrl> downloadForDragOut(const QList<RemoteEntry> &entries, QStringList &tempPathsOut);

signals:
    // Emitted on THIS view (the drop target) when files/folders are
    // dropped onto it from a different pane's FileTreeView. sourcePane
    // identifies where they came from; FilePaneWidget just forwards this
    // signal upward (see FilePaneWidget::filesDropped). Carries full
    // RemoteEntry (not just names) so isDir survives to MainWindow's
    // routing between TransferManager::enqueue()/enqueueFolder().
    void filesDroppedFrom(FilePaneWidget *sourcePane, const QList<RemoteEntry> &entries);

    // Files/folders dragged in from OUTSIDE this process — the OS's own
    // file manager (Nautilus, Explorer, Finder), or any other
    // application that offers real `text/uri-list` data — dropped onto
    // this view. A genuinely separate MIME shape from filesDroppedFrom
    // above (real QUrl/file:// data, not this app's own same-process
    // pointer-carrying custom format), so this is a parallel signal, not
    // a variant of it. Carries absolute local filesystem paths (already
    // resolved via QUrl::toLocalFile() in dropEvent()) — isDir is
    // resolved by whoever consumes this, via a plain QFileInfo check,
    // same as MainWindow::enqueueEntries() already does for entry.isDir.
    void externalFilesDropped(const QStringList &localPaths);

    // Keyboard shortcuts, all emitted from keyPressEvent() below and
    // consumed by FilePaneWidget (connected in its constructor, same
    // "view notices input, tell the owning pane to act" split
    // filesDroppedFrom above already establishes) — this class stays a
    // thin input-to-signal translator, not where the actual delete/
    // rename/copy/paste/refresh logic lives.
    void deleteKeyPressed();
    void renameKeyPressed();
    void refreshKeyPressed();
    void copyKeyPressed();
    void pasteKeyPressed();

protected:
    void startDrag(Qt::DropActions supportedActions) override;
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dragMoveEvent(QDragMoveEvent *event) override;
    void dropEvent(QDropEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;

private:
    FilePaneWidget *m_owningPane;   // not owned — the FilePaneWidget that created this view
};
