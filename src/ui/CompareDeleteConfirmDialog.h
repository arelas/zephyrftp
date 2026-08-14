#pragma once

#include <QDialog>
#include <QList>
#include "../compare/CompareTypes.h"

class QListWidget;
class QPushButton;

// The actual enforcement point of "nothing is deleted without being
// individually visible and confirmable" — CompareDialog's tree checkboxes
// are the SELECTION step (which rows the user marked for deletion); this
// dialog is a separate, dedicated COMMIT step: a flat itemized list of
// exactly what's about to be deleted, each row individually uncheckable,
// shown right before anything actually happens. One more explicit gate
// than FilePaneWidget::confirmAndDelete()'s existing single "Delete N
// items?" QMessageBox — justified here because a compare-driven delete
// can affect far more items at once, via bulk tree-checkbox selection,
// than any existing single-directory delete UI in this app allows.
class CompareDeleteConfirmDialog : public QDialog {
    Q_OBJECT
public:
    // side identifies which pane's files these paths belong to, purely
    // for the dialog's own header text ("Deleting from Left" /
    // "Deleting from Right") — it doesn't affect which entries are shown.
    CompareDeleteConfirmDialog(const QList<CompareEntry> &candidates, const QString &sideLabel,
                                QWidget *parent = nullptr);

    // Valid only after exec() == QDialog::Accepted. Whatever subset of the
    // constructor's candidates the user left checked when they clicked
    // Confirm Delete — always a subset, never a superset.
    QList<CompareEntry> confirmedEntries() const;

private:
    QList<CompareEntry> m_candidates;
    QListWidget *m_list = nullptr;
    QPushButton *m_confirmButton = nullptr;
};
