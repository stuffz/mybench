#include "views/graphview.h"

#include "app/api.h"
#include "app/stubbackend.h"
#include "app/theme.h"
#include "views/graphcanvas.h"

#include <QCheckBox>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QLabel>
#include <QLatin1String>
#include <QLineEdit>
#include <QList>
#include <QListWidget>
#include <QObject>
#include <QPair>
#include <QPushButton>
#include <QSignalSpy>
#include <QSlider>
#include <QString>
#include <QStringList>
#include <QTest>
#include <QTimer>
#include <QWidget>

// GraphView is the chrome around GraphCanvas: one fetch, the toolbar knobs it
// forwards, and the find box. The canvas reports its own state through
// counts(), nodeIds() and focusTable(), so every knob below is asserted
// against those rather than against the picture — tst_graphcanvas already owns
// what the filters mean, and nothing here repeats it.
namespace
{

constexpr auto ConnID = "conn-1";
constexpr int FontSize = 13;
constexpr int FlushMs = 5000;

constexpr auto FKGraphPath = "/rpc/admin/FKGraph";

constexpr auto RefreshText = "Refresh";
constexpr auto HideIsolatedText = "Hide Isolated";
constexpr auto ClearFocusText = "Clear Focus";
// The stepper glyphs: a real minus sign, not a hyphen.
constexpr auto HopsMinusText = "−";
constexpr auto HopsPlusText = "+";

// The node-size slider's own range and the size it opens at.
constexpr int MinNodeSize = 6;
constexpr int MaxNodeSize = 40;
constexpr int DefaultNodeSize = 16;
constexpr int WiderNodeSize = 24;

// setHops' clamp.
constexpr int MinHops = 1;
constexpr int MaxHops = 6;

// runFind keeps this many hits, however many matched.
constexpr int FindLimit = 12;

// alpha starts at 1 and decays half a percent a frame, so it takes a little
// over a thousand frames to reach the 0.005 floor the layout timer stops at.
constexpr int CoolingFrames = 1200;

constexpr auto Shop = "shop";
constexpr auto Warehouse = "warehouse";
constexpr auto Big = "big";

constexpr auto Denied = "admin.FKGraph: SELECT command denied to user";

// The shape admin.FKGraph answers with: each side of an edge arrives as a
// schema and a table, which the view has to pair back into one id.
QJsonObject node(const QString &schema, const QString &table, qint64 rows = 0)
{
    return QJsonObject{{"schema", schema}, {"table", table}, {"rows", rows}};
}

QJsonObject edge(
    const QString &fromSchema, const QString &fromTable, const QString &toSchema,
    const QString &toTable
)
{
    return QJsonObject{
        {"fromSchema", fromSchema},
        {"fromTable", fromTable},
        {"toSchema", toSchema},
        {"toTable", toTable},
    };
}

QJsonObject graph(const QJsonArray &nodes, const QJsonArray &edges)
{
    return QJsonObject{{"nodes", nodes}, {"edges", edges}};
}

QString tableId(const QString &schema, const QString &table)
{
    return schema + "." + table;
}

QPair<int, int> nodesAndEdges(int nodes, int edges)
{
    return {nodes, edges};
}

// One fixture behind most of the slots: a three table chain that crosses a
// schema boundary, plus a table nothing points at.
//
//     shop.orders → shop.customers → warehouse.bins      warehouse.orders
QJsonObject fkGraph()
{
    return graph(
        QJsonArray{
            node(Shop, QStringLiteral("orders"), 1200),
            node(Shop, QStringLiteral("customers"), 90),
            node(Warehouse, QStringLiteral("bins")),
            node(Warehouse, QStringLiteral("orders")),
        },
        QJsonArray{
            edge(Shop, QStringLiteral("orders"), Shop, QStringLiteral("customers")),
            edge(Shop, QStringLiteral("customers"), Warehouse, QStringLiteral("bins")),
        }
    );
}

QStringList fkGraphIds()
{
    return {
        tableId(Shop, QStringLiteral("orders")),
        tableId(Shop, QStringLiteral("customers")),
        tableId(Warehouse, QStringLiteral("bins")),
        tableId(Warehouse, QStringLiteral("orders")),
    };
}

GraphCanvas *canvasOf(const QWidget &view)
{
    return view.findChild<GraphCanvas *>();
}

QPushButton *buttonWith(const QWidget &view, const char *text)
{
    const QList<QPushButton *> buttons = view.findChildren<QPushButton *>();
    for (QPushButton *button : buttons)
    {
        if (button->text() == QString::fromUtf8(text))
        {
            return button;
        }
    }
    return nullptr;
}

// mutedLabel() builds the counts readout, and it is the only muted label on
// the toolbar.
QLabel *countsLabel(const QWidget &view)
{
    const QList<QLabel *> labels = view.findChildren<QLabel *>();
    for (QLabel *label : labels)
    {
        if (label->property("muted").toBool())
        {
            return label;
        }
    }
    return nullptr;
}

// The hops readout is the only centred label on the toolbar; the captions
// beside it keep QLabel's default alignment.
QLabel *hopsLabel(const QWidget &view)
{
    const QList<QLabel *> labels = view.findChildren<QLabel *>();
    for (QLabel *label : labels)
    {
        if (label->alignment() == Qt::AlignCenter)
        {
            return label;
        }
    }
    return nullptr;
}

// The view is never shown in a test, and a child of a hidden parent is never
// isVisible(): ask whether the widget would come up with the view instead.
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

// One fetch, run to completion. The Refresh button is the only way in from
// outside, and flush() returns once the reply has been applied.
bool fetchOnce(StubBackend &backend, const QWidget &view, const QJsonValue &reply)
{
    QPushButton *button = buttonWith(view, RefreshText);
    if (!button)
    {
        return false;
    }
    backend.replyWithResult(reply);
    button->click();
    api()->flush(FlushMs);
    return true;
}

int fetchesSeen(const StubBackend &backend)
{
    int seen = 0;
    for (const StubBackend::Request &req : backend.requests())
    {
        if (req.path == QLatin1String(FKGraphPath))
        {
            ++seen;
        }
    }
    return seen;
}

// Nothing ticks the layout timer in a test with no event loop: emit its
// timeout to drive whole frames until the simulation reaches its alpha floor.
bool settle(GraphCanvas &canvas)
{
    QTimer *timer = canvas.findChild<QTimer *>();
    if (!timer)
    {
        return false;
    }
    for (int i = 0; i < CoolingFrames && timer->isActive(); ++i)
    {
        QMetaObject::invokeMethod(timer, "timeout");
    }
    return !timer->isActive();
}

bool layoutRunning(const GraphCanvas &canvas)
{
    const QTimer *timer = canvas.findChild<QTimer *>();
    return timer && timer->isActive();
}

} // namespace

