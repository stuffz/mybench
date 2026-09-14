#include "shell/mainwindow.h"

#include "app/api.h"
#include "app/stubbackend.h"
#include "app/theme.h"
#include "editor/editortab.h"
#include "editor/sqleditor.h"
#include "shell/servertabbar.h"
#include "shell/sidebar.h"
#include "shell/statusstrip.h"
#include "shell/tabs.h"
#include "views/graphcanvas.h"
#include "views/graphview.h"

#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QByteArray>
#include <QCoreApplication>
#include <QEvent>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QKeySequence>
#include <QLatin1String>
#include <QList>
#include <QMenu>
#include <QMetaObject>
#include <QMouseEvent>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QObject>
#include <QPoint>
#include <QShortcut>
#include <QSplitter>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QString>
#include <QStringList>
#include <QTabBar>
#include <QTabWidget>
#include <QTest>
#include <QTimer>
#include <QWidget>
#include <functional>

namespace
{

constexpr auto Conn = "c1";
constexpr auto OtherConn = "c2";

constexpr auto ListPath = "/rpc/conn/List";
constexpr auto ClosePath = "/rpc/conn/Close";
constexpr auto LoadPath = "/rpc/workspace/Load";
constexpr auto SavePath = "/rpc/workspace/Save";

constexpr int BaseFontPx = 13;
// Any other slider value: applyTheme() re-derives every font-sized piece of
// chrome from it, which is what rebuilds the tab close buttons.
constexpr int BiggerFontPx = 17;

// The window's save coalescing interval. Saves are fired by hand here, so this
// only tells the save timer apart from the geometry one beside it.
constexpr int SaveCoalesceMs = 50;

constexpr int FlushMs = 2000;

bool waitUntil(const std::function<bool()> &done)
{
    for (int waited = 0; waited < 5000; waited += 10)
    {
        if (done())
        {
            return true;
        }
        QTest::qWait(10);
    }
    return false;
}

// The calls that went to one RPC method. A window with tabs open polls status,
// panels and schemas the whole time, so every assertion picks out the
// conversation it is about.
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

// A saved profile as conn.List hands one over. Only the id carries weight: it
// is what a restored tab's connection is matched against.
QJsonObject savedConn(const char *id)
{
    QJsonObject c;
    c.insert(QStringLiteral("id"), QString::fromLatin1(id));
    c.insert(QStringLiteral("name"), QString::fromLatin1(id).toUpper());
    return c;
}

// The sidebar | panes splitter, which is the one widget both halves hang off.
// Found by position rather than by type: an open editor tab carries a splitter
// of its own, and the sidebar carries a stack of its own.
QSplitter *splitOf(const MainWindow &mw)
{
    QWidget *central = mw.centralWidget();
    if (!central)
    {
        return nullptr;
    }
    return central->findChildren<QSplitter *>(QString(), Qt::FindDirectChildrenOnly).value(0);
}

QStackedWidget *stackOf(const MainWindow &mw)
{
    QSplitter *split = splitOf(mw);
    return split ? qobject_cast<QStackedWidget *>(split->widget(1)) : nullptr;
}

Sidebar *sidebarOf(const MainWindow &mw)
{
    QSplitter *split = splitOf(mw);
    return split ? qobject_cast<Sidebar *>(split->widget(0)) : nullptr;
}

ServerTabBar *serverTabsOf(const MainWindow &mw)
{
    return mw.findChild<ServerTabBar *>();
}

StatusStrip *statusStripOf(const MainWindow &mw)
{
    return mw.findChild<StatusStrip *>();
}

// The query tab strip of one connection, or nullptr while that connection has
// no tabs. The pane carries its connID as a property, which is also how the
// window itself gets from a tab bar back to the connection it belongs to.
QTabWidget *paneOf(const MainWindow &mw, const char *connID)
{
    QStackedWidget *stack = stackOf(mw);
    if (!stack)
    {
        return nullptr;
    }
    for (int i = 0; i < stack->count(); ++i)
    {
        auto *pane = qobject_cast<QTabWidget *>(stack->widget(i));
        if (pane && pane->property("connID").toString() == QLatin1String(connID))
        {
            return pane;
        }
    }
    return nullptr;
}

QAbstractButton *closeButton(QTabWidget *pane, int index)
{
    return qobject_cast<QAbstractButton *>(pane->tabBar()->tabButton(index, QTabBar::RightSide));
}

QShortcut *shortcutFor(const MainWindow &mw, const QKeySequence &keys)
{
    for (QShortcut *sc : mw.findChildren<QShortcut *>())
    {
        if (sc->key() == keys)
        {
            return sc;
        }
    }
    return nullptr;
}

// Both window timers hang off the window itself; the interval is what tells
// them apart.
QTimer *saveTimerOf(const MainWindow &mw)
{
    for (QTimer *t : mw.findChildren<QTimer *>(QString(), Qt::FindDirectChildrenOnly))
    {
        if (t->interval() == SaveCoalesceMs)
        {
            return t;
        }
    }
    return nullptr;
}

// Fires the coalesced save now instead of waiting the interval out. The timer
// is stopped first so it cannot fire a second time inside a later wait, which
// would make "one change, one save" unprovable.
void fireSave(const MainWindow &mw)
{
    QTimer *timer = saveTimerOf(mw);
    QVERIFY(timer);
    timer->stop();
    QMetaObject::invokeMethod(timer, "timeout");
    // The save is a fire-and-forget POST: it only reaches the stub once the
    // loop has run.
    api()->flush(FlushMs);
}

QJsonObject lastSave(const StubBackend &backend)
{
    const QList<StubBackend::Request> saves = callsTo(backend, SavePath);
    if (saves.isEmpty())
    {
        return {};
    }
    return QJsonDocument::fromJson(saves.last().args.at(0).toString().toUtf8()).object();
}

QJsonObject savedTab(const QJsonObject &ws, int index)
{
    return ws.value(QStringLiteral("tabs")).toArray().at(index).toObject();
}

// Saves are suppressed until the stored workspace has been read back, so a
// test that looks at one has to let the window do that first.
//
// The stub answers every request with the same canned body, and the workspace
// blob is a string where the profile list is an array: the Load here comes
// back as the profile array, which is not a workspace. That is enough — the
// window records that the store has been read (which is what unblocks saving)
// before it looks at what came back, and keeps its empty tab tree.
bool restoreEmptyWorkspace(MainWindow &mw, StubBackend &backend)
{
    backend.replyWithResult(QJsonArray{savedConn(Conn), savedConn(OtherConn)});
    mw.onBackendReady();
    const bool read = waitUntil([&backend] { return !callsTo(backend, LoadPath).isEmpty(); });
    api()->flush(FlushMs);
    backend.clearRequests();
    return read;
}

// The two calls behind onBackendReady() want different shapes back: conn.List
// an array of profiles, the workspace.Load behind it the stored blob as a
// string. The stub has one canned body, so the second answer is armed here.
//
// The manager's finished signal is connected when the reply is created —
// before Api connects its own handler to that same signal — so this runs after
// the List reply has arrived and before the Load request it triggers goes out.
// `scope` bounds the connection: the manager belongs to the Api singleton and
// outlives the test object.
void answerLoadWith(StubBackend &backend, const QJsonObject &ws, QObject &scope)
{
    auto *net = api()->findChild<QNetworkAccessManager *>();
    QVERIFY(net);
    QObject::connect(
        net, &QNetworkAccessManager::finished, &scope,
        [&backend, ws](QNetworkReply *reply)
        {
            if (reply->url().path() != QLatin1String(ListPath))
            {
                return;
            }
            backend.replyWithResult(
                QString::fromUtf8(QJsonDocument(ws).toJson(QJsonDocument::Compact))
            );
        }
    );
}

// A stored workspace: two tabs on one connection, the highest id t9. The first
// carries a stale title without the titled flag, the second a name the user
// typed.
QJsonObject storedWorkspace()
{
    QJsonObject untitled;
    untitled.insert(QStringLiteral("tabID"), QStringLiteral("t3"));
    untitled.insert(QStringLiteral("connID"), QString::fromLatin1(Conn));
    untitled.insert(QStringLiteral("title"), QStringLiteral("Query 7"));
    untitled.insert(QStringLiteral("view"), QStringLiteral("editor"));
    untitled.insert(QStringLiteral("sql"), QStringLiteral("select 1"));

    QJsonObject named;
    named.insert(QStringLiteral("tabID"), QStringLiteral("t9"));
    named.insert(QStringLiteral("connID"), QString::fromLatin1(Conn));
    named.insert(QStringLiteral("title"), QStringLiteral("Scratch"));
    named.insert(QStringLiteral("titled"), true);
    named.insert(QStringLiteral("view"), QStringLiteral("editor"));

    QJsonObject active;
    active.insert(QString::fromLatin1(Conn), QStringLiteral("t9"));

    QJsonObject prefs;
    prefs.insert(QStringLiteral("defaultRowLimit"), 100);

    QJsonObject ws;
    ws.insert(QStringLiteral("version"), 2);
    ws.insert(QStringLiteral("tabs"), QJsonArray{untitled, named});
    ws.insert(QStringLiteral("activeConn"), QString::fromLatin1(Conn));
    ws.insert(QStringLiteral("activePerConn"), active);
    ws.insert(QStringLiteral("prefs"), prefs);
    return ws;
}

// showTabContextMenu builds its menu on the stack and blocks in exec(), so the
// menu can only be read from inside that loop. The menu is a direct child of
// the window, which is what separates it from any menu a view puts up.
void inTabMenu(
    const MainWindow &mw, QTabWidget *pane, int index, const std::function<void(QMenu *)> &then
)
{
    QTimer::singleShot(
        0, pane,
        [&mw, then]()
        {
            QMenu *menu = mw.findChildren<QMenu *>(QString(), Qt::FindDirectChildrenOnly).value(0);
            if (!menu)
            {
                return;
            }
            then(menu);
            menu->close();
        }
    );
    QTabBar *bar = pane->tabBar();
    emit bar->customContextMenuRequested(bar->tabRect(index).center());
}

// What the menu offers on that tab, leaving out the entries it greys out: an
// action that is there but disabled is not on offer.
QStringList enabledTabMenuItems(const MainWindow &mw, QTabWidget *pane, int index)
{
    QStringList items;
    inTabMenu(
        mw, pane, index,
        [&items](QMenu *menu)
        {
            for (QAction *action : menu->actions())
            {
                if (!action->isSeparator() && action->isEnabled())
                {
                    items.append(action->text());
                }
            }
        }
    );
    return items;
}

void triggerTabMenuItem(const MainWindow &mw, QTabWidget *pane, int index, const QString &label)
{
    inTabMenu(
        mw, pane, index,
        [label](QMenu *menu)
        {
            for (QAction *action : menu->actions())
            {
                if (action->text() == label && action->isEnabled())
                {
                    action->trigger();
                }
            }
        }
    );
}

} // namespace

