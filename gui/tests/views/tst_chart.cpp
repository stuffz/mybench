#include "views/chart.h"

#include "app/theme.h"

#include <QColor>
#include <QCoreApplication>
#include <QEvent>
#include <QImage>
#include <QLocale>
#include <QMouseEvent>
#include <QObject>
#include <QPixmap>
#include <QSize>
#include <QString>
#include <QTest>
#include <QVector>

// TimeChart keeps its samples private and reports nothing about them, so the
// only way in from outside is the painted result: two charts that hold the
// same window paint the same image, and one that does not, does not. Nothing
// below asserts a colour or a coordinate, only that two renders of the same
// size agree or differ.
namespace
{

constexpr int ChartWidth = 400;
constexpr int ChartHeight = 200;
constexpr int Samples = 40;

// A flat baseline with one value far above it: while the high sample is inside
// the window it alone sets the auto-scaled ceiling, which is what makes its
// arrival and eviction visible in the image.
constexpr double Baseline = 10;
constexpr double Spike = 1000;

constexpr auto Title = "Queries";
constexpr Qt::GlobalColor SeriesA = Qt::red;
constexpr Qt::GlobalColor SeriesB = Qt::cyan;
constexpr Qt::GlobalColor RecolouredA = Qt::green;
constexpr Qt::GlobalColor RecolouredB = Qt::magenta;

QString seriesName(int index)
{
    return QStringLiteral("s%1").arg(index);
}

// Compared charts have to match in everything except what is under test, size
// included: images of different sizes are never equal.
void setUpChart(
    TimeChart &chart, int seriesCount, const QColor &first = SeriesA, const QColor &second = SeriesB
)
{
    for (int i = 0; i < seriesCount; ++i)
    {
        chart.addSeries(seriesName(i), i == 0 ? first : second);
    }
    chart.resize(ChartWidth, ChartHeight);
}

void pushSamples(TimeChart &chart, int times, const QVector<double> &values)
{
    for (int i = 0; i < times; ++i)
    {
        chart.push(values);
    }
}

QImage paint(TimeChart &chart)
{
    QPixmap canvas(chart.size());
    canvas.fill(Qt::transparent);
    chart.render(&canvas);
    return canvas.toImage();
}

// The canvas starts transparent, so an image that comes back unchanged means
// paintEvent drew nothing at all.
bool drewSomething(const QImage &image)
{
    if (image.isNull())
    {
        return false;
    }
    QImage untouched(image.size(), image.format());
    untouched.fill(Qt::transparent);
    return image != untouched;
}

} // namespace

class TestChart : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();

    void capacityDropsTheOldestSampleWhenFull();
    void capacityKeepsOnlyTheLastWindow();

    void pushPadsAMissingValueWithZero();
    void pushIgnoresValuesPastTheLastSeries();
    void clearSamplesReturnsTheChartToEmpty();

    void ceilingOverridesTheWindowPeak();
    void ceilingOfZeroAutoScalesToANiceMaximum();

    void formatChangesHowValuesAreWritten();
    void subtitleAppearsAndClearsAgain();

    void seriesColoursRecolourInPlaceAndKeepHistory();
    void seriesColoursStopAtTheShorterList();

    void sizeHintIsAtLeastTheMinimumHeight();

    void hoverMarksTheSampleUnderTheCursor();
    void hoverOutsideThePlotClearsItself();
    void leavingTheWidgetClearsTheHover();

    void paintingSurvivesEveryFormatAndCeiling();
    void paintingSurvivesAnEmptyWindow();
    void paintingSurvivesAFlatWindow();
};

void TestChart::initTestCase()
{
    // The chart reads theme::current() and theme::scaledPx() on every paint;
    // apply() is what installs both.
    theme::apply(theme::defaultApp, 13);

    // Every painted label goes through QLocale(), so pin it rather than let
    // the host locale decide what the chart draws.
    QLocale::setDefault(QLocale(QLocale::English, QLocale::UnitedStates));
}