class TestGraphView : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void init();

    void theGraphIsFetchedForOneConnection();
    void bothHalvesOfEachEdgeEndArePairedBackTogether();
    void aReplyWithNoGraphInItDrawsNothing();
    void theCountsLineNamesWhatTheCanvasKept();

    void hideIsolatedDropsTheTablesNoForeignKeyTouches();
    void theNodeSizeSliderReflowsWithoutRebuilding();

    void theHopsStepperWidensTheFocusInPlace();
    void theHopsStepperStopsAtOneAndAtSix();

    void theFindBoxRanksTheTablesThatMatch();
    void aFindHitFocusesTheTableItNames();
    void theFindListKeepsTwelveHitsAtMost();
    void aQueryThatMatchesNothingShowsNoList();

    void clearFocusPutsTheWholeGraphBack();
    void anActivatedNodeBecomesTheFocus();

    void aFailedFetchSurfacesAndKeepsTheGraph();

private:
    StubBackend m_backend;
};

void TestGraphView::initTestCase()
{
    // The toolbar sizes its stepper with theme::scaledPx() and the error label
    // colours itself from theme::current(); apply() is what installs both.
    theme::apply(theme::defaultApp, FontSize);
}

void TestGraphView::init()
{
    m_backend.clearRequests();
    QVERIFY(m_backend.listening());
    api()->setEndpoint(m_backend.base(), QString());
}