// The shell owns what no single tab can: which server is on screen, the tab
// tree of every connection that is open, the ids those tabs are handed, and
// the workspace blob the whole lot round-trips through. None of it is visible
// in one place, so most of what is asserted below is read back out of the save
// payload, which is the window's own account of its state.
class TestMainWindow : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void init();
    void cleanup();

    void aFreshWindowShowsTheEmptyState();
    void activatingAServerBringsUpItsChrome();

    void everyViewOpensTheWidgetItNamesAndItsTitle_data();
    void everyViewOpensTheWidgetItNamesAndItsTitle();
    void aPanelIsReactivatedInsteadOfOpenedTwice();
    void anEditorOpensAgainEveryTimeItIsAskedFor();
    void showInGraphRefocusesTheOpenGraph();

    void theTabKeysOpenAndCloseTabs();
    void closingTheLastTabLeavesAFreshQueryTab();
    void middleClickOnATabClosesThatTab();
    void aRethemedCloseButtonStillClosesItsTab();
    void theTabMenuOffersOnlyWhatTheTabCanDo();
    void closeOtherTabsLeavesOnlyTheOneClickedOn();

    void theCurrentTabIsRememberedPerConnection();
    void disconnectingKeepsTheTabsAndTheirText();

    void nothingIsSavedBeforeTheWorkspaceIsRead();
    void oneSaveCarriesEveryChangeInTheWindow();
    void theFinalSaveGoesOutWhileTheWindowCloses();
    void restoredTabIdsAreNeverHandedOutAgain();

