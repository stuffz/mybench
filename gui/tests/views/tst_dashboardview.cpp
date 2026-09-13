#include "views/dashboardview.h"

#include "app/api.h"
#include "app/stubbackend.h"
#include "app/theme.h"
#include "views/chart.h"
#include "views/statcard.h"

#include <QCheckBox>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QLatin1String>
#include <QLayout>
#include <QList>
#include <QLocale>
#include <QObject>
#include <QPixmap>
#include <QPushButton>
#include <QResizeEvent>
#include <QSize>
#include <QString>
#include <QTest>
#include <QTimer>
#include <QWidget>

// The panel keeps every counter it has read private, and the charts it draws
// report nothing about their samples either, so what a test can reach is the
// stat cards' text, the error strip and the two poll timers. The numbers worth
// pinning are the derived ones: a per-second rate is a delta over an interval,
// and both halves go wrong quietly.
//
// The interval is wall clock between the two replies: applySnapshot reads
// QDateTime::currentMSecsSinceEpoch() rather than the snapshot's own sampledAt,
// so an exact rate is not reproducible from outside. The slots below pin the
// cases where the rate is defined to be zero, compare two rates that share one
// interval, and bound one rate by an interval the test measures itself.
namespace
{

constexpr auto ConnID = "conn-1";
constexpr auto RefreshText = "Refresh";
constexpr int FontSize = 13;
constexpr int FlushMs = 5000;

// Mirrors PollMs and SectionPollMs in dashboardview.cpp: the interval is what
// tells the two timers apart from outside.
constexpr int PollMs = 2000;
constexpr int SectionPollMs = 60000;

constexpr auto DashboardPath = "/rpc/admin/Dashboard";

// The row PanelBase parks the error strip in, between the title row and the
// body (pinned by tst_panelbase).
constexpr int ErrorRow = 1;

// Wide enough that StatCard::elide() leaves the text it was handed alone.
// MainWindow gives the sidebar 240 and clamps the content pane to 400 below
// that (mainwindow.cpp), so this is the tightest layout a tile ever sees.
constexpr int NarrowestContentWidth = 400;
constexpr int ContentHeight = 800;
constexpr int CardWidth = 640;
constexpr int BaseFontPx = 13;
// StatCard::elide() keeps 24px of the tile for padding.
constexpr int CardTextPadding = 24;

// Which of the palette's two alarm colours the card's value should be wearing.
enum class Tone
{
    None,
    Warning,
    Destructive,
};
constexpr int CardHeight = 80;

constexpr auto ValueName = "kpiValue";
constexpr auto SubName = "kpiSub";

// The label each card is built with, which is what tells the tiles apart.
constexpr auto Queries = "Queries / s";
constexpr auto Threads = "Threads Running";
constexpr auto Connections = "Connections";
constexpr auto Slow = "Slow Queries";
constexpr auto PoolHit = "Buffer Pool Hit";
constexpr auto PoolDirty = "Buffer Pool Dirty";
constexpr auto Rows = "InnoDB Rows / s";
constexpr auto DiskIO = "Disk I/O";
constexpr auto RedoLog = "Redo Log";
constexpr auto RowLocks = "Row Lock Waits";
constexpr auto Deadlocks = "Deadlocks";
constexpr auto History = "History List";

constexpr int ChartWidth = 400;
constexpr int ChartHeight = 200;
constexpr int ChartCount = 8;

constexpr double MsPerSecond = 1000.0;

// One counter moved by this much across one interval. Small enough that the
// rate stays inside fmtCompact's plain branch for any interval a test can see.
constexpr int QuestionsBase = 1000;
constexpr int QuestionDelta = 500;
constexpr int WaitMs = 250;
// fmtRate rounds a rate past 10 to whole queries.
constexpr double Rounding = 1.0;

// A power of two: three rates summed are then bit for bit the rate of their
// summed delta, whatever the interval divides out to.
constexpr int RowDelta = 256;

// What one poll later looks like on a server under load.
constexpr int Bump = 1000;

constexpr auto Version = "8.0.36";
constexpr auto SampledAt = "2026-09-13 10:00:00";
// One day and two hours, the largest two units fmtUptime keeps.
constexpr int UptimeSeconds = 93784;

constexpr auto Denied = "admin.Dashboard: Access denied for user";
constexpr auto MetricsNote = "InnoDB metrics unavailable: no SELECT on information_schema";
constexpr auto LockWaitsNote = "Lock waits unavailable: performance_schema is off";

enum class Live
{
    On,
    Off
};

// The shape admin.Dashboard answers with (backend/internal/admin/dashboard.go).
// The fields only one slot cares about (innodb, trx, notes) are inserted there
// rather than carried through here.
QJsonObject snapshot(const QJsonObject &status, const QJsonObject &vars = {})
{
    QJsonObject snap;
    snap.insert("status", status);
    snap.insert("vars", vars);
    return snap;
}

// One poll of a server that has been up a while, with every counter the cards
// read present so that a zero in a test means a withheld rate, not a gap.
QJsonObject warmStatus()
{
    return QJsonObject{
        {"Questions", QuestionsBase},
        {"Threads_connected", 50},
        {"Threads_running", 3},
        {"Threads_cached", 7},
        {"Max_used_connections", 60},
        {"Slow_queries", 5},
        {"Innodb_buffer_pool_read_requests", 9900},
        {"Innodb_buffer_pool_reads", 100},
        {"Innodb_buffer_pool_pages_total", 1000},
        {"Innodb_buffer_pool_pages_data", 800},
        {"Innodb_buffer_pool_pages_free", 170},
        {"Innodb_buffer_pool_pages_dirty", 25},
        {"Innodb_rows_read", 4000},
        {"Innodb_rows_inserted", 100},
        {"Innodb_rows_updated", 200},
        {"Innodb_rows_deleted", 300},
        {"Innodb_data_reads", 400},
        {"Innodb_data_writes", 600},
        {"Innodb_data_read", 65536},
        {"Innodb_data_written", 131072},
        {"Innodb_data_fsyncs", 40},
        {"Innodb_log_writes", 90},
        {"Innodb_os_log_written", 8192},
        {"Innodb_log_waits", 0},
        {"Innodb_row_lock_waits", 12},
        {"Innodb_row_lock_current_waits", 0},
        {"Innodb_row_lock_time_avg", 4},
        {"Innodb_deadlocks", 2},
        {"Innodb_history_list_length", 42},
    };
}

QJsonObject busyStatus()
{
    QJsonObject status = warmStatus();
    for (const QString &key : status.keys())
    {
        status.insert(key, status.value(key).toDouble() + Bump);
    }
    return status;
}

QJsonObject serverVars()
{
    return QJsonObject{
        {"max_connections", "200"},
        {"long_query_time", "10.000000"},
        {"innodb_buffer_pool_size", "134217728"},
    };
}

StatCard *cardFor(const QWidget &view, const QString &label)
{
    const QList<StatCard *> cards = view.findChildren<StatCard *>();
    for (StatCard *card : cards)
    {
        const QList<QLabel *> texts = card->findChildren<QLabel *>();
        for (const QLabel *text : texts)
        {
            // The value and sub lines carry object names; the unnamed label
            // with text in it is the one the card was built with.
            if (text->objectName().isEmpty() && text->text() == label)
            {
                return card;
            }
        }
    }
    return nullptr;
}

// StatCard elides both lines to its own width, and a hidden widget is never
// handed the resize event that recomputes them: widen the tile and deliver one,
// or a five-digit count reads back as "1,2…".
QString cardLine(const QWidget &view, const char *label, const char *line)
{
    StatCard *card = cardFor(view, QString::fromLatin1(label));
    if (!card)
    {
        return {};
    }
    card->resize(CardWidth, CardHeight);
    QResizeEvent resized(card->size(), QSize());
    QCoreApplication::sendEvent(card, &resized);

    const QLabel *text = card->findChild<QLabel *>(QString::fromLatin1(line));
    return text ? text->text() : QString();
}

QString cardValue(const QWidget &view, const char *label)
{
    return cardLine(view, label, ValueName);
}

QString cardSub(const QWidget &view, const char *label)
{
    return cardLine(view, label, SubName);
}

bool showsLabel(const QWidget &view, const QString &text)
{
    const QList<QLabel *> labels = view.findChildren<QLabel *>();
    for (const QLabel *label : labels)
    {
        if (label->text() == text)
        {
            return true;
        }
    }
    return false;
}

QLabel *errorStrip(const QWidget &view)
{
    QLayout *root = view.layout();
    if (!root || root->count() <= ErrorRow)
    {
        return nullptr;
    }
    return qobject_cast<QLabel *>(root->itemAt(ErrorRow)->widget());
}

// The panel is never shown in a test, and a child of a hidden parent is never
// isVisible(): ask whether the strip would come up with the panel instead.
bool errorShown(const QWidget &view)
{
    const QLabel *strip = errorStrip(view);
    return strip && strip->isVisibleTo(&view);
}

QTimer *timerWith(const QWidget &view, int interval)
{
    const QList<QTimer *> timers = view.findChildren<QTimer *>();
    for (QTimer *timer : timers)
    {
        if (timer->interval() == interval)
        {
            return timer;
        }
    }
    return nullptr;
}

QPushButton *refreshButton(const QWidget &view)
{
    const QList<QPushButton *> buttons = view.findChildren<QPushButton *>();
    for (QPushButton *button : buttons)
    {
        if (button->text() == QLatin1String(RefreshText))
        {
            return button;
        }
    }
    return nullptr;
}

bool setLive(const QWidget &view, Live state)
{
    QCheckBox *box = view.findChild<QCheckBox *>();
    if (!box)
    {
        return false;
    }
    box->setChecked(state == Live::On);
    return true;
}

// One poll, run to completion. The Refresh button is the only way in from
// outside, and flush() returns once the reply has been applied.
bool pollOnce(StubBackend &backend, const QWidget &view, const QJsonObject &snap)
{
    QPushButton *button = refreshButton(view);
    if (!button)
    {
        return false;
    }
    backend.replyWithResult(snap);
    button->click();
    api()->flush(FlushMs);
    return true;
}

// The section poll rides the same stub reply, so count the counter polls alone.
int pollsSeen(const StubBackend &backend)
{
    int seen = 0;
    for (const StubBackend::Request &req : backend.requests())
    {
        if (req.path == QLatin1String(DashboardPath))
        {
            ++seen;
        }
    }
    return seen;
}

} // namespace

