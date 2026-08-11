#pragma once

#include <QWidget>

class QPlainTextEdit;
class QLabel;

// Live, read-only log of protocol traffic — modeled on FileZilla's message
// log. Deliberately dumb: it has no idea which pane or backend a line came
// from, it just appends whatever text MainWindow feeds it via appendLine()
// (wired to both panes' RemoteBackend::commandLogged — see FilePaneWidget's
// own forwarding signal of the same name, and RemoteBackend.h's doc comment
// on what each backend actually emits). No raw-command input by design —
// see ARCHITECTURE.md/commit history if that's ever reconsidered: letting
// someone inject arbitrary commands into a live control connection risks
// leaving this app's own state (current directory, an in-flight transfer)
// out of sync with what the server actually did.
class CommandsPaneWidget : public QWidget {
    Q_OBJECT
public:
    explicit CommandsPaneWidget(QWidget *parent = nullptr);

public slots:
    void appendLine(const QString &line);

private slots:
    void showContextMenu(const QPoint &pos);

private:
    // Refreshes m_footerLabel's line count — called after every append
    // and after Clear, matching FilePaneWidget's own m_statusLabel
    // (same widget, same styling via theme.qss's global QLabel rule,
    // same bottom-of-layout position) rather than inventing a new
    // footer treatment: every other pane in the app already ends in a
    // small text line like this, and Commands was the one exception.
    void updateFooter();

    QPlainTextEdit *m_log;
    QLabel *m_footerLabel;
};