private:
    StubBackend m_backend;
    // Nothing this file drives may open a dialog: an exec()'d modal has nobody
    // to answer it under the offscreen platform, so the run would hang in it
    // rather than fail. The timer only ever gets to run if something spins the
    // event loop, which is exactly what exec() does — it shuts the dialog so
    // cleanup() can report it.
    QTimer m_modalTrap;
    bool m_modalOpened = false;
};

void TestMainWindow::initTestCase()
{
    // The window remembers its geometry in QSettings and apply() renders the
    // dropdown chevron into the cache dir; test mode keeps this run's copies
    // out of the real ones.
    QCoreApplication::setOrganizationName(QStringLiteral("mybench-test"));
    QCoreApplication::setApplicationName(QStringLiteral("tst_mainwindow"));
    QStandardPaths::setTestModeEnabled(true);

    // The window renders its header icons and sizes its chrome from
    // theme::current() in the constructor, so a palette has to be installed
    // before the first one.
    theme::apply(theme::defaultApp, BaseFontPx);

    m_modalTrap.setInterval(0);
    connect(
        &m_modalTrap, &QTimer::timeout, this,
        [this]()
        {
            QWidget *modal = QApplication::activeModalWidget();
            if (!modal)
            {
                return;
            }
            m_modalOpened = true;
            modal->close();
        }
    );
}

void TestMainWindow::init()
{
    QVERIFY(m_backend.listening());
    api()->setEndpoint(m_backend.base(), QString());
    // The sidebar, the status strip and every panel call out on their own
    // schedule; an empty array is a shape all of them tolerate.
    m_backend.replyWithResult(QJsonArray{});
    m_backend.clearRequests();

    // A slot that switches theme leaves it switched, and the next window
    // builds its chrome from whatever is current.
    theme::apply(theme::defaultApp, BaseFontPx);

    m_modalOpened = false;
    m_modalTrap.start();
}

