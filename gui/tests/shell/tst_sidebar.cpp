#include "shell/sidebar.h"

#include "app/api.h"
#include "app/stubbackend.h"
#include "app/theme.h"
#include "shell/tabs.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QLatin1String>
#include <QLineEdit>
#include <QList>
#include <QListWidget>
#include <QListWidgetItem>
#include <QObject>
#include <QSignalSpy>
#include <QString>
#include <QStringList>
#include <QTest>
#include <QTreeWidget>
#include <QTreeWidgetItem>

namespace
{

constexpr auto ConnID = "c1";
constexpr auto OtherConn = "c2";
constexpr auto SchemasPath = "/rpc/admin/Schemas";
constexpr auto TablesPath = "/rpc/admin/Tables";
constexpr auto ColumnsPath = "/rpc/admin/Columns";
constexpr int FlushMs = 5000;
constexpr int FontSize = 13;

enum class Kind
{
    Base,
    View
};

QJsonObject tableRow(const QString &name, Kind kind)
{
    QJsonObject o;
    o.insert(QStringLiteral("name"), name);
    o.insert(
        QStringLiteral("type"),
        kind == Kind::View ? QStringLiteral("VIEW") : QStringLiteral("BASE TABLE")
    );
    return o;
}

QJsonObject columnRow(const QString &name, const QString &type, const QString &key = QString())
{
    QJsonObject o;
    o.insert(QStringLiteral("name"), name);
    o.insert(QStringLiteral("type"), type);
    o.insert(QStringLiteral("key"), key);
    return o;
}

QJsonArray twoSchemas()
{
    return {QStringLiteral("shop"), QStringLiteral("warehouse")};
}

QJsonArray shopTables()
{
    return {
        tableRow(QStringLiteral("orders"), Kind::Base),
        tableRow(QStringLiteral("customers"), Kind::Base),
    };
}

// The sidebar keeps its widgets to itself, and each of these is the only one of
// its kind in there.
QTreeWidget *tree(const Sidebar &side)
{
    return side.findChild<QTreeWidget *>();
}

QLineEdit *filterEdit(const Sidebar &side)
{
    return side.findChild<QLineEdit *>();
}

QListWidget *adminList(const Sidebar &side)
{
    return side.findChild<QListWidget *>();
}

QTreeWidgetItem *schemaNamed(const Sidebar &side, const QString &name)
{
    const QList<QTreeWidgetItem *> hits = tree(side)->findItems(name, Qt::MatchExactly);
    return hits.isEmpty() ? nullptr : hits.first();
}

QTreeWidgetItem *childNamed(QTreeWidgetItem *parent, const QString &name)
{
    for (int i = 0; i < parent->childCount(); ++i)
    {
        if (parent->child(i)->text(0) == name)
        {
            return parent->child(i);
        }
    }
    return nullptr;
}

QStringList childNames(QTreeWidgetItem *parent)
{
    QStringList out;
    for (int i = 0; i < parent->childCount(); ++i)
    {
        out.append(parent->child(i)->text(0));
    }
    return out;
}

// The calls that went to one RPC method, so a slot asserts on the conversation
// it is about and not on whatever the tree fetched alongside it.
QList<StubBackend::Request> callsTo(const StubBackend &backend, const char *path)
{
    QList<StubBackend::Request> out;
    for (const StubBackend::Request &req : backend.requests())
    {
        if (req.path == QLatin1String(path))
        {
            out.append(req);
        }
    }
    return out;
}

// flush() returns only once the reply has been delivered and its callback has
// run, so none of this waits on a timer.
void connectWith(Sidebar &side, StubBackend &backend, const QJsonArray &schemas)
{
    backend.replyWithResult(schemas);
    side.setConnection(QString::fromLatin1(ConnID), true);
    api()->flush(FlushMs);
}

void expandWith(Sidebar &side, StubBackend &backend, QTreeWidgetItem *item, const QJsonArray &rows)
{
    backend.replyWithResult(rows);
    tree(side)->expandItem(item);
    api()->flush(FlushMs);
}

// The state the filter and activation slots start from: two schemas, with
// shop's two tables in. Null if the schema list never arrived.
QTreeWidgetItem *loadShop(Sidebar &side, StubBackend &backend)
{
    connectWith(side, backend, twoSchemas());
    QTreeWidgetItem *shop = schemaNamed(side, QStringLiteral("shop"));
    if (!shop)
    {
        return nullptr;
    }
    expandWith(side, backend, shop, shopTables());
    return shop;
}

} // namespace

