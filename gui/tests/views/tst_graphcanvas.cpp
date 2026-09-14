#include "views/graphcanvas.h"

#include "app/theme.h"

#include <QCoreApplication>
#include <QEvent>
#include <QImage>
#include <QMouseEvent>
#include <QObject>
#include <QPair>
#include <QPixmap>
#include <QPointF>
#include <QSignalSpy>
#include <QString>
#include <QStringList>
#include <QTest>
#include <QTimer>
#include <QVector>

// GraphCanvas reports its own state through counts(), nodeIds(), focusTable()
// and countsChanged, so the filters below are asserted against those rather
// than against the picture. Node positions are not exposed anywhere, so
// nothing here pins the layout: the paint tests only assert that a graph
// draws at all.
namespace
{

constexpr int CanvasWidth = 400;
constexpr int CanvasHeight = 300;

constexpr auto Schema = "s";
constexpr auto Other = "t";

// A row count big enough to sit at the top of the log10 size scale, so the
// radii in a paint test span the whole range instead of all landing on the
// floor.
constexpr qint64 HugeRows = 5000000;

// alpha starts at 1 and decays half a percent a frame, so it takes a little
// over a thousand frames to reach the 0.005 floor the timer stops at.
constexpr int CoolingFrames = 1200;

// A node is {schema, table, rows}, the row count as text: that is the shape
// GraphView builds out of the FKGraph reply.
QVector<QString> tableNode(const QString &schema, const QString &table, qint64 rows = 0)
{
    return {schema, table, QString::number(rows)};
}

QString tableId(const QString &schema, const QString &table)
{
    return schema + "." + table;
}

QPair<int, int> nodesAndEdges(int nodes, int edges)
{
    return {nodes, edges};
}

// a-b-c-d in one schema: a path long enough that each hop limit keeps a
// different set, and short enough that the counts name that set exactly.
QVector<QVector<QString>> chainNodes()
{
    return {
        tableNode(Schema, "a"),
        tableNode(Schema, "b"),
        tableNode(Schema, "c"),
        tableNode(Schema, "d"),
    };
}

QVector<QPair<QString, QString>> chainEdges()
{
    return {
        {tableId(Schema, "a"), tableId(Schema, "b")},
        {tableId(Schema, "b"), tableId(Schema, "c")},
        {tableId(Schema, "c"), tableId(Schema, "d")},
    };
}

QStringList chainIds()
{
    return {
        tableId(Schema, "a"),
        tableId(Schema, "b"),
        tableId(Schema, "c"),
        tableId(Schema, "d"),
    };
}

QImage paint(GraphCanvas &canvas)
{
    QPixmap pixmap(canvas.size());
    pixmap.fill(Qt::transparent);
    canvas.render(&pixmap);
    return pixmap.toImage();
}

// The canvas paints its own background before anything else, so an image that
// comes back transparent means nothing ran at all.
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

class TestGraphCanvas : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();

    void setGraphReportsWhatWentIn();
    void setGraphReplacesThePreviousGraph();
    void edgesWithoutTwoRealEndsAreDropped();
    void hideIsolatedJudgesByTheEdgesItDraws();
    void duplicateEdgesAreKeptAsTheyArrive();

    void hideIsolatedDropsNodesWithNoEdges();
    void nodeIdsAlwaysListTheWholeGraph();

    void focusKeepsWhatIsWithinTheHopLimit();
    void focusOfNoHopsKeepsOnlyTheTable();
    void focusWalksEdgesInBothDirections();
    void focusOutranksHideIsolated();
    void clearingTheFocusRestoresEveryNode();
    void anUnknownFocusTableFiltersNothing();

    void countsChangedFiresOnEveryRebuild();
    void fitAndMaxNodeSizeLeaveTheGraphAlone();

    void doubleClickActivatesTheNodeUnderTheCursor();
    void simulationStopsItselfOnceItCools();

    void paintingSurvivesAnEmptyGraph();
    void paintingSurvivesASingleNode();
    void paintingSurvivesEdgesAcrossSchemas();
};