void TestMainWindow::cleanup()
{
    // The window and its tabs post CloseTab and CloseResult as they go;
    // draining them here keeps them out of the next slot's requests.
    api()->flush(FlushMs);
    m_modalTrap.stop();
    QVERIFY2(!m_modalOpened, "a modal dialog was opened by something this file drives");
}

void TestMainWindow::aFreshWindowShowsTheEmptyState()
{
    MainWindow mw;

    QStackedWidget *stack = stackOf(mw);
    QVERIFY(stack);
    // One page, and it is not a pane: nothing is open, so there is nothing to
    // switch to.
    QCOMPARE(stack->count(), 1);
    QVERIFY(!qobject_cast<QTabWidget *>(stack->currentWidget()));
    QVERIFY(!sidebarOf(mw)->isVisibleTo(&mw));
    QVERIFY(!statusStripOf(mw)->isVisibleTo(&mw));

    // The window comes up before the backend does, so it must not have said
    // anything to it yet.
    api()->flush(FlushMs);
    QVERIFY(m_backend.requests().isEmpty());
}

void TestMainWindow::activatingAServerBringsUpItsChrome()
{
    MainWindow mw;
    emit serverTabsOf(mw)->activated(QString::fromLatin1(Conn));

    // The sidebar and the footer belong to whichever server is in front.
    QVERIFY(sidebarOf(mw)->isVisibleTo(&mw));
    QVERIFY(statusStripOf(mw)->isVisibleTo(&mw));
    // Picking a server opens nothing by itself.
    QVERIFY(!paneOf(mw, Conn));
    QCOMPARE(stackOf(mw)->count(), 1);
    QVERIFY(saveTimerOf(mw)->isActive());
}

void TestMainWindow::everyViewOpensTheWidgetItNamesAndItsTitle_data()
{
    QTest::addColumn<TabView>("view");
    QTest::addColumn<QString>("schema");
    QTest::addColumn<QString>("table");
    QTest::addColumn<QString>("title");
    QTest::addColumn<QByteArray>("widget");

    const QString none;
    QTest::newRow("editor") << TabView::Editor << none << none << QStringLiteral("Query 1")
                            << QByteArray("EditorTab");
    QTest::newRow("dashboard") << TabView::Dashboard << none << none << QStringLiteral("Dashboard")
                               << QByteArray("DashboardView");
    QTest::newRow("processlist") << TabView::Processlist << none << none
                                 << QStringLiteral("Client Connections")
                                 << QByteArray("ProcesslistView");
    QTest::newRow("users") << TabView::Users << none << none << QStringLiteral("Users")
                           << QByteArray("UsersView");
    QTest::newRow("serverinfo") << TabView::ServerInfo << none << none
                                << QStringLiteral("Server Info") << QByteArray("ServerInfoView");
    QTest::newRow("innodb") << TabView::InnoDB << none << none << QStringLiteral("InnoDB Status")
                            << QByteArray("InnoDBView");
    QTest::newRow("graph") << TabView::Graph << none << none << QStringLiteral("Schema Graph")
                           << QByteArray("GraphView");
    QTest::newRow("history") << TabView::History << none << none << QStringLiteral("Query History")
                             << QByteArray("HistoryView");
    QTest::newRow("table") << TabView::TableInspect << QStringLiteral("shop")
                           << QStringLiteral("orders") << QStringLiteral("shop.orders")
                           << QByteArray("InspectorView");
    QTest::newRow("schema") << TabView::SchemaInspect << QStringLiteral("shop") << none
                            << QStringLiteral("shop (schema)") << QByteArray("InspectorView");
}

void TestMainWindow::everyViewOpensTheWidgetItNamesAndItsTitle()
{
    QFETCH(TabView, view);
    QFETCH(QString, schema);
    QFETCH(QString, table);
    QFETCH(QString, title);
    QFETCH(QByteArray, widget);

    MainWindow mw;
    emit serverTabsOf(mw)->activated(QString::fromLatin1(Conn));

    TabRequest req;
    req.view = view;
    req.schema = schema;
    req.table = table;
    emit sidebarOf(mw)->tabRequested(req);

    QTabWidget *pane = paneOf(mw, Conn);
    QVERIFY(pane);
    QCOMPARE(pane->count(), 1);
    QCOMPARE(pane->tabText(0), title);
    // The view a request names is the view that is built: a switch arm wired
    // to the wrong class would still open a tab, with the wrong panel in it.
    QCOMPARE(QByteArray(pane->widget(0)->metaObject()->className()), widget);
    // The pane is what the window shows once it has one.
    QCOMPARE(stackOf(mw)->currentWidget(), pane);
}