// The sidebar is how a table gets opened, so what is asserted here is the tree
// the backend's replies end up as, which rows a needle leaves standing, and the
// request an activated row turns into. The right-click menu is out of reach
// from a test: showMenu() ends in QMenu::exec().
class TestSidebar : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void init();
    void cleanup();

    void anUnconnectedSidebarAsksForNothing();
    void connectingLoadsTheSchemaList();
    void defaultSchemasAreHiddenUntilAskedFor();
    void aServerWithNoSchemasLeavesAnEmptyTree();
    void aSchemaErrorIsRaisedRatherThanReadingAsEmpty();
    void reselectingTheSameServerDoesNotRefetch();
    void switchingServersRebuildsTheTree();
    void aReloadDropsTheReplyItRacedPast();
    void expandingASchemaFetchesItsTablesOnce();
    void aTableErrorLeavesTheSchemaRetryable();
    void expandingATableFetchesItsColumns();
    void filteringOnATableNameKeepsItsSchema();
    void filteringOnASchemaNameKeepsAllOfItsTables();
    void aNeedleThatMatchesNothingHidesEveryRow();
    void tablesArrivingLaterObeyTheStandingFilter();
    void doubleClickingATableOpensItInTheEditor();
    void clickingAnAdminEntryOpensThatPanel();

private:
    StubBackend m_backend;
};

void TestSidebar::initTestCase()
{
    // Every row is built with an icon and a foreground read from the installed
    // palette, so there has to be one before the first schema lands.
    theme::apply(theme::defaultApp, FontSize);
}

void TestSidebar::init()
{
    QVERIFY(m_backend.listening());
    api()->setEndpoint(m_backend.base(), QString());
    m_backend.clearRequests();
    // Neither a schema list nor an error, so a slot only ever sees the reply it
    // set up for itself.
    m_backend.replyWithResult({});
}

void TestSidebar::cleanup()
{
    // A sidebar destroyed while a call is in flight drops its callback but not
    // its request; draining it here keeps it out of the next slot's list.
    api()->flush(FlushMs);
}

void TestSidebar::anUnconnectedSidebarAsksForNothing()
{
    Sidebar side;
    QVERIFY(tree(side));
    QCOMPARE(tree(side)->topLevelItemCount(), 0);

    // A profile that is selected but not connected, and the no-profile state:
    // either one has nothing to run a schema list on.
    side.setConnection(QString::fromLatin1(ConnID), false);
    side.setConnection(QString(), true);
    api()->flush(FlushMs);

    QVERIFY(m_backend.requests().isEmpty());
    QCOMPARE(tree(side)->topLevelItemCount(), 0);
}

void TestSidebar::connectingLoadsTheSchemaList()
{
    Sidebar side;
    connectWith(side, m_backend, twoSchemas());

    const QList<StubBackend::Request> asked = callsTo(m_backend, SchemasPath);
    QCOMPARE(asked.size(), 1);
    QCOMPARE(asked.at(0).args, QJsonArray({QString::fromLatin1(ConnID)}));

    QTreeWidget *t = tree(side);
    QCOMPARE(t->topLevelItemCount(), 2);
    QCOMPARE(t->topLevelItem(0)->text(0), QStringLiteral("shop"));
    QCOMPARE(t->topLevelItem(1)->text(0), QStringLiteral("warehouse"));

    // The tables do not come with the list: each schema gets one placeholder
    // child, which is what puts an expander on the row.
    QVERIFY(callsTo(m_backend, TablesPath).isEmpty());
    QCOMPARE(childNames(t->topLevelItem(0)), QStringList({QStringLiteral("…")}));
}