void TestGraphCanvas::initTestCase()
{
    // Every paint reads theme::current(), theme::monoFamily() and
    // theme::scaledPx(); apply() is what installs all three.
    theme::apply(theme::defaultApp, 13);
}

void TestGraphCanvas::setGraphReportsWhatWentIn()
{
    GraphCanvas canvas;
    canvas.setGraph(chainNodes(), chainEdges());

    QCOMPARE(canvas.counts(), nodesAndEdges(4, 3));
    QCOMPARE(canvas.nodeIds(), chainIds());

    // The row count is read with value(), so a node that arrives without one
    // is a zero-row table rather than a dropped one.
    canvas.setGraph({{Schema, "a"}}, {});
    QCOMPARE(canvas.counts(), nodesAndEdges(1, 0));
    QCOMPARE(canvas.nodeIds(), QStringList({tableId(Schema, "a")}));
}

void TestGraphCanvas::setGraphReplacesThePreviousGraph()
{
    GraphCanvas canvas;
    canvas.setGraph(chainNodes(), chainEdges());

    canvas.setGraph({tableNode(Other, "x")}, {});
    QCOMPARE(canvas.counts(), nodesAndEdges(1, 0));
    QCOMPARE(canvas.nodeIds(), QStringList({tableId(Other, "x")}));

    // The edges are replaced too, not merged: the same tables with no FKs is
    // an edgeless graph, not the chain again.
    canvas.setGraph(chainNodes(), {});
    QCOMPARE(canvas.counts(), nodesAndEdges(4, 0));

    canvas.setGraph({}, {});
    QCOMPARE(canvas.counts(), nodesAndEdges(0, 0));
    QVERIFY(canvas.nodeIds().isEmpty());
}

void TestGraphCanvas::edgesWithoutTwoRealEndsAreDropped()
{
    const QString a = tableId(Schema, "a");
    const QString b = tableId(Schema, "b");
    const QString ghost = tableId(Schema, "ghost");

    GraphCanvas canvas;
    canvas.setGraph(
        {tableNode(Schema, "a"), tableNode(Schema, "b")}, {{a, b}, {a, ghost}, {ghost, b}}
    );

    // A FK whose other end is not a node in this graph has nothing to draw to.
    QCOMPARE(canvas.counts(), nodesAndEdges(2, 1));

    // A table with a FK to itself is one node and no line.
    canvas.setGraph({tableNode(Schema, "a")}, {{a, a}});
    QCOMPARE(canvas.counts(), nodesAndEdges(1, 0));
}

void TestGraphCanvas::duplicateEdgesAreKeptAsTheyArrive()
{
    const QString a = tableId(Schema, "a");
    const QString b = tableId(Schema, "b");

    GraphCanvas canvas;
    canvas.setGraph({tableNode(Schema, "a"), tableNode(Schema, "b")}, {{a, b}, {a, b}, {b, a}});

    // Two tables can be joined by several foreign keys, so nothing dedupes
    // them: three edges in, three edges out, direction included.
    QCOMPARE(canvas.counts(), nodesAndEdges(2, 3));
}

void TestGraphCanvas::hideIsolatedDropsNodesWithNoEdges()
{
    GraphCanvas canvas;
    canvas.setGraph(
        {tableNode(Schema, "a"), tableNode(Schema, "b"), tableNode(Schema, "lonely")},
        {{tableId(Schema, "a"), tableId(Schema, "b")}}
    );
    QCOMPARE(canvas.counts(), nodesAndEdges(3, 1));

    canvas.setHideIsolated(true);
    QCOMPARE(canvas.counts(), nodesAndEdges(2, 1));

    canvas.setHideIsolated(false);
    QCOMPARE(canvas.counts(), nodesAndEdges(3, 1));
}

