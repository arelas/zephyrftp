#include "FileTreeView.h"
#include "FilePaneWidget.h"

#include <QDrag>
#include <QMimeData>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QKeyEvent>
#include <QRandomGenerator>
#include <cstring>

namespace {
// Custom MIME types meant only to carry data between two FileTreeViews
// inside this one app during a single drag gesture — NOT meant to
// interoperate with another application. The underlying OS drag-and-drop
// transport doesn't actually enforce that intent, though (see
// kProcessDragToken below): a custom MIME type is visible to any
// receiving process that recognizes its name, same-app or not.
const QString kSourcePaneMimeType = QStringLiteral("application/x-zephyrftp-sourcepane");
const QString kFileNamesMimeType = QStringLiteral("application/x-zephyrftp-filenames");

// Generated fresh once per process, at static-initialization time, and
// never transmitted anywhere else — see FileTreeView.h's own comment for
// the real cross-process pointer-dereference bug this closes. A drop
// carrying any other value (a different process, or in principle a
// different, non-ZephyrFTP application that happened to offer the same
// custom MIME type name) is rejected before the pointer bytes that
// follow it are ever reconstructed. Deliberately not hardened against a
// hostile process on the same machine deliberately guessing/brute-
// forcing this exact 64-bit value — that's a much larger threat model a
// malicious local binary could attack this process through a dozen
// other ways regardless — this closes the ordinary, non-adversarial
// case of two genuine ZephyrFTP windows (or a second instance) sharing
// one desktop.
const quint64 kProcessDragToken = QRandomGenerator::global()->generate64();
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

    // Token first, then the pointer — see kProcessDragToken's own comment
    // for why. dropEvent() checks the token before it ever looks at the
    // pointer bytes that follow it.
    const quintptr sourcePanePtr = reinterpret_cast<quintptr>(m_owningPane);
    QByteArray payload(reinterpret_cast<const char *>(&kProcessDragToken), sizeof(kProcessDragToken));
    payload.append(reinterpret_cast<const char *>(&sourcePanePtr), sizeof(sourcePanePtr));
    mimeData->setData(kSourcePaneMimeType, payload);

    // Encoded as "<0 or 1>\t<name>" per entry, entries joined by NUL
    // bytes — not '\n', which a real bug found by code review used to
    // separate entries with: '\n' is legal in a POSIX filename (e.g. on
    // ext4), so a name containing one got split into multiple bogus
    // "lines" here, corrupting the drop (a truncated name, and a
    // leftover fragment with no isDir prefix silently discarded by
    // dropEvent()'s own parsing below). A literal NUL byte can never
    // appear in a real filename on any filesystem this app targets — the
    // OS path APIs themselves treat it as a string terminator — making
    // it a genuinely unambiguous record separator, unlike '\n' or '\t'
    // (both technically legal, just rare in practice).
    QByteArray namesPayload;
    for (const RemoteEntry &entry : entries) {
        if (!namesPayload.isEmpty())
            namesPayload.append('\0');
        namesPayload.append(((entry.isDir ? QStringLiteral("1\t") : QStringLiteral("0\t")) + entry.name).toUtf8());
    }
    mimeData->setData(kFileNamesMimeType, namesPayload);

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

    const QByteArray payload = mimeData->data(kSourcePaneMimeType);
    if (payload.size() != static_cast<int>(sizeof(kProcessDragToken) + sizeof(quintptr))) {
        event->ignore();
        return;
    }

    // Checked BEFORE the pointer bytes that follow are ever reconstructed
    // — see kProcessDragToken's own comment for the real cross-process
    // pointer-dereference bug this prevents. A mismatch means this drop
    // didn't originate from THIS process's own startDrag() (almost
    // certainly a different ZephyrFTP instance), so the pointer bytes
    // are never even looked at, let alone dereferenced.
    quint64 receivedToken = 0;
    std::memcpy(&receivedToken, payload.constData(), sizeof(receivedToken));
    if (receivedToken != kProcessDragToken) {
        event->ignore();
        return;
    }

    quintptr sourcePanePtr = 0;
    std::memcpy(&sourcePanePtr, payload.constData() + sizeof(receivedToken), sizeof(quintptr));
    auto *sourcePane = reinterpret_cast<FilePaneWidget *>(sourcePanePtr);

    if (sourcePane == m_owningPane) {
        // Dropped onto its own source pane — no-op, not an error.
        event->ignore();
        return;
    }

    // Split on NUL bytes, matching startDrag()'s own encoding — see its
    // comment for why '\n' used to be (and can't safely be) the record
    // separator here.
    const QList<QByteArray> rawLines = mimeData->data(kFileNamesMimeType).split('\0');
    QList<RemoteEntry> entries;
    for (const QByteArray &rawLine : rawLines) {
        if (rawLine.isEmpty())
            continue;
        const QString line = QString::fromUtf8(rawLine);
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

void FileTreeView::keyPressEvent(QKeyEvent *event)
{
    // Plain Delete, no modifiers — Ctrl+Delete/Shift+Delete etc. fall
    // through to the base class unhandled rather than silently doing the
    // same thing as plain Delete, matching how none of the shortcuts
    // below fire on an unexpected modifier combination either.
    if (event->key() == Qt::Key_Delete && event->modifiers() == Qt::NoModifier) {
        emit deleteKeyPressed();
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_F2 && event->modifiers() == Qt::NoModifier) {
        emit renameKeyPressed();
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_F5 && event->modifiers() == Qt::NoModifier) {
        emit refreshKeyPressed();
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_C && event->modifiers() == Qt::ControlModifier) {
        emit copyKeyPressed();
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_V && event->modifiers() == Qt::ControlModifier) {
        emit pasteKeyPressed();
        event->accept();
        return;
    }
    // Ctrl+A: QAbstractItemView already implements selectAll() — no new
    // logic needed, just wiring the shortcut to the existing method
    // (unlike the other five above, this one has no cross-pane or
    // FilePaneWidget-level meaning, so it's handled entirely here rather
    // than round-tripping through a signal).
    if (event->key() == Qt::Key_A && event->modifiers() == Qt::ControlModifier) {
        selectAll();
        event->accept();
        return;
    }
    QTreeView::keyPressEvent(event);
}
