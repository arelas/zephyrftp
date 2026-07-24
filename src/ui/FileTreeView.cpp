#include "FileTreeView.h"
#include "FilePaneWidget.h"

#include <QDrag>
#include <QMimeData>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <cstring>

namespace {
// Same-process-only MIME types — never meant to interoperate with the OS
// or another application, just to carry data between two FileTreeViews
// inside this one app during a single drag gesture.
const QString kSourcePaneMimeType = QStringLiteral("application/x-zephyrftp-sourcepane");
const QString kFileNamesMimeType = QStringLiteral("application/x-zephyrftp-filenames");
}

FileTreeView::FileTreeView(FilePaneWidget *owningPane, QWidget *parent)
    : QTreeView(parent)
    , m_owningPane(owningPane)
{
    setDragEnabled(true);
    setAcceptDrops(true);
    setDropIndicatorShown(true);
    setDragDropMode(QAbstractItemView::DragDrop);
}

void FileTreeView::startDrag(Qt::DropActions supportedActions)
{
    Q_UNUSED(supportedActions);

    const QStringList names = m_owningPane->selectedFileNames();
    if (names.isEmpty())
        return;   // selection was empty or directories-only — nothing to drag

    auto *mimeData = new QMimeData;

    const quintptr sourcePanePtr = reinterpret_cast<quintptr>(m_owningPane);
    mimeData->setData(kSourcePaneMimeType,
                       QByteArray(reinterpret_cast<const char *>(&sourcePanePtr), sizeof(sourcePanePtr)));
    mimeData->setData(kFileNamesMimeType, names.join('\n').toUtf8());

    auto *drag = new QDrag(this);
    drag->setMimeData(mimeData);
    drag->exec(Qt::CopyAction);
}

void FileTreeView::dragEnterEvent(QDragEnterEvent *event)
{
    if (event->mimeData()->hasFormat(kSourcePaneMimeType))
        event->acceptProposedAction();
}

void FileTreeView::dragMoveEvent(QDragMoveEvent *event)
{
    if (event->mimeData()->hasFormat(kSourcePaneMimeType))
        event->acceptProposedAction();
}

void FileTreeView::dropEvent(QDropEvent *event)
{
    const QMimeData *mimeData = event->mimeData();
    if (!mimeData->hasFormat(kSourcePaneMimeType)) {
        event->ignore();
        return;
    }

    const QByteArray ptrBytes = mimeData->data(kSourcePaneMimeType);
    if (ptrBytes.size() != sizeof(quintptr)) {
        event->ignore();
        return;
    }
    quintptr sourcePanePtr = 0;
    std::memcpy(&sourcePanePtr, ptrBytes.constData(), sizeof(quintptr));
    auto *sourcePane = reinterpret_cast<FilePaneWidget *>(sourcePanePtr);

    if (sourcePane == m_owningPane) {
        // Dropped onto its own source pane — no-op, not an error.
        event->ignore();
        return;
    }

    const QStringList names = QString::fromUtf8(mimeData->data(kFileNamesMimeType))
                                   .split('\n', Qt::SkipEmptyParts);
    if (!names.isEmpty())
        emit filesDroppedFrom(sourcePane, names);

    event->acceptProposedAction();
}
