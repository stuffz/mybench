#include "app/backend.h"

#include "app/api.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QProcessEnvironment>
#include <QUrl>
#include <QUuid>

#ifdef MYBENCH_EMBED_BACKEND
#include <cstdlib>
// From the Go c-archive (backend/cmd/mybench-backend-lib), which documents
// these signatures; returned strings are malloc'd and freed here.
extern "C"
{
    char *mybenchBackendStart(char *token, char **addrOut);
    void mybenchBackendStop(void);
}
#endif

Backend::Backend(QObject *parent) : QObject(parent) {}

QString Backend::locateBinary() const
{
    const QString exe =
#ifdef Q_OS_WIN
        QStringLiteral("mybench-backend.exe");
#else
        QStringLiteral("mybench-backend");
#endif
    // Beside the client first (how it ships), then the repo's bin/ (dev).
    const QString appDir = QCoreApplication::applicationDirPath();
    for (const QString &cand :
         {appDir + "/" + exe, appDir + "/../bin/" + exe, appDir + "/../../bin/" + exe})
    {
        if (QFileInfo(cand).isExecutable())
        {
            return QDir::cleanPath(cand);
        }
    }
    return {};
}

void Backend::start()
{
    const QString attach = qEnvironmentVariable("MYBENCH_RPC");
    if (!attach.isEmpty())
    {
        api()->setEndpoint(QUrl("http://" + attach), qEnvironmentVariable("MYBENCH_TOKEN"));
        emit ready();
        return;
    }

#ifdef MYBENCH_EMBED_BACKEND
    // Single-exe build: the Go backend is linked in, not spawned. Start() is
    // storage-open plus a localhost listen, quick enough for the UI thread,
    // and the per-session token gates the RPC surface exactly as below.
    const QString token = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QByteArray tokenUtf8 = token.toUtf8();
    char *addr = nullptr;
    if (char *err = mybenchBackendStart(tokenUtf8.data(), &addr))
    {
        emit failed(tr("mybench-backend: %1").arg(QString::fromUtf8(err)));
        free(err);
        return;
    }
    m_ready = true;
    api()->setEndpoint(QUrl("http://" + QString::fromUtf8(addr)), token);
    free(addr);
    emit ready();
#else
    const QString bin = locateBinary();
    if (bin.isEmpty())
    {
        emit failed(QStringLiteral("mybench-backend not found next to the app or in bin/"));
        return;
    }

    m_proc = new QProcess(this);
    m_proc->setProgram(bin);
    m_proc->setArguments({"-addr", "127.0.0.1:0", "-watch-stdin"});
    m_proc->setProcessChannelMode(QProcess::SeparateChannels);

    // The backend listens on localhost where any local process could reach
    // it; a fresh per-session token gates the RPC surface. Always set (never
    // inherited), so an exported MYBENCH_TOKEN can't desync the two halves.
    const QString token = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("MYBENCH_TOKEN"), token);
    m_proc->setProcessEnvironment(env);

    connect(
        m_proc, &QProcess::readyReadStandardOutput, this,
        [this, token]()
        {
            m_buf += QString::fromUtf8(m_proc->readAllStandardOutput());
            if (m_ready)
            {
                return; // a later log line containing the marker must not re-point the endpoint
            }
            static const QString marker = QStringLiteral("listening ");
            const qsizetype at = m_buf.indexOf(marker);
            if (at < 0)
            {
                return;
            }
            const qsizetype nl = m_buf.indexOf('\n', at);
            if (nl < 0)
            {
                return;
            }
            const QString addr = m_buf.mid(at + marker.size(), nl - at - marker.size()).trimmed();
            m_buf.clear();
            m_ready = true;
            api()->setEndpoint(QUrl("http://" + addr), token);
            emit ready();
        }
    );
    // stderr is where the backend explains its death; keep a bounded tail.
    connect(
        m_proc, &QProcess::readyReadStandardError, this,
        [this]()
        {
            m_errBuf += QString::fromUtf8(m_proc->readAllStandardError());
            if (m_errBuf.size() > 4096)
            {
                m_errBuf = m_errBuf.right(4096);
            }
        }
    );
    connect(
        m_proc, &QProcess::errorOccurred, this,
        [this](QProcess::ProcessError)
        {
            if (m_stopping)
            {
                return;
            }
            emit failed(tr("mybench-backend: %1").arg(m_proc->errorString()));
        }
    );
    // errorOccurred only covers FailedToStart/Crashed; a clean nonzero exit
    // (config error, port in use) would otherwise die silently.
    connect(
        m_proc, &QProcess::finished, this,
        [this](int code, QProcess::ExitStatus st)
        {
            if (m_stopping || st == QProcess::CrashExit)
            {
                return; // crashes already surfaced via errorOccurred
            }
            const QString tail = m_errBuf.trimmed();
            emit failed(
                (m_ready ? tr("mybench-backend exited with code %1")
                         : tr("mybench-backend exited with code %1 before it was ready"))
                    .arg(code) +
                (tail.isEmpty() ? QString() : "\n" + tail)
            );
        }
    );
    m_proc->start();
#endif
}

void Backend::stop()
{
#ifdef MYBENCH_EMBED_BACKEND
    if (!m_ready)
    {
        return;
    }
    // Drops SSH tunnels and pools, the same shutdown closing stdin triggers
    // for the spawned backend.
    mybenchBackendStop();
#else
    if (!m_proc)
    {
        return;
    }
    m_stopping = true; // the exit we are about to cause is not a failure
    // Closing stdin is the polite shutdown the backend watches for.
    m_proc->closeWriteChannel();
    if (!m_proc->waitForFinished(3000))
    {
        m_proc->kill();
    }
#endif
}
