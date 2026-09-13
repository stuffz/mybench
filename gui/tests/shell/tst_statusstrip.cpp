#include "shell/statusstrip.h"

#include "app/api.h"
#include "app/stubbackend.h"
#include "app/theme.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QLocale>
#include <QObject>
#include <QRegularExpression>
#include <QSize>
#include <QString>
#include <QTest>
#include <QTimer>

#include <cmath>

// The footer is the one line of server state that is on screen the whole time,
// so its exact text is the contract — and so is what it stops showing, because
// a number left over from a server nobody is watching any more reads as fact.
//
// Only the glyphs of each item carry an object name, so every value label below
// is reached through the item it sits in rather than by name or by position.
namespace
{

constexpr auto Conn = "c1";
constexpr auto OtherConn = "c2";
constexpr auto Label = "Prod";
constexpr auto OtherLabel = "Dev";
constexpr auto Inserted = "7 rows inserted into app.users";
constexpr int MessageMs = 8000;
constexpr auto Boom = "dial tcp 127.0.0.1:3306: connection refused";

constexpr auto Version = "8.0.36-log";
constexpr qint64 UptimeSeconds = 90061;
constexpr auto UptimeText = "1d 1h";
constexpr int Running = 3;
constexpr int Connected = 42;
constexpr auto ThreadsText = "3/42";
constexpr qint64 Questions = 1000;
constexpr qint64 QuestionsDelta = 6000;

// StatusPollMs in statusstrip.cpp: how often the footer refreshes itself.
constexpr int PollMs = 5000;
constexpr int FlushMs = 5000;

// The strip times the gap between two replies itself, in whole milliseconds,
// so the samples have to be pushed apart: far enough that the gap is measurable
// at all, and far enough that the rate it derives lands in a narrow band.
constexpr int SampleGapMs = 300;

constexpr int DotPx = 8;

// One GlobalStatus reply, the shape admin.StatusSnapshot marshals to.
QJsonObject snapshot(
    const QString &version, qint64 uptimeSeconds, int running, int connected, qint64 questions = 0
)
{
    QJsonObject o;
    o.insert(QStringLiteral("version"), version);
    o.insert(QStringLiteral("uptimeSeconds"), uptimeSeconds);
    o.insert(QStringLiteral("threadsRunning"), running);
    o.insert(QStringLiteral("threadsConnected"), connected);
    o.insert(QStringLiteral("questions"), questions);
    return o;
}

// The same server sampled again: only the counter moves.
QJsonObject sample(qint64 questions)
{
    return snapshot(QString::fromLatin1(Version), UptimeSeconds, Running, Connected, questions);
}

QLabel *besideIt(QLabel *known)
{
    if (!known)
    {
        return nullptr;
    }
    for (QLabel *l : known->parentWidget()->findChildren<QLabel *>())
    {
        if (l != known)
        {
            return l;
        }
    }
    return nullptr;
}

QLabel *metricValue(const StatusStrip &strip, const QString &iconName)
{
    return besideIt(strip.findChild<QLabel *>(QStringLiteral("glyph_") + iconName));
}

// The health item has no glyph of its own: its dot is the one label pinned to
// 8x8, and the round-trip text is what sits beside it.
QLabel *healthDot(const StatusStrip &strip)
{
    for (QLabel *l : strip.findChildren<QLabel *>())
    {
        if (l->minimumSize() == QSize(DotPx, DotPx) && l->maximumSize() == QSize(DotPx, DotPx))
        {
            return l;
        }
    }
    return nullptr;
}

// The error line is the only label parented straight to the strip; every other
// one sits inside an item.
QLabel *errorLine(const StatusStrip &strip)
{
    // By name, not by position: an unnamed lookup takes the first direct-child
    // label, which silently follows whatever order the constructor builds in.
    return strip.findChild<QLabel *>(QStringLiteral("statusError"));
}

// The number out of "<n> qps". A derived rate is bounded rather than matched,
// because the interval it was divided by is wall-clock time.
int rateFrom(const QString &text, bool *ok)
{
    const QString suffix = QStringLiteral(" qps");
    if (!text.endsWith(suffix))
    {
        *ok = false;
        return 0;
    }
    return text.chopped(suffix.size()).toInt(ok);
}

} // namespace

