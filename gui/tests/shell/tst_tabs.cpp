#include "shell/tabs.h"

#include <QObject>
#include <QString>
#include <QTest>

// quoteIdent feeds generated SQL and viewId/viewFromId are the workspace.json
// (version 2) spelling of a tab, so both are covered as rows rather than a run
// of QCOMPAREs: a QCOMPARE failure returns from the slot, which would let one
// bad input hide every input after it.
class TestTabs : public QObject
{
    Q_OBJECT

private slots:
    void quoteIdentDoublesEveryBacktick_data();
    void quoteIdentDoublesEveryBacktick();
    void quoteIdentLeavesEverythingElseAlone_data();
    void quoteIdentLeavesEverythingElseAlone();

    void viewFromIdReadsEveryStoredId_data();
    void viewFromIdReadsEveryStoredId();
    void viewIdSpellsEachViewAndRoundTrips_data();
    void viewIdSpellsEachViewAndRoundTrips();
    void singletonViewsAreThePanelsPerConnection_data();
    void singletonViewsAreThePanelsPerConnection();

    void tabRequestDefaultsToAnEmptyEditor();
    void tabStartsUntitledAndWithoutAWidget();
};

namespace
{

// The property that makes the quoting safe: inside the delimiters a backtick
// only ever appears in an even-length run, so nothing in the body can close the
// identifier early and continue as statement text.
void verifyBacktickRunsAreEven(const QString &quoted)
{
    QVERIFY(quoted.size() >= 2);
    QVERIFY(quoted.startsWith(QChar('`')));
    QVERIFY(quoted.endsWith(QChar('`')));

    const QString body = quoted.mid(1, quoted.size() - 2);
    int at = 0;
    while (at < body.size())
    {
        if (body.at(at) != QChar('`'))
        {
            ++at;
            continue;
        }

        int run = 0;
        while (at < body.size() && body.at(at) == QChar('`'))
        {
            ++run;
            ++at;
        }
        QCOMPARE(run % 2, 0);
    }
}

} // namespace

void TestTabs::quoteIdentDoublesEveryBacktick_data()
{
    QTest::addColumn<QString>("ident");
    QTest::addColumn<QString>("quoted");

    QTest::newRow("plain") << QStringLiteral("users") << QStringLiteral("`users`");
    QTest::newRow("empty") << QString() << QStringLiteral("``");

    QTest::newRow("one backtick") << QStringLiteral("a`b") << QStringLiteral("`a``b`");
    QTest::newRow("several backticks") << QStringLiteral("a`b`c") << QStringLiteral("`a``b``c`");
    QTest::newRow("leading backtick") << QStringLiteral("`x") << QStringLiteral("```x`");
    QTest::newRow("trailing backtick") << QStringLiteral("x`") << QStringLiteral("`x```");

    // An already doubled pair is doubled again: quoting is not idempotent, and
    // feeding a quoted name back in is a caller bug, not an escape hatch.
    QTest::newRow("adjacent pair") << QStringLiteral("a``b") << QStringLiteral("`a````b`");

    QTest::newRow("nothing but one backtick") << QStringLiteral("`") << QStringLiteral("````");
    QTest::newRow("nothing but a pair") << QStringLiteral("``") << QStringLiteral("``````");
    QTest::newRow("nothing but three") << QStringLiteral("```") << QStringLiteral("````````");

    // The shape that matters: a name crafted to close the identifier and append
    // its own SQL stays one identifier.
    QTest::newRow("closing attempt")
        << QStringLiteral("t` OR 1=1 -- ") << QStringLiteral("`t`` OR 1=1 -- `");
}

void TestTabs::quoteIdentDoublesEveryBacktick()
{
    QFETCH(QString, ident);
    QFETCH(QString, quoted);

    const QString actual = quoteIdent(ident);
    QCOMPARE(actual, quoted);
    verifyBacktickRunsAreEven(actual);
}

void TestTabs::quoteIdentLeavesEverythingElseAlone_data()
{
    QTest::addColumn<QString>("ident");
    QTest::addColumn<QString>("quoted");

    // Quoting is not qualification: a dot is part of this one name, so it must
    // not be split into schema and table.
    QTest::newRow("dot") << QStringLiteral("mydb.users") << QStringLiteral("`mydb.users`");

    QTest::newRow("space") << QStringLiteral("my table") << QStringLiteral("`my table`");
    QTest::newRow("single quote") << QStringLiteral("it's") << QStringLiteral("`it's`");
    QTest::newRow("double quote") << QStringLiteral("a\"b") << QStringLiteral("`a\"b`");

    // A backslash is literal inside a backquoted identifier, so doubling it
    // would change the name.
    QTest::newRow("backslash") << QStringLiteral("a\\b") << QStringLiteral("`a\\b`");

    QTest::newRow("semicolon") << QStringLiteral("a;b") << QStringLiteral("`a;b`");
}

void TestTabs::quoteIdentLeavesEverythingElseAlone()
{
    QFETCH(QString, ident);
    QFETCH(QString, quoted);

    const QString actual = quoteIdent(ident);
    QCOMPARE(actual, quoted);
    verifyBacktickRunsAreEven(actual);
}