void TestGraphCanvas::nodeIdsAlwaysListTheWholeGraph()
{
    QVector<QVector<QString>> nodes = chainNodes();
    nodes.append(tableNode(Schema, "lonely"));
    QStringList ids = chainIds();
    ids << tableId(Schema, "lonely");

    GraphCanvas canvas;
    canvas.setGraph(nodes, chainEdges());

    // The filters pick what is drawn. nodeIds() stays the whole fetched graph
    // either way, which is what the find box searches.
    canvas.setHideIsolated(true);
    QCOMPARE(canvas.counts(), nodesAndEdges(4, 3));
    QCOMPARE(canvas.nodeIds(), ids);

    canvas.setFocusTable(tableId(Schema, "a"), 0);
    QCOMPARE(canvas.counts(), nodesAndEdges(1, 0));
    QCOMPARE(canvas.nodeIds(), ids);
}

void TestGraphCanvas::focusKeepsWhatIsWithinTheHopLimit()
{
    GraphCanvas canvas;
    canvas.setGraph(chainNodes(), chainEdges());

    const QString a = tableId(Schema, "a");

    // a-b-c-d is a path, so the edge count names the reach set exactly: one
    // edge can only be a-b, and a stray c would have brought b-c with it.
    canvas.setFocusTable(a, 1);
    QCOMPARE(canvas.counts(), nodesAndEdges(2, 1));
    QCOMPARE(canvas.focusTable(), a);

    canvas.setFocusTable(a, 2);
    QCOMPARE(canvas.counts(), nodesAndEdges(3, 2));

    canvas.setFocusTable(a, 3);
    QCOMPARE(canvas.counts(), nodesAndEdges(4, 3));

    // The walk runs out of frontier at the end of the chain.
    canvas.setFocusTable(a, 9);
    QCOMPARE(canvas.counts(), nodesAndEdges(4, 3));
}

void TestGraphCanvas::focusOfNoHopsKeepsOnlyTheTable()
{
    GraphCanvas canvas;
    canvas.setGraph(chainNodes(), chainEdges());

    // The toolbar clamps hops to 1, but the API takes 0 and answers with the
    // focused table alone.
    canvas.setFocusTable(tableId(Schema, "b"), 0);
    QCOMPARE(canvas.counts(), nodesAndEdges(1, 0));
    QCOMPARE(canvas.focusTable(), tableId(Schema, "b"));
}

void TestGraphCanvas::focusWalksEdgesInBothDirections()
{
    GraphCanvas canvas;
    canvas.setGraph(chainNodes(), chainEdges());

    // b is the target of a-b and the source of b-c: one hop reaches both, so
    // the direction the FK was declared in does not matter.
    canvas.setFocusTable(tableId(Schema, "b"), 1);
    QCOMPARE(canvas.counts(), nodesAndEdges(3, 2));
}

void TestGraphCanvas::focusOutranksHideIsolated()
{
    GraphCanvas canvas;
    canvas.setGraph(
        {tableNode(Schema, "a"), tableNode(Schema, "b"), tableNode(Schema, "lonely")},
        {{tableId(Schema, "a"), tableId(Schema, "b")}}
    );
    canvas.setHideIsolated(true);
    QCOMPARE(canvas.counts(), nodesAndEdges(2, 1));

    // While a focus is set the reach set is the whole filter, so a table the
    // isolated filter had dropped is the one left on screen.
    canvas.setFocusTable(tableId(Schema, "lonely"), 2);
    QCOMPARE(canvas.counts(), nodesAndEdges(1, 0));
}

void TestGraphCanvas::clearingTheFocusRestoresEveryNode()
{
    GraphCanvas canvas;
    canvas.setGraph(chainNodes(), chainEdges());
    canvas.setFocusTable(tableId(Schema, "a"), 1);
    QCOMPARE(canvas.counts(), nodesAndEdges(2, 1));

    canvas.setFocusTable(QString(), 1);
    QCOMPARE(canvas.counts(), nodesAndEdges(4, 3));
    QVERIFY(canvas.focusTable().isEmpty());
}

void TestGraphCanvas::anUnknownFocusTableFiltersNothing()
{
    GraphCanvas canvas;
    canvas.setGraph(chainNodes(), chainEdges());

    // An id with no node behind it cannot be a centre, so the graph stays
    // whole. The canvas keeps the id anyway.
    canvas.setFocusTable(tableId(Schema, "nope"), 1);
    QCOMPARE(canvas.counts(), nodesAndEdges(4, 3));
    QCOMPARE(canvas.focusTable(), tableId(Schema, "nope"));

    // Kept, and not inert: the same focus filters as soon as a later fetch
    // brings that table with it.
    QVector<QVector<QString>> nodes = chainNodes();
    nodes.append(tableNode(Schema, "nope"));
    canvas.setGraph(nodes, chainEdges());
    QCOMPARE(canvas.counts(), nodesAndEdges(1, 0));
}

