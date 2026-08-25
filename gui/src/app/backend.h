#pragma once
// Supervises the Go half (cmd/mybench-backend): spawn it, learn its port from
// stdout, and keep its stdin open so it dies with us — no orphaned SSH
// tunnels or pools if the window goes away.
#include <QObject>
#include <QProcess>
#include <QString>

class Backend : public QObject
{
    Q_OBJECT
public:
    explicit Backend(QObject *parent = nullptr);

    // Attaches to MYBENCH_RPC (e.g. "127.0.0.1:8099") when set — the dev loop
    // runs the backend by hand — otherwise spawns the bundled binary.
    void start();
    void stop();

signals:
    void ready();
    void failed(const QString &why);

private:
    QString locateBinary() const;
    QProcess *m_proc = nullptr;
    QString m_buf;
    QString m_errBuf;
    bool m_ready = false;
    bool m_stopping = false;
};
