#include "app/backend.h"

#include "app/api.h"
#include "app/stubbackend.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QFileInfo>
#include <QJsonValue>
#include <QObject>
#include <QProcess>
#include <QSignalSpy>
#include <QString>
#include <QTest>
#include <QUrl>

// start() forks between attaching to MYBENCH_RPC and spawning the Go binary.
// Every slot here stays on the attach side of that fork, or on the path where
// the binary is missing, so the suite never starts a backend and never needs a
// database. What happens once a process is running — the stdout port parsing,
// the errorOccurred and finished handlers — is reachable only through a real
// child and is not covered.
namespace
{

constexpr auto RpcVar = "MYBENCH_RPC";
constexpr auto TokenVar = "MYBENCH_TOKEN";
constexpr auto Token = "s3cret";
constexpr auto Service = "query";
constexpr auto Method = "Rows";

// One call against whatever endpoint start() left behind. Returns false if the
// reply never arrived.
bool callAndWait(QObject *context, QString *error)
{
    bool done = false;
    api()->call(
        QString::fromLatin1(Service), QString::fromLatin1(Method), {}, context,
        [&](const QJsonValue &, const QString &err)
        {
            *error = err;
            done = true;
        }
    );
    // QTRY_VERIFY cannot be used from a helper: it returns from its function on
    // failure, which here would only skip the wait.
    for (int waited = 0; !done && waited < 5000; waited += 10)
    {
        QTest::qWait(10);
    }
    return done;
}

// The three candidates locateBinary() tries. They hang off the running
// executable's own directory, which no environment variable moves, so a tree
// that happens to keep a backend next to the tests can only be skipped.
QString backendInSearchPath()
{
    const QString exe =
#ifdef Q_OS_WIN
        QStringLiteral("mybench-backend.exe");
#else
        QStringLiteral("mybench-backend");
#endif
    const QString appDir = QCoreApplication::applicationDirPath();
    for (const QString &cand :
         {appDir + "/" + exe, appDir + "/../bin/" + exe, appDir + "/../../bin/" + exe})
    {
        if (QFileInfo(cand).isExecutable())
        {
            return cand;
        }
    }
    return {};
}

} // namespace

class TestBackend : public QObject
{
    Q_OBJECT

private slots:
    void init();

    void attachesToMybenchRpcWithoutSpawningAnything();
    void attachSendsTheTokenFromTheEnvironment();
    void attachTakesAMalformedAddressAsGiven_data();
    void attachTakesAMalformedAddressAsGiven();
    void reportsTheBinaryItCannotFind_data();
    void reportsTheBinaryItCannotFind();
    void stoppingBeforeStartIsNotAFailure();
    void stoppingTwiceIsNotAFailure();

private:
    StubBackend m_backend;
};

void TestBackend::init()
{
    // The environment is process-wide and picks the path start() takes, so a
    // value left by the last slot — or exported by whoever ran the suite —
    // decides the next one.
    qunsetenv(RpcVar);
    qunsetenv(TokenVar);

    m_backend.clearRequests();
    QVERIFY(m_backend.listening());
    api()->setEndpoint(QUrl(), QString());
}

void TestBackend::attachesToMybenchRpcWithoutSpawningAnything()
{
    qputenv(RpcVar, m_backend.base().authority().toUtf8());

    Backend backend;
    QSignalSpy ready(&backend, &Backend::ready);
    QSignalSpy failed(&backend, &Backend::failed);
    backend.start();

    QCOMPARE(ready.count(), 1);
    QCOMPARE(failed.count(), 0);
    // The dev loop already runs the backend by hand; a second copy would fight
    // it for the port.
    QVERIFY2(!backend.findChild<QProcess *>(), "the attach path must spawn nothing");

    // Nothing exposes the endpoint, so the only proof it was pointed at the
    // attach address is a call that arrives there.
    m_backend.replyWithResult(QJsonValue(1));
    QString error;
    QVERIFY(callAndWait(this, &error));
    QVERIFY(error.isEmpty());
    QCOMPARE(m_backend.requests().size(), 1);
    QCOMPARE(m_backend.requests().at(0).path, QStringLiteral("/rpc/query/Rows"));
    QVERIFY(m_backend.requests().at(0).token.isEmpty());
}

