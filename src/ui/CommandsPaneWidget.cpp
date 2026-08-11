#include "CommandsPaneWidget.h"

#include <QPlainTextEdit>
#include <QVBoxLayout>
#include <QMenu>
#include <QAction>
#include <QFontDatabase>
#include <QScrollBar>
#include <QFrame>

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

    // A proper bottom-of-pane division — Commands was the one pane that
    // just stopped at the log's own edge with nothing grounding it. A
    // plain divider line, not a status bar: there's no ongoing summary
    // worth showing here the way FilePaneWidget's own item-count footer
    // has, so a bit of actual status text (tried first, then reverted)
    // read as more chrome than this pane actually needed.
    auto *divider = new QFrame(this);
    divider->setObjectName(QStringLiteral("commandsDivider"));
    divider->setFrameShape(QFrame::HLine);
    divider->setFixedHeight(1);
    layout->addWidget(divider);

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
}

void CommandsPaneWidget::showContextMenu(const QPoint &pos)
{
    QMenu *menu = m_log->createStandardContextMenu();
    menu->addSeparator();
    QAction *clearAction = menu->addAction(tr("Clear"));
    connect(clearAction, &QAction::triggered, m_log, &QPlainTextEdit::clear);
    menu->exec(m_log->viewport()->mapToGlobal(pos));
    delete menu;
}
