#include "CompareDialog.h"
#include "CompareDeleteConfirmDialog.h"
#include "FilePaneWidget.h"
#include "IconTheme.h"
#include "../compare/DirectoryComparer.h"
#include "../backends/RemoteBackend.h"
#include "../transfer/TransferManager.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QTreeWidgetItemIterator>
#include <QCheckBox>
#include <QPushButton>
#include <QProgressBar>
#include <QMessageBox>
#include <QLocale>

namespace {

QString statusText(CompareStatus status)
{
    switch (status) {
    case CompareStatus::OnlyLeft: return QObject::tr("Only in Left");
    case CompareStatus::OnlyRight: return QObject::tr("Only in Right");
    case CompareStatus::Differs: return QObject::tr("Differs");
    case CompareStatus::Identical: return QObject::tr("Identical");
    }
    return QString();
}

QString sideText(bool present, bool isDir, qint64 size, const QDateTime &modified)
{
    if (!present)
        return QStringLiteral("—");   // em dash — nothing on this side
    if (isDir)
        return QObject::tr("(folder)");
    const QString sizeText = QLocale().formattedDataSize(size);
    return modified.isValid() ? QStringLiteral("%1, %2").arg(sizeText, modified.toString(Qt::ISODate))
                               : sizeText;
}

} // namespace

CompareDialog::CompareDialog(FilePaneWidget *leftPane, FilePaneWidget *rightPane,
                              TransferManager *transferManager, QWidget *parent)
    : QDialog(parent)
    , m_leftPane(leftPane)
    , m_rightPane(rightPane)
    , m_transferManager(transferManager)
    , m_executor(new CompareSyncExecutor(leftPane, rightPane, transferManager, this))
    , m_leftRoot(leftPane->currentDirectory())
    , m_rightRoot(rightPane->currentDirectory())
{
    setWindowTitle(tr("Compare Directories"));
    setAttribute(Qt::WA_DeleteOnClose);
    resize(760, 520);

    buildUi();
    runCompare();
}

void CompareDialog::buildUi()
{
    auto *layout = new QVBoxLayout(this);

    m_rootsLabel = new QLabel(tr("Left: %1\nRight: %2").arg(m_leftRoot, m_rightRoot), this);
    layout->addWidget(m_rootsLabel);

    m_progressLabel = new QLabel(tr("Comparing…"), this);
    layout->addWidget(m_progressLabel);
    m_progressBar = new QProgressBar(this);
    m_progressBar->setRange(0, 0);   // indeterminate — DirectoryComparer gives no incremental progress
    layout->addWidget(m_progressBar);

    m_staleWarningLabel = new QLabel(
        tr("A pane has navigated away from the compared directory. Actions are disabled — "
           "re-run Compare Directories to continue."),
        this);
    m_staleWarningLabel->setWordWrap(true);
    m_staleWarningLabel->setStyleSheet(QStringLiteral("color: #e0574f;"));   // IconTheme::Red, matches its hex
    m_staleWarningLabel->setVisible(false);
    layout->addWidget(m_staleWarningLabel);

    m_hideIdenticalCheck = new QCheckBox(tr("Hide identical"), this);
    connect(m_hideIdenticalCheck, &QCheckBox::toggled, this, &CompareDialog::applyHideIdentical);
    layout->addWidget(m_hideIdenticalCheck);

    m_tree = new QTreeWidget(this);
    m_tree->setColumnCount(4);
    m_tree->setHeaderLabels({tr("Name"), tr("Status"), tr("Left"), tr("Right")});
    m_tree->setUniformRowHeights(true);
    connect(m_tree, &QTreeWidget::itemChanged, this, &CompareDialog::onItemChanged);
    layout->addWidget(m_tree, /*stretch=*/1);

    m_allowDeleteCheck = new QCheckBox(
        tr("Also allow deleting files not present at source"), this);
    m_allowDeleteCheck->setChecked(false);
    layout->addWidget(m_allowDeleteCheck);

    auto *buttonRow = new QHBoxLayout();
    m_copyToRightButton = new QPushButton(tr("Copy Selected to Right"), this);
    connect(m_copyToRightButton, &QPushButton::clicked, this, &CompareDialog::onCopyToRight);
    buttonRow->addWidget(m_copyToRightButton);

    m_copyToLeftButton = new QPushButton(tr("Copy Selected to Left"), this);
    connect(m_copyToLeftButton, &QPushButton::clicked, this, &CompareDialog::onCopyToLeft);
    buttonRow->addWidget(m_copyToLeftButton);

    m_deleteExtrasButton = new QPushButton(tr("Delete Selected Extras"), this);
    m_deleteExtrasButton->setEnabled(false);   // gated on m_allowDeleteCheck, see the connect() just below
    connect(m_deleteExtrasButton, &QPushButton::clicked, this, &CompareDialog::onDeleteExtras);
    buttonRow->addWidget(m_deleteExtrasButton);

    // Connected here, now that m_deleteExtrasButton actually exists —
    // toggling the checkbox just flips this one button's enabled state,
    // never clears the tree's own checkbox selection (see this button's
    // own construction comment / the header's doc comment on
    // m_allowDeleteCheck's gating behavior).
    connect(m_allowDeleteCheck, &QCheckBox::toggled, m_deleteExtrasButton, [this](bool checked) {
        m_deleteExtrasButton->setEnabled(checked);
    });

    buttonRow->addStretch(1);
    auto *closeButton = new QPushButton(tr("Close"), this);
    connect(closeButton, &QPushButton::clicked, this, &QDialog::close);
    buttonRow->addWidget(closeButton);

    layout->addLayout(buttonRow);

    m_tree->setVisible(false);
    m_hideIdenticalCheck->setVisible(false);
    m_copyToRightButton->setEnabled(false);
    m_copyToLeftButton->setEnabled(false);
}