class TestStatusStrip : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void init();
    void cleanup();

    void startsBlankAndIdle();
    void rendersEveryFieldOfTheReply_data();
    void rendersEveryFieldOfTheReply();
    void pollsOnceAtWatchAndAgainOnEveryTimeout();
    void theFirstSampleHasNoRateToShow();
    void aFlatCounterReadsAsZeroQps();
    void aRisingCounterIsDividedByTheGapBetweenSamples();
    void aCounterThatWentBackwardsPrintsNoRate();
    void anErrorSurfacesAndTakesTheStripOffline();
    void anErrorKeepsTheLastSampleButDropsTheRate();
    void theRateStartsOverAfterAnOutage();
    void watchingAnotherServerDropsTheOldValues();
    void aReplyForAConnectionThatIsGoneIsIgnored();
    void noConnectionHidesTheStripAndStopsPolling();

    void aMessageIsVisibleAndClearsItself();
    void aMessageDoesNotWearTheErrorColour();
    void watchingAnotherServerDropsTheOldHealthColour();

private:
    void watchAndWait();
    void poll();

    StubBackend m_backend;
    StatusStrip *m_strip = nullptr;
    QTimer *m_timer = nullptr;
    QLabel *m_latency = nullptr;
    QLabel *m_version = nullptr;
    QLabel *m_uptime = nullptr;
    QLabel *m_threads = nullptr;
    QLabel *m_qps = nullptr;
    QLabel *m_error = nullptr;
};

void TestStatusStrip::initTestCase()
{
    // Every item is built with a themed glyph and a themed label, so a palette
    // has to be installed before the first strip exists.
    theme::apply(theme::defaultApp, 13);
    // fmtUptime is the one formatter the strip prints through and it does not
    // read the locale, but its neighbours in fmt.cpp do; pinning it here rather
    // than relying on that staying true.
    QLocale::setDefault(QLocale(QLocale::English, QLocale::UnitedStates));
}

void TestStatusStrip::init()
{
    QVERIFY(m_backend.listening());
    api()->setEndpoint(m_backend.base(), QString());
    m_backend.clearRequests();
    // Neither a snapshot nor an error, so a slot only ever sees the reply it
    // asked the stub for.
    m_backend.replyWithResult(QJsonObject());

    m_strip = new StatusStrip;
    m_timer = m_strip->findChild<QTimer *>();
    m_latency = besideIt(healthDot(*m_strip));
    m_version = metricValue(*m_strip, QStringLiteral("database"));
    m_uptime = metricValue(*m_strip, QStringLiteral("clock"));
    m_threads = metricValue(*m_strip, QStringLiteral("cpu"));
    m_qps = metricValue(*m_strip, QStringLiteral("activity"));
    m_error = errorLine(*m_strip);

    QVERIFY(m_timer);
    for (QLabel *l : {m_latency, m_version, m_uptime, m_threads, m_qps, m_error})
    {
        QVERIFY(l);
    }
}

void TestStatusStrip::cleanup()
{
    // Deleting the strip drops the callback of a poll still in flight, but the
    // request is on the wire either way: drain it so the next slot starts with
    // an empty request list.
    delete m_strip;
    m_strip = nullptr;
    api()->flush(FlushMs);
}

void TestStatusStrip::startsBlankAndIdle()
{
    // Nothing is being watched yet, so there is no server to report on and no
    // reason to be asking one for its counters.
    for (QLabel *l : {m_latency, m_version, m_uptime, m_threads, m_qps, m_error})
    {
        QVERIFY(l->text().isEmpty());
    }
    QVERIFY(!m_timer->isActive());
    QVERIFY(m_backend.requests().isEmpty());
}

void TestStatusStrip::rendersEveryFieldOfTheReply_data()
{
    QTest::addColumn<QJsonObject>("status");
    QTest::addColumn<QString>("version");
    QTest::addColumn<QString>("uptime");
    QTest::addColumn<QString>("threads");

    QTest::newRow("a running server")
        << sample(Questions) << QString::fromLatin1(Version) << QString::fromLatin1(UptimeText)
        << QString::fromLatin1(ThreadsText);

    // Under an hour the uptime is minutes alone, and a server with nothing
    // running still has the one connection doing the asking.
    QTest::newRow("just restarted")
        << snapshot(QStringLiteral("10.11.6-MariaDB"), 45, 0, 1)
        << QStringLiteral("10.11.6-MariaDB") << QStringLiteral("0m") << QStringLiteral("0/1");

    // The counters are read out of the reply one key at a time, so a reply with
    // none of them in it prints the zeros rather than nothing.
    QTest::newRow("a reply with nothing in it")
        << QJsonObject() << QString() << QStringLiteral("0m") << QStringLiteral("0/0");
}

