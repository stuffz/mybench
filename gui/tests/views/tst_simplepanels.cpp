#include "views/historyview.h"
#include "views/innodbview.h"
#include "views/serverinfoview.h"
#include "views/usersview.h"

#include "app/api.h"
#include "app/stubbackend.h"
#include "app/theme.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QLatin1String>
#include <QLayout>
#include <QLineEdit>
#include <QList>
#include <QLocale>
#include <QObject>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QString>
#include <QStringList>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTest>
#include <QTimer>
#include <QVariant>
#include <QWidget>

// Four panels of one shape: a single RPC from the constructor, a read-only
// table or text box for the reply, and PanelBase's error strip for whatever
// arrived instead. They keep every widget private and report nothing about
// what they drew, so what a test can reach is the cells themselves, the strip,
// and the conversation the stub recorded.
//
// One class rather than four files: split up, these would be four copies of
// the same scaffolding around twenty lines of assertions each.
namespace
{

constexpr auto ConnID = "c1";
constexpr int FontSize = 13;
constexpr int FlushMs = 5000;

constexpr auto RefreshText = "Refresh";

// The row PanelBase parks the error strip in, between the title row and the
// body (pinned by tst_panelbase).
constexpr int ErrorRow = 1;

constexpr auto UsersPath = "/rpc/admin/Users";
constexpr auto GrantsPath = "/rpc/admin/Grants";
constexpr auto ServerInfoPath = "/rpc/admin/ServerInfoSnapshot";
constexpr auto InnoDBPath = "/rpc/admin/InnoDBStatus";
constexpr auto HistoryPath = "/rpc/query/History";
constexpr auto ClearPath = "/rpc/query/ClearHistory";

constexpr auto Denied = "admin.Users: Access denied for user";
constexpr auto NoGrants = "admin.Grants: You are not allowed to execute this";
constexpr auto NoEngine = "admin.InnoDBStatus: PROCESS privilege required";
constexpr auto NoHistory = "query.History: history storage unavailable";

constexpr int UserCol = 0;
constexpr int HostCol = 1;
constexpr int PluginCol = 2;
constexpr int LockedCol = 3;
constexpr auto LockedText = "yes";
constexpr auto GrantsPlaceholder = "Select an account to see its grants.";

// The header each of ServerInfoView's two tables is built with, which is what
// tells them apart from outside.
constexpr auto VarsHeader = "Variable";
constexpr auto StatusHeader = "Status";
constexpr int NameCol = 0;
constexpr int ValueCol = 1;

constexpr int StartedCol = 0;
constexpr int DurationCol = 1;
constexpr int RowsCol = 2;
constexpr int SourceCol = 3;
constexpr int StatementCol = 4;

// Mirrors HistoryLimit in historyview.cpp, and the 4000 ms the Clear button
// stays armed for: both reach the outside only as a number, so both are worth
// pinning here.
constexpr int HistoryLimit = 500;
constexpr int ArmMs = 4000;

constexpr auto ClearText = "Clear History";
constexpr auto ConfirmText = "Confirm Clear";
constexpr auto VariantProp = "variant";
constexpr auto DestructiveVariant = "destructive";

// The stamp the backend sends (RFC3339) and the format the Started column
// renders it in.
constexpr auto Started = "2026-09-13T10:00:00Z";
constexpr auto ClockFormat = "yyyy-MM-dd HH:mm:ss";
constexpr auto SourceEditor = "editor";
constexpr auto SourceMcp = "mcp";

enum class Lock
{
    Open,
    Locked
};

// One row of admin.Users (UserRow in backend/internal/admin/service.go).
QJsonObject account(const QString &user, const QString &host, const QString &plugin, Lock lock)
{
    QJsonObject o;
    o.insert(QStringLiteral("user"), user);
    o.insert(QStringLiteral("host"), host);
    o.insert(QStringLiteral("plugin"), plugin);
    o.insert(QStringLiteral("locked"), lock == Lock::Locked);
    return o;
}

// One KV of admin.ServerInfoSnapshot.
QJsonObject kv(const QString &name, const QString &value)
{
    QJsonObject o;
    o.insert(QStringLiteral("name"), name);
    o.insert(QStringLiteral("value"), value);
    return o;
}

QJsonObject serverInfo(const QJsonArray &variables, const QJsonArray &status)
{
    QJsonObject o;
    o.insert(QStringLiteral("variables"), variables);
    o.insert(QStringLiteral("status"), status);
    return o;
}

// One row of query.History (HistoryEntry in backend/internal/storage).
QJsonObject statement(
    const QString &query, const QString &startedAt, int durationMs, int rows,
    const QString &source = {}, const QString &error = {}
)
{
    QJsonObject o;
    o.insert(QStringLiteral("query"), query);
    o.insert(QStringLiteral("startedAt"), startedAt);
    o.insert(QStringLiteral("durationMs"), durationMs);
    o.insert(QStringLiteral("rowCount"), rows);
    o.insert(QStringLiteral("source"), source);
    o.insert(QStringLiteral("error"), error);
    return o;
}

QTableWidget *tableOf(const QWidget &view)
{
    return view.findChild<QTableWidget *>();
}

// ServerInfoView is the one panel with two tables; the header of the first
// column is what names them.
QTableWidget *tableHeaded(const QWidget &view, const char *header)
{
    const QList<QTableWidget *> tables = view.findChildren<QTableWidget *>();
    for (QTableWidget *table : tables)
    {
        const QTableWidgetItem *first = table->horizontalHeaderItem(NameCol);
        if (first && first->text() == QLatin1String(header))
        {
            return table;
        }
    }
    return nullptr;
}

// -1 where the panel has no table at all, which fails a comparison rather than
// the process.
int rowsOf(const QWidget &view)
{
    const QTableWidget *table = tableOf(view);
    return table ? table->rowCount() : -1;
}

QString cell(const QTableWidget *table, int row, int column)
{
    const QTableWidgetItem *item = table ? table->item(row, column) : nullptr;
    return item ? item->text() : QString();
}

// Row order is whatever the table's live sort makes of it, so every assertion
// on a row finds it by the cell that identifies it rather than by index.
int rowOf(const QTableWidget *table, int column, const QString &text)
{
    for (int row = 0; row < table->rowCount(); ++row)
    {
        if (cell(table, row, column) == text)
        {
            return row;
        }
    }
    return -1;
}

QStringList namesIn(const QTableWidget *table)
{
    QStringList out;
    for (int row = 0; row < table->rowCount(); ++row)
    {
        out.append(cell(table, row, NameCol));
    }
    return out;
}

QPushButton *buttonWith(const QWidget &view, const char *text)
{
    const QList<QPushButton *> buttons = view.findChildren<QPushButton *>();
    for (QPushButton *button : buttons)
    {
        if (button->text() == QLatin1String(text))
        {
            return button;
        }
    }
    return nullptr;
}

QLabel *errorStrip(const QWidget &view)
{
    QLayout *root = view.layout();
    if (!root || root->count() <= ErrorRow)
    {
        return nullptr;
    }
    return qobject_cast<QLabel *>(root->itemAt(ErrorRow)->widget());
}

// The panel is never shown in a test, and a child of a hidden parent is never
// isVisible(): ask whether the strip would come up with the panel instead.
bool errorShown(const QWidget &view)
{
    const QLabel *strip = errorStrip(view);
    return strip && strip->isVisibleTo(&view);
}

QString errorText(const QWidget &view)
{
    const QLabel *strip = errorStrip(view);
    return strip ? strip->text() : QString();
}

// HistoryView's arm timer is its only direct QTimer child; the table and the
// filter box keep their own timers further down.
QTimer *armTimer(const QWidget &view)
{
    return view.findChild<QTimer *>(QString(), Qt::FindDirectChildrenOnly);
}

// The calls that went to one RPC method, so a slot asserts on the exchange it
// is about and not on whatever the panel fetched alongside it.
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

} // namespace

