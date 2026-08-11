#pragma once

#include <QAbstractSocket>
#include <QByteArray>
#include <QHostAddress>
#include <QString>
#include <QVariant>

// Shared interface between the two concrete transports FtpBackend's
// control and data connections can use: an ordinary Qt socket (plain
// FTP, and any FTPS data channel that ends up unencrypted — never
// happens today since PROT P failure is a hard connect-time error, but
// the interface doesn't assume that) and FtpTlsSocket (FTPS's raw-
// OpenSSL implementation — see that class's own header comment for
// why QSslSocket can't be reused there). Lets sendCommand()/readReply()
// and the downloadFile()/uploadFile() data loops be written ONCE
// against this interface instead of duplicated per transport — those
// loops in particular carry several hard-won, non-obvious fixes (see
// their own comments) that a second, drifting copy would risk losing
// track of.
class FtpSocket {
public:
    virtual ~FtpSocket() = default;

    virtual bool canReadLine() const = 0;
    virtual QByteArray readLine() = 0;
    virtual bool waitForReadyRead(int timeoutMs) = 0;
    virtual qint64 bytesAvailable() const = 0;
    virtual qint64 read(char *buf, qint64 maxSize) = 0;
    virtual QByteArray readAll() = 0;
    virtual qint64 write(const char *data, qint64 len) = 0;
    virtual bool waitForBytesWritten(int timeoutMs) = 0;

    virtual bool isConnected() const = 0;
    virtual QString errorString() const = 0;
    virtual QHostAddress localAddress() const = 0;
    virtual void setNoDelay() = 0;

    // Graceful (flush + protocol-level close, e.g. TLS close_notify —
    // vsftpd is confirmed to reject an upload that skips this, see
    // FtpBackend::uploadFile()'s own comment) vs. abrupt (drop the
    // connection immediately — used once a transfer is already known
    // complete, see FtpBackend::downloadFile()'s own comment on why
    // waiting for a clean shutdown there isn't worth it).
    virtual void disconnectGracefully() = 0;
    virtual void closeAbruptly() = 0;
    virtual bool waitForFullyDisconnected(int timeoutMs) = 0;
};

// Thin forwarding wrapper around a QAbstractSocket (QSslSocket used
// purely as TCP for plain FTP's control connection; QSslSocket or
// QTcpSocket for a data connection, matching whatever
// SslAcceptingTcpServer/openDataChannel() already hand out) — every
// call below forwards 1:1 to the exact same method the pre-FtpSocket
// code called directly, so plain FTP's runtime behavior is unchanged:
// same object, same calls, just reached through this interface instead
// of a raw pointer.
class QtSocketAdapter : public FtpSocket {
public:
    explicit QtSocketAdapter(QAbstractSocket *socket) : m_socket(socket) {}
    ~QtSocketAdapter() override { delete m_socket; }

    QAbstractSocket *raw() const { return m_socket; }

    bool canReadLine() const override { return m_socket->canReadLine(); }
    QByteArray readLine() override { return m_socket->readLine(); }
    bool waitForReadyRead(int timeoutMs) override { return m_socket->waitForReadyRead(timeoutMs); }
    qint64 bytesAvailable() const override { return m_socket->bytesAvailable(); }
    qint64 read(char *buf, qint64 maxSize) override { return m_socket->read(buf, maxSize); }
    QByteArray readAll() override { return m_socket->readAll(); }
    qint64 write(const char *data, qint64 len) override { return m_socket->write(data, len); }
    bool waitForBytesWritten(int timeoutMs) override { return m_socket->waitForBytesWritten(timeoutMs); }

    bool isConnected() const override { return m_socket->state() == QAbstractSocket::ConnectedState; }
    QString errorString() const override { return m_socket->errorString(); }
    QHostAddress localAddress() const override { return m_socket->localAddress(); }
    void setNoDelay() override { m_socket->setSocketOption(QAbstractSocket::LowDelayOption, QVariant(1)); }

    void disconnectGracefully() override { m_socket->disconnectFromHost(); }
    void closeAbruptly() override { m_socket->close(); }
    bool waitForFullyDisconnected(int timeoutMs) override
    {
        return m_socket->state() == QAbstractSocket::UnconnectedState || m_socket->waitForDisconnected(timeoutMs);
    }

private:
    QAbstractSocket *m_socket;
};