class TestDashboardView : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void init();

    void theFirstSampleShowsNoRatesAtAll();
    void aSecondSampleDividesTheDeltaByTheInterval();
    void everyRateInASampleSharesOneInterval();
    void aCounterThatWentBackwardsReportsNothing();
    void aCounterMissingFromEitherSampleHasNoRate();

    void theCardsReadOneSampleOfAWarmServer();
    void theHitRateFitsTheCardEvenAtTheNarrowestWindow();
    void theBufferPoolHitRateOfAnIdleServerIsZero();
    void theHeadlineIsTheWindowWhileTheSubLineIsTheLifetime();
    void anIdleWindowShowsNoReadingRatherThanZero();
    void theHitRateToneFollowsTheThresholds_data();
    void theHitRateToneFollowsTheThresholds();
    void theBufferPoolHitRateDividesByTheRequestsAlone();
    void aPoolThatMissedEveryRequestReadsAsZero();
    void readsWithoutRequestsDoNotDivideByZero();
    void percentagesWithNoTotalReadAsZero();
    void innodbMetricsWinOverTheStatusCounters();
    void theHeaderLineNamesTheServerAndItsUptime();

    void theDashboardOpensPausedSoNothingSamplesUnasked();
    void thePollTimerSamplesWhileLiveIsChecked();
    void clearingLiveStopsTheSampling();
    void aTickIsSkippedWhileAPollIsStillInFlight();

    void aFailedPollSurfacesInsteadOfBlankingTheCards();
    void theNotesOfASnapshotReachTheErrorStrip();

    void theChartsTakeASampleWithoutTrouble();