void TestGraphCanvas::countsChangedFiresOnEveryRebuild()
{
    GraphCanvas canvas;
    QSignalSpy spy(&canvas, &GraphCanvas::countsChanged);

    canvas.setGraph(chainNodes(), chainEdges());
    QCOMPARE(spy.size(), 1);
    QCOMPARE(spy.at(0).at(0).toInt(), 4);
    QCOMPARE(spy.at(0).at(1).toInt(), 3);

    canvas.setFocusTable(tableId(Schema, "a"), 1);
    QCOMPARE(spy.size(), 2);
    QCOMPARE(spy.at(1).at(0).toInt(), 2);
    QCOMPARE(spy.at(1).at(1).toInt(), 1);

    canvas.setFocusTable(QString(), 1);
    QCOMPARE(spy.size(), 3);

    // Every rebuild reports, changed or not: nothing in the chain is isolated,
    // so this filter drops nobody and still repeats the same pair.
    canvas.setHideIsolated(true);
    QCOMPARE(spy.size(), 4);
    QCOMPARE(spy.at(3).at(0).toInt(), 4);
    QCOMPARE(spy.at(3).at(1).toInt(), 3);
}

void TestGraphCanvas::fitAndMaxNodeSizeLeaveTheGraphAlone()
{
    GraphCanvas canvas;
    canvas.resize(CanvasWidth, CanvasHeight);
    canvas.setGraph(chainNodes(), chainEdges());

    QSignalSpy spy(&canvas, &GraphCanvas::countsChanged);
    canvas.setMaxNodeSize(48);
    canvas.fit();
    canvas.setMaxNodeSize(4);

    // Neither rebuilds: the radii and the view change, the graph does not.
    QCOMPARE(canvas.counts(), nodesAndEdges(4, 3));
    QCOMPARE(spy.size(), 0);
    QVERIFY(drewSomething(paint(canvas)));

    // The toolbar is live before the first fetch answers, so both have to cope
    // with no graph at all.
    GraphCanvas empty;
    empty.resize(CanvasWidth, CanvasHeight);
    empty.setMaxNodeSize(24);
    empty.fit();
    QCOMPARE(empty.counts(), nodesAndEdges(0, 0));
}

namespace
{

// mouseDoubleClickEvent is protected and the canvas is never mapped under
// offscreen, so the event goes straight to it rather than through the window
// system.
void sendDoubleClick(GraphCanvas &canvas, const QPointF &at)
{
    QMouseEvent click(
        QEvent::MouseButtonDblClick, at, at, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier
    );
    QCoreApplication::sendEvent(&canvas, &click);
}

} // namespace

void TestGraphCanvas::doubleClickActivatesTheNodeUnderTheCursor()
{
    GraphCanvas canvas;
    canvas.resize(CanvasWidth, CanvasHeight);
    canvas.setGraph({tableNode(Schema, "solo", HugeRows)}, {});

    // hitTest reads the pan and zoom that fit-to-view works out while
    // painting, so the graph has to have been drawn once. A lone node is the
    // centre of its own bounding box, which fit-to-view puts at the centre of
    // the widget whatever radius it ends up with.
    QVERIFY(drewSomething(paint(canvas)));

    QSignalSpy spy(&canvas, &GraphCanvas::tableActivated);
    sendDoubleClick(canvas, QPointF(CanvasWidth / 2.0, CanvasHeight / 2.0));
    QCOMPARE(spy.size(), 1);
    QCOMPARE(spy.at(0).at(0).toString(), QString(Schema));
    QCOMPARE(spy.at(0).at(1).toString(), QStringLiteral("solo"));

    // Off the node, a double click on the background opens nothing.
    sendDoubleClick(canvas, QPointF(1, 1));
    QCOMPARE(spy.size(), 1);
}

