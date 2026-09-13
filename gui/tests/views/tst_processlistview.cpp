#include "views/processlistview.h"

#include "app/api.h"
#include "app/stubbackend.h"
#include "app/theme.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QLatin1Char>
#include <QLatin1String>
#include <QLayout>
#include <QList>
#include <QObject>
#include <QPushButton>
#include <QString>
#include <QStringList>
#include <QTableWidget>
#include <QTest>
#include <QTimer>
#include <QWidget>

// The panel is a poll loop around one table: what a test can reach is the
// cells it filled, the two switches and the interval box above them, the poll
// timer, and the error strip PanelBase owns.
//
// The two KILL actions are out of reach. kill() is private and the only way in
// is the context menu, which ends in QMenu::exec(); the action itself then ends
// in QMessageBox::warning(). Driving either means answering a modal from inside
// its own event loop, and the thing on the far side of it terminates a live
// session, so nothing here goes near it. Show Query is the same shape — only
// its early return, on a row with no statement, is reachable.
namespace
{

constexpr auto ConnID = "conn-1";
constexpr auto RefreshText = "Refresh";
constexpr int FontSize = 13;
constexpr int FlushMs = 5000;

constexpr auto ProcesslistPath = "/rpc/admin/Processlist";

// The row PanelBase parks the error strip in, between the title row and the
// body (pinned by tst_panelbase).
constexpr int ErrorRow = 1;

// The columns makeTable was handed, in order. InfoColumn is the one the panel
// itself names: Show Query reads the statement out of it.
constexpr int IdColumn = 0;
constexpr int UserColumn = 1;
constexpr int HostColumn = 2;
constexpr int DbColumn = 3;
constexpr int CommandColumn = 4;
constexpr int TimeColumn = 5;
constexpr int StateColumn = 6;
constexpr int InfoColumn = 7;

// Mirrors PollChoicesSecs and DefaultPollSecs in processlistview.cpp: the
// interval is what tells the timer apart from outside.
constexpr int IntervalChoices = 5;
constexpr int DefaultIntervalIndex = 1;
constexpr int DefaultPollMs = 2000;

constexpr auto HideSleepingText = "Hide Sleeping";
constexpr auto AutoRefreshText = "Auto Refresh";

constexpr auto Denied = "admin.Processlist: Access denied for user";

// performance_schema.processlist keeps INFO in a fixed 1024-byte buffer and
// clips anything longer, which is why the backend reads information_schema
// first. A statement past that length is what a truncating cell gives away.
constexpr int ClipAtBytes = 1024;
constexpr int LongQueryChars = 3000;

// One client connection, in the shape admin.Processlist answers with
// (backend/internal/admin/service.go). The fields the filter slots do not care
// about are filled in so a blank cell means a dropped value, not a gap.
QJsonObject process(qint64 id, const QString &command, const QString &info = {})
{
    return QJsonObject{
        {"id", id},
        {"user", QStringLiteral("app")},
        {"host", QStringLiteral("10.0.0.9:51000")},
        {"db", QStringLiteral("shop")},
        {"command", command},
        {"time", 0},
        {"state", QStringLiteral("Sending data")},
        {"info", info},
    };
}

QTableWidget *tableOf(const QWidget &view)
{
    return view.findChild<QTableWidget *>();
}

QString cell(const QWidget &view, int row, int column)
{
    const QTableWidget *table = tableOf(view);
    if (!table || !table->item(row, column))
    {
        return {};
    }
    return table->item(row, column)->text();
}

int rowsShown(const QWidget &view)
{
    const QTableWidget *table = tableOf(view);
    return table ? table->rowCount() : -1;
}

QStringList headerLabels(const QTableWidget &table)
{
    QStringList out;
    for (int c = 0; c < table.columnCount(); ++c)
    {
        const QTableWidgetItem *head = table.horizontalHeaderItem(c);
        out << (head ? head->text() : QString());
    }
    return out;
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

QCheckBox *switchWith(const QWidget &view, const char *text)
{
    const QList<QCheckBox *> boxes = view.findChildren<QCheckBox *>();
    for (QCheckBox *box : boxes)
    {
        if (box->text() == QLatin1String(text))
        {
            return box;
        }
    }
    return nullptr;
}

QPushButton *refreshButton(const QWidget &view)
{
    const QList<QPushButton *> buttons = view.findChildren<QPushButton *>();
    for (QPushButton *button : buttons)
    {
        if (button->text() == QLatin1String(RefreshText))
        {
            return button;
        }
    }
    return nullptr;
}

QTimer *timerWith(const QWidget &view, int interval)
{
    const QList<QTimer *> timers = view.findChildren<QTimer *>();
    for (QTimer *timer : timers)
    {
        if (timer->interval() == interval)
        {
            return timer;
        }
    }
    return nullptr;
}

// One poll, run to completion. The Refresh button is the only way in from
// outside, and flush() returns once the reply has been applied.
bool pollOnce(StubBackend &backend, const QWidget &view, const QJsonArray &rows)
{
    QPushButton *button = refreshButton(view);
    if (!button)
    {
        return false;
    }
    backend.replyWithResult(rows);
    button->click();
    api()->flush(FlushMs);
    return true;
}

int pollsSeen(const StubBackend &backend)
{
    int seen = 0;
    for (const StubBackend::Request &req : backend.requests())
    {
        if (req.path == QLatin1String(ProcesslistPath))
        {
            ++seen;
        }
    }
    return seen;
}

// showQuery() ends in a modal exec(), so a double click on a row that has a
// statement cannot be driven from a test. A row without one has to return
// before reaching it: the timer below closes whatever does open, so a
// regression fails here rather than hanging in the dialog's event loop.
bool opensADialog(const QWidget &view, int row)
{
    bool opened = false;
    QTimer::singleShot(
        0, &view,
        [&opened]()
        {
            QWidget *modal = QApplication::activeModalWidget();
            if (!modal)
            {
                return;
            }
            opened = true;
            modal->close();
        }
    );

    QMetaObject::invokeMethod(tableOf(view), "cellDoubleClicked", Q_ARG(int, row), Q_ARG(int, 0));
    // Drains the timer above when nothing blocked long enough to run it.
    QCoreApplication::processEvents();
    return opened;
}

} // namespace

class TestProcesslistView : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void init();