void TestMainWindow::aPanelIsReactivatedInsteadOfOpenedTwice()
{
    MainWindow mw;
    emit serverTabsOf(mw)->activated(QString::fromLatin1(Conn));

    TabRequest dashboard;
    dashboard.view = TabView::Dashboard;
    emit sidebarOf(mw)->tabRequested(dashboard);
    emit sidebarOf(mw)->tabRequested(TabRequest{});

    QTabWidget *pane = paneOf(mw, Conn);
    QVERIFY(pane);
    QCOMPARE(pane->count(), 2);
    QCOMPARE(pane->currentIndex(), 1);
    QWidget *panel = pane->widget(0);

    emit sidebarOf(mw)->tabRequested(dashboard);

    // One dashboard per connection: asking again brings the open one forward
    // rather than adding a second panel polling the same server.
    QCOMPARE(pane->count(), 2);
    QCOMPARE(pane->currentWidget(), panel);
}

void TestMainWindow::anEditorOpensAgainEveryTimeItIsAskedFor()
{
    MainWindow mw;
    emit serverTabsOf(mw)->activated(QString::fromLatin1(Conn));

    TabRequest inspect;
    inspect.view = TabView::TableInspect;
    inspect.schema = QStringLiteral("shop");
    inspect.table = QStringLiteral("orders");

    emit sidebarOf(mw)->tabRequested(TabRequest{});
    emit sidebarOf(mw)->tabRequested(TabRequest{});
    emit sidebarOf(mw)->tabRequested(inspect);
    emit sidebarOf(mw)->tabRequested(inspect);

    // Neither is a panel: two queries against one server, or the same table
    // open twice, are both things to work in side by side.
    QTabWidget *pane = paneOf(mw, Conn);
    QVERIFY(pane);
    QCOMPARE(pane->count(), 4);
    QCOMPARE(pane->tabText(0), QStringLiteral("Query 1"));
    QCOMPARE(pane->tabText(1), QStringLiteral("Query 2"));
    QCOMPARE(pane->tabText(2), QStringLiteral("shop.orders"));
    QCOMPARE(pane->tabText(3), QStringLiteral("shop.orders"));
}

void TestMainWindow::showInGraphRefocusesTheOpenGraph()
{
    MainWindow mw;
    emit serverTabsOf(mw)->activated(QString::fromLatin1(Conn));

    emit sidebarOf(mw)->graphFocusRequested(QStringLiteral("shop"), QStringLiteral("orders"));

    QTabWidget *pane = paneOf(mw, Conn);
    QVERIFY(pane);
    QCOMPARE(pane->count(), 1);
    auto *graph = qobject_cast<GraphView *>(pane->widget(0));
    QVERIFY(graph);
    GraphCanvas *canvas = graph->findChild<GraphCanvas *>();
    QVERIFY(canvas);
    QCOMPARE(canvas->focusTable(), QStringLiteral("shop.orders"));

    emit sidebarOf(mw)->graphFocusRequested(QStringLiteral("shop"), QStringLiteral("customers"));

    // The graph is a panel, so the second ask lands on the one already open:
    // it has to re-focus there, or "Show in Graph" would do nothing at all.
    QCOMPARE(pane->count(), 1);
    QCOMPARE(canvas->focusTable(), QStringLiteral("shop.customers"));
}

void TestMainWindow::theTabKeysOpenAndCloseTabs()
{
    MainWindow mw;
    QShortcut *newTab = shortcutFor(mw, QKeySequence(Qt::CTRL | Qt::Key_T));
    QShortcut *closeTab = shortcutFor(mw, QKeySequence(Qt::CTRL | Qt::Key_W));
    QVERIFY(newTab);
    QVERIFY(closeTab);

    emit newTab->activated();

    // No server in front, so there is nothing to open a tab on.
    QCOMPARE(stackOf(mw)->count(), 1);

    emit serverTabsOf(mw)->activated(QString::fromLatin1(Conn));
    emit newTab->activated();
    emit newTab->activated();

    QTabWidget *pane = paneOf(mw, Conn);
    QVERIFY(pane);
    QCOMPARE(pane->count(), 2);
    QCOMPARE(pane->currentIndex(), 1);
    QWidget *first = pane->widget(0);

    emit closeTab->activated();

    // Ctrl+W closes the tab in front, not the one that was opened first.
    QCOMPARE(pane->count(), 1);
    QCOMPARE(pane->widget(0), first);
}

void TestMainWindow::closingTheLastTabLeavesAFreshQueryTab()
{
    MainWindow mw;
    emit serverTabsOf(mw)->activated(QString::fromLatin1(Conn));
    emit sidebarOf(mw)->tabRequested(TabRequest{});

    QTabWidget *pane = paneOf(mw, Conn);
    QVERIFY(pane);
    QAbstractButton *close = closeButton(pane, 0);
    QVERIFY(close);
    QWidget *only = pane->widget(0);

    close->click();

    // A pane is never left blank: one close too many lands on a new query tab
    // rather than on a server with nothing on it.
    QCOMPARE(pane->count(), 1);
    QVERIFY(pane->widget(0) != only);
    QCOMPARE(pane->tabText(0), QStringLiteral("Query 1"));
}

