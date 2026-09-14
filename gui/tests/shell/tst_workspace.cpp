#include "shell/workspace.h"

#include "app/theme.h"
#include "editor/editortab.h"
#include "shell/tabs.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QObject>
#include <QSet>
#include <QString>
#include <QTest>

// workspace.json version 2 predates this client and the backend stores it
// verbatim, so the key names below are the contract, not an implementation
// detail: a renamed or dropped key silently loses somebody's tabs.
namespace
{

constexpr auto Conn = "c1";
constexpr auto OtherConn = "c2";

Tab editorTab(const QString &tabID, const QString &sql)
{
    Tab t;
    t.tabID = tabID;
    t.connID = QString::fromLatin1(Conn);
    t.title = QStringLiteral("Query");
    t.view = TabView::Editor;
    t.sql = sql;
    return t;
}

QJsonObject firstTab(const QJsonObject &ws)
{
    return ws.value(QStringLiteral("tabs")).toArray().at(0).toObject();
}

QJsonObject encodeOne(const Tab &t, int sidebarWidth = 240, const QJsonObject &prefs = {})
{
    return encodeWorkspace({t}, QString::fromLatin1(Conn), {}, prefs, sidebarWidth);
}

} // namespace

class TestWorkspace : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();

    void encodeStampsTheFormatVersion();
    void emptyFieldsAreLeftOutEntirely();
    void onlyEditorTabsCarryTheirSql();
    void aLiveEditorIsAskedForItsCurrentSql();
    void aDormantTabFallsBackToItsStoredSql();
    void activeConnIsNullWhenThereIsNone();
    void aHiddenSidebarCarriesTheStoredWidthForward();

    void decodeDropsTabsForConnectionsThatAreGone();
    void decodeDefaultsTheSidebarWidth();
    void maxTabSeqTracksTheHighestNumberedTab();
    void aRoundTripPreservesEveryTabField();
};

void TestWorkspace::initTestCase()
{
    theme::apply(QString::fromLatin1(theme::defaultApp), 13);
}

void TestWorkspace::encodeStampsTheFormatVersion()
{
    const QJsonObject ws = encodeOne(editorTab(QStringLiteral("t1"), QStringLiteral("select 1")));

    QCOMPARE(ws.value(QStringLiteral("version")).toInt(), 2);
    QVERIFY(ws.contains(QStringLiteral("tabs")));
    QVERIFY(ws.contains(QStringLiteral("activePerConn")));
    QVERIFY(ws.contains(QStringLiteral("prefs")));
}

void TestWorkspace::emptyFieldsAreLeftOutEntirely()
{
    Tab t = editorTab(QStringLiteral("t1"), QString());
    t.titled = false;
    const QJsonObject o = firstTab(encodeOne(t));

    // Absent rather than empty: the backend stores what it is handed, and an
    // older client reading these back treats a present empty string as a value.
    QVERIFY(!o.contains(QStringLiteral("titled")));
    QVERIFY(!o.contains(QStringLiteral("schema")));
    QVERIFY(!o.contains(QStringLiteral("table")));
    QVERIFY(!o.contains(QStringLiteral("section")));
    QVERIFY(!o.contains(QStringLiteral("editorH")));

    t.titled = true;
    t.schema = QStringLiteral("shop");
    t.table = QStringLiteral("orders");
    t.section = QStringLiteral("columns");
    t.editorH = 120;
    const QJsonObject full = firstTab(encodeOne(t));

    QCOMPARE(full.value(QStringLiteral("titled")).toBool(), true);
    QCOMPARE(full.value(QStringLiteral("schema")).toString(), QStringLiteral("shop"));
    QCOMPARE(full.value(QStringLiteral("table")).toString(), QStringLiteral("orders"));
    QCOMPARE(full.value(QStringLiteral("section")).toString(), QStringLiteral("columns"));
    QCOMPARE(full.value(QStringLiteral("editorH")).toInt(), 120);
}

void TestWorkspace::onlyEditorTabsCarryTheirSql()
{
    Tab panel = editorTab(QStringLiteral("t1"), QStringLiteral("select 1"));
    panel.view = TabView::Dashboard;
    panel.editorH = 120;

    const QJsonObject o = firstTab(encodeOne(panel));
    QCOMPARE(o.value(QStringLiteral("view")).toString(), viewId(TabView::Dashboard));
    QVERIFY(!o.contains(QStringLiteral("sql")));
    QVERIFY(!o.contains(QStringLiteral("editorH")));
}

void TestWorkspace::aLiveEditorIsAskedForItsCurrentSql()
{
    // The snapshot has to reflect what is on screen. Reading the last synced
    // copy instead would drop whatever was typed since, which on the way out
    // means losing it for good.
    EditorTab live(
        QString::fromLatin1(Conn), QStringLiteral("t1"), QStringLiteral("select typed_just_now")
    );
    live.setEditorHeight(321);

    Tab t = editorTab(QStringLiteral("t1"), QStringLiteral("select stale_copy"));
    t.editorH = 99;
    t.widget = &live;

    const QJsonObject o = firstTab(encodeOne(t));
    QCOMPARE(o.value(QStringLiteral("sql")).toString(), live.sql());
    QVERIFY(o.value(QStringLiteral("sql")).toString() != QStringLiteral("select stale_copy"));
    QCOMPARE(o.value(QStringLiteral("editorH")).toInt(), live.editorHeight());
}