    void theListAsksTheBackendForOneConnection();
    void aRowIsRenderedColumnForColumn();
    void aProcessMissingEveryOptionalFieldStillGetsARow();

    void theWholeStatementReachesTheInfoCell();
    void aDoubleClickOnARowWithNoStatementOpensNothing();

    void hideSleepingDropsOnlySleepingThreads_data();
    void hideSleepingDropsOnlySleepingThreads();

    void thePollTimerRunsAtTwoSecondsByDefault();
    void theIntervalChoiceSetsThePollPeriod_data();
    void theIntervalChoiceSetsThePollPeriod();

    void thePanelOpensPausedSoNothingPollsUnasked();
    void clearingAutoRefreshStopsThePolling();
    void checkingAutoRefreshPollsStraightAway();
    void aTickIsSkippedWhileAPollIsStillInFlight();

    void aFailedPollSurfacesInsteadOfBlankingTheList();

private:
    StubBackend m_backend;
};

void TestProcesslistView::initTestCase()
{
    // The switches, the table and the error strip all build themselves from
    // theme::current(); apply() is what installs it.
    theme::apply(theme::defaultApp, FontSize);
}

void TestProcesslistView::init()
{
    m_backend.clearRequests();
    QVERIFY(m_backend.listening());
    api()->setEndpoint(m_backend.base(), QString());
}