class TestSimplePanels : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void init();

    void usersViewAsksForTheAccountsOfItsConnection();
    void usersViewRendersEveryColumnOfAnAccount();
    void usersViewAsksForTheGrantsOfTheSelectedRow();
    void usersViewPutsAGrantFailureWhereTheGrantsGo();
    void usersViewWithNoAccountsShowsAnEmptyTable();
    void usersViewKeepsItsRowsAndSaysAReadFailed();

    void serverInfoViewAsksForOneSnapshotOfBoth();
    void serverInfoViewRendersVariablesBesideStatus();
    void serverInfoViewFiltersWithoutAskingAgain();
    void serverInfoViewFilterKeepsMatchingRows_data();
    void serverInfoViewFilterKeepsMatchingRows();
    void serverInfoViewWithNoRowsShowsEmptyTables();
    void serverInfoViewKeepsItsRowsAndSaysAReadFailed();

    void innodbViewAsksForTheEngineReport();
    void innodbViewShowsTheReportVerbatim();
    void innodbViewWithNoReportIsEmpty();
    void innodbViewKeepsTheReportAndSaysAReadFailed();

    void historyViewAsksForItsConnectionsStatements();
    void historyViewRendersEveryColumnOfAStatement();
    void historyViewShowsAnUnparsableStampAsItCame();
    void historyViewMarksTheStatementThatFailed();
    void historyViewSearchesOnReturn();
    void historyViewLeavesTheRowLimitToTheBackend();
    void historyViewArmsTheClearButtonFirst();
    void historyViewDisarmsTheClearButtonOnTimeout();
    void historyViewClearsAndReloadsOnTheSecondClick();
    void historyViewWithNoStatementsShowsAnEmptyTable();
    void historyViewKeepsItsRowsAndSaysAReadFailed();

private:
    StubBackend m_backend;
};

void TestSimplePanels::initTestCase()
{
    // The panels, their tables and the error strip all build themselves from
    // theme::current(); apply() is what installs it.
    theme::apply(theme::defaultApp, FontSize);

    // Nothing here formats through fmt.h, but the number cells go through
    // QVariant and the clock column through QDateTime: a pinned locale keeps
    // the host's out of both.
    QLocale::setDefault(QLocale(QLocale::English, QLocale::UnitedStates));
}