private:
    StubBackend m_backend;
};

void TestDashboardView::initTestCase()
{
    // The cards, the charts and the error strip all build themselves from
    // theme::current(); apply() is what installs it.
    theme::apply(theme::defaultApp, FontSize);

    // Every number on the page is printed through fmt.cpp, which goes through
    // QLocale(): pin it rather than let the host locale group the thousands.
    QLocale::setDefault(QLocale(QLocale::English, QLocale::UnitedStates));
}

void TestDashboardView::init()
{
    m_backend.clearRequests();
    QVERIFY(m_backend.listening());
    api()->setEndpoint(m_backend.base(), QString());
}

void TestDashboardView::theFirstSampleShowsNoRatesAtAll()
{
    // The panel polls once from its constructor, so the snapshot canned here is
    // the one with no predecessor: no interval, and every rate has to read as
    // nothing. Dividing by the interval that is not there would print "inf".
    m_backend.replyWithResult(snapshot(warmStatus(), serverVars()));
    DashboardView view(QString::fromLatin1(ConnID));
    api()->flush(FlushMs);

    QCOMPARE(cardValue(view, Queries), QStringLiteral("0"));
    QCOMPARE(cardValue(view, Rows), QStringLiteral("0"));
    QCOMPARE(cardSub(view, Rows), QStringLiteral("0 read · 0 written"));
    QCOMPARE(cardValue(view, DiskIO), QStringLiteral("0 IOPS"));
    QCOMPARE(cardSub(view, DiskIO), QStringLiteral("0 B/s in · 0 B/s out · 0 fsync/s"));
    QCOMPARE(cardValue(view, RedoLog), QStringLiteral("0 writes/s"));
    QCOMPARE(cardSub(view, RedoLog), QStringLiteral("0 B/s · 0 log waits"));

    // The counters themselves come off the same snapshot: this is a missing
    // interval, not a missing sample.
    QCOMPARE(cardSub(view, Queries), QStringLiteral("1,000 total"));
    QCOMPARE(cardValue(view, Threads), QStringLiteral("3"));
}

void TestDashboardView::aSecondSampleDividesTheDeltaByTheInterval()
{
    // Both ends of the interval are known: the wait below is the shortest it
    // can be, and the window around the whole exchange is the longest.
    QElapsedTimer window;
    window.start();

    m_backend.replyWithResult(snapshot(QJsonObject{{"Questions", QuestionsBase}}));
    DashboardView view(QString::fromLatin1(ConnID));
    api()->flush(FlushMs);
    QVERIFY(setLive(view, Live::Off));

    QTest::qWait(WaitMs);
    QVERIFY(pollOnce(
        m_backend, view, snapshot(QJsonObject{{"Questions", QuestionsBase + QuestionDelta}})
    ));

    const double longest = double(window.elapsed()) / MsPerSecond;
    bool ok = false;
    const double shown = QLocale().toDouble(cardValue(view, Queries), &ok);
    QVERIFY2(ok, "the rate has to read back as a number");

    QVERIFY(shown <= QuestionDelta / (WaitMs / MsPerSecond) + Rounding);
    QVERIFY(shown >= QuestionDelta / longest - Rounding);

    // The card's own sub line is the counter, not the rate.
    QCOMPARE(cardSub(view, Queries), QStringLiteral("1,500 total"));
}