void TestGraphView::theGraphIsFetchedForOneConnection()
{
    m_backend.replyWithResult(fkGraph());
    GraphView view(QString::fromLatin1(ConnID));
    api()->flush(FlushMs);

    QCOMPARE(fetchesSeen(m_backend), 1);
    const StubBackend::Request &req = m_backend.requests().at(0);
    QCOMPARE(req.verb, QStringLiteral("POST"));
    QCOMPARE(req.path, QString::fromLatin1(FKGraphPath));
    QCOMPARE(req.args, QJsonArray({QString::fromLatin1(ConnID)}));

    // Every node in the reply reaches the canvas, in the order it arrived, and
    // both foreign keys found the nodes they name.
    GraphCanvas *canvas = canvasOf(view);
    QVERIFY(canvas);
    QCOMPARE(canvas->nodeIds(), fkGraphIds());
    QCOMPARE(canvas->counts(), nodesAndEdges(4, 2));
}

void TestGraphView::bothHalvesOfEachEdgeEndArePairedBackTogether()
{
    // Both ends of this foreign key cross a schema boundary, so the schema and
    // the table of one side have to be joined to each other: "shop.bins" and
    // "warehouse.customers" are not nodes in this graph, and the canvas drops
    // an edge whose end it cannot find.
    m_backend.replyWithResult(graph(
        QJsonArray{
            node(Shop, QStringLiteral("customers")), node(Warehouse, QStringLiteral("bins"))
        },
        QJsonArray{edge(Shop, QStringLiteral("customers"), Warehouse, QStringLiteral("bins"))}
    ));
    GraphView view(QString::fromLatin1(ConnID));
    api()->flush(FlushMs);

    GraphCanvas *canvas = canvasOf(view);
    QVERIFY(canvas);
    QCOMPARE(canvas->counts(), nodesAndEdges(2, 1));
}

void TestGraphView::aReplyWithNoGraphInItDrawsNothing()
{
    // A server with no user tables answers with neither key rather than with
    // empty arrays, and that has to read as an empty graph, not as a reply
    // worth keeping the previous one over.
    m_backend.replyWithResult(fkGraph());
    GraphView view(QString::fromLatin1(ConnID));
    api()->flush(FlushMs);

    GraphCanvas *canvas = canvasOf(view);
    QVERIFY(canvas);
    QCOMPARE(canvas->counts(), nodesAndEdges(4, 2));

    QVERIFY(fetchOnce(m_backend, view, QJsonObject{}));
    QCOMPARE(canvas->counts(), nodesAndEdges(0, 0));
    QVERIFY(canvas->nodeIds().isEmpty());

    const QLabel *counts = countsLabel(view);
    QVERIFY(counts);
    QCOMPARE(counts->text(), QStringLiteral("0 tables · 0 foreign keys"));
}

void TestGraphView::theCountsLineNamesWhatTheCanvasKept()
{
    m_backend.replyWithResult(fkGraph());
    GraphView view(QString::fromLatin1(ConnID));
    api()->flush(FlushMs);

    const QLabel *counts = countsLabel(view);
    QVERIFY(counts);
    QCOMPARE(counts->text(), QStringLiteral("4 tables · 2 foreign keys"));

    // The line follows what is drawn, not what was fetched, so a filter has to
    // move it.
    QCheckBox *hide = view.findChild<QCheckBox *>();
    QVERIFY(hide);
    hide->setChecked(true);
    QCOMPARE(counts->text(), QStringLiteral("3 tables · 2 foreign keys"));
}

void TestGraphView::hideIsolatedDropsTheTablesNoForeignKeyTouches()
{
    m_backend.replyWithResult(fkGraph());
    GraphView view(QString::fromLatin1(ConnID));
    api()->flush(FlushMs);

    QCheckBox *hide = view.findChild<QCheckBox *>();
    QVERIFY(hide);
    QCOMPARE(hide->text(), QString::fromLatin1(HideIsolatedText));
    QVERIFY2(!hide->isChecked(), "the view opens with the whole server on screen");

    GraphCanvas *canvas = canvasOf(view);
    QVERIFY(canvas);
    hide->setChecked(true);
    QCOMPARE(canvas->counts(), nodesAndEdges(3, 2));

    // Filtered, not refetched: the dropped table is still in the graph the
    // find box searches.
    QCOMPARE(fetchesSeen(m_backend), 1);
    QCOMPARE(canvas->nodeIds(), fkGraphIds());

    hide->setChecked(false);
    QCOMPARE(canvas->counts(), nodesAndEdges(4, 2));
}