void TestStatusStrip::rendersEveryFieldOfTheReply()
{
    QFETCH(QJsonObject, status);
    QFETCH(QString, version);
    QFETCH(QString, uptime);
    QFETCH(QString, threads);

    m_backend.replyWithResult(status);
    watchAndWait();

    QCOMPARE(m_version->text(), version);
    QCOMPARE(m_uptime->text(), uptime);
    QCOMPARE(m_threads->text(), threads);
    QVERIFY(m_error->text().isEmpty());

    // The round trip is measured time, so only its shape can be pinned.
    static const QRegularExpression roundTrip(QStringLiteral("^\\d+ms$"));
    QVERIFY2(roundTrip.match(m_latency->text()).hasMatch(), qPrintable(m_latency->text()));
    QVERIFY(m_latency->toolTip().startsWith(QString::fromLatin1(Label) + " · healthy · "));
    QVERIFY(m_latency->toolTip().contains(QStringLiteral(" · last check ")));
}

void TestStatusStrip::pollsOnceAtWatchAndAgainOnEveryTimeout()
{
    m_backend.replyWithResult(sample(Questions));
    watchAndWait();

    // Watching a server asks it at once rather than leaving the footer blank
    // for the first five seconds.
    QCOMPARE(m_backend.requests().size(), 1);
    const StubBackend::Request &first = m_backend.requests().at(0);
    QCOMPARE(first.path, QStringLiteral("/rpc/admin/GlobalStatus"));
    QCOMPARE(first.args, QJsonArray({QString::fromLatin1(Conn)}));
    QVERIFY2(m_timer->isActive(), "a watched server has to keep being sampled");
    QCOMPARE(m_timer->interval(), PollMs);

    m_backend.replyWithResult(snapshot(QStringLiteral("8.4.0"), UptimeSeconds, Running, Connected));
    poll();

    QCOMPARE(m_backend.requests().size(), 2);
    QCOMPARE(m_backend.requests().at(1).args, QJsonArray({QString::fromLatin1(Conn)}));
    QCOMPARE(m_version->text(), QStringLiteral("8.4.0"));
}

void TestStatusStrip::theFirstSampleHasNoRateToShow()
{
    m_backend.replyWithResult(sample(Questions));
    watchAndWait();

    // One reading of a monotonic counter is not a rate: there is nothing to
    // subtract it from and no interval to divide it by.
    QVERIFY2(m_qps->text().isEmpty(), qPrintable(m_qps->text()));
    QCOMPARE(m_version->text(), QString::fromLatin1(Version));
}

void TestStatusStrip::aFlatCounterReadsAsZeroQps()
{
    m_backend.replyWithResult(sample(Questions));
    watchAndWait();

    QTest::qWait(SampleGapMs);
    m_backend.replyWithResult(sample(Questions));
    poll();

    // An idle server: the counter has not moved, and whatever the interval was
    // the rate across it is zero.
    QCOMPARE(m_qps->text(), QStringLiteral("0 qps"));
}

void TestStatusStrip::aRisingCounterIsDividedByTheGapBetweenSamples()
{
    m_backend.replyWithResult(sample(Questions));
    const qint64 beforeFirst = QDateTime::currentMSecsSinceEpoch();
    watchAndWait();
    const qint64 afterFirst = QDateTime::currentMSecsSinceEpoch();

    QTest::qWait(SampleGapMs);

    m_backend.replyWithResult(sample(Questions + QuestionsDelta));
    const qint64 beforeSecond = QDateTime::currentMSecsSinceEpoch();
    poll();
    const qint64 afterSecond = QDateTime::currentMSecsSinceEpoch();

    // The strip stamps each sample as its reply lands, and those two instants
    // are not observable from here — but they are bracketed by the four taken
    // above, which bounds the interval and so bounds the rate:
    //
    //   beforeFirst [ .. first sample .. ] afterFirst ... beforeSecond [ .. ] afterSecond
    //               |<------------------- widest ------------------------------->|
    //                                                |<-- narrowest -->|
    const double widest = double(afterSecond - beforeFirst) / 1000.0;
    const double narrowest = double(beforeSecond - afterFirst) / 1000.0;
    QVERIFY(narrowest > 0);

    bool ok = false;
    const int rate = rateFrom(m_qps->text(), &ok);
    QVERIFY2(ok, qPrintable(m_qps->text()));
    QVERIFY2(rate >= int(std::floor(double(QuestionsDelta) / widest)), qPrintable(m_qps->text()));
    QVERIFY2(rate <= int(std::ceil(double(QuestionsDelta) / narrowest)), qPrintable(m_qps->text()));
}