void TestSidebar::defaultSchemasAreHiddenUntilAskedFor()
{
    Sidebar side;
    connectWith(
        side, m_backend,
        {QStringLiteral("information_schema"), QStringLiteral("mysql"),
         QStringLiteral("performance_schema"), QStringLiteral("shop"), QStringLiteral("sys")}
    );

    // MySQL's own four are left out of the tree rather than greyed, and that is
    // where the preference starts.
    QCOMPARE(tree(side)->topLevelItemCount(), 1);
    QCOMPARE(tree(side)->topLevelItem(0)->text(0), QStringLiteral("shop"));

    side.setHideDefaultDBs(false);
    api()->flush(FlushMs);

    QCOMPARE(callsTo(m_backend, SchemasPath).size(), 2);
    QCOMPARE(tree(side)->topLevelItemCount(), 5);
    QCOMPARE(tree(side)->topLevelItem(0)->text(0), QStringLiteral("information_schema"));

    // Setting the preference to what it already is is not a change.
    side.setHideDefaultDBs(false);
    api()->flush(FlushMs);
    QCOMPARE(callsTo(m_backend, SchemasPath).size(), 2);
}

void TestSidebar::aServerWithNoSchemasLeavesAnEmptyTree()
{
    Sidebar side;
    QSignalSpy raised(&side, &Sidebar::errorRaised);
    connectWith(side, m_backend, {});

    // Empty because the server answered with nothing, not because nobody asked.
    QCOMPARE(callsTo(m_backend, SchemasPath).size(), 1);
    QCOMPARE(tree(side)->topLevelItemCount(), 0);
    QVERIFY(raised.isEmpty());
}

void TestSidebar::aSchemaErrorIsRaisedRatherThanReadingAsEmpty()
{
    Sidebar side;
    QSignalSpy raised(&side, &Sidebar::errorRaised);
    m_backend.replyWithError(QStringLiteral("Access denied for user 'app'"));

    side.setConnection(QString::fromLatin1(ConnID), true);
    api()->flush(FlushMs);

    // An empty tree is the truthful answer for a server with no schemas, so a
    // failure that left one behind would read as "this server has none".
    QCOMPARE(raised.size(), 1);
    QCOMPARE(raised.at(0).at(0).toString(), QStringLiteral("Access denied for user 'app'"));
    QCOMPARE(tree(side)->topLevelItemCount(), 0);
}

void TestSidebar::reselectingTheSameServerDoesNotRefetch()
{
    Sidebar side;
    connectWith(side, m_backend, twoSchemas());
    m_backend.clearRequests();

    side.setConnection(QString::fromLatin1(ConnID), true);
    api()->flush(FlushMs);

    // Clicking the server that is already selected must not throw the tree
    // away: every expanded schema would collapse under the user.
    QVERIFY(m_backend.requests().isEmpty());
    QCOMPARE(tree(side)->topLevelItemCount(), 2);

    // The refresh button's path: a schema somebody else created shows up only
    // when the tree is asked again.
    side.reload();
    api()->flush(FlushMs);
    QCOMPARE(callsTo(m_backend, SchemasPath).size(), 1);
    QCOMPARE(tree(side)->topLevelItemCount(), 2);
}

void TestSidebar::switchingServersRebuildsTheTree()
{
    Sidebar side;
    connectWith(side, m_backend, twoSchemas());
    m_backend.clearRequests();
    m_backend.replyWithResult(QJsonArray({QStringLiteral("other")}));

    side.setConnection(QString::fromLatin1(OtherConn), true);
    api()->flush(FlushMs);

    const QList<StubBackend::Request> asked = callsTo(m_backend, SchemasPath);
    QCOMPARE(asked.size(), 1);
    QCOMPARE(asked.at(0).args, QJsonArray({QString::fromLatin1(OtherConn)}));
    QCOMPARE(tree(side)->topLevelItemCount(), 1);
    QCOMPARE(tree(side)->topLevelItem(0)->text(0), QStringLiteral("other"));
}

