#include "editor/planview.h"

#include "app/theme.h"

#include <QObject>
#include <QString>
#include <QTest>
#include <QVector>
#include <QtNumeric>

// Everything the view shows comes out of parsePlanTree, and every annotation it
// reads rides on a single "-> " line, so the cases below are plan text rather
// than widgets.
//
// fmtNum and fmtMs are pure mappings and take rows instead of a run of
// QCOMPAREs: a QCOMPARE failure returns from the slot, which would let one bad
// input hide every input after it.
namespace
{

// apply() wants a slider value; nothing here depends on the size.
constexpr int BaseFontPx = 13;

// One node with both sides annotated, which is all the misestimate flag reads.
QString rowsLine(int estRows, int actualRows)
{
    return QStringLiteral("-> Scan  (cost=1 rows=%1) (actual time=1..2 rows=%2 loops=1)")
        .arg(estRows)
        .arg(actualRows);
}

} // namespace

class TestPlanView : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();

    void numbersSwitchUnitAtTheirThresholds_data();
    void numbersSwitchUnitAtTheirThresholds();
    void aNonFiniteNumberGetsNoUnit();
    void timesSwitchUnitAtTheirThresholds_data();
    void timesSwitchUnitAtTheirThresholds();

    void nodesNestByTheirIndentation();
    void aLineBackAtTheRootIndentStartsANewTree();

    void aNodeCarriesItsEstimateAndItsActuals();
    void aNodeWithoutAnnotationsLeavesThemUnset();
    void anEstimateAloneLeavesTheActualsUnset();
    void neverExecutedLeavesTheActualsUnset();

    void aCostRangeKeepsItsLowEnd();
    void misestimateFlagsAHundredFoldGap_data();
    void misestimateFlagsAHundredFoldGap();

    void textThatIsNotATreeParsesToNothing();
};

void TestPlanView::initTestCase()
{
    // planview.cpp is a widget TU: the colours it gives an item come from
    // theme::current(), and apply() is what installs a palette.
    theme::apply(theme::defaultApp, BaseFontPx);
}

void TestPlanView::numbersSwitchUnitAtTheirThresholds_data()
{
    QTest::addColumn<double>("value");
    QTest::addColumn<QString>("text");

    // Row counts arrive as doubles but are whole, and a plan reads worse with
    // ".00" on every one of them.
    QTest::newRow("zero") << 0.0 << QStringLiteral("0");
    QTest::newRow("whole") << 42.0 << QStringLiteral("42");

    QTest::newRow("fraction") << 0.5 << QStringLiteral("0.5");
    QTest::newRow("three significant digits") << 12.345 << QStringLiteral("12.3");

    QTest::newRow("last plain number") << 999.0 << QStringLiteral("999");
    QTest::newRow("first kilo") << 1000.0 << QStringLiteral("1k");
    QTest::newRow("kilo") << 1234.0 << QStringLiteral("1.23k");
    QTest::newRow("last kilo") << 999000.0 << QStringLiteral("999k");
    QTest::newRow("first mega") << 1e6 << QStringLiteral("1M");
    QTest::newRow("mega") << 1.5e6 << QStringLiteral("1.5M");
    QTest::newRow("last mega") << 999e6 << QStringLiteral("999M");
    QTest::newRow("first giga") << 1e9 << QStringLiteral("1G");
    QTest::newRow("giga") << 2.5e9 << QStringLiteral("2.5G");
}

void TestPlanView::numbersSwitchUnitAtTheirThresholds()
{
    QFETCH(double, value);
    QFETCH(QString, text);
    QCOMPARE(fmtNum(value), text);
}

void TestPlanView::aNonFiniteNumberGetsNoUnit()
{
    // Infinity clears every threshold, so without the guard ahead of them it
    // would come out as a quantity: "infG".
    QCOMPARE(fmtNum(qInf()), QString::number(qInf()));
    QCOMPARE(fmtNum(qQNaN()), QString::number(qQNaN()));
}

void TestPlanView::timesSwitchUnitAtTheirThresholds_data()
{
    QTest::addColumn<double>("ms");
    QTest::addColumn<QString>("text");

    QTest::newRow("zero") << 0.0 << QStringLiteral("0µs");
    QTest::newRow("well under a microsecond") << 0.0005 << QStringLiteral("0.5µs");
    QTest::newRow("microseconds") << 0.0234 << QStringLiteral("23.4µs");
    QTest::newRow("last microsecond") << 0.999 << QStringLiteral("999µs");
    QTest::newRow("first millisecond") << 1.0 << QStringLiteral("1ms");
    QTest::newRow("milliseconds") << 12.345 << QStringLiteral("12.3ms");
    QTest::newRow("last millisecond") << 999.0 << QStringLiteral("999ms");
    QTest::newRow("first second") << 1000.0 << QStringLiteral("1s");
    QTest::newRow("seconds") << 1500.0 << QStringLiteral("1.5s");
    QTest::newRow("a minute and a half") << 90000.0 << QStringLiteral("90s");
}

