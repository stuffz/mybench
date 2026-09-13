#include "ui/fmt.h"

#include <QLocale>
#include <QObject>
#include <QTest>

// Every formatter here goes through QLocale(), so the expectations below only
// hold against a pinned locale: the group separator and decimal point are part
// of what is being asserted.
//
// Each formatter is a pure mapping, so its cases are rows rather than a run of
// QCOMPAREs: a QCOMPARE failure returns from the slot, which would let one bad
// input hide every input after it.
class TestFmt : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();

    void countsCarryGroupSeparators_data();
    void countsCarryGroupSeparators();
    void ratesKeepOneDecimalOnlyBelowTen_data();
    void ratesKeepOneDecimalOnlyBelowTen();
    void compactSwitchesUnitAtItsThresholds_data();
    void compactSwitchesUnitAtItsThresholds();
    void percentagesLoseTheDecimalAtTen_data();
    void percentagesLoseTheDecimalAtTen();
    void percentagesKeepTheDecimalsTheCallerAsksFor_data();
    void percentagesKeepTheDecimalsTheCallerAsksFor();
    void percentOfClampsAndRefusesAMissingWhole_data();
    void percentOfClampsAndRefusesAMissingWhole();
    void bytesClimbUnitsAndStopAtTebibytes_data();
    void bytesClimbUnitsAndStopAtTebibytes();
    void byteRatesRoundThenFormat_data();
    void byteRatesRoundThenFormat();
    void uptimeKeepsTheLargestTwoUnits_data();
    void uptimeKeepsTheLargestTwoUnits();
    void trimZerosOnlyTouchesDecimals_data();
    void trimZerosOnlyTouchesDecimals();
};

void TestFmt::initTestCase()
{
    QLocale::setDefault(QLocale(QLocale::English, QLocale::UnitedStates));
}

void TestFmt::countsCarryGroupSeparators_data()
{
    QTest::addColumn<qint64>("count");
    QTest::addColumn<QString>("text");

    QTest::newRow("zero") << 0LL << QStringLiteral("0");
    QTest::newRow("below the first separator") << 999LL << QStringLiteral("999");
    QTest::newRow("grouped") << 1234567LL << QStringLiteral("1,234,567");
    QTest::newRow("negative") << -4200LL << QStringLiteral("-4,200");
}

void TestFmt::countsCarryGroupSeparators()
{
    QFETCH(qint64, count);
    QFETCH(QString, text);
    QCOMPARE(fmtCount(count), text);
}

void TestFmt::ratesKeepOneDecimalOnlyBelowTen_data()
{
    QTest::addColumn<double>("rate");
    QTest::addColumn<QString>("text");

    // A small rate rounded to a whole number would read as nothing at all.
    QTest::newRow("fraction keeps its decimal") << 0.4 << QStringLiteral("0.4");
    QTest::newRow("just below ten") << 9.5 << QStringLiteral("9.5");

    // At ten and above the decimal stops earning its place.
    QTest::newRow("at ten") << 10.0 << QStringLiteral("10");
    QTest::newRow("rounds and groups") << 1234.6 << QStringLiteral("1,235");

    // Zero and negatives take the rounding branch, not the decimal one.
    QTest::newRow("zero") << 0.0 << QStringLiteral("0");
    QTest::newRow("negative") << -3.2 << QStringLiteral("-3");
}

void TestFmt::ratesKeepOneDecimalOnlyBelowTen()
{
    QFETCH(double, rate);
    QFETCH(QString, text);
    QCOMPARE(fmtRate(rate), text);
}