void CompareDialog::runCompare()
{
    m_progressLabel->setVisible(true);
    m_progressBar->setVisible(true);
    m_staleWarningLabel->setVisible(false);
    m_tree->setVisible(false);
    m_hideIdenticalCheck->setVisible(false);
    m_copyToRightButton->setEnabled(false);
    m_copyToLeftButton->setEnabled(false);

    if (m_comparer)
        m_comparer->deleteLater();

    m_comparer = new DirectoryComparer(m_leftPane->backend(), m_leftRoot,
                                        m_rightPane->backend(), m_rightRoot, this);
    connect(m_comparer, &DirectoryComparer::finished, this, &CompareDialog::onCompareFinished);
    connect(m_comparer, &DirectoryComparer::failed, this, &CompareDialog::onCompareFailed);
    m_comparer->start();
}

void CompareDialog::onCompareFinished(const QList<CompareEntry> &results)
{
    m_results = results;
    m_progressLabel->setVisible(false);
    m_progressBar->setVisible(false);
    m_tree->setVisible(true);
    m_hideIdenticalCheck->setVisible(true);
    m_copyToRightButton->setEnabled(true);
    m_copyToLeftButton->setEnabled(true);
    populateTree();
}

void CompareDialog::onCompareFailed(const QString &reason)
{
    m_progressLabel->setVisible(false);
    m_progressBar->setVisible(false);
    QMessageBox::warning(this, tr("Compare Failed"), reason);
    close();
}

void CompareDialog::populateTree()
{
    m_tree->blockSignals(true);   // avoid onItemChanged's cascade firing while building
    m_tree->clear();

    QHash<QString, QTreeWidgetItem *> itemsByPath;

    for (int i = 0; i < m_results.size(); ++i) {
        const CompareEntry &entry = m_results.at(i);
        const int slashIndex = entry.relativePath.lastIndexOf(QLatin1Char('/'));
        const QString parentPath = (slashIndex >= 0) ? entry.relativePath.left(slashIndex) : QString();
        const QString name = (slashIndex >= 0) ? entry.relativePath.mid(slashIndex + 1) : entry.relativePath;

        QTreeWidgetItem *parentItem = parentPath.isEmpty() ? nullptr : itemsByPath.value(parentPath, nullptr);
        auto *item = parentItem ? new QTreeWidgetItem(parentItem) : new QTreeWidgetItem(m_tree);

        item->setText(0, name);
        item->setIcon(0, IconTheme::tintedIcon(entry.isDir ? QStringLiteral(":/icons/folder.svg")
                                                             : QStringLiteral(":/icons/file.svg"),
                                                IconTheme::Gray, 16));
        item->setText(1, statusText(entry.status));
        item->setText(2, sideText(entry.status != CompareStatus::OnlyRight, entry.isDir,
                                   entry.leftSize, entry.leftModified));
        item->setText(3, sideText(entry.status != CompareStatus::OnlyLeft, entry.isDir,
                                   entry.rightSize, entry.rightModified));
        item->setData(0, Qt::UserRole, i);

        const bool checkable = entry.status != CompareStatus::Identical;
        Qt::ItemFlags flags = item->flags() | Qt::ItemIsUserCheckable;
        if (!checkable)
            flags &= ~Qt::ItemIsEnabled;
        item->setFlags(flags);
        item->setCheckState(0, Qt::Unchecked);

        itemsByPath.insert(entry.relativePath, item);
    }

    m_tree->blockSignals(false);
    m_tree->expandAll();
    applyHideIdentical(m_hideIdenticalCheck->isChecked());
}

namespace {

void cascadeCheckState(QTreeWidgetItem *item, Qt::CheckState state)
{
    for (int i = 0; i < item->childCount(); ++i) {
        QTreeWidgetItem *child = item->child(i);
        if (child->flags() & Qt::ItemIsUserCheckable)
            child->setCheckState(0, state);
        cascadeCheckState(child, state);
    }
}

} // namespace