void TestStatusStrip::aCounterThatWentBackwardsPrintsNoRate()
{
    m_backend.replyWithResult(sample(Questions));
    watchAndWait();

    QTest::qWait(SampleGapMs);
    m_backend.replyWithResult(sample(0));
    poll();

    // The counter resets when the server restarts, and a negative rate would
    // be a lie about a server that was just down.
    QVERIFY2(m_qps->text().isEmpty(), qPrintable(m_qps->text()));
}

void TestStatusStrip::anErrorSurfacesAndTakesTheStripOffline()
{
    m_backend.replyWithError(QString::fromLatin1(Boom));
    watchAndWait();

    QCOMPARE(m_error->text(), QString::fromLatin1(Boom));
    QCOMPARE(m_error->toolTip(), QString::fromLatin1(Boom));
    QCOMPARE(m_latency->text(), QStringLiteral("offline"));

    const QString tip = m_latency->toolTip();
    QVERIFY2(
        tip.startsWith(QString::fromLatin1(Label) + " · unreachable: " + QString::fromLatin1(Boom)),
        qPrintable(tip)
    );
    QVERIFY2(tip.contains(QStringLiteral(" · last check ")), qPrintable(tip));
}

void TestStatusStrip::anErrorKeepsTheLastSampleButDropsTheRate()
{
    m_backend.replyWithResult(sample(Questions));
    watchAndWait();
    QTest::qWait(SampleGapMs);
    m_backend.replyWithResult(sample(Questions + QuestionsDelta));
    poll();
    QVERIFY(!m_qps->text().isEmpty());

    m_backend.replyWithError(QString::fromLatin1(Boom));
    poll();

    // The counters stay put: they are still the last thing known about the
    // server, and the dot and the round-trip text carry the bad news.
    QCOMPARE(m_version->text(), QString::fromLatin1(Version));
    QCOMPARE(m_uptime->text(), QString::fromLatin1(UptimeText));
    QCOMPARE(m_threads->text(), QString::fromLatin1(ThreadsText));
    // The rate is the one value that cannot survive the gap, so it goes.
    QVERIFY2(m_qps->text().isEmpty(), qPrintable(m_qps->text()));
}

void TestStatusStrip::theRateStartsOverAfterAnOutage()
{
    m_backend.replyWithResult(sample(Questions));
    watchAndWait();

    m_backend.replyWithError(QString::fromLatin1(Boom));
    poll();

    QTest::qWait(SampleGapMs);
    m_backend.replyWithResult(sample(Questions + QuestionsDelta));
    poll();

    // The counter kept moving while the server was unreachable. Dividing all of
    // it by the one interval that happens to be measurable would print a spike
    // that never happened, so the first sample back is a first sample again.
    QVERIFY2(m_qps->text().isEmpty(), qPrintable(m_qps->text()));
}

void TestStatusStrip::watchingAnotherServerDropsTheOldValues()
{
    m_backend.replyWithResult(sample(Questions));
    watchAndWait();
    QVERIFY(!m_version->text().isEmpty());

    // Deliberately not waited on: what matters is what the strip shows in the
    // moment it is pointed somewhere else, before that server has answered.
    m_strip->watch(QString::fromLatin1(OtherConn), QString::fromLatin1(OtherLabel));

    for (QLabel *l : {m_latency, m_version, m_uptime, m_threads, m_qps, m_error})
    {
        QVERIFY2(l->text().isEmpty(), qPrintable(l->text()));
    }
    QCOMPARE(m_latency->toolTip(), QString::fromLatin1(OtherLabel));
}