void TestSimplePanels::init()
{
    m_backend.clearRequests();
    QVERIFY(m_backend.listening());
    api()->setEndpoint(m_backend.base(), QString());
}

void TestSimplePanels::usersViewAsksForTheAccountsOfItsConnection()
{
    m_backend.replyWithResult(QJsonArray{account(
        QStringLiteral("app"), QStringLiteral("%"), QStringLiteral("caching_sha2_password"),
        Lock::Open
    )});
    UsersView view(QString::fromLatin1(ConnID));
    api()->flush(FlushMs);

    const QList<StubBackend::Request> users = callsTo(m_backend, UsersPath);
    QCOMPARE(users.size(), 1);
    QCOMPARE(users.at(0).args, QJsonArray({QString::fromLatin1(ConnID)}));

    // Grants are one statement per account, so the panel must not fetch any
    // before a row is picked.
    QCOMPARE(callsTo(m_backend, GrantsPath).size(), 0);

    const QPlainTextEdit *grants = view.findChild<QPlainTextEdit *>();
    QVERIFY(grants);
    QVERIFY(grants->toPlainText().isEmpty());
    QCOMPARE(grants->placeholderText(), QString::fromLatin1(GrantsPlaceholder));
    QVERIFY2(grants->isReadOnly(), "the grants pane is a report, not an editor");
}

void TestSimplePanels::usersViewRendersEveryColumnOfAnAccount()
{
    m_backend.replyWithResult(QJsonArray{
        account(
            QStringLiteral("app"), QStringLiteral("%"), QStringLiteral("caching_sha2_password"),
            Lock::Open
        ),
        account(
            QStringLiteral("reporting"), QStringLiteral("10.0.0.%"),
            QStringLiteral("mysql_native_password"), Lock::Locked
        ),
    });
    UsersView view(QString::fromLatin1(ConnID));
    api()->flush(FlushMs);

    QTableWidget *table = tableOf(view);
    QVERIFY(table);
    QCOMPARE(table->rowCount(), 2);
    QCOMPARE(table->columnCount(), 4);

    const int app = rowOf(table, UserCol, QStringLiteral("app"));
    QVERIFY(app >= 0);
    QCOMPARE(cell(table, app, HostCol), QStringLiteral("%"));
    QCOMPARE(cell(table, app, PluginCol), QStringLiteral("caching_sha2_password"));

    // An account nobody locked leaves the column empty rather than saying
    // "no": a column of "no" reads as a warning at a glance.
    QVERIFY(cell(table, app, LockedCol).isEmpty());

    const int reporting = rowOf(table, UserCol, QStringLiteral("reporting"));
    QVERIFY(reporting >= 0);
    QCOMPARE(cell(table, reporting, HostCol), QStringLiteral("10.0.0.%"));
    QCOMPARE(cell(table, reporting, PluginCol), QStringLiteral("mysql_native_password"));
    QCOMPARE(cell(table, reporting, LockedCol), QString::fromLatin1(LockedText));
}

void TestSimplePanels::usersViewAsksForTheGrantsOfTheSelectedRow()
{
    m_backend.replyWithResult(QJsonArray{account(
        QStringLiteral("app"), QStringLiteral("10.0.0.%"), QStringLiteral("mysql_native_password"),
        Lock::Open
    )});
    UsersView view(QString::fromLatin1(ConnID));
    api()->flush(FlushMs);

    m_backend.clearRequests();
    m_backend.replyWithResult(QJsonArray{
        QStringLiteral("GRANT USAGE ON *.* TO `app`@`10.0.0.%`"),
        QStringLiteral("GRANT SELECT ON `shop`.* TO `app`@`10.0.0.%`"),
    });

    QTableWidget *table = tableOf(view);
    QVERIFY(table);
    table->setCurrentCell(0, UserCol);
    api()->flush(FlushMs);

    // The account is addressed by both halves of its identity: user alone
    // would fetch the grants of whichever host sorted first.
    const QList<StubBackend::Request> grants = callsTo(m_backend, GrantsPath);
    QCOMPARE(grants.size(), 1);
    QCOMPARE(
        grants.at(0).args,
        QJsonArray({QString::fromLatin1(ConnID), QStringLiteral("app"), QStringLiteral("10.0.0.%")})
    );

    // SHOW GRANTS returns statements without their terminator; the pane is
    // meant to be copied into a client, so each line gets one.
    const QPlainTextEdit *pane = view.findChild<QPlainTextEdit *>();
    QVERIFY(pane);
    QCOMPARE(
        pane->toPlainText(), QStringLiteral("GRANT USAGE ON *.* TO `app`@`10.0.0.%`;\n"
                                            "GRANT SELECT ON `shop`.* TO `app`@`10.0.0.%`;")
    );
}