void TestProcesslistView::theListAsksTheBackendForOneConnection()
{
    m_backend.replyWithResult(QJsonArray{});
    ProcesslistView view(QString::fromLatin1(ConnID));
    api()->flush(FlushMs);

    QCOMPARE(pollsSeen(m_backend), 1);
    const StubBackend::Request &req = m_backend.requests().at(0);
    QCOMPARE(req.verb, QStringLiteral("POST"));
    QCOMPARE(req.path, QString::fromLatin1(ProcesslistPath));
    QCOMPARE(req.args, QJsonArray({QString::fromLatin1(ConnID)}));

    // The column order is a contract, not a layout choice: showQuery() and the
    // kill actions read the id, the user, the host and the statement out of
    // fixed columns.
    const QTableWidget *table = tableOf(view);
    QVERIFY(table);
    QCOMPARE(
        headerLabels(*table),
        QStringList(
            {QStringLiteral("Id"), QStringLiteral("User"), QStringLiteral("Host"),
             QStringLiteral("DB"), QStringLiteral("Command"), QStringLiteral("Time"),
             QStringLiteral("State"), QStringLiteral("Info")}
        )
    );
    QCOMPARE(rowsShown(view), 0);
}

void TestProcesslistView::aRowIsRenderedColumnForColumn()
{
    m_backend.replyWithResult(QJsonArray{QJsonObject{
        {"id", 4711},
        {"user", QStringLiteral("app")},
        {"host", QStringLiteral("10.0.0.9:51000")},
        {"db", QStringLiteral("shop")},
        {"command", QStringLiteral("Query")},
        {"time", 12},
        {"state", QStringLiteral("Sending data")},
        {"info", QStringLiteral("SELECT * FROM orders")},
    }});
    ProcesslistView view(QString::fromLatin1(ConnID));
    api()->flush(FlushMs);

    QCOMPARE(rowsShown(view), 1);
    QCOMPARE(cell(view, 0, IdColumn), QStringLiteral("4711"));
    QCOMPARE(cell(view, 0, UserColumn), QStringLiteral("app"));
    QCOMPARE(cell(view, 0, HostColumn), QStringLiteral("10.0.0.9:51000"));
    QCOMPARE(cell(view, 0, DbColumn), QStringLiteral("shop"));
    QCOMPARE(cell(view, 0, CommandColumn), QStringLiteral("Query"));
    QCOMPARE(cell(view, 0, TimeColumn), QStringLiteral("12"));
    QCOMPARE(cell(view, 0, StateColumn), QStringLiteral("Sending data"));
    QCOMPARE(cell(view, 0, InfoColumn), QStringLiteral("SELECT * FROM orders"));

    // The id and the time are sortable numbers rather than text, or a thread
    // list sorts 1000 before 999.
    const QTableWidget *table = tableOf(view);
    QVERIFY(table);
    QCOMPARE(table->item(0, IdColumn)->data(Qt::DisplayRole).toLongLong(), 4711);
    QCOMPARE(table->item(0, TimeColumn)->data(Qt::DisplayRole).toLongLong(), 12);
}

void TestProcesslistView::aProcessMissingEveryOptionalFieldStillGetsARow()
{
    // The backend coalesces the nullable columns, but a reply that omits them
    // has to read as a blank cell rather than drop the connection from view.
    m_backend.replyWithResult(QJsonArray{QJsonObject{{"id", 8}}});
    ProcesslistView view(QString::fromLatin1(ConnID));
    api()->flush(FlushMs);

    QCOMPARE(rowsShown(view), 1);
    QCOMPARE(cell(view, 0, IdColumn), QStringLiteral("8"));
    QCOMPARE(cell(view, 0, TimeColumn), QStringLiteral("0"));
    QVERIFY(cell(view, 0, UserColumn).isEmpty());
    QVERIFY(cell(view, 0, HostColumn).isEmpty());
    QVERIFY(cell(view, 0, DbColumn).isEmpty());
    QVERIFY(cell(view, 0, CommandColumn).isEmpty());
    QVERIFY(cell(view, 0, StateColumn).isEmpty());
    QVERIFY(cell(view, 0, InfoColumn).isEmpty());
}