void TestMainWindow::middleClickOnATabClosesThatTab()
{
    MainWindow mw;
    emit serverTabsOf(mw)->activated(QString::fromLatin1(Conn));
    emit sidebarOf(mw)->tabRequested(TabRequest{});
    emit sidebarOf(mw)->tabRequested(TabRequest{});

    QTabWidget *pane = paneOf(mw, Conn);
    QVERIFY(pane);
    QWidget *second = pane->widget(1);

    QTabBar *bar = pane->tabBar();
    const QPoint at = bar->tabRect(0).center();
    QCOMPARE(bar->tabAt(at), 0);

    QMouseEvent click(
        QEvent::MouseButtonRelease, at, bar->mapToGlobal(at), Qt::MiddleButton, Qt::NoButton,
        Qt::NoModifier
    );
    QCoreApplication::sendEvent(bar, &click);

    // The close is the window's own event filter on the bar; the tab under the
    // pointer goes, not the current one.
    QCOMPARE(pane->count(), 1);
    QCOMPARE(pane->widget(0), second);
}

void TestMainWindow::aRethemedCloseButtonStillClosesItsTab()
{
    MainWindow mw;
    emit serverTabsOf(mw)->activated(QString::fromLatin1(Conn));
    emit sidebarOf(mw)->tabRequested(TabRequest{});
    emit sidebarOf(mw)->tabRequested(TabRequest{});

    QTabWidget *pane = paneOf(mw, Conn);
    QVERIFY(pane);
    QAbstractButton *before = closeButton(pane, 0);
    QVERIFY(before);

    theme::apply(theme::defaultApp, BiggerFontPx);

    // The × is rendered at the new size, so every close button is built again.
    // One that came back unwired would leave its tab unclosable, with nothing
    // on screen to say so.
    QAbstractButton *after = closeButton(pane, 0);
    QVERIFY(after);
    QVERIFY(after != before);

    QWidget *second = pane->widget(1);
    after->click();

    QCOMPARE(pane->count(), 1);
    QCOMPARE(pane->widget(0), second);
}

void TestMainWindow::theTabMenuOffersOnlyWhatTheTabCanDo()
{
    MainWindow mw;
    emit serverTabsOf(mw)->activated(QString::fromLatin1(Conn));
    emit sidebarOf(mw)->tabRequested(TabRequest{});

    QTabWidget *pane = paneOf(mw, Conn);
    QVERIFY(pane);

    const QStringList alone = enabledTabMenuItems(mw, pane, 0);
    QVERIFY(alone.contains(QStringLiteral("New Query Tab")));
    QVERIFY(alone.contains(QStringLiteral("Duplicate Tab")));
    QVERIFY(alone.contains(QStringLiteral("Rename Tab…")));
    QVERIFY(alone.contains(QStringLiteral("Close Tab")));
    // Nothing to close around the only tab there is.
    QVERIFY(!alone.contains(QStringLiteral("Close Other Tabs")));
    QVERIFY(!alone.contains(QStringLiteral("Close Tabs to the Left")));
    QVERIFY(!alone.contains(QStringLiteral("Close Tabs to the Right")));

    TabRequest dashboard;
    dashboard.view = TabView::Dashboard;
    emit sidebarOf(mw)->tabRequested(dashboard);

    const QStringList onEditor = enabledTabMenuItems(mw, pane, 0);
    QVERIFY(onEditor.contains(QStringLiteral("Close Other Tabs")));
    QVERIFY(onEditor.contains(QStringLiteral("Close Tabs to the Right")));
    // Nothing sits left of the first tab.
    QVERIFY(!onEditor.contains(QStringLiteral("Close Tabs to the Left")));

    const QStringList onPanel = enabledTabMenuItems(mw, pane, 1);
    // A panel exists once per connection, so duplicating one would only
    // reactivate it, and its title already says what it is.
    QVERIFY(!onPanel.contains(QStringLiteral("Duplicate Tab")));
    QVERIFY(!onPanel.contains(QStringLiteral("Rename Tab…")));
    QVERIFY(onPanel.contains(QStringLiteral("Close Tab")));
    QVERIFY(onPanel.contains(QStringLiteral("Close Tabs to the Left")));
}

void TestMainWindow::closeOtherTabsLeavesOnlyTheOneClickedOn()
{
    MainWindow mw;
    emit serverTabsOf(mw)->activated(QString::fromLatin1(Conn));
    for (int i = 0; i < 3; ++i)
    {
        emit sidebarOf(mw)->tabRequested(TabRequest{});
    }

    QTabWidget *pane = paneOf(mw, Conn);
    QVERIFY(pane);
    QCOMPARE(pane->count(), 3);
    QWidget *kept = pane->widget(1);

    triggerTabMenuItem(mw, pane, 1, QStringLiteral("Close Other Tabs"));

    // Closing walks widgets rather than indices: every index past the first
    // close has moved, so counting through them would take the wrong tabs.
    QCOMPARE(pane->count(), 1);
    QCOMPARE(pane->widget(0), kept);
    QCOMPARE(pane->tabText(0), QStringLiteral("Query 2"));
}