void TestSimplePanels::usersViewPutsAGrantFailureWhereTheGrantsGo()
{
    m_backend.replyWithResult(QJsonArray{account(
        QStringLiteral("app"), QStringLiteral("%"), QStringLiteral("caching_sha2_password"),
        Lock::Open
    )});
    UsersView view(QString::fromLatin1(ConnID));
    api()->flush(FlushMs);

    m_backend.replyWithError(QString::fromLatin1(NoGrants));
    QTableWidget *table = tableOf(view);
    QVERIFY(table);
    table->setCurrentCell(0, UserCol);
    api()->flush(FlushMs);

    // The message lands in the pane the grants would have filled, so it is
    // read where it was looked for. The strip stays down: the account list
    // itself is still good.
    const QPlainTextEdit *pane = view.findChild<QPlainTextEdit *>();
    QVERIFY(pane);
    QCOMPARE(pane->toPlainText(), QString::fromLatin1(NoGrants));
    QVERIFY(!errorShown(view));
    QCOMPARE(table->rowCount(), 1);
}

void TestSimplePanels::usersViewWithNoAccountsShowsAnEmptyTable()
{
    m_backend.replyWithResult(QJsonArray());
    UsersView view(QString::fromLatin1(ConnID));
    api()->flush(FlushMs);

    QTableWidget *table = tableOf(view);
    QVERIFY(table);
    QCOMPARE(table->rowCount(), 0);
    QVERIFY2(!errorShown(view), "an empty account list is an answer, not a failure");
}

void TestSimplePanels::usersViewKeepsItsRowsAndSaysAReadFailed()
{
    m_backend.replyWithResult(QJsonArray{account(
        QStringLiteral("app"), QStringLiteral("%"), QStringLiteral("caching_sha2_password"),
        Lock::Open
    )});
    UsersView view(QString::fromLatin1(ConnID));
    api()->flush(FlushMs);
    QVERIFY(!errorShown(view));

    m_backend.replyWithError(QString::fromLatin1(Denied));
    QPushButton *refresh = buttonWith(view, RefreshText);
    QVERIFY(refresh);
    refresh->click();
    api()->flush(FlushMs);

    QVERIFY2(errorShown(view), "a failed read must say so, not read as a server with no accounts");
    QCOMPARE(errorText(view), QString::fromLatin1(Denied));

    // The last good list stays on screen: blanking it would read as the
    // accounts having been dropped.
    QTableWidget *table = tableOf(view);
    QVERIFY(table);
    QCOMPARE(table->rowCount(), 1);
    QCOMPARE(cell(table, 0, UserCol), QStringLiteral("app"));
}

void TestSimplePanels::serverInfoViewAsksForOneSnapshotOfBoth()
{
    m_backend.replyWithResult(serverInfo({}, {}));
    ServerInfoView view(QString::fromLatin1(ConnID));
    api()->flush(FlushMs);

    // Variables and status come off one snapshot: two calls would show a
    // counter and the variable that bounds it read a round trip apart.
    const QList<StubBackend::Request> snapshots = callsTo(m_backend, ServerInfoPath);
    QCOMPARE(snapshots.size(), 1);
    QCOMPARE(snapshots.at(0).args, QJsonArray({QString::fromLatin1(ConnID)}));
}

void TestSimplePanels::serverInfoViewRendersVariablesBesideStatus()
{
    m_backend.replyWithResult(serverInfo(
        QJsonArray{kv(QStringLiteral("max_connections"), QStringLiteral("151"))},
        QJsonArray{kv(QStringLiteral("Threads_connected"), QStringLiteral("12"))}
    ));
    ServerInfoView view(QString::fromLatin1(ConnID));
    api()->flush(FlushMs);

    QTableWidget *vars = tableHeaded(view, VarsHeader);
    QTableWidget *status = tableHeaded(view, StatusHeader);
    QVERIFY(vars);
    QVERIFY(status);

    QCOMPARE(vars->rowCount(), 1);
    QCOMPARE(cell(vars, 0, NameCol), QStringLiteral("max_connections"));
    QCOMPARE(cell(vars, 0, ValueCol), QStringLiteral("151"));

    QCOMPARE(status->rowCount(), 1);
    QCOMPARE(cell(status, 0, NameCol), QStringLiteral("Threads_connected"));
    QCOMPARE(cell(status, 0, ValueCol), QStringLiteral("12"));

    // A value column is narrow and its contents can run long (a plugin list,
    // a path): the tooltip is the only way to read one in full.
    QVERIFY(vars->item(0, ValueCol));
    QCOMPARE(vars->item(0, ValueCol)->toolTip(), QStringLiteral("151"));
}

void TestSimplePanels::serverInfoViewFiltersWithoutAskingAgain()
{
    m_backend.replyWithResult(serverInfo(
        QJsonArray{
            kv(QStringLiteral("max_connections"), QStringLiteral("151")),
            kv(QStringLiteral("character_set_server"), QStringLiteral("utf8mb4")),
        },
        QJsonArray{kv(QStringLiteral("Threads_connected"), QStringLiteral("12"))}
    ));
    ServerInfoView view(QString::fromLatin1(ConnID));
    api()->flush(FlushMs);
    m_backend.clearRequests();

    QLineEdit *filter = view.findChild<QLineEdit *>();
    QTableWidget *vars = tableHeaded(view, VarsHeader);
    QTableWidget *status = tableHeaded(view, StatusHeader);
    QVERIFY(filter);
    QVERIFY(vars);
    QVERIFY(status);

    filter->setText(QStringLiteral("max"));
    api()->flush(FlushMs);

    // The rows are held in memory, so filtering is a redraw. A round trip per
    // keystroke would put a query on the server for every letter typed.
    QCOMPARE(m_backend.requests().size(), 0);
    QCOMPARE(namesIn(vars), QStringList{QStringLiteral("max_connections")});

    // Clearing the box puts every row back rather than needing a refresh.
    filter->clear();
    QCOMPARE(vars->rowCount(), 2);
    QCOMPARE(status->rowCount(), 1);
    QCOMPARE(m_backend.requests().size(), 0);
}