void TestDashboardView::everyRateInASampleSharesOneInterval()
{
    // Questions moves by exactly as much as the three row-write counters do
    // between them. One interval divides all four, so the two cards have to
    // print the same number whatever that interval turned out to be.
    m_backend.replyWithResult(snapshot(QJsonObject{
        {"Questions", 0},
        {"Innodb_rows_read", 0},
        {"Innodb_rows_inserted", 0},
        {"Innodb_rows_updated", 0},
        {"Innodb_rows_deleted", 0},
    }));
    DashboardView view(QString::fromLatin1(ConnID));
    api()->flush(FlushMs);
    QVERIFY(setLive(view, Live::Off));

    QVERIFY(pollOnce(
        m_backend, view,
        snapshot(QJsonObject{
            {"Questions", 4 * RowDelta},
            {"Innodb_rows_read", 0},
            {"Innodb_rows_inserted", RowDelta},
            {"Innodb_rows_updated", RowDelta},
            {"Innodb_rows_deleted", 2 * RowDelta},
        })
    ));

    const QString rate = cardValue(view, Queries);
    QVERIFY2(rate != QStringLiteral("0"), "a counter that moved has a rate");

    QCOMPARE(cardValue(view, Rows), rate);
    QCOMPARE(cardSub(view, Rows), QStringLiteral("0 read · %1 written").arg(rate));
}

void TestDashboardView::aCounterThatWentBackwardsReportsNothing()
{
    // A restart puts the counters back to nearly zero. The delta is negative,
    // and a rate of nothing is what the panel reports rather than a dip.
    m_backend.replyWithResult(
        snapshot(QJsonObject{{"Questions", QuestionsBase}, {"Innodb_rows_read", 5000}})
    );
    DashboardView view(QString::fromLatin1(ConnID));
    api()->flush(FlushMs);
    QVERIFY(setLive(view, Live::Off));

    QVERIFY(pollOnce(
        m_backend, view, snapshot(QJsonObject{{"Questions", 10}, {"Innodb_rows_read", 20}})
    ));

    QCOMPARE(cardValue(view, Queries), QStringLiteral("0"));
    QCOMPARE(cardValue(view, Rows), QStringLiteral("0"));
    QCOMPARE(cardSub(view, Rows), QStringLiteral("0 read · 0 written"));

    // The restarted counters are still read: only the rate is withheld.
    QCOMPARE(cardSub(view, Queries), QStringLiteral("10 total"));
}

void TestDashboardView::aCounterMissingFromEitherSampleHasNoRate()
{
    // The I/O counters are in the first sample only and Slow_queries in the
    // second only. With nothing to subtract at one end, neither can produce a
    // rate, and an arriving key must not read as a jump from zero.
    m_backend.replyWithResult(
        snapshot(QJsonObject{{"Innodb_data_reads", 400}, {"Innodb_data_writes", 600}}, serverVars())
    );
    DashboardView view(QString::fromLatin1(ConnID));
    api()->flush(FlushMs);
    QVERIFY(setLive(view, Live::Off));

    QVERIFY(pollOnce(m_backend, view, snapshot(QJsonObject{{"Slow_queries", 40}}, serverVars())));

    QCOMPARE(cardValue(view, Slow), QStringLiteral("40"));
    QCOMPARE(cardSub(view, Slow), QStringLiteral("0 / s · over 10 s"));
    QCOMPARE(cardValue(view, DiskIO), QStringLiteral("0 IOPS"));
}

void TestDashboardView::theCardsReadOneSampleOfAWarmServer()
{
    // Nothing here needs an interval: these are the counters, the percentages
    // and the two variables the cards divide by, read off one snapshot.
    m_backend.replyWithResult(snapshot(warmStatus(), serverVars()));
    DashboardView view(QString::fromLatin1(ConnID));
    api()->flush(FlushMs);

    QCOMPARE(cardValue(view, Threads), QStringLiteral("3"));
    QCOMPARE(cardSub(view, Threads), QStringLiteral("50 connected · 7 cached"));

    QCOMPARE(cardValue(view, Connections), QStringLiteral("25%"));
    QCOMPARE(cardSub(view, Connections), QStringLiteral("50 of 200 · peak 60"));

    QCOMPARE(cardValue(view, Slow), QStringLiteral("5"));
    QCOMPARE(cardSub(view, Slow), QStringLiteral("0 / s · over 10 s"));

    // One sample is a point, not an interval, so the headline has nothing to
    // divide. 100 of the reads went to disk out of 9,900 read requests, and a
    // disk read is itself a logical request, so the lifetime ratio on the sub
    // line is 100/9,900 rather than 100/10,000.
    QCOMPARE(cardValue(view, PoolHit), QStringLiteral("—"));
    QCOMPARE(cardSub(view, PoolHit), QStringLiteral("128 MiB pool · 98.9899% since start"));

    // 25 dirty, 800 data and 170 free pages of a 1,000 page pool.
    QCOMPARE(cardValue(view, PoolDirty), QStringLiteral("2.5%"));
    QCOMPARE(cardSub(view, PoolDirty), QStringLiteral("80% data · 17% free"));

    QCOMPARE(cardValue(view, RowLocks), QStringLiteral("12"));
    QCOMPARE(cardSub(view, RowLocks), QStringLiteral("0 waiting now · avg 4 ms"));

    // No INNODB_METRICS in this snapshot, which is the MariaDB shape: both
    // cards fall back to the status counter.
    QCOMPARE(cardValue(view, Deadlocks), QStringLiteral("2"));
    QCOMPARE(cardSub(view, Deadlocks), QStringLiteral("0 lock timeouts"));
    QCOMPARE(cardValue(view, History), QStringLiteral("42"));
    QCOMPARE(cardSub(view, History), QStringLiteral("0 live transactions"));
}