void TestChart::capacityDropsTheOldestSampleWhenFull()
{
    TimeChart brimming(Title);
    setUpChart(brimming, 1);
    brimming.push({Spike});
    pushSamples(brimming, TimeChart::Capacity - 1, {Baseline});

    TimeChart evicted(Title);
    setUpChart(evicted, 1);
    evicted.push({Spike});
    pushSamples(evicted, TimeChart::Capacity, {Baseline});

    TimeChart flat(Title);
    setUpChart(flat, 1);
    pushSamples(flat, TimeChart::Capacity, {Baseline});

    QVERIFY2(paint(brimming) != paint(flat), "a full window still holds its oldest sample");

    // One sample past full is where the spike falls off the left.
    QCOMPARE(paint(evicted), paint(flat));
}

void TestChart::capacityKeepsOnlyTheLastWindow()
{
    TimeChart churned(Title);
    setUpChart(churned, 1);
    pushSamples(churned, 3 * TimeChart::Capacity, {Spike});
    pushSamples(churned, TimeChart::Capacity - 1, {Baseline});
    churned.push({Spike});

    TimeChart window(Title);
    setUpChart(window, 1);
    pushSamples(window, TimeChart::Capacity - 1, {Baseline});
    window.push({Spike});

    QCOMPARE(paint(churned), paint(window));

    TimeChart flat(Title);
    setUpChart(flat, 1);
    pushSamples(flat, TimeChart::Capacity, {Baseline});
    QVERIFY2(paint(window) != paint(flat), "the newest sample belongs to the window");
}

void TestChart::pushPadsAMissingValueWithZero()
{
    TimeChart missing(Title);
    setUpChart(missing, 2);

    TimeChart padded(Title);
    setUpChart(padded, 2);

    for (int i = 0; i < Samples; ++i)
    {
        missing.push({Baseline});
        padded.push({Baseline, 0.0});
    }

    // The short series still gets a sample, so it keeps a line and stays the
    // same length as its neighbour.
    QCOMPARE(paint(missing), paint(padded));
}

void TestChart::pushIgnoresValuesPastTheLastSeries()
{
    TimeChart extra(Title);
    setUpChart(extra, 1);

    TimeChart exact(Title);
    setUpChart(exact, 1);

    for (int i = 0; i < Samples; ++i)
    {
        // A value with no series of its own is dropped, not folded into one
        // that exists: a stray spike would show up in the auto ceiling.
        extra.push({Baseline, Spike});
        exact.push({Baseline});
    }

    QCOMPARE(paint(extra), paint(exact));
}

void TestChart::clearSamplesReturnsTheChartToEmpty()
{
    TimeChart chart(Title);
    setUpChart(chart, 2);
    pushSamples(chart, Samples, {Baseline, Spike});

    TimeChart empty(Title);
    setUpChart(empty, 2);
    QVERIFY(paint(chart) != paint(empty));

    chart.clearSamples();
    QCOMPARE(paint(chart), paint(empty));
}

void TestChart::ceilingOverridesTheWindowPeak()
{
    TimeChart fixed(Title);
    setUpChart(fixed, 1);
    fixed.setCeiling(Spike);
    pushSamples(fixed, Samples, {Baseline});

    TimeChart automatic(Title);
    setUpChart(automatic, 1);
    pushSamples(automatic, Samples, {Baseline});

    QVERIFY2(
        paint(fixed) != paint(automatic), "a fixed ceiling should not follow the window's peak"
    );
}

void TestChart::ceilingOfZeroAutoScalesToANiceMaximum()
{
    TimeChart explicitZero(Title);
    setUpChart(explicitZero, 1);
    explicitZero.setCeiling(0);
    pushSamples(explicitZero, Samples, {50});

    TimeChart untouched(Title);
    setUpChart(untouched, 1);
    pushSamples(untouched, Samples, {50});
    QCOMPARE(paint(explicitZero), paint(untouched));

    // A flat 50 pads by 5% and rounds up to the next 1/2/5 x 10ⁿ, which is
    // 100: the same chart a fixed ceiling of 100 draws.
    TimeChart pinned(Title);
    setUpChart(pinned, 1);
    pinned.setCeiling(100);
    pushSamples(pinned, Samples, {50});
    QCOMPARE(paint(untouched), paint(pinned));
}