void TestSimplePanels::serverInfoViewFilterKeepsMatchingRows_data()
{
    QTest::addColumn<QString>("filter");
    QTest::addColumn<QStringList>("variables");
    QTest::addColumn<QStringList>("status");

    const QStringList allVars{
        QStringLiteral("max_connections"), QStringLiteral("innodb_buffer_pool_size"),
        QStringLiteral("character_set_server")
    };
    const QStringList allStatus{
        QStringLiteral("Threads_connected"), QStringLiteral("Innodb_rows_read")
    };

    QTest::newRow("no filter keeps everything") << QString() << allVars << allStatus;
    QTest::newRow("name substring")
        << QStringLiteral("max") << QStringList{QStringLiteral("max_connections")} << QStringList();

    // One box over both tables, and the server spells the same subsystem two
    // ways: a case-sensitive match would hide half of what was asked for.
    QTest::newRow("case is ignored on both sides")
        << QStringLiteral("innodb") << QStringList{QStringLiteral("innodb_buffer_pool_size")}
        << QStringList{QStringLiteral("Innodb_rows_read")};

    // Values match too: the collation of a server is looked up by its value
    // far more often than by the name of the variable holding it.
    QTest::newRow("value substring")
        << QStringLiteral("utf8mb4") << QStringList{QStringLiteral("character_set_server")}
        << QStringList();

    // Pasted names arrive with their whitespace attached.
    QTest::newRow("padding is trimmed")
        << QStringLiteral("  max  ") << QStringList{QStringLiteral("max_connections")}
        << QStringList();

    QTest::newRow("no match empties both")
        << QStringLiteral("zzz") << QStringList() << QStringList();
}

void TestSimplePanels::serverInfoViewFilterKeepsMatchingRows()
{
    QFETCH(QString, filter);
    QFETCH(QStringList, variables);
    QFETCH(QStringList, status);

    m_backend.replyWithResult(serverInfo(
        QJsonArray{
            kv(QStringLiteral("max_connections"), QStringLiteral("151")),
            kv(QStringLiteral("innodb_buffer_pool_size"), QStringLiteral("134217728")),
            kv(QStringLiteral("character_set_server"), QStringLiteral("utf8mb4")),
        },
        QJsonArray{
            kv(QStringLiteral("Threads_connected"), QStringLiteral("12")),
            kv(QStringLiteral("Innodb_rows_read"), QStringLiteral("4000")),
        }
    ));
    ServerInfoView view(QString::fromLatin1(ConnID));
    api()->flush(FlushMs);

    QLineEdit *box = view.findChild<QLineEdit *>();
    QTableWidget *varsTable = tableHeaded(view, VarsHeader);
    QTableWidget *statusTable = tableHeaded(view, StatusHeader);
    QVERIFY(box);
    QVERIFY(varsTable);
    QVERIFY(statusTable);

    box->setText(filter);

    QCOMPARE(namesIn(varsTable), variables);
    QCOMPARE(namesIn(statusTable), status);
}

void TestSimplePanels::serverInfoViewWithNoRowsShowsEmptyTables()
{
    m_backend.replyWithResult(serverInfo({}, {}));
    ServerInfoView view(QString::fromLatin1(ConnID));
    api()->flush(FlushMs);

    QTableWidget *vars = tableHeaded(view, VarsHeader);
    QTableWidget *status = tableHeaded(view, StatusHeader);
    QVERIFY(vars);
    QVERIFY(status);
    QCOMPARE(vars->rowCount(), 0);
    QCOMPARE(status->rowCount(), 0);
    QVERIFY(!errorShown(view));
}

void TestSimplePanels::serverInfoViewKeepsItsRowsAndSaysAReadFailed()
{
    m_backend.replyWithResult(serverInfo(
        QJsonArray{kv(QStringLiteral("max_connections"), QStringLiteral("151"))},
        QJsonArray{kv(QStringLiteral("Threads_connected"), QStringLiteral("12"))}
    ));
    ServerInfoView view(QString::fromLatin1(ConnID));
    api()->flush(FlushMs);

    m_backend.replyWithError(QString::fromLatin1(Denied));
    QPushButton *refresh = buttonWith(view, RefreshText);
    QVERIFY(refresh);
    refresh->click();
    api()->flush(FlushMs);

    QVERIFY2(errorShown(view), "a failed read must say so, not read as a server with no variables");
    QCOMPARE(errorText(view), QString::fromLatin1(Denied));

    QTableWidget *vars = tableHeaded(view, VarsHeader);
    QTableWidget *status = tableHeaded(view, StatusHeader);
    QVERIFY(vars);
    QVERIFY(status);
    QCOMPARE(vars->rowCount(), 1);
    QCOMPARE(status->rowCount(), 1);
}