void TestProcesslistView::theWholeStatementReachesTheInfoCell()
{
    // The cell elides on screen and the column is width-capped, but the item
    // itself has to hold the whole statement: it is what Show Query opens and
    // what the tooltip hovers, and the backend went to information_schema
    // precisely so nothing arrives pre-truncated.
    const QString sql =
        QStringLiteral("SELECT * FROM orders WHERE note = '") + QString(LongQueryChars, u'x');
    QVERIFY2(sql.size() > ClipAtBytes, "the statement has to outrun performance_schema's buffer");

    m_backend.replyWithResult(QJsonArray{process(21, QStringLiteral("Query"), sql)});
    ProcesslistView view(QString::fromLatin1(ConnID));
    api()->flush(FlushMs);

    QCOMPARE(rowsShown(view), 1);
    QCOMPARE(cell(view, 0, InfoColumn), sql);

    const QTableWidget *table = tableOf(view);
    QVERIFY(table);
    QCOMPARE(table->item(0, InfoColumn)->toolTip(), sql);
}

void TestProcesslistView::aDoubleClickOnARowWithNoStatementOpensNothing()
{
    // An idle connection has no INFO. The context menu greys Show Query out
    // for it, but a double click reaches showQuery() with nothing in the way,
    // so the guard it returns on is the only thing between the user and an
    // empty dialog.
    m_backend.replyWithResult(QJsonArray{process(3, QStringLiteral("Query"))});
    ProcesslistView view(QString::fromLatin1(ConnID));
    api()->flush(FlushMs);

    QCOMPARE(rowsShown(view), 1);
    QVERIFY(cell(view, 0, InfoColumn).isEmpty());
    QVERIFY(!opensADialog(view, 0));
}

void TestProcesslistView::hideSleepingDropsOnlySleepingThreads_data()
{
    QTest::addColumn<QString>("command");
    QTest::addColumn<int>("shown");

    QTest::newRow("sleep") << QStringLiteral("Sleep") << 0;
    QTest::newRow("lowercase") << QStringLiteral("sleep") << 0;
    QTest::newRow("shouted") << QStringLiteral("SLEEP") << 0;
    // The match is the whole command, not a prefix: a state that merely starts
    // with the word is a working connection.
    QTest::newRow("sleeping") << QStringLiteral("Sleeping") << 1;
    QTest::newRow("query") << QStringLiteral("Query") << 1;
    QTest::newRow("daemon") << QStringLiteral("Daemon") << 1;
    QTest::newRow("nothing") << QString() << 1;
}

void TestProcesslistView::hideSleepingDropsOnlySleepingThreads()
{
    QFETCH(QString, command);
    QFETCH(int, shown);

    m_backend.replyWithResult(QJsonArray{process(1, command)});
    ProcesslistView view(QString::fromLatin1(ConnID));
    api()->flush(FlushMs);

    QCheckBox *hide = switchWith(view, HideSleepingText);
    QVERIFY(hide);
    QVERIFY2(hide->isChecked(), "the panel opens with the idle clients out of the way");
    QCOMPARE(rowsShown(view), shown);

    // The filter runs over the reply rather than over the rows already drawn,
    // so clearing it has to go back to the server. The stub keeps answering
    // with the same list.
    hide->setChecked(false);
    api()->flush(FlushMs);
    QCOMPARE(pollsSeen(m_backend), 2);
    QCOMPARE(rowsShown(view), 1);
}

void TestProcesslistView::thePollTimerRunsAtTwoSecondsByDefault()
{
    m_backend.replyWithResult(QJsonArray{});
    ProcesslistView view(QString::fromLatin1(ConnID));
    api()->flush(FlushMs);

    const QComboBox *interval = view.findChild<QComboBox *>();
    QVERIFY(interval);
    QCOMPARE(interval->count(), IntervalChoices);
    QCOMPARE(interval->currentIndex(), DefaultIntervalIndex);

    QTimer *timer = timerWith(view, DefaultPollMs);
    QVERIFY2(timer, "the list is polled on a two second timer");
    QVERIFY(timer->isActive());

    // Polling is opt-in, so the interval is only observable once it is on.
    QCheckBox *autoRefresh = switchWith(view, AutoRefreshText);
    QVERIFY(autoRefresh);
    autoRefresh->setChecked(true);
    api()->flush(FlushMs);

    // Nothing ticks a timer in a test that never sits in an event loop for two
    // seconds: emit the timeout rather than wait for it.
    m_backend.clearRequests();
    m_backend.replyWithResult(QJsonArray{process(9, QStringLiteral("Query"))});
    QMetaObject::invokeMethod(timer, "timeout");
    api()->flush(FlushMs);

    QCOMPARE(pollsSeen(m_backend), 1);
    QCOMPARE(m_backend.requests().at(0).args, QJsonArray({QString::fromLatin1(ConnID)}));
    QCOMPARE(rowsShown(view), 1);
}

