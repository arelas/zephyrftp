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

    const QList<RemoteEntry> entries = m_owningPane->selectedEntries();
    if (entries.isEmpty())
        return;   // nothing selected — nothing to drag

    auto *mimeData = new QMimeData;

    const quintptr sourcePanePtr = reinterpret_cast<quintptr>(m_owningPane);
    mimeData->setData(kSourcePaneMimeType,
                       QByteArray(reinterpret_cast<const char *>(&sourcePanePtr), sizeof(sourcePanePtr)));

    // Encoded as "<0 or 1>\t<name>" per line — the isDir flag needs to
    // survive the drag too now that whole folders can be dragged, not
    // just files.
    QStringList lines;
    for (const RemoteEntry &entry : entries)
        lines.append((entry.isDir ? QStringLiteral("1\t") : QStringLiteral("0\t")) + entry.name);
    mimeData->setData(kFileNamesMimeType, lines.join('\n').toUtf8());

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

    const QStringList lines = QString::fromUtf8(mimeData->data(kFileNamesMimeType))
                                   .split('\n', Qt::SkipEmptyParts);
    QList<RemoteEntry> entries;
    for (const QString &line : lines) {
        const int tabIndex = line.indexOf('\t');
        if (tabIndex < 0)
            continue;   // malformed — shouldn't happen given startDrag() always writes this format, but don't crash on it
        RemoteEntry entry;
        entry.isDir = line.left(tabIndex) == QLatin1String("1");
        entry.name = line.mid(tabIndex + 1);
        entries.append(entry);
    }
    if (!entries.isEmpty())
        emit filesDroppedFrom(sourcePane, entries);

    event->acceptProposedAction();
}