void TestPlanView::timesSwitchUnitAtTheirThresholds()
{
    QFETCH(double, ms);
    QFETCH(QString, text);
    QCOMPARE(fmtMs(ms), text);
}

void TestPlanView::nodesNestByTheirIndentation()
{
    // MySQL indents a child four spaces past its parent. The last line dedents
    // back to the filter's indent, which makes it the join's second child
    // rather than the table scan's sibling.
    const QString plan =
        QStringLiteral("-> Nested loop inner join  (cost=1.55 rows=2)\n"
                       "    -> Filter: (a.x > 1)  (cost=0.85 rows=2)\n"
                       "        -> Table scan on a  (cost=0.85 rows=2)\n"
                       "    -> Index lookup on b using PRIMARY (id=a.id)  (cost=0.35 rows=1)\n");

    const QVector<PlanNode> roots = parsePlanTree(plan);
    QCOMPARE(roots.size(), 1);

    const PlanNode &join = roots.first();
    QCOMPARE(join.name, QStringLiteral("Nested loop inner join"));
    QCOMPARE(join.children.size(), 2);

    const PlanNode &filter = join.children.at(0);
    QCOMPARE(filter.name, QStringLiteral("Filter: (a.x > 1)"));
    QCOMPARE(filter.children.size(), 1);
    QCOMPARE(filter.children.at(0).name, QStringLiteral("Table scan on a"));
    QCOMPARE(filter.children.at(0).children.size(), 0);

    const PlanNode &lookup = join.children.at(1);
    QCOMPARE(lookup.name, QStringLiteral("Index lookup on b using PRIMARY (id=a.id)"));
    QCOMPARE(lookup.children.size(), 0);
}

void TestPlanView::aLineBackAtTheRootIndentStartsANewTree()
{
    const QString plan = QStringLiteral("-> Table scan on a  (cost=0.85 rows=2)\n"
                                        "    -> Table scan on b  (cost=0.85 rows=2)\n"
                                        "-> Table scan on c  (cost=0.85 rows=2)\n");

    const QVector<PlanNode> roots = parsePlanTree(plan);
    QCOMPARE(roots.size(), 2);
    QCOMPARE(roots.at(0).children.size(), 1);
    QCOMPARE(roots.at(1).name, QStringLiteral("Table scan on c"));
    QCOMPARE(roots.at(1).children.size(), 0);
}

void TestPlanView::aNodeCarriesItsEstimateAndItsActuals()
{
    const QString line = QStringLiteral("-> Table scan on t  (cost=1.05 rows=10) "
                                        "(actual time=0.0234..0.0456 rows=8 loops=3)");

    const QVector<PlanNode> roots = parsePlanTree(line);
    QCOMPARE(roots.size(), 1);

    // Both annotations are stripped from the name, which is what the operation
    // column shows on its own.
    const PlanNode &n = roots.first();
    QCOMPARE(n.name, QStringLiteral("Table scan on t"));

    QVERIFY(n.estCost && n.estRows);
    QCOMPARE(*n.estCost, 1.05);
    QCOMPARE(*n.estRows, 10.0);

    QVERIFY(n.actualFirstMs && n.actualLastMs && n.actualRows && n.loops);
    QCOMPARE(*n.actualFirstMs, 0.0234);
    QCOMPARE(*n.actualLastMs, 0.0456);
    QCOMPARE(*n.actualRows, 8.0);
    QCOMPARE(*n.loops, 3.0);

    QVERIFY(!n.neverExecuted);
    QVERIFY(!n.misestimate);
}

void TestPlanView::aNodeWithoutAnnotationsLeavesThemUnset()
{
    const QVector<PlanNode> roots = parsePlanTree(QStringLiteral("-> Table scan on t"));
    QCOMPARE(roots.size(), 1);

    const PlanNode &n = roots.first();
    QCOMPARE(n.name, QStringLiteral("Table scan on t"));
    QVERIFY(!n.estCost);
    QVERIFY(!n.estRows);
    QVERIFY(!n.actualFirstMs);
    QVERIFY(!n.actualLastMs);
    QVERIFY(!n.actualRows);
    QVERIFY(!n.loops);
    QVERIFY(!n.neverExecuted);
    QVERIFY(!n.misestimate);
}