void TestDashboardView::theHitRateFitsTheCardEvenAtTheNarrowestWindow()
{
    // Four decimals cost four characters over "100%". StatCard elides to its
    // own width, so the reading has to be checked at the width the tile really
    // gets rather than at the one cardValue() forces on it. 400 is the content
    // pane's clamp; the grid floors the view above it, which is the tightest
    // layout a dashboard tile ever sees.
    theme::apply(theme::defaultApp, BaseFontPx);
    m_backend.replyWithResult(snapshot(
        QJsonObject{
            {"Innodb_buffer_pool_read_requests", 12557783895289LL},
            {"Innodb_buffer_pool_reads", 473864960}
        },
        serverVars()
    ));
    DashboardView view(QString::fromLatin1(ConnID));
    api()->flush(FlushMs);
    QVERIFY(setLive(view, Live::Off));
    QVERIFY(pollOnce(
        m_backend, view,
        snapshot(
            QJsonObject{
                {"Innodb_buffer_pool_read_requests", 12557783895289LL + 1000000},
                {"Innodb_buffer_pool_reads", 473864960 + 38}
            },
            serverVars()
        )
    ));

    view.resize(NarrowestContentWidth, ContentHeight);
    view.layout()->activate();

    StatCard *card = cardFor(view, QString::fromLatin1(PoolHit));
    QVERIFY(card);
    const QLabel *value = card->findChild<QLabel *>(QString::fromLatin1(ValueName));
    QVERIFY(value);
    QCOMPARE(value->text(), QStringLiteral("99.9962%"));

    // And say how much room is actually spare, so a future font bump fails here
    // rather than silently truncating the only digits that carry the signal.
    const int used = QFontMetrics(value->font()).horizontalAdvance(QStringLiteral("99.9962%"));
    QVERIFY2(
        used <= card->width() - CardTextPadding,
        qPrintable(QStringLiteral("hit rate needs %1px, tile offers %2px")
                       .arg(used)
                       .arg(card->width() - CardTextPadding))
    );
}

void TestDashboardView::theBufferPoolHitRateOfAnIdleServerIsZero()
{
    // A server that has served no read at all divides by a total of nothing.
    m_backend.replyWithResult(snapshot(
        QJsonObject{{"Innodb_buffer_pool_read_requests", 0}, {"Innodb_buffer_pool_reads", 0}},
        serverVars()
    ));
    DashboardView view(QString::fromLatin1(ConnID));
    api()->flush(FlushMs);

    QCOMPARE(cardValue(view, PoolHit), QStringLiteral("—"));
    QCOMPARE(cardSub(view, PoolHit), QStringLiteral("128 MiB pool · 0.0000% since start"));
}

void TestDashboardView::theBufferPoolHitRateDividesByTheRequestsAlone()
{
    // Innodb_buffer_pool_reads counts the subset of read requests that missed,
    // so it is already inside read_requests. MySQL prints this ratio itself as
    // "Buffer pool hit rate 1000 / 1000" in SHOW ENGINE INNODB STATUS.
    // Counting the misses a second time in the denominator reads 99.9000% here.
    m_backend.replyWithResult(snapshot(
        QJsonObject{{"Innodb_buffer_pool_read_requests", 1000}, {"Innodb_buffer_pool_reads", 1}},
        serverVars()
    ));
    DashboardView view(QString::fromLatin1(ConnID));
    api()->flush(FlushMs);

    QCOMPARE(cardSub(view, PoolHit), QStringLiteral("128 MiB pool · 99.9000% since start"));
}

void TestDashboardView::aPoolThatMissedEveryRequestReadsAsZero()
{
    // Every request went to disk. The card has to be able to say so; summing
    // the two counters would halve the miss ratio and report 50%.
    m_backend.replyWithResult(snapshot(
        QJsonObject{{"Innodb_buffer_pool_read_requests", 100}, {"Innodb_buffer_pool_reads", 100}},
        serverVars()
    ));
    DashboardView view(QString::fromLatin1(ConnID));
    api()->flush(FlushMs);

    QCOMPARE(cardSub(view, PoolHit), QStringLiteral("128 MiB pool · 0.0000% since start"));
}

void TestDashboardView::readsWithoutRequestsDoNotDivideByZero()
{
    // Counters this shape should not occur, but the guard covers the whole
    // denominator rather than the pair, so a lone reads value cannot produce
    // an infinity on the card.
    m_backend.replyWithResult(snapshot(
        QJsonObject{{"Innodb_buffer_pool_read_requests", 0}, {"Innodb_buffer_pool_reads", 5}},
        serverVars()
    ));
    DashboardView view(QString::fromLatin1(ConnID));
    api()->flush(FlushMs);

    QCOMPARE(cardSub(view, PoolHit), QStringLiteral("128 MiB pool · 0.0000% since start"));
}

void TestDashboardView::theHeadlineIsTheWindowWhileTheSubLineIsTheLifetime()
{
    // The whole point of the window. 12.5 trillion good requests are banked, so
    // the lifetime ratio cannot move; every request since the last sample went
    // to disk. The headline has to say so while the sub line keeps the history.
    const qint64 requests = 12557783895289LL;
    const qint64 reads = 473864960;
    m_backend.replyWithResult(snapshot(
        QJsonObject{
            {"Innodb_buffer_pool_read_requests", requests}, {"Innodb_buffer_pool_reads", reads}
        },
        serverVars()
    ));
    DashboardView view(QString::fromLatin1(ConnID));
    api()->flush(FlushMs);
    QVERIFY(setLive(view, Live::Off));

    QVERIFY(pollOnce(
        m_backend, view,
        snapshot(
            QJsonObject{
                {"Innodb_buffer_pool_read_requests", requests + 500},
                {"Innodb_buffer_pool_reads", reads + 500}
            },
            serverVars()
        )
    ));

    QCOMPARE(cardValue(view, PoolHit), QStringLiteral("0.0000%"));
    QCOMPARE(cardSub(view, PoolHit), QStringLiteral("128 MiB pool · 99.9962% since start"));
}

