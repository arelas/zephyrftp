#include "TransferQueueWidget.h"
#include "../transfer/TransferManager.h"

#include <QTableWidget>
#include <QHeaderView>
#include <QVBoxLayout>
#include <QMenu>
#include <QAction>

namespace {
constexpr int ColName = 0;
constexpr int ColDirection = 1;
constexpr int ColStatus = 2;
constexpr int ColProgress = 3;
constexpr int IdRole = Qt::UserRole;
}

TransferQueueWidget::TransferQueueWidget(TransferManager *manager, QWidget *parent)
    : QWidget(parent)
    , m_table(new QTableWidget(this))
    , m_manager(manager)
{
    m_table->setColumnCount(4);
    m_table->setHorizontalHeaderLabels({tr("File"), tr("Direction"), tr("Status"), tr("Progress")});
    m_table->horizontalHeader()->setSectionResizeMode(ColName, QHeaderView::Stretch);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionMode(QAbstractItemView::NoSelection);
    m_table->verticalHeader()->setVisible(false);
    m_table->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_table, &QTableWidget::customContextMenuRequested,
            this, &TransferQueueWidget::showContextMenu);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(m_table);

    connect(manager, &TransferManager::itemAdded, this, &TransferQueueWidget::onItemAdded);
    connect(manager, &TransferManager::itemUpdated, this, &TransferQueueWidget::onItemUpdated);
}

int TransferQueueWidget::rowForId(int id) const
{
    for (int row = 0; row < m_table->rowCount(); ++row) {
        auto *item = m_table->item(row, ColName);
        if (item && item->data(IdRole).toInt() == id)
            return row;
    }
    return -1;
}

QString TransferQueueWidget::directionText(TransferDirection direction)
{
    switch (direction) {
    case TransferDirection::LocalToRemote: return tr("local -> remote");
    case TransferDirection::RemoteToLocal: return tr("remote -> local");
    case TransferDirection::LocalToLocal:  return tr("local copy");
    case TransferDirection::Unsupported:   return tr("unsupported");
    }
    return {};
}

QString TransferQueueWidget::statusText(const TransferItem &item)
{
    switch (item.status) {
    case TransferStatus::Queued:      return tr("Queued");
    case TransferStatus::InProgress:  return tr("Transferring");
    case TransferStatus::Done:        return tr("Done");
    case TransferStatus::Failed:      return item.errorMessage.isEmpty()
                                              ? tr("Failed")
                                              : tr("Failed: %1").arg(item.errorMessage);
    case TransferStatus::Cancelled:   return tr("Cancelled");
    }
    return {};
}

void TransferQueueWidget::onItemAdded(const TransferItem &item)
{
    const int row = m_table->rowCount();
    m_table->insertRow(row);

    auto *nameItem = new QTableWidgetItem(item.fileName);
    nameItem->setData(IdRole, item.id);
    m_table->setItem(row, ColName, nameItem);
    m_table->setItem(row, ColDirection, new QTableWidgetItem(directionText(item.direction)));
    m_table->setItem(row, ColStatus, new QTableWidgetItem(statusText(item)));
    m_table->setItem(row, ColProgress, new QTableWidgetItem(QString()));
}

void TransferQueueWidget::onItemUpdated(const TransferItem &item)
{
    const int row = rowForId(item.id);
    if (row < 0)
        return;

    m_table->item(row, ColStatus)->setText(statusText(item));

    QString progressText;
    if (item.status == TransferStatus::InProgress && item.bytesTotal > 0) {
        const int percent = static_cast<int>((item.bytesDone * 100) / item.bytesTotal);
        progressText = QStringLiteral("%1%").arg(percent);
    } else if (item.status == TransferStatus::Done) {
        progressText = tr("100%");
    }
    m_table->item(row, ColProgress)->setText(progressText);
}

void TransferQueueWidget::showContextMenu(const QPoint &pos)
{
    const int row = m_table->rowAt(pos.y());
    if (row < 0)
        return;

    auto *nameItem = m_table->item(row, ColName);
    if (!nameItem)
        return;
    const int id = nameItem->data(IdRole).toInt();

    // Look up the item's current status from the manager rather than
    // trusting the table's displayed text — the manager is the source of
    // truth, the table is just a mirror of it.
    TransferStatus status = TransferStatus::Queued;
    bool found = false;
    for (const TransferItem &item : m_manager->items()) {
        if (item.id == id) {
            status = item.status;
            found = true;
            break;
        }
    }
    if (!found)
        return;

    QMenu menu(this);
    QAction *cancelAction = menu.addAction(tr("Cancel"));
    cancelAction->setEnabled(status == TransferStatus::Queued || status == TransferStatus::InProgress);
    QAction *retryAction = menu.addAction(tr("Retry"));
    retryAction->setEnabled(status == TransferStatus::Failed || status == TransferStatus::Cancelled);

    QAction *chosen = menu.exec(m_table->viewport()->mapToGlobal(pos));
    if (chosen == cancelAction)
        m_manager->cancelItem(id);
    else if (chosen == retryAction)
        m_manager->retryItem(id);
}
