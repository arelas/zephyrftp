#include "TransferQueueWidget.h"
#include "../transfer/TransferManager.h"
#include "../transfer/ChecksumVerifier.h"

#include <QTabWidget>
#include <QVBoxLayout>
#include <QMessageBox>
#include <QProgressDialog>

TransferQueueWidget::TransferQueueWidget(TransferManager *manager, QWidget *parent)
    : QWidget(parent)
    , m_manager(manager)
    , m_checksumVerifier(new ChecksumVerifier(manager, this))
    , m_tabs(new QTabWidget(this))
    , m_activeTable(new TransferQueueTable(manager, QueueCategory::Active, this))
    , m_completedTable(new TransferQueueTable(manager, QueueCategory::Completed, this))
    , m_failedTable(new TransferQueueTable(manager, QueueCategory::Failed, this))
{
    // Lookup names for tests — same reasoning as m_leftPane/m_rightPane's
    // own setObjectName() in MainWindow::buildLayout(): lets
    // transfer-queue-test find a SPECIFIC tab's table reliably via
    // findChild<TransferQueueTable*>("...") rather than relying on
    // findChildren()'s traversal order (there are now three siblings of
    // the same type, not one).
    m_activeTable->setObjectName(QStringLiteral("activeQueueTable"));
    m_completedTable->setObjectName(QStringLiteral("completedQueueTable"));
    m_failedTable->setObjectName(QStringLiteral("failedQueueTable"));

    m_tabs->addTab(m_activeTable, tr("Active"));
    m_tabs->addTab(m_completedTable, tr("Completed"));
    m_tabs->addTab(m_failedTable, tr("Failed"));
    m_tabs->setCurrentIndex(0);   // Active — the tab someone watching a live transfer actually wants

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(m_tabs);

    // Initial plain labels ("Active", not "Active (0)") — updateTabLabel()
    // only appends a count once a tab actually has something in it.
    updateTabLabel(QueueCategory::Active);
    updateTabLabel(QueueCategory::Completed);
    updateTabLabel(QueueCategory::Failed);

    connect(manager, &TransferManager::itemAdded, this, &TransferQueueWidget::onItemAdded);
    connect(manager, &TransferManager::itemUpdated, this, &TransferQueueWidget::onItemUpdated);

    for (TransferQueueTable *table : {m_activeTable, m_completedTable, m_failedTable}) {
        connect(table, &TransferQueueTable::verifyChecksumRequested,
                this, &TransferQueueWidget::onVerifyChecksumRequested);
    }
    connect(m_checksumVerifier, &ChecksumVerifier::verificationProgress,
            this, &TransferQueueWidget::onVerificationProgress);
    connect(m_checksumVerifier, &ChecksumVerifier::verificationFinished,
            this, &TransferQueueWidget::onVerificationFinished);
    connect(m_checksumVerifier, &ChecksumVerifier::verificationFailed,
            this, &TransferQueueWidget::onVerificationFailed);
}

TransferQueueTable *TransferQueueWidget::tableFor(QueueCategory category) const
{
    switch (category) {
    case QueueCategory::Active:    return m_activeTable;
    case QueueCategory::Completed: return m_completedTable;
    case QueueCategory::Failed:    return m_failedTable;
    }
    return m_activeTable;   // unreachable
}

void TransferQueueWidget::updateTabLabel(QueueCategory category)
{
    int index = -1;
    QString base;
    switch (category) {
    case QueueCategory::Active:    index = 0; base = tr("Active");    break;
    case QueueCategory::Completed: index = 1; base = tr("Completed"); break;
    case QueueCategory::Failed:    index = 2; base = tr("Failed");    break;
    }
    const int count = tableFor(category)->rowCount();
    m_tabs->setTabText(index, count > 0 ? QStringLiteral("%1 (%2)").arg(base).arg(count) : base);
}

void TransferQueueWidget::onItemAdded(const TransferItem &item)
{
    // Hidden checksum-verification temp-downloads never enter any tab —
    // see TransferItem::isVerificationTask's own doc comment.
    if (item.isVerificationTask)
        return;

    const QueueCategory category = categoryFor(item.status);
    m_categoryById.insert(item.id, category);
    tableFor(category)->addItem(item);
    updateTabLabel(category);
}

