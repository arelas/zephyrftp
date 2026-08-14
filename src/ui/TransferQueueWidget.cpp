#include "TransferQueueWidget.h"
#include "../transfer/TransferManager.h"

#include <QTabWidget>
#include <QVBoxLayout>

TransferQueueWidget::TransferQueueWidget(TransferManager *manager, QWidget *parent)
    : QWidget(parent)
    , m_manager(manager)
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
    const QueueCategory category = categoryFor(item.status);
    m_categoryById.insert(item.id, category);
    tableFor(category)->addItem(item);
    updateTabLabel(category);
}

void TransferQueueWidget::onItemUpdated(const TransferItem &item)
{
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