void TestDashboardView::anIdleWindowShowsNoReadingRatherThanZero()
{
    // A server nobody is querying has served no reads to divide. That is not a
    // pool failing at 0%; it is no reading, and the card has to distinguish
    // them or an idle server looks like an emergency.
    m_backend.replyWithResult(snapshot(
        QJsonObject{{"Innodb_buffer_pool_read_requests", 9900}, {"Innodb_buffer_pool_reads", 100}},
        serverVars()
    ));
    DashboardView view(QString::fromLatin1(ConnID));
    api()->flush(FlushMs);
    QVERIFY(setLive(view, Live::Off));

    QVERIFY(pollOnce(
        m_backend, view,
        snapshot(
            QJsonObject{
                {"Innodb_buffer_pool_read_requests", 9900}, {"Innodb_buffer_pool_reads", 100}
            },
            serverVars()
        )
    ));

    QCOMPARE(cardValue(view, PoolHit), QStringLiteral("—"));
    QCOMPARE(cardSub(view, PoolHit), QStringLiteral("128 MiB pool · 98.9899% since start"));

    // And no alarm colour. An idle server that looks like a failing one is the
    // reason absence and 0% are kept apart in the first place.
    StatCard *card = cardFor(view, QString::fromLatin1(PoolHit));
    QVERIFY(card);
    const QLabel *value = card->findChild<QLabel *>(QString::fromLatin1(ValueName));
    QVERIFY(value);
    QVERIFY(value->styleSheet().isEmpty());
}

void TestDashboardView::theHitRateToneFollowsTheThresholds_data()
{
    QTest::addColumn<int>("requests");
    QTest::addColumn<int>("reads");
    QTest::addColumn<int>("tone");

    // Deltas over the window, chosen to land exactly on each boundary. The
    // thresholds are "below", so a figure sitting on one stays in the calmer
    // band: 99.9% is not yet worth a look, 99% is not yet an emergency.
    QTest::newRow("healthy") << 1000000 << 1 << int(Tone::None);
    QTest::newRow("on the warn line") << 1000 << 1 << int(Tone::None);
    QTest::newRow("just under the warn line") << 10000 << 11 << int(Tone::Warning);
    QTest::newRow("on the bad line") << 1000 << 10 << int(Tone::Warning);
    QTest::newRow("just under the bad line") << 1000 << 11 << int(Tone::Destructive);
    QTest::newRow("every request missed") << 1000 << 1000 << int(Tone::Destructive);
}

void TestDashboardView::theHitRateToneFollowsTheThresholds()
{
    QFETCH(int, requests);
    QFETCH(int, reads);
    QFETCH(int, tone);

    // A long, healthy history behind a window that is whatever the row says:
    // the tone has to follow the window, not the lifetime.
    const qint64 baseRequests = 12557783895289LL;
    const qint64 baseReads = 473864960;
    m_backend.replyWithResult(snapshot(
        QJsonObject{
            {"Innodb_buffer_pool_read_requests", baseRequests},
            {"Innodb_buffer_pool_reads", baseReads}
        },
        serverVars()
    ));
    DashboardView view(QString::fromLatin1(ConnID));
    api()->flush(FlushMs);
    QVERIFY(setLive(view, Live::Off));

    QVERIFY(pollOnce(
        m_backend, view,
        snapshot(
            QJsonObject{
                {"Innodb_buffer_pool_read_requests", baseRequests + requests},
                {"Innodb_buffer_pool_reads", baseReads + reads}
            },
            serverVars()
        )
    ));

    StatCard *card = cardFor(view, QString::fromLatin1(PoolHit));
    QVERIFY(card);
    const QLabel *value = card->findChild<QLabel *>(QString::fromLatin1(ValueName));
    QVERIFY(value);

    const AppPalette &pal = theme::current();
    switch (Tone(tone))
    {
    case Tone::None:
        QVERIFY2(value->styleSheet().isEmpty(), qPrintable(value->text()));
        break;
    case Tone::Warning:
        QVERIFY2(value->styleSheet().contains(pal.warning.name()), qPrintable(value->text()));
        break;
    case Tone::Destructive:
        QVERIFY2(value->styleSheet().contains(pal.destructive.name()), qPrintable(value->text()));
        break;
    }
}