void TestBackend::attachSendsTheTokenFromTheEnvironment()
{
    qputenv(RpcVar, m_backend.base().authority().toUtf8());
    qputenv(TokenVar, Token);

    Backend backend;
    backend.start();

    // The hand-run backend was started with a token of its own, and the client
    // has no other way to learn it.
    m_backend.replyWithResult(QJsonValue(1));
    QString error;
    QVERIFY(callAndWait(this, &error));
    QCOMPARE(m_backend.requests().size(), 1);
    QCOMPARE(m_backend.requests().at(0).token, QString::fromLatin1(Token));
}

void TestBackend::attachTakesAMalformedAddressAsGiven_data()
{
    QTest::addColumn<QByteArray>("rpc");

    QTest::newRow("no colon") << QByteArray("nocolon");
    QTest::newRow("non-numeric port") << QByteArray("127.0.0.1:abc");
    QTest::newRow("no host") << QByteArray(":8099");
}

void TestBackend::attachTakesAMalformedAddressAsGiven()
{
    QFETCH(QByteArray, rpc);
    qputenv(RpcVar, rpc);

    Backend backend;
    QSignalSpy ready(&backend, &Backend::ready);
    QSignalSpy failed(&backend, &Backend::failed);
    backend.start();

    // start() asks only whether the variable is non-empty: whatever it holds
    // becomes the endpoint and the app is told it is ready. "127.0.0.1:abc"
    // does not even parse as a URL, and still gets through. A wrong address
    // surfaces later as a failed call, not as failed() here.
    QCOMPARE(ready.count(), 1);
    QCOMPARE(failed.count(), 0);
    QVERIFY(api()->ready());
    QVERIFY2(!backend.findChild<QProcess *>(), "the attach path must spawn nothing");
}

void TestBackend::reportsTheBinaryItCannotFind_data()
{
    QTest::addColumn<QByteArray>("rpc");

    // A null value stands for the variable being absent, an empty one for it
    // being exported with nothing in it. Both mean "do not attach".
    QTest::newRow("unset") << QByteArray();
    QTest::newRow("exported empty") << QByteArray("");
}

void TestBackend::reportsTheBinaryItCannotFind()
{
    const QString found = backendInSearchPath();
    if (!found.isEmpty())
    {
        // Reaching this path would spawn that binary, which this suite must
        // not do, so a tree laid out like this cannot answer the question.
        QSKIP(qPrintable(QStringLiteral("a backend sits in the search path: %1").arg(found)));
    }

    QFETCH(QByteArray, rpc);
    if (!rpc.isNull())
    {
        qputenv(RpcVar, rpc);
    }

    Backend backend;
    QSignalSpy ready(&backend, &Backend::ready);
    QSignalSpy failed(&backend, &Backend::failed);
    backend.start();

    // Answered at once rather than left hanging: the window would otherwise
    // wait on a ready() that is never coming.
    QCOMPARE(ready.count(), 0);
    QCOMPARE(failed.count(), 1);
    QCOMPARE(
        failed.at(0).at(0).toString(),
        QStringLiteral("mybench-backend not found next to the app or in bin/")
    );
    QVERIFY(!backend.findChild<QProcess *>());
}

void TestBackend::stoppingBeforeStartIsNotAFailure()
{
    Backend backend;
    QSignalSpy failed(&backend, &Backend::failed);

    backend.stop();

    QCOMPARE(failed.count(), 0);
}

void TestBackend::stoppingTwiceIsNotAFailure()
{
    qputenv(RpcVar, m_backend.base().authority().toUtf8());

    Backend backend;
    QSignalSpy failed(&backend, &Backend::failed);
    backend.start();

    backend.stop();
    backend.stop();

    // Quitting is not an error. A shutdown reported as a failure would put an
    // error dialog on the screen on the way out.
    QCOMPARE(failed.count(), 0);
}

QTEST_GUILESS_MAIN(TestBackend)

#include "tst_backend.moc"