void TestSidebar::aReloadDropsTheReplyItRacedPast()
{
    Sidebar side;
    m_backend.replyWithResult(twoSchemas());

    side.setConnection(QString::fromLatin1(ConnID), true);
    // Both calls are in flight together. The first reply belongs to a tree that
    // reload() has already thrown away, and appending it anyway would list
    // every schema twice.
    side.reload();
    api()->flush(FlushMs);

    QCOMPARE(callsTo(m_backend, SchemasPath).size(), 2);
    QCOMPARE(tree(side)->topLevelItemCount(), 2);
}

void TestSidebar::expandingASchemaFetchesItsTablesOnce()
{
    Sidebar side;
    connectWith(side, m_backend, twoSchemas());
    QTreeWidgetItem *shop = schemaNamed(side, QStringLiteral("shop"));
    QVERIFY(shop);
    m_backend.clearRequests();

    expandWith(
        side, m_backend, shop,
        {tableRow(QStringLiteral("orders"), Kind::Base),
         tableRow(QStringLiteral("order_stats"), Kind::View)}
    );

    const QList<StubBackend::Request> asked = callsTo(m_backend, TablesPath);
    QCOMPARE(asked.size(), 1);
    QCOMPARE(asked.at(0).args, QJsonArray({QString::fromLatin1(ConnID), QStringLiteral("shop")}));

    // The placeholder is replaced, and every table brings one of its own.
    QCOMPARE(
        childNames(shop), QStringList({QStringLiteral("orders"), QStringLiteral("order_stats")})
    );
    QCOMPARE(childNames(shop->child(0)), QStringList({QStringLiteral("…")}));
    QCOMPARE(shop->child(0)->toolTip(0), QStringLiteral("shop.orders"));
    QCOMPARE(shop->child(1)->toolTip(0), QStringLiteral("shop.order_stats (view)"));

    tree(side)->collapseItem(shop);
    tree(side)->expandItem(shop);
    api()->flush(FlushMs);

    // Re-expanding is free: a second fetch would cost a round trip and rebuild
    // the rows under the user's cursor.
    QCOMPARE(callsTo(m_backend, TablesPath).size(), 1);
    QCOMPARE(shop->childCount(), 2);
}

void TestSidebar::aTableErrorLeavesTheSchemaRetryable()
{
    Sidebar side;
    connectWith(side, m_backend, twoSchemas());
    QTreeWidgetItem *shop = schemaNamed(side, QStringLiteral("shop"));
    QVERIFY(shop);
    m_backend.clearRequests();
    QSignalSpy raised(&side, &Sidebar::errorRaised);

    m_backend.replyWithError(QStringLiteral("SHOW TABLES failed"));
    tree(side)->expandItem(shop);
    api()->flush(FlushMs);

    QCOMPARE(raised.size(), 1);
    QCOMPARE(raised.at(0).at(0).toString(), QStringLiteral("SHOW TABLES failed"));
    QCOMPARE(childNames(shop), QStringList({QStringLiteral("…")}));

    tree(side)->collapseItem(shop);
    expandWith(side, m_backend, shop, {tableRow(QStringLiteral("orders"), Kind::Base)});

    // A failed attempt must not count as loaded, or the schema would stay empty
    // for the rest of the session.
    QCOMPARE(callsTo(m_backend, TablesPath).size(), 2);
    QCOMPARE(childNames(shop), QStringList({QStringLiteral("orders")}));
}