void TestDashboardView::percentagesWithNoTotalReadAsZero()
{
    // An account that cannot read max_connections, and a pool that reports no
    // pages while still reporting pages of its own: both denominators are zero
    // and the parts are not, which is where a percentage spikes.
    m_backend.replyWithResult(snapshot(QJsonObject{
        {"Threads_connected", 50},
        {"Max_used_connections", 60},
        {"Innodb_buffer_pool_pages_total", 0},
        {"Innodb_buffer_pool_pages_data", 800},
        {"Innodb_buffer_pool_pages_free", 170},
        {"Innodb_buffer_pool_pages_dirty", 25},
    }));
    DashboardView view(QString::fromLatin1(ConnID));
    api()->flush(FlushMs);

    QCOMPARE(cardValue(view, Connections), QStringLiteral("0.0%"));
    QCOMPARE(cardSub(view, Connections), QStringLiteral("50 of 0 · peak 60"));

    QCOMPARE(cardValue(view, PoolDirty), QStringLiteral("0.0%"));
    QCOMPARE(cardSub(view, PoolDirty), QStringLiteral("0.0% data · 0.0% free"));
}

void TestDashboardView::innodbMetricsWinOverTheStatusCounters()
{
    // MySQL reports deadlocks and the undo history through INNODB_METRICS and
    // MariaDB through the status counters. The metric wins where there is one;
    // theCardsReadOneSampleOfAWarmServer covers the fallback.
    QJsonObject snap = snapshot(warmStatus(), serverVars());
    snap.insert(
        "innodb",
        QJsonObject{{"lock_deadlocks", 7}, {"lock_timeouts", 3}, {"trx_rseg_history_len", 900}}
    );
    snap.insert(
        "trx",
        QJsonArray{
            QJsonObject{{"thread", 41}, {"seconds", 5}},
            QJsonObject{{"thread", 42}, {"seconds", 90}}
        }
    );
    m_backend.replyWithResult(snap);
    DashboardView view(QString::fromLatin1(ConnID));
    api()->flush(FlushMs);

    QCOMPARE(cardValue(view, Deadlocks), QStringLiteral("7"));
    QCOMPARE(cardSub(view, Deadlocks), QStringLiteral("3 lock timeouts"));
    QCOMPARE(cardValue(view, History), QStringLiteral("900"));

    // The live transactions beside the history length are the rows the
    // snapshot carries, not a counter.
    QCOMPARE(cardSub(view, History), QStringLiteral("2 live transactions"));
    QVERIFY(showsLabel(view, QStringLiteral("Live Transactions (2)")));
}

void TestDashboardView::theHeaderLineNamesTheServerAndItsUptime()
{
    QJsonObject snap = snapshot(warmStatus(), serverVars());
    snap.insert("version", QString::fromLatin1(Version));
    snap.insert("uptime", UptimeSeconds);
    snap.insert("sampledAt", QString::fromLatin1(SampledAt));
    m_backend.replyWithResult(snap);
    DashboardView view(QString::fromLatin1(ConnID));
    api()->flush(FlushMs);

    QVERIFY(showsLabel(view, QStringLiteral("8.0.36 · up 1d 2h · sampled 2026-09-13 10:00:00")));
}

void TestDashboardView::theDashboardOpensPausedSoNothingSamplesUnasked()
{
    // Sampling is opt-in. A dashboard tab left open in the background must not
    // keep six statements a tick running against the server it points at.
    m_backend.replyWithResult(snapshot(QJsonObject{{"Questions", QuestionsBase}}));
    DashboardView view(QString::fromLatin1(ConnID));
    api()->flush(FlushMs);

    QCheckBox *live = view.findChild<QCheckBox *>();
    QVERIFY(live);
    QVERIFY2(!live->isChecked(), "the dashboard opens paused");

    // The cards are still filled in from the snapshot taken on open.
    QCOMPARE(cardSub(view, Queries), QStringLiteral("1,000 total"));

    m_backend.clearRequests();
    for (const int ms : {PollMs, SectionPollMs})
    {
        QTimer *timer = timerWith(view, ms);
        QVERIFY(timer);
        QMetaObject::invokeMethod(timer, "timeout");
    }
    api()->flush(FlushMs);
    QCOMPARE(pollsSeen(m_backend), 0);
}

void TestDashboardView::thePollTimerSamplesWhileLiveIsChecked()
{
    m_backend.replyWithResult(snapshot(QJsonObject{{"Questions", QuestionsBase}}));
    DashboardView view(QString::fromLatin1(ConnID));
    api()->flush(FlushMs);

    QTimer *timer = timerWith(view, PollMs);
    QVERIFY2(timer, "the counters are sampled on a two second timer");
    QVERIFY(timer->isActive());
    QVERIFY2(timerWith(view, SectionPollMs), "the InnoDB sections are read far more slowly");
    QVERIFY(setLive(view, Live::On));

    // Nothing ticks a timer in a test that never sits in an event loop for two
    // seconds: emit the timeout rather than wait for it.
    m_backend.clearRequests();
    m_backend.replyWithResult(snapshot(QJsonObject{{"Questions", QuestionsBase + QuestionDelta}}));
    QMetaObject::invokeMethod(timer, "timeout");
    api()->flush(FlushMs);

    QCOMPARE(pollsSeen(m_backend), 1);
    QCOMPARE(m_backend.requests().at(0).args, QJsonArray({QString::fromLatin1(ConnID)}));
    QCOMPARE(cardSub(view, Queries), QStringLiteral("1,500 total"));
}

