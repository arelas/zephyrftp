#pragma once

#include <QTreeView>
#include <QStringList>
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

signals:
    // Emitted on THIS view (the drop target) when files/folders are
    // dropped onto it from a different pane's FileTreeView. sourcePane
    // identifies where they came from; FilePaneWidget just forwards this
    // signal upward (see FilePaneWidget::filesDropped). Carries full
    // RemoteEntry (not just names) so isDir survives to MainWindow's
    // routing between TransferManager::enqueue()/enqueueFolder().
    void filesDroppedFrom(FilePaneWidget *sourcePane, const QList<RemoteEntry> &entries);

protected:
    void startDrag(Qt::DropActions supportedActions) override;
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dragMoveEvent(QDragMoveEvent *event) override;
    void dropEvent(QDropEvent *event) override;

private:
    FilePaneWidget *m_owningPane;   // not owned — the FilePaneWidget that created this view
};
