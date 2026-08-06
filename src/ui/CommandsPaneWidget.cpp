#include "CommandsPaneWidget.h"

#include <QPlainTextEdit>
#include <QVBoxLayout>
#include <QMenu>
#include <QAction>
#include <QFontDatabase>
#include <QScrollBar>

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

    // A blank pane on first launch reads as broken, not empty — this is
    // the one line that's never real protocol traffic (appendLine() below
    // is otherwise only ever fed genuine backend activity).
    appendLine(tr("Welcome to ZephyrFTP v%1").arg(QStringLiteral(APP_VERSION)));
}

void CommandsPaneWidget::appendLine(const QString &line)
{
    m_log->appendPlainText(line);
    // appendPlainText() alone only scrolls if the view already happened to
    // be at the bottom — explicit here so the log always tails like a
    // live console, matching what FileZilla's own message log does.
    QScrollBar *scrollBar = m_log->verticalScrollBar();
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
