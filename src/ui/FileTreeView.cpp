#include "FileTreeView.h"
#include "FilePaneWidget.h"
#include "../transfer/TransferManager.h"
#include "../backends/RemoteBackend.h"
#include "../PathUtils.h"

#include <QDrag>
#include <QMimeData>
#include <QUrl>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QKeyEvent>
#include <QRandomGenerator>
#include <QProgressDialog>
#include <QEventLoop>
#include <QFile>
#include <QHash>
#include <QSet>
#include <algorithm>
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

// Real OS file data (text/uri-list) alongside this class's own internal
// MIME format — added so dragging OUT of this app onto the OS's own
// file manager (or any other application) works, not just cross-pane
// drags within this app. A LOCAL pane's selected entries already exist
// as real files, so this is free — just their real absolute paths. A
// REMOTE pane's don't exist locally at all, and Qt has no cross-
// platform support for delayed-rendering drag data (the technique
// Windows/macOS/some Linux DE's own native apps use to hand over bytes
// lazily, as the drop target actually asks for them) — the only
// portable option is downloading first, via downloadForDragOut() below,
// which blocks this drag gesture on a real network round trip. Only
// attempted for a PURE file selection (no folders): recursively
// downloading an entire folder tree before a drag gesture can even
// start would be a real, unbounded-duration UX trap for a large folder,
// with no good way to cancel just "this one drag" once deep into it —
// a folder in the selection simply means no real OS file data is
// offered (the internal same-app drag still works either way), not an
// error or a partial attempt.
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

    // Real OS file data — see this method's own doc comment above.
    QStringList dragOutTempPaths;   // only ever populated for a remote pane's downloaded temp files
    QList<QUrl> osUrls;
    RemoteBackend *backend = m_owningPane->backend();
    if (backend && backend->isLocalFilesystem()) {
        for (const RemoteEntry &entry : entries)
            osUrls.append(QUrl::fromLocalFile(joinPath(m_owningPane->currentDirectory(), entry.name)));
    } else if (m_owningPane->transferManager()) {
        const bool anySelectedFolders = std::any_of(entries.begin(), entries.end(),
                                                      [](const RemoteEntry &e) { return e.isDir; });
        if (!anySelectedFolders)
            osUrls = downloadForDragOut(entries, dragOutTempPaths);
    }
    if (!osUrls.isEmpty())
        mimeData->setUrls(osUrls);

    auto *drag = new QDrag(this);
    drag->setMimeData(mimeData);
    drag->exec(Qt::CopyAction);

    // Safe to delete now — by the time QDrag::exec() above has
    // returned, the OS's own drag-and-drop protocol guarantees the drop
    // target (if any) has already fully read whatever data it needed
    // from these files; nothing downstream still needs them. A one-shot
    // download purely to satisfy this single drag gesture, unlike Edit-
    // in-place's own temp files, which deliberately outlive the download
    // (see TransferManager.h's own comment on that).
    for (const QString &tempPath : dragOutTempPaths)
        QFile::remove(tempPath);
}

QList<QUrl> FileTreeView::downloadForDragOut(const QList<RemoteEntry> &entries, QStringList &tempPathsOut)
{
    TransferManager *manager = m_owningPane->transferManager();

    QProgressDialog progress(
        entries.size() == 1 ? tr("Downloading \"%1\" for dragging...").arg(entries.first().name)
                             : tr("Downloading %1 files for dragging...").arg(entries.size()),
        tr("Cancel"), 0, entries.size(), this);
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(0);   // a real network wait — show immediately, not worth hiding briefly

    QSet<int> pendingIds;
    QHash<QString, QString> nameToTempPath;   // entry name -> real local temp path, filled in as each finishes
    bool anyFailed = false;
    int settledCount = 0;

    for (const RemoteEntry &entry : entries)
        pendingIds.insert(manager->startEditDownload(m_owningPane, entry.name));

    QEventLoop loop;
    const QMetaObject::Connection connection = connect(manager, &TransferManager::itemUpdated, &loop,
                                                        [&](const TransferItem &item) {
        if (!pendingIds.contains(item.id))
            return;
        if (item.status == TransferStatus::Done) {
            pendingIds.remove(item.id);
            nameToTempPath.insert(item.fileName, item.destPath);
            progress.setValue(++settledCount);
        } else if (item.status == TransferStatus::Failed || item.status == TransferStatus::Cancelled) {
            pendingIds.remove(item.id);
            anyFailed = true;
            progress.setValue(++settledCount);
        }
        if (pendingIds.isEmpty())
            loop.quit();
    });
    connect(&progress, &QProgressDialog::canceled, &loop, &QEventLoop::quit);

    loop.exec();
    QObject::disconnect(connection);

    if (!pendingIds.isEmpty()) {
        // A genuine Cancel click — whatever's still in flight gets
        // properly cancelled through the ordinary TransferManager path
        // (not just abandoned), same as any other in-progress transfer.
        for (int id : pendingIds)
            manager->cancelItem(id);
    }
    if (!pendingIds.isEmpty() || anyFailed) {
        // Cancelled, or a real download failure partway through — clean
        // up whatever DID finish and offer nothing to the OS. The
        // internal same-app MIME data (set by the caller regardless) is
        // completely unaffected, so dragging within the app still works
        // even when this returns empty.
        for (const QString &path : nameToTempPath)
            QFile::remove(path);
        return {};
    }

    QList<QUrl> urls;
    for (const RemoteEntry &entry : entries) {
        const QString tempPath = nameToTempPath.value(entry.name);
        tempPathsOut.append(tempPath);
        urls.append(QUrl::fromLocalFile(tempPath));
    }
    return urls;
}

void FileTreeView::dragEnterEvent(QDragEnterEvent *event)
{
    // hasUrls() is real OS drag-and-drop data (the file manager's own
    // text/uri-list, or any other application offering it) — a
    // genuinely separate, external source from this app's own
    // same-process custom MIME type, checked first and unaffected by
    // this addition.
    if (event->mimeData()->hasFormat(kSourcePaneMimeType) || event->mimeData()->hasUrls())
        event->acceptProposedAction();
}

void FileTreeView::dragMoveEvent(QDragMoveEvent *event)
{
    if (event->mimeData()->hasFormat(kSourcePaneMimeType) || event->mimeData()->hasUrls())
        event->acceptProposedAction();
}

void FileTreeView::dropEvent(QDropEvent *event)
{
    const QMimeData *mimeData = event->mimeData();
    if (!mimeData->hasFormat(kSourcePaneMimeType)) {
        // Not this app's own internal drag — an external OS drop
        // (file:// URLs from Nautilus/Explorer/Finder, or any other
        // app offering real file data) is the only other thing this
        // view accepts (see dragEnterEvent() above). Anything else
        // (text, an image, a URL to a webpage) is rejected here — only
        // a URL that actually resolves to a real local file is used.
        if (mimeData->hasUrls()) {
            QStringList localPaths;
            for (const QUrl &url : mimeData->urls()) {
                if (url.isLocalFile())
                    localPaths.append(url.toLocalFile());
            }
            if (!localPaths.isEmpty()) {
                emit externalFilesDropped(localPaths);
                event->acceptProposedAction();
                return;
            }
        }
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
