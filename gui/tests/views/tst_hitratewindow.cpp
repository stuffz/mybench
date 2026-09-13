// The rolling buffer pool hit rate. The counters behind it are monotonic since
// the server started, so the lifetime ratio cannot move on a dashboard's
// timescale; this window is what makes the card answer "is the pool coping
// now" rather than "was it sized right in July".
#include "views/hitratewindow.h"

#include <optional>

#include <QTest>

namespace
{

constexpr qint64 WindowMs = 60000;
constexpr qint64 Start = 1'000'000;

// A server whose lifetime ratio is excellent and whose last minute is not.
constexpr double LifetimeRequests = 12'557'783'895'289.0;
constexpr double LifetimeReads = 473'864'960.0;

} // namespace

class TestHitRateWindow : public QObject
{
    Q_OBJECT
private slots:
    void oneSampleHasNothingToDivide();
    void twoSamplesDivideTheDeltasNotTheTotals();
    void theLiveRatioIgnoresAHealthyHistory();
    void anIdleServerReportsNothingRatherThanZero();
    void aCounterResetReportsNothingRatherThanASpike();
    void readsFallingWhileRequestsRiseReportsNothing();
    void samplesOlderThanTheWindowAreDropped();
    void theWindowKeepsTheCurrentSampleWhateverHappens();
};

void TestHitRateWindow::oneSampleHasNothingToDivide()
{
    HitRateWindow w(WindowMs);
    w.push(Start, 1000, 10);
    QVERIFY(!w.ratio().has_value());
}

void TestHitRateWindow::twoSamplesDivideTheDeltasNotTheTotals()
{
    HitRateWindow w(WindowMs);
    w.push(Start, 1000, 10);
    // 100 more requests, 50 of which missed: half the interval went to disk.
    w.push(Start + 2000, 1100, 60);

    const std::optional<double> got = w.ratio();
    QVERIFY(got.has_value());
    QCOMPARE(*got, 50.0);
}

void TestHitRateWindow::theLiveRatioIgnoresAHealthyHistory()
{
    // The whole point: 12.5 trillion good requests banked, and every request in
    // the last interval went to disk. The card has to say 0%, not 99.9962%.
    HitRateWindow w(WindowMs);
    w.push(Start, LifetimeRequests, LifetimeReads);
    w.push(Start + 2000, LifetimeRequests + 500, LifetimeReads + 500);

    const std::optional<double> got = w.ratio();
    QVERIFY(got.has_value());
    QCOMPARE(*got, 0.0);
}

void TestHitRateWindow::anIdleServerReportsNothingRatherThanZero()
{
    // No reads at all in the interval is not 0% and not 100%; it is no data.
    HitRateWindow w(WindowMs);
    w.push(Start, 1000, 10);
    w.push(Start + 2000, 1000, 10);
    QVERIFY(!w.ratio().has_value());
}

void TestHitRateWindow::aCounterResetReportsNothingRatherThanASpike()
{
    HitRateWindow w(WindowMs);
    w.push(Start, 1000, 10);
    w.push(Start + 2000, 5, 1);
    QVERIFY2(!w.ratio().has_value(), "a restart must not read as a collapse in the hit rate");
}

void TestHitRateWindow::readsFallingWhileRequestsRiseReportsNothing()
{
    // Not the restart case above, where both counters drop together and the
    // request delta already catches it. This is reads alone going backwards,
    // which no healthy server does and which divides out to 200%.
    HitRateWindow w(WindowMs);
    w.push(Start, 1000, 500);
    w.push(Start + 2000, 1100, 400);
    QVERIFY2(!w.ratio().has_value(), "a hit rate over 100% is not a reading, it is a bug");
}

void TestHitRateWindow::samplesOlderThanTheWindowAreDropped()
{
    HitRateWindow w(WindowMs);
    // A bad minute, then a good one. Once the bad samples age out the ratio has
    // to reflect only what is still inside the window.
    w.push(Start, 1000, 0);
    w.push(Start + 1000, 2000, 1000); // 100% miss

    const std::optional<double> during = w.ratio();
    QVERIFY(during.has_value());
    QCOMPARE(*during, 0.0);

    w.push(Start + WindowMs + 1000, 3000, 1000); // 0% miss, and the first two age out
    const std::optional<double> after = w.ratio();
    QVERIFY(after.has_value());
    QCOMPARE(*after, 100.0);
}

void TestHitRateWindow::theWindowKeepsTheCurrentSampleWhateverHappens()
{
    // A poll that arrives after a long pause must not empty the window and
    // leave the next one with nothing to divide against.
    HitRateWindow w(WindowMs);
    w.push(Start, 1000, 0);
    w.push(Start + 10 * WindowMs, 2000, 0);
    QVERIFY(!w.ratio().has_value());

    w.push(Start + 10 * WindowMs + 2000, 3000, 0);
    const std::optional<double> got = w.ratio();
    QVERIFY2(got.has_value(), "the sample that survived the trim is the one to divide against");
    QCOMPARE(*got, 100.0);
}

QTEST_MAIN(TestHitRateWindow)

#include "tst_hitratewindow.moc"