void TestGraphView::theNodeSizeSliderReflowsWithoutRebuilding()
{
    m_backend.replyWithResult(fkGraph());
    GraphView view(QString::fromLatin1(ConnID));
    api()->flush(FlushMs);

    QSlider *size = view.findChild<QSlider *>();
    QVERIFY(size);
    QCOMPARE(size->minimum(), MinNodeSize);
    QCOMPARE(size->maximum(), MaxNodeSize);
    QCOMPARE(size->value(), DefaultNodeSize);

    GraphCanvas *canvas = canvasOf(view);
    QVERIFY(canvas);
    QVERIFY2(settle(*canvas), "the layout has to come to rest before a nudge shows");

    QSignalSpy spy(canvas, &GraphCanvas::countsChanged);
    size->setValue(WiderNodeSize);

    // New radii mean new spring rest lengths, so the canvas reheats its
    // layout — but it never rebuilds, or the drag and the zoom behind the
    // slider would be thrown away mid-gesture.
    QVERIFY2(layoutRunning(*canvas), "a resize has to reflow the springs");
    QCOMPARE(spy.size(), 0);
    QCOMPARE(canvas->counts(), nodesAndEdges(4, 2));
}

void TestGraphView::theHopsStepperWidensTheFocusInPlace()
{
    m_backend.replyWithResult(fkGraph());
    GraphView view(QString::fromLatin1(ConnID));
    api()->flush(FlushMs);

    GraphCanvas *canvas = canvasOf(view);
    QVERIFY(canvas);
    const QLabel *hops = hopsLabel(view);
    QVERIFY(hops);
    QCOMPARE(hops->text(), QString::number(MinHops));

    // One hop off shop.orders reaches shop.customers and nothing else.
    view.focusOn(QString::fromLatin1(Shop), QStringLiteral("orders"));
    QCOMPARE(canvas->focusTable(), tableId(Shop, QStringLiteral("orders")));
    QCOMPARE(canvas->counts(), nodesAndEdges(2, 1));

    QPushButton *plus = buttonWith(view, HopsPlusText);
    QVERIFY(plus);
    plus->click();

    // The step re-applies the focus it already had, so the reach set widens
    // without another trip to the server.
    QCOMPARE(hops->text(), QStringLiteral("2"));
    QCOMPARE(canvas->counts(), nodesAndEdges(3, 2));
    QCOMPARE(fetchesSeen(m_backend), 1);

    QPushButton *minus = buttonWith(view, HopsMinusText);
    QVERIFY(minus);
    minus->click();
    QCOMPARE(hops->text(), QString::number(MinHops));
    QCOMPARE(canvas->counts(), nodesAndEdges(2, 1));
}

void TestGraphView::theHopsStepperStopsAtOneAndAtSix()
{
    m_backend.replyWithResult(fkGraph());
    GraphView view(QString::fromLatin1(ConnID));
    api()->flush(FlushMs);

    GraphCanvas *canvas = canvasOf(view);
    QVERIFY(canvas);
    view.focusOn(QString::fromLatin1(Shop), QStringLiteral("orders"));

    const QLabel *hops = hopsLabel(view);
    QVERIFY(hops);
    QPushButton *minus = buttonWith(view, HopsMinusText);
    QPushButton *plus = buttonWith(view, HopsPlusText);
    QVERIFY(minus);
    QVERIFY(plus);

    QSignalSpy spy(canvas, &GraphCanvas::countsChanged);
    minus->click();
    QCOMPARE(hops->text(), QString::number(MinHops));
    QVERIFY2(spy.isEmpty(), "a step that lands on the same radius must not rebuild");

    for (int step = MinHops; step < MaxHops; ++step)
    {
        plus->click();
    }
    QCOMPARE(hops->text(), QString::number(MaxHops));

    const qsizetype rebuilds = spy.size();
    plus->click();
    QCOMPARE(hops->text(), QString::number(MaxHops));
    QCOMPARE(spy.size(), rebuilds);
}

