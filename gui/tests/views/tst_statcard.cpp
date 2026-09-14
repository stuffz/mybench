#include "views/statcard.h"

#include "app/theme.h"

#include <QChar>
#include <QColor>
#include <QLabel>
#include <QList>
#include <QObject>
#include <QPixmap>
#include <QString>
#include <QTest>
#include <QVariant>
#include <QWidget>

namespace
{

constexpr int kUiFontSize = 13;

// A lucide glyph the qrc really holds, and one it does not: the name is
// resolved at run time, so a typo is only visible as a blank corner.
constexpr auto kGlyph = "activity";
constexpr auto kMissingGlyph = "no-such-icon";

constexpr int kWideTile = 400;
constexpr int kNarrowTile = 120;
constexpr int kTileHeight = 80;

// Long enough that neither width can show it whole.
constexpr int kOverlongChars = 200;

// The two lines the sheet sizes by name, which is also the only way into a
// tile from outside: it keeps every label private.
QLabel *valueLabel(const StatCard &card)
{
    return card.findChild<QLabel *>(QStringLiteral("kpiValue"));
}

QLabel *subLabel(const StatCard &card)
{
    return card.findChild<QLabel *>(QStringLiteral("kpiSub"));
}

// The caption and the glyph carry no object name, so each is found by what it
// holds: the caption by its text, the glyph by being the one label with a
// pixmap in it.
QLabel *captionLabel(const StatCard &card, const QString &text)
{
    for (QLabel *l : card.findChildren<QLabel *>())
    {
        if (l->text() == text)
        {
            return l;
        }
    }
    return nullptr;
}

int glyphCount(const StatCard &card)
{
    int glyphs = 0;
    for (const QLabel *l : card.findChildren<QLabel *>())
    {
        if (!l->pixmap().isNull())
        {
            ++glyphs;
        }
    }
    return glyphs;
}

// U+2014, written as an escape so this file stays ASCII: the tile shows one
// as its empty value and joins the tooltip's two lines with another.
QString emDash()
{
    return QStringLiteral("\u2014");
}

QString overlong(QChar fill)
{
    return QString(kOverlongChars, fill);
}

} // namespace

// A tile is a widget with no accessors, so everything below reads its labels
// back through findChild. Nothing here asserts a colour or a position: the
// tile paints nothing itself, it writes a sheet and lets the style do it.
//
// Real widgets, so the run needs a platform plugin: CTest sets
// QT_QPA_PLATFORM=offscreen for it.
class TestStatCard : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();

    void aNewTileShowsADashAndItsCaption();
    void setValueWritesBothLines();
    void aValueWithNoContextLineClearsTheSub();
    void theTooltipsCarryTheUnelidedReading();
    void aReadingTooWideForTheTileIsElided();
    void resizingRedoesTheElision();
    void theToneReachesTheValueAndNothingElse();
    void theGlyphComesFromTheIconName();
};

void TestStatCard::initTestCase()
{
    // The tile writes its own sheet from theme::current() and renders its
    // glyph in the palette's muted foreground; apply() installs both.
    theme::apply(theme::defaultApp, kUiFontSize);
}

void TestStatCard::aNewTileShowsADashAndItsCaption()
{
    const QString caption = QStringLiteral("Queries / s");
    const StatCard card(kGlyph, caption);

    QLabel *const label = captionLabel(card, caption);
    QVERIFY(label != nullptr);
    QCOMPARE(label->property("muted"), QVariant(true));

    QLabel *const value = valueLabel(card);
    QVERIFY(value != nullptr);
    // What the tile reads before its first sample arrives.
    QCOMPARE(value->text(), emDash());

    QLabel *const sub = subLabel(card);
    QVERIFY(sub != nullptr);
    QVERIFY(sub->text().isEmpty());
    QCOMPARE(sub->property("muted"), QVariant(true));

    // Without this a plain QWidget drops the sheet's background and border.
    QVERIFY(card.testAttribute(Qt::WA_StyledBackground));
}

void TestStatCard::setValueWritesBothLines()
{
    StatCard card(kGlyph, QStringLiteral("Connections"));
    card.resize(kWideTile, kTileHeight);

    QLabel *const value = valueLabel(card);
    QLabel *const sub = subLabel(card);
    QVERIFY(value != nullptr);
    QVERIFY(sub != nullptr);

    card.setValue(QStringLiteral("1,234"), QStringLiteral("5 of 200"));

    QCOMPARE(value->text(), QStringLiteral("1,234"));
    QCOMPARE(sub->text(), QStringLiteral("5 of 200"));

    card.setValue(QStringLiteral("1,235"), QStringLiteral("6 of 200"));

    QCOMPARE(value->text(), QStringLiteral("1,235"));
    QCOMPARE(sub->text(), QStringLiteral("6 of 200"));
}

