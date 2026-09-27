#pragma once
// One mybench per user: a second launch hands its activation token to the
// running instance, which raises its window, and exits.
#include <QLocalServer>
#include <QLockFile>
#include <QObject>
#include <QString>

class SingleInstance : public QObject
{
    Q_OBJECT
public:
    enum class Role
    {
        Primary,     // nothing else running; this process owns the window
        Unguarded,   // owns the window, but cannot listen for later launches
        Forwarded,   // the running instance was asked to come to the front
        Unreachable, // one is running but never accepted the request
    };

    // dir holds the lock file and, on Unix, the socket; it must be private to
    // the user.
    explicit SingleInstance(const QString &dir, QObject *parent = nullptr);

    static QString defaultDir();

    Role claim();
    QString errorString() const;

signals:
    void activationRequested();

private:
    void onNewConnection();
    bool forward();

    QString m_name;
    QLockFile m_lock;
    QLocalServer m_server;
};
