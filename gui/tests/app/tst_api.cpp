#include "app/api.h"

#include "app/stubbackend.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QObject>
#include <QString>
#include <QTest>

// Api is a singleton, so each slot points it at its own stub and every
// assertion is about the call it just made.
namespace
{

constexpr auto Token = "s3cret";
constexpr auto Service = "query";
constexpr auto Method = "Rows";

// One call, run to completion. Returns false if the reply never arrived.
bool callAndWait(
    QObject *context, const QJsonArray &args, QJsonValue *result, QString *error,
    const QString &service = QString::fromLatin1(Service),
    const QString &method = QString::fromLatin1(Method)
)
{
    bool done = false;
    api()->call(
        service, method, args, context,
        [&](const QJsonValue &res, const QString &err)
        {
            if (result)
            {
                *result = res;
            }
            if (error)
            {
                *error = err;
            }
            done = true;
        }
    );
    // QTRY_VERIFY cannot be used from a helper: it returns from its function
    // on failure, which here would only skip the wait.
    for (int waited = 0; !done && waited < 5000; waited += 10)
    {
        QTest::qWait(10);
    }
    return done;
}

} // namespace

class TestApi : public QObject
{
    Q_OBJECT

private slots:
    void init();

    void refusesToCallBeforeAnEndpointIsSet();
    void postsPositionalArgsToTheServiceMethodPath();
    void sendsTheTokenOnlyWhenThereIsOne();
    void deliversTheResultField();
    void deliversTheErrorField();
    void treatsAnOmittedResultAsUndefinedNotNull();
    void reportsABodyThatIsNotJson();
    void aTransportFailureWithJsonBodyIsStillAFailure();
    void dropsTheCallbackWhenTheContextIsGone();
    void postIgnoresWhateverComesBack();
    void flushWaitsForTheCallInFlight();

private:
    StubBackend m_backend;
};

void TestApi::init()
{
    m_backend.clearRequests();
    QVERIFY(m_backend.listening());
    api()->setEndpoint(m_backend.base(), QString::fromLatin1(Token));
}

void TestApi::refusesToCallBeforeAnEndpointIsSet()
{
    api()->setEndpoint(QUrl(), QString());
    QVERIFY(!api()->ready());

    QJsonValue result;
    QString error;
    QVERIFY(callAndWait(this, {}, &result, &error));

    // Answered inline rather than left hanging, so a view that asks before the
    // backend is up gets told instead of waiting forever.
    QCOMPARE(error, QStringLiteral("backend not ready"));
    QVERIFY(m_backend.requests().isEmpty());
}

void TestApi::postsPositionalArgsToTheServiceMethodPath()
{
    m_backend.replyWithResult(QJsonValue(1));
    QVERIFY(callAndWait(this, {QStringLiteral("r1"), 0, 200}, nullptr, nullptr));

    QCOMPARE(m_backend.requests().size(), 1);
    const StubBackend::Request &req = m_backend.requests().at(0);
    QCOMPARE(req.verb, QStringLiteral("POST"));
    QCOMPARE(req.path, QStringLiteral("/rpc/query/Rows"));
    QCOMPARE(req.args, QJsonArray({QStringLiteral("r1"), 0, 200}));
}

void TestApi::sendsTheTokenOnlyWhenThereIsOne()
{
    m_backend.replyWithResult({});
    QVERIFY(callAndWait(this, {}, nullptr, nullptr));
    QCOMPARE(m_backend.requests().at(0).token, QString::fromLatin1(Token));

    m_backend.clearRequests();
    api()->setEndpoint(m_backend.base(), QString());
    QVERIFY(callAndWait(this, {}, nullptr, nullptr));
    QVERIFY(m_backend.requests().at(0).token.isEmpty());
}

void TestApi::deliversTheResultField()
{
    QJsonObject payload;
    payload.insert(QStringLiteral("rows"), QJsonArray({QJsonArray({QStringLiteral("a")})}));
    m_backend.replyWithResult(payload);

    QJsonValue result;
    QString error;
    QVERIFY(callAndWait(this, {}, &result, &error));

    QVERIFY(error.isEmpty());
    QCOMPARE(result.toObject(), payload);
}

void TestApi::deliversTheErrorField()
{
    m_backend.replyWithError(QStringLiteral("table is gone"));

    QJsonValue result;
    QString error;
    QVERIFY(callAndWait(this, {}, &result, &error));

    QCOMPARE(error, QStringLiteral("table is gone"));
}

void TestApi::treatsAnOmittedResultAsUndefinedNotNull()
{
    // Both reply fields are omitempty on the Go side, so a call that returns
    // nothing sends neither key. Callers read result with .toObject()/.toArray(),
    // which must degrade to empty rather than to a null they might dereference.
    m_backend.replyWith("{}");

    QJsonValue result;
    QString error;
    QVERIFY(callAndWait(this, {}, &result, &error));

    QVERIFY(error.isEmpty());
    QVERIFY(result.isUndefined());
    QVERIFY(result.toObject().isEmpty());
}

void TestApi::reportsABodyThatIsNotJson()
{
    m_backend.replyWith("<html>gateway blew up</html>");

    QJsonValue result;
    QString error;
    QVERIFY(callAndWait(this, {}, &result, &error));

    QVERIFY(error.contains(QStringLiteral("query.Rows")));
    QVERIFY(error.contains(QStringLiteral("gateway blew up")));
}

void TestApi::aTransportFailureWithJsonBodyIsStillAFailure()
{
    // A 500 whose body happens to parse as JSON without an "error" key would
    // otherwise read as a successful call returning nothing.
    m_backend.replyWith("{\"unrelated\":true}", 500);

    QJsonValue result;
    QString error;
    QVERIFY(callAndWait(this, {}, &result, &error));

    QVERIFY2(!error.isEmpty(), "a 500 must not be reported as success");
    QVERIFY(error.contains(QStringLiteral("query.Rows")));
}

void TestApi::dropsTheCallbackWhenTheContextIsGone()
{
    m_backend.replyWithResult(QJsonValue(1));

    bool fired = false;
    auto *context = new QObject;
    api()->call(
        QString::fromLatin1(Service), QString::fromLatin1(Method), {}, context,
        [&fired](const QJsonValue &, const QString &) { fired = true; }
    );
    // Closing a tab or dialog mid-request is routine, and the callback would
    // touch the freed object.
    delete context;

    api()->flush(2000);
    QTest::qWait(50);
    QVERIFY2(!fired, "the callback of a destroyed context must not run");
}

void TestApi::postIgnoresWhateverComesBack()
{
    m_backend.replyWithError(QStringLiteral("nobody is listening"));
    api()->post(
        QString::fromLatin1(Service), QStringLiteral("CloseResult"), {QStringLiteral("r1")}
    );

    api()->flush(2000);
    QCOMPARE(m_backend.requests().size(), 1);
    QCOMPARE(m_backend.requests().at(0).path, QStringLiteral("/rpc/query/CloseResult"));
}

void TestApi::flushWaitsForTheCallInFlight()
{
    m_backend.replyWithResult(QJsonValue(1));

    bool fired = false;
    api()->call(
        QString::fromLatin1(Service), QString::fromLatin1(Method), {}, this,
        [&fired](const QJsonValue &, const QString &) { fired = true; }
    );

    // The shutdown path: returning to the event loop instead would let the
    // application exit with the POST unsent.
    api()->flush(5000);
    QVERIFY(fired);
}

QTEST_MAIN(TestApi)

#include "tst_api.moc"