void TestFmt::compactSwitchesUnitAtItsThresholds_data()
{
    QTest::addColumn<double>("value");
    QTest::addColumn<QString>("text");

    QTest::newRow("giga") << 2.5e9 << QStringLiteral("2.5G");
    QTest::newRow("first giga") << 1e9 << QStringLiteral("1.0G");
    QTest::newRow("mega") << 5.4e6 << QStringLiteral("5.4M");
    QTest::newRow("first mega") << 1e6 << QStringLiteral("1.0M");
    QTest::newRow("kilo") << 12345.0 << QStringLiteral("12k");
    QTest::newRow("first kilo") << 10000.0 << QStringLiteral("10k");

    // Below the 'k' threshold it falls through to fmtRate and keeps its
    // separators rather than collapsing to "10k".
    QTest::newRow("just below kilo") << 9999.0 << QStringLiteral("9,999");
    QTest::newRow("small") << 5.5 << QStringLiteral("5.5");
}

void TestFmt::compactSwitchesUnitAtItsThresholds()
{
    QFETCH(double, value);
    QFETCH(QString, text);
    QCOMPARE(fmtCompact(value), text);
}

void TestFmt::percentagesLoseTheDecimalAtTen_data()
{
    QTest::addColumn<double>("percent");
    QTest::addColumn<QString>("text");

    QTest::newRow("zero") << 0.0 << QStringLiteral("0.0%");
    QTest::newRow("fraction") << 3.14159 << QStringLiteral("3.1%");
    QTest::newRow("at ten") << 10.0 << QStringLiteral("10%");
    QTest::newRow("rounds") << 42.7 << QStringLiteral("43%");
    QTest::newRow("full") << 100.0 << QStringLiteral("100%");
}

void TestFmt::percentagesLoseTheDecimalAtTen()
{
    QFETCH(double, percent);
    QFETCH(QString, text);
    QCOMPARE(fmtPercent(percent), text);
}

void TestFmt::percentagesKeepTheDecimalsTheCallerAsksFor_data()
{
    QTest::addColumn<double>("percent");
    QTest::addColumn<int>("decimals");
    QTest::addColumn<QString>("text");

    // The adaptive rule would round all three of these to "100%", which is the
    // whole reason the overload exists.
    QTest::newRow("four nines") << 99.996227 << 4 << QStringLiteral("99.9962%");
    QTest::newRow("ten times worse") << 99.96227 << 4 << QStringLiteral("99.9623%");
    QTest::newRow("hundred times worse") << 99.6227 << 4 << QStringLiteral("99.6227%");

    QTest::newRow("zero decimals") << 42.7 << 0 << QStringLiteral("43%");
    QTest::newRow("below ten keeps none unasked") << 3.14159 << 0 << QStringLiteral("3%");
    QTest::newRow("full") << 100.0 << 4 << QStringLiteral("100.0000%");
}

void TestFmt::percentagesKeepTheDecimalsTheCallerAsksFor()
{
    QFETCH(double, percent);
    QFETCH(int, decimals);
    QFETCH(QString, text);
    QCOMPARE(fmtPercent(percent, decimals), text);
}

void TestFmt::percentOfClampsAndRefusesAMissingWhole_data()
{
    QTest::addColumn<double>("part");
    QTest::addColumn<double>("whole");
    QTest::addColumn<double>("percent");

    QTest::newRow("quarter") << 50.0 << 200.0 << 25.0;
    QTest::newRow("all of it") << 200.0 << 200.0 << 100.0;

    // A missing denominator reads as nothing rather than as a spike.
    QTest::newRow("zero whole") << 5.0 << 0.0 << 0.0;
    QTest::newRow("negative whole") << 5.0 << -1.0 << 0.0;

    QTest::newRow("over the top clamps") << 300.0 << 100.0 << 100.0;
    QTest::newRow("below zero clamps") << -5.0 << 100.0 << 0.0;
}

void TestFmt::percentOfClampsAndRefusesAMissingWhole()
{
    QFETCH(double, part);
    QFETCH(double, whole);
    QFETCH(double, percent);
    QCOMPARE(percentOf(part, whole), percent);
}

