#include "CompareDeleteConfirmDialog.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QListWidgetItem>
#include <QPushButton>
#include <QDialogButtonBox>

CompareDeleteConfirmDialog::CompareDeleteConfirmDialog(const QList<CompareEntry> &candidates,
                                                         const QString &sideLabel, QWidget *parent)
    : QDialog(parent)
    , m_candidates(candidates)
{
    setWindowTitle(tr("Confirm Delete"));

    auto *layout = new QVBoxLayout(this);

    auto *headerLabel = new QLabel(
        tr("The following %1 item(s) will be permanently deleted from %2. This can't be undone.")
            .arg(candidates.size())
            .arg(sideLabel),
        this);
    headerLabel->setWordWrap(true);
    layout->addWidget(headerLabel);

    m_list = new QListWidget(this);
    for (const CompareEntry &entry : m_candidates) {
        auto *item = new QListWidgetItem(entry.relativePath, m_list);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(Qt::Checked);
        m_list->addItem(item);
    }
    layout->addWidget(m_list);

    connect(m_list, &QListWidget::itemChanged, this, [this](QListWidgetItem *) {
        bool anyChecked = false;
        for (int i = 0; i < m_list->count(); ++i) {
            if (m_list->item(i)->checkState() == Qt::Checked) {
                anyChecked = true;
                break;
            }
        }
        m_confirmButton->setEnabled(anyChecked);
    });

    auto *buttonBox = new QDialogButtonBox(this);
    m_confirmButton = buttonBox->addButton(tr("Confirm Delete"), QDialogButtonBox::AcceptRole);
    buttonBox->addButton(QDialogButtonBox::Cancel);
    connect(buttonBox, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttonBox, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttonBox);

    m_confirmButton->setEnabled(!m_candidates.isEmpty());

    resize(480, 360);
}

QList<CompareEntry> CompareDeleteConfirmDialog::confirmedEntries() const
{
    QList<CompareEntry> result;
    for (int i = 0; i < m_list->count(); ++i) {
        if (m_list->item(i)->checkState() == Qt::Checked)
            result.append(m_candidates.at(i));
    }
    return result;
}
