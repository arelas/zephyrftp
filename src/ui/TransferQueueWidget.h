#pragma once

#include <QWidget>
#include <QPoint>
#include <QIcon>
#include <QColor>
#include "../transfer/TransferItem.h"

class QTableWidget;
class TransferManager;

// Table showing the transfer queue: filename, direction, status, progress.
// Mostly a view mirroring TransferManager, but also the entry point for
// per-item actions (right-click Cancel/Retry) — those just call back into
// the manager, which owns all the actual state.
class TransferQueueWidget : public QWidget {
    Q_OBJECT
public:
    explicit TransferQueueWidget(TransferManager *manager, QWidget *parent = nullptr);

private slots:
    void onItemAdded(const TransferItem &item);
    void onItemUpdated(const TransferItem &item);
    void showContextMenu(const QPoint &pos);

private:
    int rowForId(int id) const;
    static QString directionText(TransferDirection direction);
    static QString statusText(const TransferItem &item);

    // Direction arrow (up/down/left-right) recolored per the item's
    // current status, or check/x for the two terminal states — per
    // ICON-MAP.md's "Transfer direction & status" table.
    static QIcon statusIcon(const TransferItem &item);
    static QColor statusTextColor(TransferStatus status);

    QTableWidget *m_table;
    TransferManager *m_manager;
};