void TestGraphCanvas::simulationStopsItselfOnceItCools()
{
    GraphCanvas canvas;
    canvas.resize(CanvasWidth, CanvasHeight);
    canvas.setGraph(chainNodes(), chainEdges());

    QTimer *timer = canvas.findChild<QTimer *>();
    QVERIFY(timer);
    QVERIFY2(timer->isActive(), "a new graph has to run the layout");

    // Nothing ticks the timer in a test with no event loop, so emit its
    // timeout to drive whole frames rather than wait on the clock.
    for (int i = 0; i < CoolingFrames && timer->isActive(); ++i)
    {
        QMetaObject::invokeMethod(timer, "timeout");
    }
    QVERIFY2(!timer->isActive(), "a settled canvas must not keep a 16ms timer running");

    QCOMPARE(canvas.counts(), nodesAndEdges(4, 3));
    QVERIFY(drewSomething(paint(canvas)));
}

void TestGraphCanvas::paintingSurvivesAnEmptyGraph()
{
    GraphCanvas canvas;
    canvas.resize(CanvasWidth, CanvasHeight);

    // Nothing has been fetched yet: paintEvent fills the background and stops
    // before the layout maths.
    QVERIFY(drewSomething(paint(canvas)));

    canvas.setGraph({}, {});
    QCOMPARE(canvas.counts(), nodesAndEdges(0, 0));
    QVERIFY(drewSomething(paint(canvas)));
}

void TestGraphCanvas::paintingSurvivesASingleNode()
{
    GraphCanvas canvas;
    canvas.resize(CanvasWidth, CanvasHeight);

    // One node, one schema and no edges: the edge pass, the neighbour lookup
    // and the schema captions all run over nothing.
    canvas.setGraph({tableNode(Schema, "solo", HugeRows)}, {});
    QCOMPARE(canvas.counts(), nodesAndEdges(1, 0));
    QVERIFY(drewSomething(paint(canvas)));
}

void TestGraphCanvas::paintingSurvivesEdgesAcrossSchemas()
{
    GraphCanvas canvas;
    canvas.resize(CanvasWidth, CanvasHeight);
    canvas.setGraph(
        {tableNode(Schema, "a", 10), tableNode(Schema, "b", 1000), tableNode(Other, "x", HugeRows),
         tableNode(Other, "y")},
        {{tableId(Schema, "a"), tableId(Schema, "b")},
         {tableId(Schema, "b"), tableId(Other, "x")},
         {tableId(Other, "x"), tableId(Other, "y")}}
    );
    QCOMPARE(canvas.counts(), nodesAndEdges(4, 3));

    // A second schema puts the clusters on a ring and zooms out far enough for
    // the schema captions, which a single-schema graph never draws.
    QVERIFY(drewSomething(paint(canvas)));

    // Two hops from x reaches the whole graph, so this only adds the ring the
    // focused node is drawn with.
    canvas.setFocusTable(tableId(Other, "x"), 2);
    QCOMPARE(canvas.counts(), nodesAndEdges(4, 3));
    QVERIFY(drewSomething(paint(canvas)));
}

void TestGraphCanvas::hideIsolatedJudgesByTheEdgesItDraws()
{
    const QString alone = tableId(Schema, "alone");
    const QString loop = tableId(Schema, "loop");
    const QString left = tableId(Schema, "left");
    const QString right = tableId(Schema, "right");
    const QString ghost = tableId(Schema, "ghost");

    GraphCanvas canvas;
    canvas.setGraph(
        {tableNode(Schema, "alone"), tableNode(Schema, "loop"), tableNode(Schema, "left"),
         tableNode(Schema, "right")},
        {{alone, ghost}, {loop, loop}, {left, right}}
    );
    canvas.setHideIsolated(true);

    // A dangling edge and a self reference both draw nothing, so the tables
    // holding them are as isolated on screen as one with no edge at all.
    QCOMPARE(canvas.counts(), nodesAndEdges(2, 1));
}

QTEST_MAIN(TestGraphCanvas)

#include "tst_graphcanvas.moc"
