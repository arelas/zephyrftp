#pragma once

#include <QMainWindow>

class FilePaneWidget;
class TransferManager;

// Top-level window: two FilePaneWidgets side by side (commander style),
// a docked transfer queue along the bottom, and a toolbar for
// connect/disconnect. Deliberately thin — transfer orchestration lives in
// TransferManager, backend I/O lives in the backends, layout/wiring lives
// here.
class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);

private slots:
    void onLeftFileActivated(const QString &name);
    void onRightFileActivated(const QString &name);
    void onConnectTriggered();
    void onDisconnectTriggered();
    void onTransferSucceeded();

private:
    void buildLayout();
    void buildToolbar();
    void buildTransferQueue();

    FilePaneWidget *m_leftPane = nullptr;   // local, by default
    FilePaneWidget *m_rightPane = nullptr;  // remote, once connected
    TransferManager *m_transferManager = nullptr;
};