void TestGraphView::theFindBoxRanksTheTablesThatMatch()
{
    m_backend.replyWithResult(fkGraph());
    GraphView view(QString::fromLatin1(ConnID));
    api()->flush(FlushMs);

    QLineEdit *find = view.findChild<QLineEdit *>();
    QListWidget *hits = view.findChild<QListWidget *>();
    QVERIFY(find);
    QVERIFY(hits);
    QVERIFY2(!hits->isVisibleTo(&view), "the list stays down until something is typed");

    find->setText(QStringLiteral("ord"));

    // fuzzyScore charges for the gap before each matched character, so the id
    // that reaches "ord" soonest wins: shop.orders over warehouse.orders.
    // Neither shop.customers nor warehouse.bins matches at all.
    QVERIFY(hits->isVisibleTo(&view));
    QCOMPARE(hits->count(), 2);
    QCOMPARE(hits->item(0)->text(), tableId(Shop, QStringLiteral("orders")));
    QCOMPARE(hits->item(1)->text(), tableId(Warehouse, QStringLiteral("orders")));
    QVERIFY2(hits->currentRow() == 0, "the best hit is preselected, so Enter takes it");
}

void TestGraphView::aFindHitFocusesTheTableItNames()
{
    m_backend.replyWithResult(fkGraph());
    GraphView view(QString::fromLatin1(ConnID));
    api()->flush(FlushMs);

    QLineEdit *find = view.findChild<QLineEdit *>();
    QListWidget *hits = view.findChild<QListWidget *>();
    QVERIFY(find);
    QVERIFY(hits);
    find->setText(QStringLiteral("ord"));
    QVERIFY(hits->count() > 0);

    // itemActivated is where a click and Enter both end up, and neither can be
    // delivered to a list inside a view that is never shown.
    emit hits->itemActivated(hits->item(0));

    GraphCanvas *canvas = canvasOf(view);
    QVERIFY(canvas);
    QCOMPARE(canvas->focusTable(), tableId(Shop, QStringLiteral("orders")));
    QCOMPARE(canvas->counts(), nodesAndEdges(2, 1));

    // The box clears itself and the list goes away, so the next search starts
    // from nothing rather than from the hit that was just taken.
    QVERIFY(find->text().isEmpty());
    QVERIFY(!hits->isVisibleTo(&view));

    const QPushButton *clear = buttonWith(view, ClearFocusText);
    QVERIFY(clear);
    QVERIFY2(clear->isVisibleTo(&view), "the way out of a focus appears with the focus");
}

void TestGraphView::theFindListKeepsTwelveHitsAtMost()
{
    // Every id here matches "t", which is the case the cap exists for: a
    // server-wide graph would otherwise drop a list of thousands over the
    // canvas.
    QJsonArray nodes;
    for (int i = 0; i < FindLimit + 3; ++i)
    {
        nodes.append(node(Big, QStringLiteral("t%1").arg(i, 2, 10, QLatin1Char('0'))));
    }
    m_backend.replyWithResult(graph(nodes, QJsonArray{}));
    GraphView view(QString::fromLatin1(ConnID));
    api()->flush(FlushMs);

    GraphCanvas *canvas = canvasOf(view);
    QVERIFY(canvas);
    QCOMPARE(canvas->counts(), nodesAndEdges(FindLimit + 3, 0));

    QLineEdit *find = view.findChild<QLineEdit *>();
    QListWidget *hits = view.findChild<QListWidget *>();
    QVERIFY(find);
    QVERIFY(hits);
    find->setText(QStringLiteral("t"));
    QCOMPARE(hits->count(), FindLimit);
}

