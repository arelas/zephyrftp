#pragma once

#include <QTreeView>
#include <QStringList>

class FilePaneWidget;

// Minimal QTreeView subclass adding cross-pane drag-and-drop. Qt's default
// item-view DnD machinery builds its MIME data from the MODEL
// (QAbstractItemView::startDrag() calls model()->mimeData(...)), which is
// oriented around reordering within a single model — it doesn't fit "drag
// a file from one pane's tree onto a different pane's tree", so drag
// start and drop handling are both done here manually instead.
//
// Same-process only: the dragged MIME data embeds a raw FilePaneWidget*
// (as a quintptr) identifying the source pane, resolved back to a pointer
// on drop. This never needs to survive outside this app's own process —
// there's no real OS-level drag target involved — so this is a standard,
// legitimate technique for internal Qt drag-and-drop, not a hack. It does
// mean the source pane must still exist when the drop lands; in practice
// that's guaranteed here since a drag's nested event loop (QDrag::exec())
// keeps running until the drop completes, and nothing in this app
// destroys a FilePaneWidget while that's happening.
class FileTreeView : public QTreeView {
    Q_OBJECT
public:
    explicit FileTreeView(FilePaneWidget *owningPane, QWidget *parent = nullptr);

signals:
    // Emitted on THIS view (the drop target) when files are dropped onto
    // it from a different pane's FileTreeView. sourcePane identifies where
    // they came from; FilePaneWidget just forwards this signal upward
    // (see FilePaneWidget::filesDropped).
    void filesDroppedFrom(FilePaneWidget *sourcePane, const QStringList &names);

protected:
    void startDrag(Qt::DropActions supportedActions) override;
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dragMoveEvent(QDragMoveEvent *event) override;
    void dropEvent(QDropEvent *event) override;

private:
    FilePaneWidget *m_owningPane;   // not owned — the FilePaneWidget that created this view
};