void TestSimplePanels::innodbViewAsksForTheEngineReport()
{
    m_backend.replyWithResult(QString());
    InnoDBView view(QString::fromLatin1(ConnID));
    api()->flush(FlushMs);

    const QList<StubBackend::Request> reports = callsTo(m_backend, InnoDBPath);
    QCOMPARE(reports.size(), 1);
    QCOMPARE(reports.at(0).args, QJsonArray({QString::fromLatin1(ConnID)}));
}

void TestSimplePanels::innodbViewShowsTheReportVerbatim()
{
    // The engine lays the report out in columns of its own, so the panel has
    // to keep the line breaks and the runs of spaces it was given.
    const QString report =
        QStringLiteral("=====================================\n"
                       "2026-09-13 10:00:00 0x7f INNODB MONITOR OUTPUT\n"
                       "Per second averages calculated from the last 30 seconds\n"
                       "-------------\n"
                       "TRANSACTIONS\n"
                       "Trx id counter 1234567\n");
    m_backend.replyWithResult(report);
    InnoDBView view(QString::fromLatin1(ConnID));
    api()->flush(FlushMs);

    QPlainTextEdit *text = view.findChild<QPlainTextEdit *>();
    QVERIFY(text);
    QCOMPARE(text->toPlainText(), report);
    QVERIFY2(text->isReadOnly(), "the report is a reading, not a document");

    // Wrapping the report would fold its columns into each other.
    QCOMPARE(text->lineWrapMode(), QPlainTextEdit::NoWrap);
    QVERIFY(!errorShown(view));
}

void TestSimplePanels::innodbViewWithNoReportIsEmpty()
{
    // A server with the InnoDB engine disabled answers with nothing at all.
    m_backend.replyWithResult(QString());
    InnoDBView view(QString::fromLatin1(ConnID));
    api()->flush(FlushMs);

    QPlainTextEdit *text = view.findChild<QPlainTextEdit *>();
    QVERIFY(text);
    QVERIFY(text->toPlainText().isEmpty());
    QVERIFY(!errorShown(view));
}

void TestSimplePanels::innodbViewKeepsTheReportAndSaysAReadFailed()
{
    const QString report = QStringLiteral("TRANSACTIONS\nTrx id counter 1234567\n");
    m_backend.replyWithResult(report);
    InnoDBView view(QString::fromLatin1(ConnID));
    api()->flush(FlushMs);

    m_backend.replyWithError(QString::fromLatin1(NoEngine));
    QPushButton *refresh = buttonWith(view, RefreshText);
    QVERIFY(refresh);
    refresh->click();
    api()->flush(FlushMs);

    QVERIFY2(errorShown(view), "a failed read must say so, not read as an engine gone quiet");
    QCOMPARE(errorText(view), QString::fromLatin1(NoEngine));

    // The last report stays readable: it is a snapshot, and an old one still
    // says more than an empty box.
    const QPlainTextEdit *text = view.findChild<QPlainTextEdit *>();
    QVERIFY(text);
    QCOMPARE(text->toPlainText(), report);
}

void TestSimplePanels::historyViewAsksForItsConnectionsStatements()
{
    m_backend.replyWithResult(QJsonArray());
    HistoryView view(QString::fromLatin1(ConnID));
    api()->flush(FlushMs);

    const QList<StubBackend::Request> reads = callsTo(m_backend, HistoryPath);
    QCOMPARE(reads.size(), 1);
    QCOMPARE(reads.at(0).args, QJsonArray({QString::fromLatin1(ConnID), QString(), HistoryLimit}));
}

void TestSimplePanels::historyViewRendersEveryColumnOfAStatement()
{
    const QString query = QStringLiteral("SELECT *\n  FROM orders\n WHERE id = 1");
    m_backend.replyWithResult(QJsonArray{
        statement(query, QString::fromLatin1(Started), 12, 3),
        statement(
            QStringLiteral("SHOW TABLES"), QString::fromLatin1(Started), 1, 40,
            QString::fromLatin1(SourceMcp)
        ),
    });
    HistoryView view(QString::fromLatin1(ConnID));
    api()->flush(FlushMs);

    QTableWidget *table = tableOf(view);
    QVERIFY(table);
    QCOMPARE(table->rowCount(), 2);
    QCOMPARE(table->columnCount(), 5);

    // A statement spanning lines has to fit one row, and the raw text is kept
    // beside it for the tooltip and for Copy Statement.
    const int run = rowOf(table, StatementCol, QStringLiteral("SELECT * FROM orders WHERE id = 1"));
    QVERIFY(run >= 0);
    QCOMPARE(table->item(run, StatementCol)->toolTip(), query);
    QCOMPARE(table->item(run, StatementCol)->data(Qt::UserRole).toString(), query);

    QCOMPARE(cell(table, run, DurationCol), QStringLiteral("12"));
    QCOMPARE(cell(table, run, RowsCol), QStringLiteral("3"));

    // The backend leaves the source empty for the editor and names every
    // other one; an empty cell would read as a statement from nowhere.
    QCOMPARE(cell(table, run, SourceCol), QString::fromLatin1(SourceEditor));

    const int shown = rowOf(table, StatementCol, QStringLiteral("SHOW TABLES"));
    QVERIFY(shown >= 0);
    QCOMPARE(cell(table, shown, SourceCol), QString::fromLatin1(SourceMcp));

    // RFC3339 is too wide for the column, so it is shown as the local clock
    // time and kept whole in the tooltip. Read the clock back rather than
    // rebuild the format, so the assertion holds in any zone.
    const QDateTime clock =
        QDateTime::fromString(cell(table, run, StartedCol), QString::fromLatin1(ClockFormat));
    QVERIFY2(clock.isValid(), "the Started column has to read back as a time");
    QCOMPARE(clock, QDateTime::fromString(QString::fromLatin1(Started), Qt::ISODate));
    QCOMPARE(table->item(run, StartedCol)->toolTip(), QString::fromLatin1(Started));
}