void TestTabs::viewFromIdReadsEveryStoredId_data()
{
    QTest::addColumn<QString>("id");
    QTest::addColumn<TabView>("view");

    QTest::newRow("editor") << QStringLiteral("editor") << TabView::Editor;
    QTest::newRow("dashboard") << QStringLiteral("dashboard") << TabView::Dashboard;
    QTest::newRow("processlist") << QStringLiteral("processlist") << TabView::Processlist;
    QTest::newRow("users") << QStringLiteral("users") << TabView::Users;
    QTest::newRow("serverinfo") << QStringLiteral("serverinfo") << TabView::ServerInfo;
    QTest::newRow("innodb") << QStringLiteral("innodb") << TabView::InnoDB;
    QTest::newRow("tableinspect") << QStringLiteral("tableinspect") << TabView::TableInspect;
    QTest::newRow("schemainspect") << QStringLiteral("schemainspect") << TabView::SchemaInspect;
    QTest::newRow("graph") << QStringLiteral("graph") << TabView::Graph;
    QTest::newRow("history") << QStringLiteral("history") << TabView::History;

    // Anything unrecognised restores as an editor rather than as no tab at all,
    // which is what a workspace written by a newer version arrives as.
    QTest::newRow("unknown") << QStringLiteral("nope") << TabView::Editor;
    QTest::newRow("empty") << QString() << TabView::Editor;

    // The ids are compared exactly, so only viewId's own spelling is accepted.
    QTest::newRow("wrong case") << QStringLiteral("Dashboard") << TabView::Editor;
    QTest::newRow("padded") << QStringLiteral(" dashboard") << TabView::Editor;
}

void TestTabs::viewFromIdReadsEveryStoredId()
{
    QFETCH(QString, id);
    QFETCH(TabView, view);
    QCOMPARE(viewFromId(id), view);
}

void TestTabs::viewIdSpellsEachViewAndRoundTrips_data()
{
    QTest::addColumn<TabView>("view");
    QTest::addColumn<QString>("id");

    QTest::newRow("editor") << TabView::Editor << QStringLiteral("editor");
    QTest::newRow("dashboard") << TabView::Dashboard << QStringLiteral("dashboard");
    QTest::newRow("processlist") << TabView::Processlist << QStringLiteral("processlist");
    QTest::newRow("users") << TabView::Users << QStringLiteral("users");
    QTest::newRow("serverinfo") << TabView::ServerInfo << QStringLiteral("serverinfo");
    QTest::newRow("innodb") << TabView::InnoDB << QStringLiteral("innodb");
    QTest::newRow("tableinspect") << TabView::TableInspect << QStringLiteral("tableinspect");
    QTest::newRow("schemainspect") << TabView::SchemaInspect << QStringLiteral("schemainspect");
    QTest::newRow("graph") << TabView::Graph << QStringLiteral("graph");
    QTest::newRow("history") << TabView::History << QStringLiteral("history");
}

void TestTabs::viewIdSpellsEachViewAndRoundTrips()
{
    QFETCH(TabView, view);
    QFETCH(QString, id);

    QCOMPARE(viewId(view), id);
    QCOMPARE(viewFromId(viewId(view)), view);
}

void TestTabs::singletonViewsAreThePanelsPerConnection_data()
{
    QTest::addColumn<TabView>("view");
    QTest::addColumn<bool>("singleton");

    QTest::newRow("dashboard") << TabView::Dashboard << true;
    QTest::newRow("processlist") << TabView::Processlist << true;
    QTest::newRow("users") << TabView::Users << true;
    QTest::newRow("serverinfo") << TabView::ServerInfo << true;
    QTest::newRow("innodb") << TabView::InnoDB << true;
    QTest::newRow("graph") << TabView::Graph << true;
    QTest::newRow("history") << TabView::History << true;

    // The views that carry a target: one per schema or table, and any number of
    // editors.
    QTest::newRow("editor") << TabView::Editor << false;
    QTest::newRow("tableinspect") << TabView::TableInspect << false;
    QTest::newRow("schemainspect") << TabView::SchemaInspect << false;
}

void TestTabs::singletonViewsAreThePanelsPerConnection()
{
    QFETCH(TabView, view);
    QFETCH(bool, singleton);
    QCOMPARE(isSingletonView(view), singleton);
}

void TestTabs::tabRequestDefaultsToAnEmptyEditor()
{
    const TabRequest request;

    QCOMPARE(request.view, TabView::Editor);
    QVERIFY(request.schema.isEmpty());
    QVERIFY(request.table.isEmpty());
    QVERIFY(request.section.isEmpty());
    QVERIFY(request.sql.isEmpty());
}

void TestTabs::tabStartsUntitledAndWithoutAWidget()
{
    const Tab tab;

    // Restore depends on all four: the tab is dormant until its connection
    // opens, and an untitled tab has its title re-derived rather than kept.
    QCOMPARE(tab.widget, nullptr);
    QCOMPARE(tab.titled, false);
    QCOMPARE(tab.view, TabView::Editor);
    QCOMPARE(tab.editorH, 0);

    QVERIFY(tab.tabID.isEmpty());
    QVERIFY(tab.connID.isEmpty());
    QVERIFY(tab.title.isEmpty());
    QVERIFY(tab.schema.isEmpty());
    QVERIFY(tab.table.isEmpty());
    QVERIFY(tab.section.isEmpty());
    QVERIFY(tab.sql.isEmpty());
}

QTEST_APPLESS_MAIN(TestTabs)

#include "tst_tabs.moc"