void TestProcesslistView::theIntervalChoiceSetsThePollPeriod_data()
{
    QTest::addColumn<int>("index");
    QTest::addColumn<QString>("label");
    QTest::addColumn<int>("periodMs");

    QTest::newRow("1s") << 0 << QStringLiteral("Every 1 s") << 1000;
    QTest::newRow("2s") << 1 << QStringLiteral("Every 2 s") << 2000;
    QTest::newRow("5s") << 2 << QStringLiteral("Every 5 s") << 5000;
    QTest::newRow("10s") << 3 << QStringLiteral("Every 10 s") << 10000;
    QTest::newRow("30s") << 4 << QStringLiteral("Every 30 s") << 30000;
}

void TestProcesslistView::theIntervalChoiceSetsThePollPeriod()
{
    QFETCH(int, index);
    QFETCH(QString, label);
    QFETCH(int, periodMs);

    m_backend.replyWithResult(QJsonArray{});
    ProcesslistView view(QString::fromLatin1(ConnID));
    api()->flush(FlushMs);

    QComboBox *interval = view.findChild<QComboBox *>();
    QVERIFY(interval);
    QCOMPARE(interval->itemText(index), label);
    QCOMPARE(interval->itemData(index).toInt(), periodMs);

    // The period rides on the item's data, not on its position, so a reordered
    // list would still have to poll at what it says.
    interval->setCurrentIndex(index);
    QVERIFY2(timerWith(view, periodMs), "the chosen period is what the timer runs at");
}

void TestProcesslistView::thePanelOpensPausedSoNothingPollsUnasked()
{
    // Polling is opt-in: a tab opened and left alone must not keep asking the
    // server for a list nobody is reading. The one snapshot on open stays, so
    // the panel still has rows to show.
    m_backend.replyWithResult(QJsonArray{process(9, QStringLiteral("Query"))});
    ProcesslistView view(QString::fromLatin1(ConnID));
    api()->flush(FlushMs);

    QCOMPARE(pollsSeen(m_backend), 1);
    QCOMPARE(rowsShown(view), 1);

    QCheckBox *autoRefresh = switchWith(view, AutoRefreshText);
    QVERIFY(autoRefresh);
    QVERIFY2(!autoRefresh->isChecked(), "the panel opens paused");

    QComboBox *interval = view.findChild<QComboBox *>();
    QVERIFY(interval);
    QVERIFY2(!interval->isEnabled(), "a period with nothing polling on it is a dead control");

    // Ticking the timer proves the pause is real rather than a label.
    m_backend.clearRequests();
    QTimer *timer = timerWith(view, DefaultPollMs);
    QVERIFY(timer);
    QMetaObject::invokeMethod(timer, "timeout");
    api()->flush(FlushMs);
    QCOMPARE(pollsSeen(m_backend), 0);
}

void TestProcesslistView::clearingAutoRefreshStopsThePolling()
{
    m_backend.replyWithResult(QJsonArray{});
    ProcesslistView view(QString::fromLatin1(ConnID));
    api()->flush(FlushMs);

    QCheckBox *autoRefresh = switchWith(view, AutoRefreshText);
    QVERIFY(autoRefresh);
    autoRefresh->setChecked(true);
    api()->flush(FlushMs);
    QWidget *interval = view.findChild<QComboBox *>();
    QVERIFY(interval);
    QVERIFY(interval->isEnabled());

    m_backend.clearRequests();
    autoRefresh->setChecked(false);
    api()->flush(FlushMs);
    QCOMPARE(pollsSeen(m_backend), 0);
    QVERIFY2(!interval->isEnabled(), "a period with nothing polling on it is a dead control");

    QTimer *timer = timerWith(view, DefaultPollMs);
    QVERIFY(timer);
    QMetaObject::invokeMethod(timer, "timeout");
    api()->flush(FlushMs);
    QCOMPARE(pollsSeen(m_backend), 0);

    // The switch is read inside the tick rather than stopping the timer, so
    // checking it again resumes the polling on its own.
    QVERIFY(timer->isActive());
}