void TestMainWindow::theCurrentTabIsRememberedPerConnection()
{
    MainWindow mw;
    QVERIFY(restoreEmptyWorkspace(mw, m_backend));

    emit serverTabsOf(mw)->activated(QString::fromLatin1(Conn));
    emit sidebarOf(mw)->tabRequested(TabRequest{});
    emit sidebarOf(mw)->tabRequested(TabRequest{});

    emit serverTabsOf(mw)->activated(QString::fromLatin1(OtherConn));
    emit sidebarOf(mw)->tabRequested(TabRequest{});

    QTabWidget *first = paneOf(mw, Conn);
    QTabWidget *second = paneOf(mw, OtherConn);
    QVERIFY(first);
    QVERIFY(second);
    QCOMPARE(stackOf(mw)->currentWidget(), second);

    // Each server keeps its own strip of tabs: switching back brings that
    // strip up, not the other server's.
    emit serverTabsOf(mw)->activated(QString::fromLatin1(Conn));
    QCOMPARE(stackOf(mw)->currentWidget(), first);
    first->setCurrentIndex(0);

    fireSave(mw);

    const QJsonObject ws = lastSave(m_backend);
    QCOMPARE(ws.value(QStringLiteral("activeConn")).toString(), QString::fromLatin1(Conn));
    const QJsonObject active = ws.value(QStringLiteral("activePerConn")).toObject();
    QCOMPARE(active.value(QString::fromLatin1(Conn)).toString(), QStringLiteral("t1"));
    QCOMPARE(active.value(QString::fromLatin1(OtherConn)).toString(), QStringLiteral("t3"));
}

void TestMainWindow::disconnectingKeepsTheTabsAndTheirText()
{
    MainWindow mw;
    QVERIFY(restoreEmptyWorkspace(mw, m_backend));

    emit serverTabsOf(mw)->activated(QString::fromLatin1(Conn));
    emit sidebarOf(mw)->tabRequested(TabRequest{});

    QTabWidget *pane = paneOf(mw, Conn);
    QVERIFY(pane);
    auto *editor = qobject_cast<EditorTab *>(pane->widget(0));
    QVERIFY(editor);
    SqlEditor *buffer = editor->findChild<SqlEditor *>();
    QVERIFY(buffer);
    buffer->setPlainText(QStringLiteral("select typed_before_the_disconnect"));

    emit serverTabsOf(mw)->closeRequested(QString::fromLatin1(Conn));
    fireSave(mw);

    // The session is dropped, the pane with it, and the window is back to the
    // empty state with no server in front.
    QCOMPARE(callsTo(m_backend, ClosePath).size(), 1);
    QCOMPARE(callsTo(m_backend, ClosePath).at(0).args, QJsonArray({QString::fromLatin1(Conn)}));
    QVERIFY(!paneOf(mw, Conn));
    QVERIFY(!sidebarOf(mw)->isVisibleTo(&mw));
    QVERIFY(!statusStripOf(mw)->isVisibleTo(&mw));

    // The tab itself stays, holding what was typed into it: the editor widget
    // dies with the pane, so a disconnect that did not sync it back would lose
    // the buffer for good.
    const QJsonObject ws = lastSave(m_backend);
    QCOMPARE(ws.value(QStringLiteral("tabs")).toArray().size(), 1);
    QCOMPARE(
        savedTab(ws, 0).value(QStringLiteral("sql")).toString(),
        QStringLiteral("select typed_before_the_disconnect")
    );
    QVERIFY(ws.value(QStringLiteral("activeConn")).isNull());
}

void TestMainWindow::nothingIsSavedBeforeTheWorkspaceIsRead()
{
    MainWindow mw;
    emit serverTabsOf(mw)->activated(QString::fromLatin1(Conn));
    emit sidebarOf(mw)->tabRequested(TabRequest{});

    // The change is scheduled like any other...
    QVERIFY(saveTimerOf(mw)->isActive());
    fireSave(mw);

    // ...but until the stored workspace has been read back there is nothing to
    // write it over, and this window's empty tree would replace somebody's
    // tabs with nothing.
    QVERIFY(callsTo(m_backend, SavePath).isEmpty());
}