void TestGraphView::aQueryThatMatchesNothingShowsNoList()
{
    m_backend.replyWithResult(fkGraph());
    GraphView view(QString::fromLatin1(ConnID));
    api()->flush(FlushMs);

    QLineEdit *find = view.findChild<QLineEdit *>();
    QListWidget *hits = view.findChild<QListWidget *>();
    QVERIFY(find);
    QVERIFY(hits);

    find->setText(QStringLiteral("ord"));
    QVERIFY(hits->isVisibleTo(&view));

    // An empty popup over the canvas would read as a hung search.
    find->setText(QStringLiteral("zzz"));
    QVERIFY(!hits->isVisibleTo(&view));

    find->setText(QStringLiteral("ord"));
    QVERIFY(hits->isVisibleTo(&view));

    find->clear();
    QVERIFY(!hits->isVisibleTo(&view));
}

void TestGraphView::clearFocusPutsTheWholeGraphBack()
{
    m_backend.replyWithResult(fkGraph());
    GraphView view(QString::fromLatin1(ConnID));
    api()->flush(FlushMs);

    GraphCanvas *canvas = canvasOf(view);
    QPushButton *clear = buttonWith(view, ClearFocusText);
    QVERIFY(canvas);
    QVERIFY(clear);
    QVERIFY2(!clear->isVisibleTo(&view), "nothing is focused yet");

    view.focusOn(QString::fromLatin1(Shop), QStringLiteral("orders"));
    QCOMPARE(canvas->counts(), nodesAndEdges(2, 1));
    QVERIFY(clear->isVisibleTo(&view));

    clear->click();
    QVERIFY(canvas->focusTable().isEmpty());
    QCOMPARE(canvas->counts(), nodesAndEdges(4, 2));
    QVERIFY(!clear->isVisibleTo(&view));
}

void TestGraphView::anActivatedNodeBecomesTheFocus()
{
    m_backend.replyWithResult(fkGraph());
    GraphView view(QString::fromLatin1(ConnID));
    api()->flush(FlushMs);

    GraphCanvas *canvas = canvasOf(view);
    QVERIFY(canvas);

    // Double-clicking a node is the canvas' own gesture (tst_graphcanvas owns
    // the hit test); what it reaches here is the same focus the find list and
    // "Show in Graph" apply.
    emit canvas->tableActivated(QString::fromLatin1(Warehouse), QStringLiteral("bins"));

    QCOMPARE(canvas->focusTable(), tableId(Warehouse, QStringLiteral("bins")));
    QCOMPARE(canvas->counts(), nodesAndEdges(2, 1));

    const QPushButton *clear = buttonWith(view, ClearFocusText);
    QVERIFY(clear);
    QVERIFY(clear->isVisibleTo(&view));
}

void TestGraphView::aFailedFetchSurfacesAndKeepsTheGraph()
{
    m_backend.replyWithResult(fkGraph());
    GraphView view(QString::fromLatin1(ConnID));
    api()->flush(FlushMs);

    GraphCanvas *canvas = canvasOf(view);
    QVERIFY(canvas);
    QVERIFY(!showsLabel(view, QString::fromLatin1(Denied)));

    m_backend.replyWithError(QString::fromLatin1(Denied));
    QPushButton *button = buttonWith(view, RefreshText);
    QVERIFY(button);
    button->click();
    api()->flush(FlushMs);

    QVERIFY2(
        showsLabel(view, QString::fromLatin1(Denied)),
        "a failed fetch must say so, not read as a server with no foreign keys"
    );

    // The graph that did arrive stays on screen: emptying the canvas would
    // read as a schema whose tables are gone.
    QCOMPARE(canvas->counts(), nodesAndEdges(4, 2));
    const QLabel *counts = countsLabel(view);
    QVERIFY(counts);
    QCOMPARE(counts->text(), QStringLiteral("4 tables · 2 foreign keys"));

    QVERIFY(fetchOnce(m_backend, view, fkGraph()));
    QVERIFY2(
        !showsLabel(view, QString::fromLatin1(Denied)),
        "the message would otherwise outlive what it reported"
    );
}

QTEST_MAIN(TestGraphView)

#include "tst_graphview.moc"