void TestPlanView::anEstimateAloneLeavesTheActualsUnset()
{
    // EXPLAIN without ANALYZE: nothing ran, so there is an estimate and nothing
    // to hold it against.
    const QVector<PlanNode> roots =
        parsePlanTree(QStringLiteral("-> Filter: (t.x > 1)  (cost=2.5 rows=4)"));
    QCOMPARE(roots.size(), 1);

    const PlanNode &n = roots.first();
    QCOMPARE(n.name, QStringLiteral("Filter: (t.x > 1)"));

    QVERIFY(n.estCost && n.estRows);
    QCOMPARE(*n.estCost, 2.5);
    QCOMPARE(*n.estRows, 4.0);

    QVERIFY(!n.actualFirstMs);
    QVERIFY(!n.actualLastMs);
    QVERIFY(!n.actualRows);
    QVERIFY(!n.loops);
    QVERIFY(!n.misestimate);
}

void TestPlanView::neverExecutedLeavesTheActualsUnset()
{
    const QString line = QStringLiteral("-> Index lookup on b using PRIMARY (id=a.id)  "
                                        "(cost=0.35 rows=1) (never executed)");

    const QVector<PlanNode> roots = parsePlanTree(line);
    QCOMPARE(roots.size(), 1);

    const PlanNode &n = roots.first();
    QVERIFY(n.neverExecuted);
    QCOMPARE(n.name, QStringLiteral("Index lookup on b using PRIMARY (id=a.id)"));

    // The branch the view takes instead of a timing, so there is no actual side
    // behind it.
    QVERIFY(!n.actualFirstMs);
    QVERIFY(!n.actualLastMs);
    QVERIFY(!n.actualRows);
    QVERIFY(!n.loops);

    // The estimate survives the annotation next to it being stripped.
    QVERIFY(n.estRows);
    QCOMPARE(*n.estRows, 1.0);
}

void TestPlanView::misestimateFlagsAHundredFoldGap_data()
{
    QTest::addColumn<QString>("line");
    QTest::addColumn<bool>("flagged");

    QTest::newRow("a hundred times more rows than estimated") << rowsLine(10, 1000) << true;
    QTest::newRow("just under the factor") << rowsLine(10, 999) << false;
    QTest::newRow("a hundred times fewer rows than estimated") << rowsLine(1000, 10) << true;
    QTest::newRow("just under the factor the other way") << rowsLine(999, 10) << false;

    // Rows estimated, none returned: the floor under the division stands in for
    // the infinity, so this is the widest gap there is rather than none.
    QTest::newRow("nothing returned at all") << rowsLine(10, 0) << true;

    // Nothing to be wrong by: an estimate of no rows is left alone rather than
    // scaled against what came back.
    QTest::newRow("nothing estimated") << rowsLine(0, 1000) << false;

    QTest::newRow("no actual side") << QStringLiteral("-> Scan  (cost=1 rows=10)") << false;
    QTest::newRow("no estimated side")
        << QStringLiteral("-> Scan  (actual time=1..2 rows=1000 loops=1)") << false;
}

void TestPlanView::misestimateFlagsAHundredFoldGap()
{
    QFETCH(QString, line);
    QFETCH(bool, flagged);

    const QVector<PlanNode> roots = parsePlanTree(line);
    QCOMPARE(roots.size(), 1);
    QCOMPARE(roots.first().misestimate, flagged);
}

void TestPlanView::textThatIsNotATreeParsesToNothing()
{
    // An empty result is the view's cue to show the raw text instead: an older
    // server, MariaDB or a FORMAT override answers with a table, and no line of
    // one opens a node.
    const QString tabular = QStringLiteral("id\tselect_type\ttable\ttype\trows\tExtra\n"
                                           "1\tSIMPLE\tusers\tALL\t42\tUsing where\n");
    QVERIFY(parsePlanTree(tabular).isEmpty());

    // The arrow opens a node only at the start of a line.
    QVERIFY(parsePlanTree(QStringLiteral("1 SIMPLE users -> nope")).isEmpty());

    QVERIFY(parsePlanTree(QStringLiteral("\n  \n")).isEmpty());
    QVERIFY(parsePlanTree(QString()).isEmpty());
}

void TestPlanView::aCostRangeKeepsItsLowEnd()
{
    // EXPLAIN FORMAT=TREE prints cost as start..total for most nodes. The low
    // end is the one that parallels actual time's first value.
    const QVector<PlanNode> nodes =
        parsePlanTree(QStringLiteral("-> Table scan on t  (cost=1.05..3.25 rows=10)"));

    QCOMPARE(nodes.size(), 1);
    QCOMPARE(nodes.at(0).name, QStringLiteral("Table scan on t"));
    QVERIFY(nodes.at(0).estCost);
    QCOMPARE(*nodes.at(0).estCost, 1.05);
    QVERIFY(nodes.at(0).estRows);
    QCOMPARE(*nodes.at(0).estRows, 10.0);
}

QTEST_MAIN(TestPlanView)

#include "tst_planview.moc"