void TestSidebar::expandingATableFetchesItsColumns()
{
    Sidebar side;
    QTreeWidgetItem *shop = loadShop(side, m_backend);
    QVERIFY(shop);
    QTreeWidgetItem *orders = childNamed(shop, QStringLiteral("orders"));
    QVERIFY(orders);
    m_backend.clearRequests();

    expandWith(
        side, m_backend, orders,
        {columnRow(QStringLiteral("id"), QStringLiteral("int"), QStringLiteral("PRI")),
         columnRow(QStringLiteral("total"), QStringLiteral("decimal(10,2)"))}
    );

    const QList<StubBackend::Request> asked = callsTo(m_backend, ColumnsPath);
    QCOMPARE(asked.size(), 1);
    QCOMPARE(
        asked.at(0).args,
        QJsonArray({QString::fromLatin1(ConnID), QStringLiteral("shop"), QStringLiteral("orders")})
    );
    QCOMPARE(
        childNames(orders),
        QStringList({QStringLiteral("id  int 🔑"), QStringLiteral("total  decimal(10,2)")})
    );

    // A column is a label rather than something to open, and the key marker is
    // the only thing that tells the primary key apart at a glance.
    QCOMPARE(orders->child(0)->flags(), Qt::ItemFlags(Qt::ItemIsEnabled));
}

void TestSidebar::filteringOnATableNameKeepsItsSchema()
{
    Sidebar side;
    QTreeWidgetItem *shop = loadShop(side, m_backend);
    QVERIFY(shop);
    QTreeWidgetItem *orders = childNamed(shop, QStringLiteral("orders"));
    QTreeWidgetItem *customers = childNamed(shop, QStringLiteral("customers"));
    QTreeWidgetItem *warehouse = schemaNamed(side, QStringLiteral("warehouse"));
    QVERIFY(orders);
    QVERIFY(customers);
    QVERIFY(warehouse);
    QLineEdit *needle = filterEdit(side);
    QVERIFY(needle);

    needle->setText(QStringLiteral("  ord  "));

    // The needle is trimmed before it is matched, and a schema stays because a
    // row under it matched, not because the schema did.
    QVERIFY(!shop->isHidden());
    QVERIFY(!orders->isHidden());
    QVERIFY(customers->isHidden());

    // warehouse matches neither by name nor through a child: its tables were
    // never fetched, so it holds nothing that could match.
    QVERIFY(warehouse->isHidden());
}

void TestSidebar::filteringOnASchemaNameKeepsAllOfItsTables()
{
    Sidebar side;
    QTreeWidgetItem *shop = loadShop(side, m_backend);
    QVERIFY(shop);
    QTreeWidgetItem *orders = childNamed(shop, QStringLiteral("orders"));
    QTreeWidgetItem *customers = childNamed(shop, QStringLiteral("customers"));
    QTreeWidgetItem *warehouse = schemaNamed(side, QStringLiteral("warehouse"));
    QVERIFY(orders);
    QVERIFY(customers);
    QVERIFY(warehouse);
    QLineEdit *needle = filterEdit(side);
    QVERIFY(needle);

    needle->setText(QStringLiteral("SHOP"));

    // Matching is case-insensitive, and a schema hit carries the whole table
    // list with it: the user asked for the schema, not for one table in it.
    QVERIFY(!shop->isHidden());
    QVERIFY(!orders->isHidden());
    QVERIFY(!customers->isHidden());
    QVERIFY(warehouse->isHidden());
}

void TestSidebar::aNeedleThatMatchesNothingHidesEveryRow()
{
    Sidebar side;
    QTreeWidgetItem *shop = loadShop(side, m_backend);
    QVERIFY(shop);
    QTreeWidgetItem *orders = childNamed(shop, QStringLiteral("orders"));
    QTreeWidgetItem *customers = childNamed(shop, QStringLiteral("customers"));
    QTreeWidgetItem *warehouse = schemaNamed(side, QStringLiteral("warehouse"));
    QVERIFY(orders);
    QVERIFY(customers);
    QVERIFY(warehouse);
    QLineEdit *needle = filterEdit(side);
    QVERIFY(needle);

    needle->setText(QStringLiteral("zzz"));

    QVERIFY(shop->isHidden());
    QVERIFY(orders->isHidden());
    QVERIFY(customers->isHidden());
    QVERIFY(warehouse->isHidden());

    // Clearing brings back every row the needle hid, without refetching any of
    // them.
    m_backend.clearRequests();
    needle->clear();

    QVERIFY(!shop->isHidden());
    QVERIFY(!orders->isHidden());
    QVERIFY(!customers->isHidden());
    QVERIFY(!warehouse->isHidden());
    QVERIFY(m_backend.requests().isEmpty());
}