void TestDashboardView::clearingLiveStopsTheSampling()
{
    m_backend.replyWithResult(snapshot(QJsonObject{{"Questions", QuestionsBase}}));
    DashboardView view(QString::fromLatin1(ConnID));
    api()->flush(FlushMs);
    QVERIFY(setLive(view, Live::Off));

    QTimer *timer = timerWith(view, PollMs);
    QVERIFY(timer);
    m_backend.clearRequests();
    QMetaObject::invokeMethod(timer, "timeout");
    api()->flush(FlushMs);
    QCOMPARE(pollsSeen(m_backend), 0);

    // The switch is read inside the tick rather than stopping the timer, so
    // checking it again resumes the sampling on its own.
    QVERIFY(timer->isActive());
    QVERIFY(setLive(view, Live::On));
    QMetaObject::invokeMethod(timer, "timeout");
    api()->flush(FlushMs);
    QCOMPARE(pollsSeen(m_backend), 1);
}

void TestDashboardView::aTickIsSkippedWhileAPollIsStillInFlight()
{
    m_backend.replyWithResult(snapshot(QJsonObject{{"Questions", QuestionsBase}}));
    DashboardView view(QString::fromLatin1(ConnID));
    api()->flush(FlushMs);

    QVERIFY(setLive(view, Live::On));

    QTimer *timer = timerWith(view, PollMs);
    QVERIFY(timer);
    m_backend.clearRequests();

    // Two ticks with no reply read in between: a stalled backend must not
    // build a queue of polls whose replies then land milliseconds apart and
    // burn a spike into the chart history.
    QMetaObject::invokeMethod(timer, "timeout");
    QMetaObject::invokeMethod(timer, "timeout");
    api()->flush(FlushMs);
    QCOMPARE(pollsSeen(m_backend), 1);

    // The reply releases the guard rather than leaving it standing.
    QMetaObject::invokeMethod(timer, "timeout");
    api()->flush(FlushMs);
    QCOMPARE(pollsSeen(m_backend), 2);
}

void TestDashboardView::aFailedPollSurfacesInsteadOfBlankingTheCards()
{
    m_backend.replyWithResult(snapshot(warmStatus(), serverVars()));
    DashboardView view(QString::fromLatin1(ConnID));
    api()->flush(FlushMs);
    QVERIFY(setLive(view, Live::Off));
    QVERIFY(!errorShown(view));

    m_backend.replyWithError(QString::fromLatin1(Denied));
    QPushButton *button = refreshButton(view);
    QVERIFY(button);
    button->click();
    api()->flush(FlushMs);

    QVERIFY2(errorShown(view), "a failed poll must say so, not read as a server gone quiet");
    const QLabel *strip = errorStrip(view);
    QVERIFY(strip);
    QCOMPARE(strip->text(), QString::fromLatin1(Denied));

    // The cards keep the last sample that arrived: zeroing them would read as
    // a server that suddenly stopped working.
    QCOMPARE(cardValue(view, Threads), QStringLiteral("3"));
    QCOMPARE(cardSub(view, Queries), QStringLiteral("1,000 total"));

    // The failure also has to release the in-flight guard, or the panel never
    // polls again.
    QVERIFY(pollOnce(m_backend, view, snapshot(warmStatus(), serverVars())));
    QVERIFY(!errorShown(view));
}

void TestDashboardView::theNotesOfASnapshotReachTheErrorStrip()
{
    // A source the account cannot read comes back as a note beside a snapshot
    // that is otherwise good: the panel says which source is missing and still
    // draws the counters it did get.
    QJsonObject snap = snapshot(warmStatus(), serverVars());
    snap.insert(
        "notes", QJsonArray{QString::fromLatin1(MetricsNote), QString::fromLatin1(LockWaitsNote)}
    );
    m_backend.replyWithResult(snap);
    DashboardView view(QString::fromLatin1(ConnID));
    api()->flush(FlushMs);
    QVERIFY(setLive(view, Live::Off));

    QVERIFY(errorShown(view));
    const QLabel *strip = errorStrip(view);
    QVERIFY(strip);
    QCOMPARE(
        strip->text(), QStringLiteral("%1 · %2").arg(
                           QString::fromLatin1(MetricsNote), QString::fromLatin1(LockWaitsNote)
                       )
    );
    QCOMPARE(cardValue(view, Threads), QStringLiteral("3"));

    // A later snapshot with nothing to report takes the strip down again.
    QVERIFY(pollOnce(m_backend, view, snapshot(warmStatus(), serverVars())));
    QVERIFY2(!errorShown(view), "the strip would otherwise outlive what it reported");
    QVERIFY(strip->text().isEmpty());
}

void TestDashboardView::theChartsTakeASampleWithoutTrouble()
{
    m_backend.replyWithResult(snapshot(warmStatus(), serverVars()));
    DashboardView view(QString::fromLatin1(ConnID));
    api()->flush(FlushMs);
    QVERIFY(setLive(view, Live::Off));

    // The second sample is the first one with an interval behind it, so it is
    // the first that reaches the charts at all.
    QVERIFY(pollOnce(m_backend, view, snapshot(busyStatus(), serverVars())));

    const QList<TimeChart *> charts = view.findChildren<TimeChart *>();
    QCOMPARE(charts.size(), ChartCount);
    for (TimeChart *chart : charts)
    {
        chart->resize(ChartWidth, ChartHeight);
        QPixmap canvas(chart->size());
        canvas.fill(Qt::transparent);
        chart->render(&canvas);
        QVERIFY(!canvas.toImage().isNull());
    }
}

QTEST_MAIN(TestDashboardView)

#include "tst_dashboardview.moc"