void TestMainWindow::oneSaveCarriesEveryChangeInTheWindow()
{
    MainWindow mw;
    QVERIFY(restoreEmptyWorkspace(mw, m_backend));

    emit serverTabsOf(mw)->activated(QString::fromLatin1(Conn));
    emit sidebarOf(mw)->tabRequested(TabRequest{});
    TabRequest dashboard;
    dashboard.view = TabView::Dashboard;
    emit sidebarOf(mw)->tabRequested(dashboard);

    fireSave(mw);

    // Three changes, one POST: the window coalesces behind its timer instead
    // of writing the whole tree once per tab.
    QCOMPARE(callsTo(m_backend, SavePath).size(), 1);

    const QJsonObject ws = lastSave(m_backend);
    QCOMPARE(ws.value(QStringLiteral("version")).toInt(), 2);
    QCOMPARE(ws.value(QStringLiteral("activeConn")).toString(), QString::fromLatin1(Conn));
    QCOMPARE(ws.value(QStringLiteral("tabs")).toArray().size(), 2);
    QCOMPARE(savedTab(ws, 0).value(QStringLiteral("tabID")).toString(), QStringLiteral("t1"));
    QCOMPARE(savedTab(ws, 0).value(QStringLiteral("view")).toString(), QStringLiteral("editor"));
    QCOMPARE(savedTab(ws, 1).value(QStringLiteral("tabID")).toString(), QStringLiteral("t2"));
    QCOMPARE(savedTab(ws, 1).value(QStringLiteral("view")).toString(), QStringLiteral("dashboard"));
    QCOMPARE(
        ws.value(QStringLiteral("activePerConn"))
            .toObject()
            .value(QString::fromLatin1(Conn))
            .toString(),
        QStringLiteral("t2")
    );
}

void TestMainWindow::theFinalSaveGoesOutWhileTheWindowCloses()
{
    MainWindow mw;
    QVERIFY(restoreEmptyWorkspace(mw, m_backend));

    emit serverTabsOf(mw)->activated(QString::fromLatin1(Conn));
    emit sidebarOf(mw)->tabRequested(TabRequest{});
    QVERIFY(saveTimerOf(mw)->isActive());
    QVERIFY(callsTo(m_backend, SavePath).isEmpty());

    mw.close();

    // A change made inside the coalescing window would die with the event loop
    // on the way out, and even a save that had fired needs the loop pumped for
    // its POST to reach the wire. Nothing is pumped here: closeEvent fires the
    // pending save and flushes it itself, or the last thing the user did is
    // gone.
    QCOMPARE(callsTo(m_backend, SavePath).size(), 1);
    QCOMPARE(lastSave(m_backend).value(QStringLiteral("tabs")).toArray().size(), 1);
}

void TestMainWindow::restoredTabIdsAreNeverHandedOutAgain()
{
    QObject scope;
    answerLoadWith(m_backend, storedWorkspace(), scope);
    m_backend.replyWithResult(QJsonArray{savedConn(Conn)});

    MainWindow mw;
    mw.onBackendReady();
    QVERIFY(waitUntil([this] { return !callsTo(m_backend, LoadPath).isEmpty(); }));
    api()->flush(FlushMs);
    m_backend.clearRequests();

    // Restore is lazy: the tab tree is read back, but no connection is dialled
    // and nothing is built until one is.
    QVERIFY(!paneOf(mw, Conn));

    emit serverTabsOf(mw)->activated(QString::fromLatin1(Conn));
    fireSave(mw);

    QJsonObject ws = lastSave(m_backend);
    QCOMPARE(ws.value(QStringLiteral("tabs")).toArray().size(), 2);
    QCOMPARE(
        ws.value(QStringLiteral("activePerConn"))
            .toObject()
            .value(QString::fromLatin1(Conn))
            .toString(),
        QStringLiteral("t9")
    );
    QCOMPARE(
        ws.value(QStringLiteral("prefs"))
            .toObject()
            .value(QStringLiteral("defaultRowLimit"))
            .toInt(),
        100
    );
    // Titles are derived again on the way in — editor tabs number themselves
    // by arrival — except the one the user typed, which comes back verbatim.
    QCOMPARE(savedTab(ws, 0).value(QStringLiteral("title")).toString(), QStringLiteral("Query 1"));
    QCOMPARE(savedTab(ws, 1).value(QStringLiteral("title")).toString(), QStringLiteral("Scratch"));

    m_backend.clearRequests();
    emit sidebarOf(mw)->tabRequested(TabRequest{});
    fireSave(mw);

    ws = lastSave(m_backend);
    QCOMPARE(ws.value(QStringLiteral("tabs")).toArray().size(), 3);
    // Ids resume above the highest one that came back: handing out t3 or t9
    // again would collide with a tab that is already there, and the workspace
    // keys everything on the id.
    QCOMPARE(savedTab(ws, 2).value(QStringLiteral("tabID")).toString(), QStringLiteral("t10"));
    QCOMPARE(savedTab(ws, 2).value(QStringLiteral("title")).toString(), QStringLiteral("Query 3"));
}

QTEST_MAIN(TestMainWindow)

#include "tst_mainwindow.moc"