void TestChart::formatChangesHowValuesAreWritten()
{
    TimeChart chart(Title);
    setUpChart(chart, 1);

    // 1536 is written "1,536", "1.5 KB/s" and "1,536%", so each format gives
    // the legend and the axis labels a different width as well as a different
    // string.
    pushSamples(chart, Samples, {1536});

    const QImage number = paint(chart);
    chart.setFormat(TimeChart::Format::Bytes);
    const QImage bytes = paint(chart);
    chart.setFormat(TimeChart::Format::Percent);
    const QImage percent = paint(chart);

    QVERIFY(number != bytes);
    QVERIFY(bytes != percent);
    QVERIFY(number != percent);

    chart.setFormat(TimeChart::Format::Number);
    QCOMPARE(paint(chart), number);
}

void TestChart::subtitleAppearsAndClearsAgain()
{
    TimeChart chart(Title);
    setUpChart(chart, 1);
    pushSamples(chart, Samples, {Baseline});

    const QImage bare = paint(chart);
    chart.setSubtitle(QStringLiteral("last 6 min"));
    QVERIFY(paint(chart) != bare);

    chart.setSubtitle(QString());
    QCOMPARE(paint(chart), bare);
}

void TestChart::seriesColoursRecolourInPlaceAndKeepHistory()
{
    TimeChart recoloured(Title);
    setUpChart(recoloured, 2);
    pushSamples(recoloured, Samples, {Baseline, Spike});

    TimeChart built(Title);
    setUpChart(built, 2, RecolouredA, RecolouredB);
    pushSamples(built, Samples, {Baseline, Spike});

    QVERIFY(paint(recoloured) != paint(built));

    // Same window, new colours: a chart that had dropped its samples would
    // paint the "collecting" placeholder instead of these lines.
    recoloured.setSeriesColours({RecolouredA, RecolouredB});
    QCOMPARE(paint(recoloured), paint(built));
}

void TestChart::seriesColoursStopAtTheShorterList()
{
    TimeChart shortList(Title);
    setUpChart(shortList, 2);
    pushSamples(shortList, Samples, {Baseline, Spike});
    shortList.setSeriesColours({RecolouredA});

    TimeChart firstOnly(Title);
    setUpChart(firstOnly, 2, RecolouredA, SeriesB);
    pushSamples(firstOnly, Samples, {Baseline, Spike});
    QCOMPARE(paint(shortList), paint(firstOnly));

    TimeChart longList(Title);
    setUpChart(longList, 2);
    pushSamples(longList, Samples, {Baseline, Spike});

    // A colour with no series is dropped rather than adding one.
    longList.setSeriesColours({RecolouredA, RecolouredB, Qt::yellow});

    TimeChart bothOnly(Title);
    setUpChart(bothOnly, 2, RecolouredA, RecolouredB);
    pushSamples(bothOnly, Samples, {Baseline, Spike});
    QCOMPARE(paint(longList), paint(bothOnly));
}

void TestChart::sizeHintIsAtLeastTheMinimumHeight()
{
    TimeChart chart(Title);
    const QSize hint = chart.sizeHint();
    QVERIFY(hint.isValid());
    QVERIFY(hint.width() > 0);

    // A hint below the minimum is one a layout would silently discard.
    QVERIFY(hint.height() >= chart.minimumHeight());
}

void TestChart::paintingSurvivesEveryFormatAndCeiling()
{
    const TimeChart::Format formats[] = {
        TimeChart::Format::Number,
        TimeChart::Format::Bytes,
        TimeChart::Format::Percent,
    };
    // Auto, then a ceiling the samples below run past, so the clamp runs too.
    const double ceilings[] = {0, 100};

    for (const TimeChart::Format format : formats)
    {
        for (const double ceiling : ceilings)
        {
            TimeChart chart(Title);
            setUpChart(chart, 2);
            chart.setFormat(format);
            chart.setCeiling(ceiling);
            chart.setSubtitle(QStringLiteral("last 6 min"));
            for (int i = 0; i < TimeChart::Capacity + Samples; ++i)
            {
                chart.push({double(i), double(i % 7)});
            }
            QVERIFY(drewSomething(paint(chart)));
        }
    }
}