void TestStatCard::aValueWithNoContextLineClearsTheSub()
{
    StatCard card(kGlyph, QStringLiteral("Deadlocks"));
    card.resize(kWideTile, kTileHeight);

    QLabel *const value = valueLabel(card);
    QLabel *const sub = subLabel(card);
    QVERIFY(value != nullptr);
    QVERIFY(sub != nullptr);

    card.setValue(QStringLiteral("3"), QStringLiteral("since restart"));
    card.setValue(QStringLiteral("4"));

    QCOMPARE(value->text(), QStringLiteral("4"));
    QVERIFY(sub->text().isEmpty());
    QVERIFY(sub->toolTip().isEmpty());
    // No context line, no separator: the tooltip is the value by itself.
    QCOMPARE(value->toolTip(), QStringLiteral("4"));
}

void TestStatCard::theTooltipsCarryTheUnelidedReading()
{
    StatCard card(kGlyph, QStringLiteral("Disk I/O"));
    card.resize(kWideTile, kTileHeight);

    QLabel *const value = valueLabel(card);
    QLabel *const sub = subLabel(card);
    QVERIFY(value != nullptr);
    QVERIFY(sub != nullptr);

    card.setValue(QStringLiteral("1.2 MiB/s"), QStringLiteral("reads 4.0 KiB/s"));

    QCOMPARE(
        value->toolTip(),
        QStringLiteral("1.2 MiB/s ") + emDash() + QStringLiteral(" reads 4.0 KiB/s")
    );
    QCOMPARE(sub->toolTip(), QStringLiteral("reads 4.0 KiB/s"));
}

void TestStatCard::aReadingTooWideForTheTileIsElided()
{
    QWidget host;
    auto *const card = new StatCard(kGlyph, QStringLiteral("History List"), &host);
    card->resize(kNarrowTile, kTileHeight);

    QLabel *const value = valueLabel(*card);
    QLabel *const sub = subLabel(*card);
    QVERIFY(value != nullptr);
    QVERIFY(sub != nullptr);

    const QString reading = overlong(QLatin1Char('9'));
    const QString context = overlong(QLatin1Char('x'));
    card->setValue(reading, context);

    QVERIFY(value->text().size() < reading.size());
    QVERIFY(!value->text().isEmpty());
    QVERIFY(sub->text().size() < context.size());
    // Cut on screen, whole in the tooltip: the tile is narrow by design.
    QVERIFY(value->toolTip().contains(reading));
    QCOMPARE(sub->toolTip(), context);
}

void TestStatCard::resizingRedoesTheElision()
{
    QWidget host;
    host.resize(kWideTile, kTileHeight);
    auto *const card = new StatCard(kGlyph, QStringLiteral("Redo Log"), &host);
    card->setValue(overlong(QLatin1Char('9')), overlong(QLatin1Char('x')));

    QLabel *const value = valueLabel(*card);
    QVERIFY(value != nullptr);

    // A hidden widget banks its resize events until it is shown, and the
    // sheet's font size only lands at polish: both have to have happened
    // before two widths can be compared.
    host.show();
    QVERIFY(card->isVisible());

    card->resize(kWideTile, kTileHeight);
    const QString wide = value->text();

    card->resize(kNarrowTile, kTileHeight);
    const QString narrow = value->text();

    QVERIFY2(narrow.size() < wide.size(), "a narrower tile has to cut the value back");
}

void TestStatCard::theToneReachesTheValueAndNothingElse()
{
    StatCard card(kGlyph, QStringLiteral("Row Lock Waits"));

    QLabel *const value = valueLabel(card);
    QLabel *const sub = subLabel(card);
    QVERIFY(value != nullptr);
    QVERIFY(sub != nullptr);
    QVERIFY(value->styleSheet().isEmpty());

    // The dashboard passes a palette colour in and the tile only writes it
    // through, so what is under test is where it lands.
    const QColor alarm(0xff, 0x55, 0x00);
    const QString tile = card.styleSheet();
    card.setTone(alarm);

    QVERIFY(value->styleSheet().contains(alarm.name()));
    QVERIFY(sub->styleSheet().isEmpty());
    QCOMPARE(card.styleSheet(), tile);

    // An invalid colour is how every caller clears the tone again.
    card.setTone(QColor());

    QVERIFY(value->styleSheet().isEmpty());
    QCOMPARE(card.styleSheet(), tile);
}

void TestStatCard::theGlyphComesFromTheIconName()
{
    const StatCard card(kGlyph, QStringLiteral("Buffer Pool Hit"));

    // The sheet the tile writes for itself selects on the class name, which
    // is what the Q_OBJECT in the header is there for.
    QVERIFY(card.styleSheet().contains(QStringLiteral("StatCard {")));
    QCOMPARE(glyphCount(card), 1);

    // A name with no SVG behind it leaves the tile glyphless rather than
    // failing, so the name is worth pinning.
    const StatCard typo(kMissingGlyph, QStringLiteral("Buffer Pool Hit"));
    QCOMPARE(glyphCount(typo), 0);
}

QTEST_MAIN(TestStatCard)

#include "tst_statcard.moc"