void TransferQueueWidget::onItemUpdated(const TransferItem &item)
{
    // Same reasoning as onItemAdded() above — ChecksumVerifier itself
    // (not this class) is the one thing that reacts to a verification
    // temp-download's progress/completion.
    if (item.isVerificationTask)
        return;

    const QueueCategory newCategory = categoryFor(item.status);
    const auto it = m_categoryById.constFind(item.id);
    // Defensive fallback for an update that somehow arrives before the
    // matching itemAdded (shouldn't happen — TransferManager always
    // appends and emits itemAdded before any itemUpdated for a given
    // id) — treated as "no migration needed", which just routes this
    // update to wherever it CURRENTLY belongs, same as after a real add.
    const QueueCategory oldCategory = (it != m_categoryById.constEnd()) ? it.value() : newCategory;

    if (oldCategory != newCategory) {
        // The actual move this whole class exists for: e.g. Active ->
        // Completed the instant an item reaches Done, or Failed ->
        // Active the instant a right-click Retry re-queues it.
        tableFor(oldCategory)->removeItem(item.id);
        m_categoryById[item.id] = newCategory;
        tableFor(newCategory)->addItem(item);
        updateTabLabel(oldCategory);
        updateTabLabel(newCategory);
    } else {
        tableFor(newCategory)->updateItem(item);
    }
}

void TransferQueueWidget::retintIcons()
{
    for (const TransferItem &item : m_manager->items())
        tableFor(categoryFor(item.status))->updateItem(item);
}

QString TransferQueueWidget::fileNameForItem(int id) const
{
    for (const TransferItem &item : m_manager->items()) {
        if (item.id == id)
            return item.fileName;
    }
    return QString();
}

void TransferQueueWidget::closeVerifyDialog(int itemId)
{
    QProgressDialog *dialog = m_verifyDialogs.take(itemId);
    if (dialog)
        dialog->deleteLater();
}

void TransferQueueWidget::onVerifyChecksumRequested(int itemId)
{
    if (m_checksumVerifier->isVerifying(itemId))
        return;   // already in flight — ignore a duplicate request

    auto *dialog = new QProgressDialog(
        tr("Verifying \"%1\"...").arg(fileNameForItem(itemId)), tr("Cancel"), 0, 100, this);
    dialog->setWindowModality(Qt::WindowModal);
    dialog->setMinimumDuration(0);
    dialog->setAutoClose(false);
    dialog->setAutoReset(false);
    dialog->setValue(0);
    // QProgressDialog sizes itself from the progress bar/button row, not
    // the label — confirmed via a real screenshot (this project's own
    // established verification technique), the label text ran straight
    // off the right edge with zero margin for an ordinary filename, and
    // would only be worse for a longer one. A fixed minimum width gives
    // the label real room without needing to touch its (not directly
    // exposed) word-wrap setting.
    dialog->setMinimumWidth(420);
    m_verifyDialogs.insert(itemId, dialog);

    connect(dialog, &QProgressDialog::canceled, this, [this, itemId]() {
        m_checksumVerifier->cancel(itemId);
    });

    // May emit verificationFailed synchronously (e.g. the item is
    // somehow no longer in the queue) — closeVerifyDialog() below
    // handles that fine either way, since the dialog is already tracked
    // by the time this call happens.
    m_checksumVerifier->verify(itemId);
}

void TransferQueueWidget::onVerificationProgress(int itemId, qint64 bytesDone, qint64 bytesTotal)
{
    QProgressDialog *dialog = m_verifyDialogs.value(itemId);
    if (!dialog || bytesTotal <= 0)
        return;
    // Percentage, not raw byte counts — same reasoning as
    // TransferQueueTable::percentFor(): QProgressDialog::setValue() takes
    // an int, and a multi-gigabyte file's byte count would silently
    // overflow if used directly.
    dialog->setValue(static_cast<int>((bytesDone * 100) / bytesTotal));
}

void TransferQueueWidget::onVerificationFinished(int itemId, bool matched, const QString &localHex,
                                                  const QString &remoteHex)
{
    closeVerifyDialog(itemId);

    const QString fileName = fileNameForItem(itemId);
    if (matched) {
        QMessageBox::information(this, tr("Checksum Verified"),
            tr("\"%1\" matches on both ends.\n\nSHA-256: %2").arg(fileName, localHex));
    } else {
        QMessageBox::warning(this, tr("Checksum Mismatch"),
            tr("\"%1\" does NOT match.\n\nLocal:  %2\nRemote: %3").arg(fileName, localHex, remoteHex));
    }
}

void TransferQueueWidget::onVerificationFailed(int itemId, const QString &reason)
{
    closeVerifyDialog(itemId);
    QMessageBox::warning(this, tr("Verification Failed"), reason);
}