void CompareDialog::onItemChanged(QTreeWidgetItem *item, int column)
{
    if (column != 0 || m_cascading || !item)
        return;
    if (item->childCount() == 0)
        return;

    // Checking/unchecking a directory row cascades to every descendant —
    // standard tree-checkbox UX. Guarded against re-entering itself: each
    // descendant's own setCheckState() below re-triggers this same slot.
    m_cascading = true;
    cascadeCheckState(item, item->checkState(0));
    m_cascading = false;
}

void CompareDialog::applyHideIdentical(bool hide)
{
    QTreeWidgetItemIterator it(m_tree);
    while (*it) {
        QTreeWidgetItem *item = *it;
        const int index = item->data(0, Qt::UserRole).toInt();
        if (index >= 0 && index < m_results.size() && m_results.at(index).status == CompareStatus::Identical)
            item->setHidden(hide);
        ++it;
    }
}

bool CompareDialog::anchorsStillValid() const
{
    return m_leftPane->currentDirectory() == m_leftRoot && m_rightPane->currentDirectory() == m_rightRoot;
}

QList<CompareEntry> CompareDialog::checkedEntriesMatching(const std::function<bool(CompareStatus)> &statusMatches) const
{
    QList<CompareEntry> result;
    QTreeWidgetItemIterator it(m_tree);
    while (*it) {
        QTreeWidgetItem *item = *it;
        if (item->checkState(0) == Qt::Checked) {
            const int index = item->data(0, Qt::UserRole).toInt();
            if (index >= 0 && index < m_results.size()) {
                const CompareEntry &entry = m_results.at(index);
                if (statusMatches(entry.status))
                    result.append(entry);
            }
        }
        ++it;
    }
    return result;
}

void CompareDialog::onCopyToRight()
{
    if (!anchorsStillValid()) {
        m_staleWarningLabel->setVisible(true);
        return;
    }
    const QList<CompareEntry> selected = checkedEntriesMatching([](CompareStatus s) {
        return s == CompareStatus::OnlyLeft || s == CompareStatus::Differs;
    });
    if (selected.isEmpty())
        return;
    m_executor->copySelected(selected, CompareSyncExecutor::CopyDirection::ToRight);
    emit copyQueued();
}

void CompareDialog::onCopyToLeft()
{
    if (!anchorsStillValid()) {
        m_staleWarningLabel->setVisible(true);
        return;
    }
    const QList<CompareEntry> selected = checkedEntriesMatching([](CompareStatus s) {
        return s == CompareStatus::OnlyRight || s == CompareStatus::Differs;
    });
    if (selected.isEmpty())
        return;
    m_executor->copySelected(selected, CompareSyncExecutor::CopyDirection::ToLeft);
    emit copyQueued();
}

void CompareDialog::onDeleteExtras()
{
    if (!m_allowDeleteCheck->isChecked())
        return;
    if (!anchorsStillValid()) {
        m_staleWarningLabel->setVisible(true);
        return;
    }

    const QList<CompareEntry> leftExtras = checkedEntriesMatching([](CompareStatus s) {
        return s == CompareStatus::OnlyLeft;
    });
    const QList<CompareEntry> rightExtras = checkedEntriesMatching([](CompareStatus s) {
        return s == CompareStatus::OnlyRight;
    });
    if (leftExtras.isEmpty() && rightExtras.isEmpty())
        return;

    bool anyConfirmed = false;

    if (!leftExtras.isEmpty()) {
        CompareDeleteConfirmDialog confirmDialog(leftExtras, tr("Left (%1)").arg(m_leftRoot), this);
        if (confirmDialog.exec() == QDialog::Accepted) {
            const QList<CompareEntry> confirmed = confirmDialog.confirmedEntries();
            if (!confirmed.isEmpty()) {
                m_executor->deleteSelected(confirmed, CompareSyncExecutor::DeleteSide::Left);
                anyConfirmed = true;
            }
        }
    }

    if (!rightExtras.isEmpty()) {
        CompareDeleteConfirmDialog confirmDialog(rightExtras, tr("Right (%1)").arg(m_rightRoot), this);
        if (confirmDialog.exec() == QDialog::Accepted) {
            const QList<CompareEntry> confirmed = confirmDialog.confirmedEntries();
            if (!confirmed.isEmpty()) {
                m_executor->deleteSelected(confirmed, CompareSyncExecutor::DeleteSide::Right);
                anyConfirmed = true;
            }
        }
    }

    // Re-run so the tree reflects reality — deleted rows would otherwise
    // stay checked/visible showing stale state (unlike copy, which queues
    // an async transfer nothing-yet-visible would make sense to re-diff
    // against).
    if (anyConfirmed)
        runCompare();
}
