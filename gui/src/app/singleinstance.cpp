#include "app/singleinstance.h"

#include <QByteArray>
#include <QDeadlineTimer>
#include <QLocalSocket>
#include <QStandardPaths>
#include <QThread>

#ifdef Q_OS_WIN
#include <QCryptographicHash>

#include <windows.h>
#endif

namespace
{

// Wayland only lets a window take focus with a token from the launcher, and
// the launcher hands it to the new process. Qt's Wayland plugin reads this
// variable in requestActivate(), so the primary sets it before raising.
constexpr auto TokenVar = "XDG_ACTIVATION_TOKEN";

// A primary that holds the lock but is not yet listening is mid-startup;
// one that stays that way this long is not going to answer.
constexpr int ConnectDeadlineMs = 3000;
constexpr int ConnectRetryMs = 50;
constexpr int WriteTimeoutMs = 1000;

QString serverName(const QString &dir)
{
#ifdef Q_OS_WIN
    // Named pipes live in one machine-wide namespace; the per-user dir keeps
    // two users apart.
    const QByteArray hash = QCryptographicHash::hash(dir.toUtf8(), QCryptographicHash::Sha1);
    return "mybench-" + QString::fromLatin1(hash.toHex().left(16));
#else
    return dir + "/mybench.sock";
#endif
}

} // namespace

SingleInstance::SingleInstance(const QString &dir, QObject *parent)
    : QObject(parent), m_name(serverName(dir)), m_lock(dir + "/mybench.lock")
{
    // A lock is stale only when its owner is dead, never by age: a primary
    // can run for weeks.
    m_lock.setStaleLockTime(0);
    // Besides locking the socket to this user, the option makes Qt bind a
    // temporary path and rename it into place, which replaces the socket file
    // a crashed primary leaves behind.
    m_server.setSocketOptions(QLocalServer::UserAccessOption);
    connect(&m_server, &QLocalServer::newConnection, this, &SingleInstance::onNewConnection);
}

QString SingleInstance::defaultDir()
{
#ifdef Q_OS_WIN
    // RuntimeLocation is the profile root on Windows; %TEMP% is per user too.
    return QStandardPaths::writableLocation(QStandardPaths::TempLocation);
#else
    return QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
#endif
}

SingleInstance::Role SingleInstance::claim()
{
    // The lock, not the socket, decides who is primary: two launches racing
    // to listen() could both find no server and both win.
    if (!m_lock.tryLock(0))
    {
        return forward() ? Role::Forwarded : Role::Unreachable;
    }

    // Holding the lock promises later launches a listener; without one they
    // would wait out the connect deadline and report a hung primary.
    if (!m_server.listen(m_name))
    {
        qWarning(
            "single instance: cannot listen on %s: %s", qPrintable(m_name),
            qPrintable(m_server.errorString())
        );
        m_lock.unlock();
        return Role::Unguarded;
    }

    return Role::Primary;
}

QString SingleInstance::errorString() const
{
    return m_server.errorString();
}

void SingleInstance::onNewConnection()
{
    while (QLocalSocket *socket = m_server.nextPendingConnection())
    {
        connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
        connect(
            socket, &QLocalSocket::readyRead, this,
            [this, socket]()
            {
                if (!socket->canReadLine())
                {
                    return;
                }

                const QByteArray token = socket->readLine().trimmed();
                socket->disconnectFromServer();
                if (!token.isEmpty())
                {
                    qputenv(TokenVar, token);
                }
                emit activationRequested();
            }
        );
    }
}

bool SingleInstance::forward()
{
    QLocalSocket socket;
    const QDeadlineTimer deadline(ConnectDeadlineMs);
    socket.connectToServer(m_name);
    while (!socket.waitForConnected(ConnectRetryMs))
    {
        if (deadline.hasExpired())
        {
            qWarning("single instance: %s holds the lock but never answered", qPrintable(m_name));
            return false;
        }
        QThread::msleep(ConnectRetryMs);
        socket.connectToServer(m_name);
    }

#ifdef Q_OS_WIN
    // Windows only lets the foreground process hand the foreground on; this
    // launch has that right, the running primary does not.
    qint64 pid = 0;
    if (m_lock.getLockInfo(&pid, nullptr, nullptr))
    {
        AllowSetForegroundWindow(static_cast<DWORD>(pid));
    }
#endif

    socket.write(qgetenv(TokenVar) + '\n');
    if (!socket.waitForBytesWritten(WriteTimeoutMs))
    {
        qWarning("single instance: request not sent: %s", qPrintable(socket.errorString()));
        return false;
    }
    socket.disconnectFromServer();
    return true;
}
