#include "TransferQueueWidget.h"
#include "../transfer/TransferManager.h"

#include <QTableWidget>
#include <QHeaderView>
#include <QVBoxLayout>

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
{
    m_table->setColumnCount(4);
    m_table->setHorizontalHeaderLabels({tr("File"), tr("Direction"), tr("Status"), tr("Progress")});
    m_table->horizontalHeader()->setSectionResizeMode(ColName, QHeaderView::Stretch);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionMode(QAbstractItemView::NoSelection);
    m_table->verticalHeader()->setVisible(false);

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
