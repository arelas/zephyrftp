#include "CommandsPaneWidget.h"

#include <QPlainTextEdit>
#include <QVBoxLayout>
#include <QMenu>
#include <QAction>
#include <QFontDatabase>
#include <QScrollBar>
#include <QLabel>

// Set via target_compile_definitions() in CMakeLists.txt for every target
// that compiles this file — defended here anyway (same reasoning as
// MainWindow.cpp's identical guard) in case a future test target compiles
// this without setting it.
#ifndef APP_VERSION
#define APP_VERSION "dev"
#endif

CommandsPaneWidget::CommandsPaneWidget(QWidget *parent)
    : QWidget(parent)
    , m_log(new QPlainTextEdit(this))
    , m_footerLabel(nullptr)
{
    m_log->setReadOnly(true);
    m_log->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    m_log->setLineWrapMode(QPlainTextEdit::NoWrap);
    // Caps memory for a long-running session — QPlainTextEdit drops the
    // oldest block automatically once this is exceeded, same trade a real
    // terminal scrollback makes.
    m_log->setMaximumBlockCount(5000);
    m_log->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_log, &QPlainTextEdit::customContextMenuRequested,
            this, &CommandsPaneWidget::showContextMenu);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(m_log);

    // A proper bottom-of-pane division, matching every other pane in the
    // app (FilePaneWidget's own m_statusLabel is the exact same widget,
    // same styling, same position) — Commands was the one pane that
    // just stopped at the log's own edge with nothing grounding it.
    m_footerLabel = new QLabel(this);
    layout->addWidget(m_footerLabel);

    // A blank pane on first launch reads as broken, not empty — this is
    // the one line that's never real protocol traffic (appendLine() below
    // is otherwise only ever fed genuine backend activity).
    appendLine(tr("Welcome to ZephyrFTP v%1").arg(QStringLiteral(APP_VERSION)));
}

void CommandsPaneWidget::appendLine(const QString &line)
{
    // A real bug found by code review: this used to force-scroll to the
    // bottom unconditionally on every call, regardless of where the view
    // actually was. Confirmed directly (a standalone probe, not assumed
    // from documentation) that QPlainTextEdit::appendPlainText() on its
    // own already does the right thing — it leaves the scroll position
    // exactly where the user left it. Forcing it back to the bottom every
    // time meant scrolling up to re-read an earlier line during an active
    // transfer (SftpBackend/FtpBackend emit commandLogged on nearly every
    // control-channel operation) got yanked straight back down on the
    // very next line, making it impossible to actually review scrollback
    // while the log was still live — undermining the pane's whole stated
    // purpose as a reviewable protocol-traffic log. Fixed by checking the
    // scroll position BEFORE appending and only re-pinning to the bottom
    // if the view was already there — real "tail like a live console"
    // behavior (matches how every terminal/log viewer/chat app actually
    // does this), not blind forcing.
    QScrollBar *scrollBar = m_log->verticalScrollBar();
    const bool wasAtBottom = scrollBar->value() == scrollBar->maximum();
    m_log->appendPlainText(line);
    if (wasAtBottom)
        scrollBar->setValue(scrollBar->maximum());
    updateFooter();
}

void CommandsPaneWidget::updateFooter()
{
    // QPlainTextEdit::blockCount() is never 0 — an empty document still
    // has exactly one (empty) block — so that specific case is handled
    // separately rather than showing the misleading "1 line" right after
    // Clear. Otherwise, blockCount() ignores the setMaximumBlockCount()
    // cap on the count itself: it reports what's actually still in the
    // document (i.e. <=5000 once that cap has started dropping old
    // lines), which is exactly what "how much scrollback do I currently
    // have" means here.
    if (m_log->document()->isEmpty())
        m_footerLabel->setText(tr("No output yet"));
    else
        m_footerLabel->setText(tr("%n line(s)", nullptr, m_log->blockCount()));
}

void CommandsPaneWidget::showContextMenu(const QPoint &pos)
{
    QMenu *menu = m_log->createStandardContextMenu();
    menu->addSeparator();
    QAction *clearAction = menu->addAction(tr("Clear"));
    connect(clearAction, &QAction::triggered, this, [this]() {
        m_log->clear();
        updateFooter();
    });
    menu->exec(m_log->viewport()->mapToGlobal(pos));
    delete menu;
}