void TestProcesslistView::checkingAutoRefreshPollsStraightAway()
{
    m_backend.replyWithResult(QJsonArray{});
    ProcesslistView view(QString::fromLatin1(ConnID));
    api()->flush(FlushMs);

    QCheckBox *autoRefresh = switchWith(view, AutoRefreshText);
    QVERIFY(autoRefresh);
    autoRefresh->setChecked(false);
    api()->flush(FlushMs);

    // Waiting out an interval before the first row appears would read as a
    // panel that did not notice the switch.
    m_backend.clearRequests();
    m_backend.replyWithResult(QJsonArray{process(9, QStringLiteral("Query"))});
    autoRefresh->setChecked(true);
    api()->flush(FlushMs);

    QCOMPARE(pollsSeen(m_backend), 1);
    QCOMPARE(rowsShown(view), 1);
    QVERIFY(view.findChild<QComboBox *>()->isEnabled());

    QTimer *timer = timerWith(view, DefaultPollMs);
    QVERIFY(timer);
    QMetaObject::invokeMethod(timer, "timeout");
    api()->flush(FlushMs);
    QCOMPARE(pollsSeen(m_backend), 2);
}

void TestProcesslistView::aTickIsSkippedWhileAPollIsStillInFlight()
{
    m_backend.replyWithResult(QJsonArray{});
    ProcesslistView view(QString::fromLatin1(ConnID));
    api()->flush(FlushMs);

    QCheckBox *autoRefresh = switchWith(view, AutoRefreshText);
    QVERIFY(autoRefresh);
    autoRefresh->setChecked(true);
    api()->flush(FlushMs);

    QTimer *timer = timerWith(view, DefaultPollMs);
    QVERIFY(timer);
    m_backend.clearRequests();

    // Two ticks with no reply read in between: a stalled backend must not
    // build a queue of polls whose replies then land milliseconds apart and
    // redraw the table several times over.
    QMetaObject::invokeMethod(timer, "timeout");
    QMetaObject::invokeMethod(timer, "timeout");
    api()->flush(FlushMs);
    QCOMPARE(pollsSeen(m_backend), 1);

    // The reply releases the guard rather than leaving it standing.
    QMetaObject::invokeMethod(timer, "timeout");
    api()->flush(FlushMs);
    QCOMPARE(pollsSeen(m_backend), 2);
}

void TestProcesslistView::aFailedPollSurfacesInsteadOfBlankingTheList()
{
    m_backend.replyWithResult(QJsonArray{process(5, QStringLiteral("Query"))});
    ProcesslistView view(QString::fromLatin1(ConnID));
    api()->flush(FlushMs);
    QVERIFY(!errorShown(view));
    QCOMPARE(rowsShown(view), 1);

    m_backend.replyWithError(QString::fromLatin1(Denied));
    QPushButton *button = refreshButton(view);
    QVERIFY(button);
    button->click();
    api()->flush(FlushMs);

    QVERIFY2(errorShown(view), "a failed poll must say so, not read as every client gone");
    const QLabel *strip = errorStrip(view);
    QVERIFY(strip);
    QCOMPARE(strip->text(), QString::fromLatin1(Denied));

    // The rows that did arrive stay: emptying the table would read as an idle
    // server rather than a poll that never landed.
    QCOMPARE(rowsShown(view), 1);
    QCOMPARE(cell(view, 0, IdColumn), QStringLiteral("5"));

    // The failure also has to release the in-flight guard, or the panel never
    // polls again.
    QVERIFY(pollOnce(m_backend, view, QJsonArray{process(5, QStringLiteral("Query"))}));
    QVERIFY(!errorShown(view));
    QVERIFY(strip->text().isEmpty());
}

QTEST_MAIN(TestProcesslistView)

#include "tst_processlistview.moc"