void TestWorkspace::aDormantTabFallsBackToItsStoredSql()
{
    // Restored but never opened: no widget exists yet, so the stored copy is
    // the only truth there is.
    Tab t = editorTab(QStringLiteral("t1"), QStringLiteral("select stored"));
    t.editorH = 150;
    QVERIFY(t.widget == nullptr);

    const QJsonObject o = firstTab(encodeOne(t));
    QCOMPARE(o.value(QStringLiteral("sql")).toString(), QStringLiteral("select stored"));
    QCOMPARE(o.value(QStringLiteral("editorH")).toInt(), 150);
}

void TestWorkspace::activeConnIsNullWhenThereIsNone()
{
    const QJsonObject ws = encodeWorkspace({}, QString(), {}, {}, 240);
    QVERIFY(ws.value(QStringLiteral("activeConn")).isNull());

    const QJsonObject named = encodeWorkspace({}, QString::fromLatin1(Conn), {}, {}, 240);
    QCOMPARE(named.value(QStringLiteral("activeConn")).toString(), QString::fromLatin1(Conn));
}

void TestWorkspace::aHiddenSidebarCarriesTheStoredWidthForward()
{
    QJsonObject prefs;
    prefs.insert(QStringLiteral("sidebarWidth"), 310);

    // Hidden means "no width to report", not "width zero": dropping it would
    // reopen the sidebar at the default next time.
    const QJsonObject hidden = encodeWorkspace({}, QString(), {}, prefs, 0);
    QCOMPARE(hidden.value(QStringLiteral("sidebarWidth")).toInt(), 310);

    const QJsonObject shown = encodeWorkspace({}, QString(), {}, prefs, 280);
    QCOMPARE(shown.value(QStringLiteral("sidebarWidth")).toInt(), 280);

    // Nothing to carry forward and nothing to report: the key stays out.
    const QJsonObject neither = encodeWorkspace({}, QString(), {}, {}, 0);
    QVERIFY(!neither.contains(QStringLiteral("sidebarWidth")));
}

void TestWorkspace::decodeDropsTabsForConnectionsThatAreGone()
{
    Tab mine = editorTab(QStringLiteral("t1"), QStringLiteral("select 1"));
    Tab theirs = editorTab(QStringLiteral("t2"), QStringLiteral("select 2"));
    theirs.connID = QString::fromLatin1(OtherConn);

    const QJsonObject ws = encodeWorkspace({mine, theirs}, QString::fromLatin1(Conn), {}, {}, 240);

    // The saved profile is gone, so that tab could never materialise.
    const WorkspaceState st = decodeWorkspace(ws, {QString::fromLatin1(Conn)});
    QCOMPARE(st.tabs.size(), 1);
    QCOMPARE(st.tabs.at(0).connID, QString::fromLatin1(Conn));

    QCOMPARE(decodeWorkspace(ws, {}).tabs.size(), 0);
}

void TestWorkspace::decodeDefaultsTheSidebarWidth()
{
    QCOMPARE(decodeWorkspace({}, {}).sidebarWidth, 240);

    QJsonObject ws;
    ws.insert(QStringLiteral("sidebarWidth"), 300);
    QCOMPARE(decodeWorkspace(ws, {}).sidebarWidth, 300);
}

void TestWorkspace::maxTabSeqTracksTheHighestNumberedTab()
{
    // New ids start above whatever the restored set already used, so a reused
    // id would collide with a tab that is already on screen.
    QVector<Tab> tabs{
        editorTab(QStringLiteral("t3"), QString()),
        editorTab(QStringLiteral("t17"), QString()),
        editorTab(QStringLiteral("t9"), QString()),
    };
    const QJsonObject ws = encodeWorkspace(tabs, QString::fromLatin1(Conn), {}, {}, 240);

    const WorkspaceState st = decodeWorkspace(ws, {QString::fromLatin1(Conn)});
    QCOMPARE(st.maxTabSeq, 17);

    // An id that carries no number contributes nothing rather than breaking.
    const QJsonObject odd = encodeWorkspace(
        {editorTab(QStringLiteral("panel"), QString())}, QString::fromLatin1(Conn), {}, {}, 240
    );
    QCOMPARE(decodeWorkspace(odd, {QString::fromLatin1(Conn)}).maxTabSeq, 0);
}

void TestWorkspace::aRoundTripPreservesEveryTabField()
{
    Tab t = editorTab(QStringLiteral("t42"), QStringLiteral("select * from shop.orders"));
    t.title = QStringLiteral("Orders");
    t.titled = true;
    t.schema = QStringLiteral("shop");
    t.table = QStringLiteral("orders");
    t.editorH = 180;

    QHash<QString, QString> active;
    active.insert(QString::fromLatin1(Conn), QStringLiteral("t42"));

    const WorkspaceState st = decodeWorkspace(
        encodeWorkspace({t}, QString::fromLatin1(Conn), active, {}, 240),
        {QString::fromLatin1(Conn)}
    );

    QCOMPARE(st.tabs.size(), 1);
    const Tab &back = st.tabs.at(0);
    QCOMPARE(back.tabID, t.tabID);
    QCOMPARE(back.connID, t.connID);
    QCOMPARE(back.title, t.title);
    QCOMPARE(back.titled, t.titled);
    QCOMPARE(back.view, t.view);
    QCOMPARE(back.schema, t.schema);
    QCOMPARE(back.table, t.table);
    QCOMPARE(back.sql, t.sql);
    QCOMPARE(back.editorH, t.editorH);
    QCOMPARE(st.activePerConn.value(QString::fromLatin1(Conn)), QStringLiteral("t42"));
}

QTEST_MAIN(TestWorkspace)

#include "tst_workspace.moc"