void TestChart::paintingSurvivesAnEmptyWindow()
{
    TimeChart noSeries(Title);
    setUpChart(noSeries, 0);
    noSeries.push({Baseline});
    QVERIFY(drewSomething(paint(noSeries)));

    TimeChart noSamples(Title);
    setUpChart(noSamples, 1);
    QVERIFY(drewSomething(paint(noSamples)));

    // One sample is a window with no width to interpolate across.
    TimeChart oneSample(Title);
    setUpChart(oneSample, 1);
    oneSample.push({Baseline});
    QVERIFY(drewSomething(paint(oneSample)));
}

void TestChart::paintingSurvivesAFlatWindow()
{
    // Every value zero against an auto ceiling: the scale has to stay clear of
    // a peak of nothing.
    TimeChart zeros(Title);
    setUpChart(zeros, 1);
    zeros.setCeiling(0);
    pushSamples(zeros, Samples, {0.0});
    QVERIFY(drewSomething(paint(zeros)));

    // A series added after the samples stays shorter than its neighbour for
    // good, so every read of a sample index has to tolerate the gap.
    TimeChart ragged(Title);
    setUpChart(ragged, 1);
    pushSamples(ragged, Samples, {Baseline});
    ragged.addSeries(seriesName(1), SeriesB);
    pushSamples(ragged, 2, {Baseline, Spike});
    QVERIFY(drewSomething(paint(ragged)));
}

namespace
{

// mouseMoveEvent is protected and the widget is never mapped under offscreen,
// so the events go straight to it rather than through the window system.
void sendMove(TimeChart &chart, const QPointF &at)
{
    QMouseEvent move(QEvent::MouseMove, at, at, Qt::NoButton, Qt::NoButton, Qt::NoModifier);
    QCoreApplication::sendEvent(&chart, &move);
}

} // namespace

void TestChart::hoverMarksTheSampleUnderTheCursor()
{
    TimeChart chart(Title);
    setUpChart(chart, 1);

    // A full window, so the samples span the whole plot: a partly filled one
    // sits against the right edge and most of the width clamps to sample zero.
    pushSamples(chart, TimeChart::Capacity, {Baseline});

    const QImage plain = paint(chart);
    sendMove(chart, QPointF(ChartWidth * 0.75, ChartHeight / 2.0));
    const QImage hovered = paint(chart);

    QVERIFY(drewSomething(hovered));
    QVERIFY2(plain != hovered, "a hovered sample should draw its marker and readout");

    // Two different samples under the cursor are two different pictures.
    sendMove(chart, QPointF(ChartWidth * 0.25, ChartHeight / 2.0));
    QVERIFY(paint(chart) != hovered);
}

void TestChart::hoverOutsideThePlotClearsItself()
{
    TimeChart chart(Title);
    setUpChart(chart, 1);
    pushSamples(chart, Samples, {Baseline});

    const QImage plain = paint(chart);
    sendMove(chart, QPointF(ChartWidth / 2.0, ChartHeight / 2.0));
    QVERIFY(paint(chart) != plain);

    // The title band above the plot is inside the widget but outside the axes.
    sendMove(chart, QPointF(ChartWidth / 2.0, 1));
    QCOMPARE(paint(chart), plain);
}

void TestChart::leavingTheWidgetClearsTheHover()
{
    TimeChart chart(Title);
    setUpChart(chart, 1);
    pushSamples(chart, Samples, {Baseline});

    const QImage plain = paint(chart);
    sendMove(chart, QPointF(ChartWidth / 2.0, ChartHeight / 2.0));
    QVERIFY(paint(chart) != plain);

    QEvent leave(QEvent::Leave);
    QCoreApplication::sendEvent(&chart, &leave);
    QCOMPARE(paint(chart), plain);
}

QTEST_MAIN(TestChart)

#include "tst_chart.moc"