void TestSidebar::tablesArrivingLaterObeyTheStandingFilter()
{
    Sidebar side;
    connectWith(side, m_backend, twoSchemas());
    QTreeWidgetItem *shop = schemaNamed(side, QStringLiteral("shop"));
    QVERIFY(shop);
    QLineEdit *needle = filterEdit(side);
    QVERIFY(needle);

    needle->setText(QStringLiteral("ord"));
    // Nothing is loaded under shop yet, so there is nothing there to match.
    QVERIFY(shop->isHidden());

    expandWith(side, m_backend, shop, shopTables());

    // The rows are filtered as they land rather than showing the whole schema
    // until the next keystroke.
    QVERIFY(!shop->isHidden());
    QTreeWidgetItem *orders = childNamed(shop, QStringLiteral("orders"));
    QTreeWidgetItem *customers = childNamed(shop, QStringLiteral("customers"));
    QVERIFY(orders);
    QVERIFY(customers);
    QVERIFY(!orders->isHidden());
    QVERIFY(customers->isHidden());
}

void TestSidebar::doubleClickingATableOpensItInTheEditor()
{
    Sidebar side;
    QTreeWidgetItem *shop = loadShop(side, m_backend);
    QVERIFY(shop);
    QTreeWidgetItem *orders = childNamed(shop, QStringLiteral("orders"));
    QVERIFY(orders);
    QSignalSpy requested(&side, &Sidebar::tabRequested);

    // Stands in for the double click: the tree turns one into this.
    emit tree(side)->itemDoubleClicked(orders, 0);

    QCOMPARE(requested.size(), 1);
    const TabRequest req = requested.at(0).at(0).value<TabRequest>();
    QCOMPARE(req.view, TabView::Editor);
    QCOMPARE(req.sql, QStringLiteral("SELECT * FROM `shop`.`orders` LIMIT 200;"));
    // The target rides in the SQL and nowhere else, so a tab opened from here
    // is an editor on the table rather than the table inspector.
    QVERIFY(req.schema.isEmpty());
    QVERIFY(req.table.isEmpty());
    QVERIFY(req.section.isEmpty());

    QTreeWidgetItem *warehouse = schemaNamed(side, QStringLiteral("warehouse"));
    QVERIFY(warehouse);
    // Neither a schema row nor the placeholder under an unopened one is a
    // table, and the placeholder carries no schema to name in a query.
    emit tree(side)->itemDoubleClicked(shop, 0);
    emit tree(side)->itemDoubleClicked(warehouse->child(0), 0);

    QCOMPARE(requested.size(), 1);
}

void TestSidebar::clickingAnAdminEntryOpensThatPanel()
{
    Sidebar side;
    QListWidget *pages = adminList(side);
    QVERIFY(pages);
    QSignalSpy requested(&side, &Sidebar::tabRequested);

    QCOMPARE(pages->item(0)->text(), QStringLiteral("Dashboard"));
    emit pages->itemClicked(pages->item(0));

    QCOMPARE(requested.size(), 1);
    QCOMPARE(requested.at(0).at(0).value<TabRequest>().view, TabView::Dashboard);

    // Label and view are written side by side in one table, where swapping two
    // of them puts the wrong panel behind the right name.
    const QList<QListWidgetItem *> conns =
        pages->findItems(QStringLiteral("Client Connections"), Qt::MatchExactly);
    QCOMPARE(conns.size(), 1);
    emit pages->itemClicked(conns.at(0));

    QCOMPARE(requested.size(), 2);
    const TabRequest req = requested.at(1).at(0).value<TabRequest>();
    QCOMPARE(req.view, TabView::Processlist);
    QVERIFY(req.schema.isEmpty());
    QVERIFY(req.sql.isEmpty());
}

QTEST_MAIN(TestSidebar)

#include "tst_sidebar.moc"