void TestSimplePanels::historyViewShowsAnUnparsableStampAsItCame()
{
    // A stamp the panel cannot read is shown as it arrived: dropping it would
    // leave the column blank with no way to tell why.
    constexpr auto Unparsable = "yesterday";
    m_backend.replyWithResult(
        QJsonArray{statement(QStringLiteral("SELECT 1"), QString::fromLatin1(Unparsable), 1, 1)}
    );
    HistoryView view(QString::fromLatin1(ConnID));
    api()->flush(FlushMs);

    QTableWidget *table = tableOf(view);
    QVERIFY(table);
    QCOMPARE(table->rowCount(), 1);
    QCOMPARE(cell(table, 0, StartedCol), QString::fromLatin1(Unparsable));
    QCOMPARE(table->item(0, StartedCol)->toolTip(), QString::fromLatin1(Unparsable));
}

void TestSimplePanels::historyViewMarksTheStatementThatFailed()
{
    constexpr auto Boom = "Table 'shop.order' doesn't exist";
    const QString bad = QStringLiteral("SELECT * FROM order");
    m_backend.replyWithResult(QJsonArray{
        statement(QStringLiteral("SELECT 1"), QString::fromLatin1(Started), 1, 1),
        statement(bad, QString::fromLatin1(Started), 2, 0, QString(), QString::fromLatin1(Boom)),
    });
    HistoryView view(QString::fromLatin1(ConnID));
    api()->flush(FlushMs);

    QTableWidget *table = tableOf(view);
    QVERIFY(table);
    const int failed = rowOf(table, StatementCol, bad);
    const int ok = rowOf(table, StatementCol, QStringLiteral("SELECT 1"));
    QVERIFY(failed >= 0);
    QVERIFY(ok >= 0);

    // Why it failed is on the statement, under the statement itself: the
    // history has no column of its own to put an error in.
    QCOMPARE(
        table->item(failed, StatementCol)->toolTip(),
        bad + QStringLiteral("\n\n") + QString::fromLatin1(Boom)
    );

    // And the row is painted apart from the ones that worked. Which colour is
    // the theme's business; that it differs at all is the panel's.
    QVERIFY2(
        table->item(failed, StatementCol)->foreground() !=
            table->item(ok, StatementCol)->foreground(),
        "a failed statement must not read like one that ran"
    );
}

void TestSimplePanels::historyViewSearchesOnReturn()
{
    m_backend.replyWithResult(QJsonArray());
    HistoryView view(QString::fromLatin1(ConnID));
    api()->flush(FlushMs);
    m_backend.clearRequests();

    QLineEdit *search = view.findChild<QLineEdit *>();
    QVERIFY(search);
    search->setText(QStringLiteral("orders"));

    // The search runs on the server, so it waits for Return rather than
    // re-reading the log on every keystroke.
    QCOMPARE(m_backend.requests().size(), 0);

    QMetaObject::invokeMethod(search, "returnPressed");
    api()->flush(FlushMs);

    const QList<StubBackend::Request> reads = callsTo(m_backend, HistoryPath);
    QCOMPARE(reads.size(), 1);
    QCOMPARE(
        reads.at(0).args,
        QJsonArray({QString::fromLatin1(ConnID), QStringLiteral("orders"), HistoryLimit})
    );
}

void TestSimplePanels::historyViewLeavesTheRowLimitToTheBackend()
{
    // The cap rides on the request, so the backend does the trimming and the
    // panel draws whatever came back. A second cap here would silently hide
    // rows the server was willing to send.
    QJsonArray rows;
    for (int i = 0; i < HistoryLimit + 2; ++i)
    {
        rows.append(
            statement(QStringLiteral("SELECT %1").arg(i), QString::fromLatin1(Started), 1, 1)
        );
    }
    m_backend.replyWithResult(rows);
    HistoryView view(QString::fromLatin1(ConnID));
    api()->flush(FlushMs);

    const QList<StubBackend::Request> reads = callsTo(m_backend, HistoryPath);
    QCOMPARE(reads.size(), 1);
    QCOMPARE(reads.at(0).args.at(2).toInt(), HistoryLimit);
    QCOMPARE(rowsOf(view), HistoryLimit + 2);
}

