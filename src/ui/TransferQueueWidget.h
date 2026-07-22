#pragma once

#include <QWidget>
#include "../transfer/TransferItem.h"

class QTableWidget;
class TransferManager;

// Read-only table showing the transfer queue: filename, direction, status,
// progress. Purely a view — all state lives in TransferManager, this just
// mirrors it.
class TransferQueueWidget : public QWidget {
    Q_OBJECT
public:
    explicit TransferQueueWidget(TransferManager *manager, QWidget *parent = nullptr);

private slots:
    void onItemAdded(const TransferItem &item);
    void onItemUpdated(const TransferItem &item);

private:
    int rowForId(int id) const;
    static QString directionText(TransferDirection direction);
    static QString statusText(const TransferItem &item);

    QTableWidget *m_table;
};