void TestStatusStrip::aReplyForAConnectionThatIsGoneIsIgnored()
{
    m_backend.replyWithResult(sample(Questions));
    // The reply cannot land before the event loop runs, so the poll below is
    // still in flight when the strip stops watching.
    m_strip->watch(QString::fromLatin1(Conn), QString::fromLatin1(Label));
    m_strip->watch({}, {});
    api()->flush(FlushMs);

    // Filling the strip back in here would leave the last server's numbers
    // under a footer that is no longer about any server at all.
    for (QLabel *l : {m_latency, m_version, m_uptime, m_threads, m_qps, m_error})
    {
        QVERIFY2(l->text().isEmpty(), qPrintable(l->text()));
    }
    QCOMPARE(m_backend.requests().size(), 1);
}

void TestStatusStrip::noConnectionHidesTheStripAndStopsPolling()
{
    m_backend.replyWithResult(sample(Questions));
    watchAndWait();
    QVERIFY(m_strip->isVisible());
    QVERIFY(m_timer->isActive());

    m_strip->watch({}, {});

    QVERIFY(!m_strip->isVisible());
    QVERIFY2(!m_timer->isActive(), "a footer about nothing must not keep asking");
}

// The immediate poll watch() makes, run to completion.
void TestStatusStrip::watchAndWait()
{
    m_strip->watch(QString::fromLatin1(Conn), QString::fromLatin1(Label));
    api()->flush(FlushMs);
}

// The poll the five-second timer would have made, without the five seconds.
void TestStatusStrip::poll()
{
    QVERIFY(QMetaObject::invokeMethod(m_timer, "timeout"));
    api()->flush(FlushMs);
}

void TestStatusStrip::aMessageIsVisibleAndClearsItself()
{
    // The window's QStatusBar is hidden for the life of the app, so anything
    // written there is never seen. Confirmations land here instead, and "here"
    // has to be a label that is actually shown.
    watchAndWait();

    QLabel *message = m_strip->findChild<QLabel *>(QStringLiteral("statusMessage"));
    QVERIFY(message);
    QVERIFY2(!message->isVisibleTo(m_strip), "an empty message must not hold space");

    m_strip->showMessage(QString::fromLatin1(Inserted), MessageMs);
    QCOMPARE(message->text(), QString::fromLatin1(Inserted));
    QVERIFY(message->isVisibleTo(m_strip));

    QTimer *clear = m_strip->findChild<QTimer *>(QStringLiteral("statusMessageTimer"));
    QVERIFY2(clear, "the message has to clear itself or it becomes furniture");
    QCOMPARE(clear->interval(), MessageMs);
    QMetaObject::invokeMethod(clear, "timeout");

    QVERIFY(message->text().isEmpty());
    QVERIFY(!message->isVisibleTo(m_strip));
}

void TestStatusStrip::aMessageDoesNotWearTheErrorColour()
{
    // The error line is styled destructive. A row-count confirmation sharing
    // that label would read as a failure.
    watchAndWait();
    m_strip->showMessage(QString::fromLatin1(Inserted), MessageMs);

    const QLabel *message = m_strip->findChild<QLabel *>(QStringLiteral("statusMessage"));
    QVERIFY(message);
    QVERIFY(message != m_error);
    QVERIFY(message->property("tone").toString() != QStringLiteral("destructive"));
}

void TestStatusStrip::watchingAnotherServerDropsTheOldHealthColour()
{
    // Pointing the footer at a different server must not leave the previous
    // one's colour and message standing. Green for a server never contacted is
    // worse than no reading at all.
    watchAndWait();
    QVERIFY(!healthDot(*m_strip)->styleSheet().isEmpty());
    m_strip->showMessage(QString::fromLatin1(Inserted), MessageMs);

    m_backend.clearRequests();
    m_strip->watch(QString::fromLatin1(OtherConn), QString::fromLatin1(OtherLabel));

    QVERIFY2(
        healthDot(*m_strip)->styleSheet().isEmpty(),
        "the dot still wears the colour of the server we just left"
    );
    const QLabel *message = m_strip->findChild<QLabel *>(QStringLiteral("statusMessage"));
    QVERIFY(message);
    QVERIFY2(message->text().isEmpty(), "the old server's confirmation followed us across");
}

QTEST_MAIN(TestStatusStrip)

#include "tst_statusstrip.moc"