void TestSimplePanels::historyViewArmsTheClearButtonFirst()
{
    m_backend.replyWithResult(
        QJsonArray{statement(QStringLiteral("SELECT 1"), QString::fromLatin1(Started), 1, 1)}
    );
    HistoryView view(QString::fromLatin1(ConnID));
    api()->flush(FlushMs);
    m_backend.clearRequests();

    QPushButton *clear = buttonWith(view, ClearText);
    QVERIFY(clear);
    clear->click();
    api()->flush(FlushMs);

    // The wipe cannot be undone, so the first click only asks. Nothing has
    // gone to the backend and nothing has left the table.
    QCOMPARE(clear->text(), QString::fromLatin1(ConfirmText));
    QCOMPARE(clear->property(VariantProp).toString(), QString::fromLatin1(DestructiveVariant));
    QCOMPARE(m_backend.requests().size(), 0);
    QCOMPARE(rowsOf(view), 1);

    QTimer *arm = armTimer(view);
    QVERIFY(arm);
    QVERIFY(arm->isActive());
    QVERIFY2(arm->isSingleShot(), "the armed window closes once, not on a cycle");
    QCOMPARE(arm->interval(), ArmMs);
}

void TestSimplePanels::historyViewDisarmsTheClearButtonOnTimeout()
{
    m_backend.replyWithResult(QJsonArray());
    HistoryView view(QString::fromLatin1(ConnID));
    api()->flush(FlushMs);
    m_backend.clearRequests();

    QPushButton *clear = buttonWith(view, ClearText);
    QVERIFY(clear);
    clear->click();
    QTimer *arm = armTimer(view);
    QVERIFY(arm);

    // Nothing ticks a timer in a test that never sits in an event loop for
    // four seconds: emit the timeout rather than wait for it.
    QMetaObject::invokeMethod(arm, "timeout");

    // A button left saying "Confirm Clear" would wipe the log on a click
    // aimed at a Refresh that had moved.
    QCOMPARE(clear->text(), QString::fromLatin1(ClearText));
    QVERIFY(!clear->property(VariantProp).isValid());

    // The next click arms again rather than clearing.
    clear->click();
    api()->flush(FlushMs);
    QCOMPARE(clear->text(), QString::fromLatin1(ConfirmText));
    QCOMPARE(callsTo(m_backend, ClearPath).size(), 0);
}

void TestSimplePanels::historyViewClearsAndReloadsOnTheSecondClick()
{
    m_backend.replyWithResult(
        QJsonArray{statement(QStringLiteral("SELECT 1"), QString::fromLatin1(Started), 1, 1)}
    );
    HistoryView view(QString::fromLatin1(ConnID));
    api()->flush(FlushMs);
    QCOMPARE(rowsOf(view), 1);

    m_backend.clearRequests();
    m_backend.replyWithResult(QJsonArray());
    QPushButton *clear = buttonWith(view, ClearText);
    QVERIFY(clear);
    clear->click();
    clear->click();
    api()->flush(FlushMs);

    const QList<StubBackend::Request> wipes = callsTo(m_backend, ClearPath);
    QCOMPARE(wipes.size(), 1);
    QCOMPARE(wipes.at(0).args, QJsonArray({QString::fromLatin1(ConnID)}));

    // The wipe is followed by a re-read, or the table keeps showing rows the
    // backend no longer has.
    QCOMPARE(callsTo(m_backend, HistoryPath).size(), 1);
    QCOMPARE(rowsOf(view), 0);

    // And the button goes back to asking, with its armed window closed.
    QCOMPARE(clear->text(), QString::fromLatin1(ClearText));
    QVERIFY(!clear->property(VariantProp).isValid());

    QTimer *arm = armTimer(view);
    QVERIFY(arm);
    QVERIFY(!arm->isActive());
}

void TestSimplePanels::historyViewWithNoStatementsShowsAnEmptyTable()
{
    m_backend.replyWithResult(QJsonArray());
    HistoryView view(QString::fromLatin1(ConnID));
    api()->flush(FlushMs);

    QCOMPARE(rowsOf(view), 0);
    QVERIFY2(!errorShown(view), "a connection nobody has queried yet has no history to show");
}

void TestSimplePanels::historyViewKeepsItsRowsAndSaysAReadFailed()
{
    m_backend.replyWithResult(
        QJsonArray{statement(QStringLiteral("SELECT 1"), QString::fromLatin1(Started), 1, 1)}
    );
    HistoryView view(QString::fromLatin1(ConnID));
    api()->flush(FlushMs);

    m_backend.replyWithError(QString::fromLatin1(NoHistory));
    QPushButton *refresh = buttonWith(view, RefreshText);
    QVERIFY(refresh);
    refresh->click();
    api()->flush(FlushMs);

    QVERIFY2(errorShown(view), "a failed read must say so, not read as a log with nothing in it");
    QCOMPARE(errorText(view), QString::fromLatin1(NoHistory));
    QCOMPARE(rowsOf(view), 1);
}

QTEST_MAIN(TestSimplePanels)

#include "tst_simplepanels.moc"