void TestFmt::bytesClimbUnitsAndStopAtTebibytes_data()
{
    QTest::addColumn<qint64>("bytes");
    QTest::addColumn<QString>("text");

    QTest::newRow("zero") << 0LL << QStringLiteral("0 B");
    QTest::newRow("last plain byte") << 1023LL << QStringLiteral("1023 B");
    QTest::newRow("first kibibyte") << 1024LL << QStringLiteral("1.0 KiB");
    QTest::newRow("one and a half") << 1536LL << QStringLiteral("1.5 KiB");

    // Ten of a unit is where the decimal is dropped, same rule as fmtRate.
    QTest::newRow("decimal dropped") << 10LL * 1024 << QStringLiteral("10 KiB");

    QTest::newRow("mebibyte") << 1024LL * 1024 << QStringLiteral("1.0 MiB");
    QTest::newRow("gibibyte") << 1024LL * 1024 * 1024 << QStringLiteral("1.0 GiB");
    QTest::newRow("tebibyte") << 1024LL * 1024 * 1024 * 1024 << QStringLiteral("1.0 TiB");

    // TiB is the last unit, so a petabyte-scale value keeps growing in TiB
    // rather than running off the end of the table.
    QTest::newRow("past the ladder")
        << 1024LL * 1024 * 1024 * 1024 * 1024 << QStringLiteral("1024 TiB");
}

void TestFmt::bytesClimbUnitsAndStopAtTebibytes()
{
    QFETCH(qint64, bytes);
    QFETCH(QString, text);
    QCOMPARE(fmtBytes(bytes), text);
}

void TestFmt::byteRatesRoundThenFormat_data()
{
    QTest::addColumn<double>("rate");
    QTest::addColumn<QString>("text");

    QTest::newRow("zero") << 0.0 << QStringLiteral("0 B/s");
    QTest::newRow("rounds down") << 2048.4 << QStringLiteral("2.0 KiB/s");
    QTest::newRow("rounds up across the unit") << 1023.6 << QStringLiteral("1.0 KiB/s");
}

void TestFmt::byteRatesRoundThenFormat()
{
    QFETCH(double, rate);
    QFETCH(QString, text);
    QCOMPARE(fmtBytesRate(rate), text);
}

void TestFmt::uptimeKeepsTheLargestTwoUnits_data()
{
    QTest::addColumn<qint64>("seconds");
    QTest::addColumn<QString>("text");

    QTest::newRow("zero") << 0LL << QStringLiteral("0m");
    QTest::newRow("under a minute") << 59LL << QStringLiteral("0m");
    QTest::newRow("minutes") << 90LL << QStringLiteral("1m");
    QTest::newRow("exact hour") << 3600LL << QStringLiteral("1h 0m");
    QTest::newRow("hour and minutes") << 3661LL << QStringLiteral("1h 1m");
    QTest::newRow("exact day") << 86400LL << QStringLiteral("1d 0h");

    // Days and hours, and the minutes are dropped rather than appended.
    QTest::newRow("day and hours") << 90061LL << QStringLiteral("1d 1h");
}

void TestFmt::uptimeKeepsTheLargestTwoUnits()
{
    QFETCH(qint64, seconds);
    QFETCH(QString, text);
    QCOMPARE(fmtUptime(seconds), text);
}

void TestFmt::trimZerosOnlyTouchesDecimals_data()
{
    QTest::addColumn<QString>("value");
    QTest::addColumn<QString>("text");

    QTest::newRow("all decimals") << QStringLiteral("10.000000") << QStringLiteral("10");
    QTest::newRow("trailing only") << QStringLiteral("1.500") << QStringLiteral("1.5");
    QTest::newRow("keeps a significant zero") << QStringLiteral("1.010") << QStringLiteral("1.01");
    QTest::newRow("zero") << QStringLiteral("0.000") << QStringLiteral("0");

    // No decimal point, no trimming: a round count must not lose its zeros.
    QTest::newRow("no decimal point") << QStringLiteral("100") << QStringLiteral("100");
    QTest::newRow("empty") << QString() << QString();
}

void TestFmt::trimZerosOnlyTouchesDecimals()
{
    QFETCH(QString, value);
    QFETCH(QString, text);
    QCOMPARE(trimZeros(value), text);
}

QTEST_APPLESS_MAIN(TestFmt)

#include "tst_fmt.moc"
