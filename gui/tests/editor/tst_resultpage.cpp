#include "editor/resultpage.h"

#include "app/api.h"
#include "app/stubbackend.h"
#include "app/theme.h"
#include "editor/resultgrid.h"
#include "editor/resultmodel.h"

#include <QAbstractItemModel>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QLabel>
#include <QLatin1String>
#include <QLineEdit>
#include <QList>
#include <QObject>
#include <QPushButton>
#include <QSignalSpy>
#include <QStackedWidget>
#include <QString>
#include <QTest>
#include <QWidget>
#include <functional>

namespace
{

constexpr auto FilterPath = "/rpc/query/Filter";
constexpr auto SortPath = "/rpc/query/Sort";
constexpr auto EditInfoPath = "/rpc/query/EditInfo";
constexpr auto ClosePath = "/rpc/query/CloseResult";

// resultpage.cpp debounces keystrokes before each Filter round-trip, so a wait
// for a call that must *not* happen has to outlast it.
constexpr int FilterDebounceMs = 200;

enum class Progress
{
    Streaming,
    Done
};

QJsonObject column(const QString &name, const QString &type)
{
    QJsonObject col;
    col.insert(QStringLiteral("name"), name);
    col.insert(QStringLiteral("type"), type);
    return col;
}

// One key column and one editable column: the shape the edit path needs.
QJsonArray testColumns()
{
    return {
        column(QStringLiteral("id"), QStringLiteral("int")),
        column(QStringLiteral("name"), QStringLiteral("varchar")),
    };
}

// A ResultState as the backend sends one, from Run or from Filter.
QJsonObject resultState(const QString &id, int rowCount, Progress progress = Progress::Done)
{
    QJsonObject state;
    state.insert(QStringLiteral("resultId"), id);
    state.insert(QStringLiteral("done"), progress == Progress::Done);
    state.insert(QStringLiteral("rowCount"), rowCount);
    state.insert(QStringLiteral("totalRows"), rowCount);
    state.insert(QStringLiteral("columns"), testColumns());
    return state;
}

// What the grid would hand stage() for a cell in the "name" column, keyed the
// way buildEdit() keys one.
StagedEdit nameEdit(int row, const QVariant &value)
{
    StagedEdit e;
    e.row = row;
    e.col = 1;
    e.colName = QStringLiteral("name");
    e.key.insert(QStringLiteral("id"), QString::number(row));
    e.value = value;
    return e;
}

// Spins the event loop until the condition holds, or gives up. A loop rather
// than QTRY_VERIFY, which cannot be used from a helper: it returns from its
// own function on failure, which here would only skip the wait.
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

// The calls that went to one RPC method. The model fetches row windows on its
// own schedule, so every assertion picks out the conversation it is about.
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

// The page shows either the grid or the one-line summary, and which one is the
// stack's business — a detail the page keeps to itself.
QWidget *shown(const ResultPage &page)
{
    auto *stack = page.findChild<QStackedWidget *>();
    return stack ? stack->currentWidget() : nullptr;
}

// The Ctrl+F bar's field and its × button, the only two of their kind here.
QLineEdit *filterEdit(const ResultPage &page)
{
    return page.findChild<QLineEdit *>();
}

QPushButton *filterClose(const ResultPage &page)
{
    return page.findChild<QPushButton *>();
}

} // namespace

// What this class adds over the grid and the model beneath it is orchestration:
// folding a ResultState into both, the Filter round-trip, the EditInfo probe,
// and releasing the backend buffer. Sorting, the row-count label and the
// apply/discard of staged edits are the editor tab's, not this class's.
class TestResultPage : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void init();
    void cleanup();

    void aResultStateFillsTheGrid();
    void anErrorStateShowsTheMessageInsteadOfTheGrid();
    void aStatementWithoutRowsSaysSo();
    void streamingGrowthKeepsTheViewInPlace();
    void columnsArrivingLateAreAdopted();
    void aHeaderClickIsRelayedOnlyOnceTheResultIsDone();
    void theModelsStagedCountAndErrorsReachTheTab();
    void closeResultReleasesTheBackendBuffer();
    void aDiscardedPageClosesItsResult();
    void typingAFilterSendsTheTrimmedNeedle();
    void aFilterErrorIsRaisedAndTheViewIsUnchanged();
    void filteringIsRefusedWhileEditsAreStaged();
    void clearingTheFilterIsRefusedWhileEditsAreStaged();
    void clearingTheFilterAsksForTheWholeResult();
    void aStreamingResultIsNotFiltered();
    void editInfoMakesTheNamedColumnsEditable();
    void anEditInfoRefusalLeavesThePageReadOnly();
    void aFailedStatementIsNeverAskedAboutEdits();

private:
    StubBackend m_backend;
};

void TestResultPage::initTestCase()
{
    // The summary label is coloured from theme::current() on every state, so
    // there has to be a palette installed before the first one lands.
    theme::apply(theme::defaultApp, 13);
}

void TestResultPage::init()
{
    QVERIFY(m_backend.listening());
    api()->setEndpoint(m_backend.base(), QString());
    m_backend.clearRequests();
    // A reply that is neither rows nor an error, so a slot only sees what it
    // asked the stub for.
    m_backend.replyWithResult({});
}

void TestResultPage::cleanup()
{
    // A page destroyed as its slot returns posts CloseResult on the way out;
    // draining it here keeps it out of the next slot's requests.
    api()->flush(2000);
}

void TestResultPage::aResultStateFillsTheGrid()
{
    ResultPage page(QStringLiteral("select * from users"));
    QSignalSpy changed(&page, &ResultPage::stateChanged);

    page.applyState(resultState(QStringLiteral("r1"), 42));

    QCOMPARE(page.sql(), QStringLiteral("select * from users"));
    QCOMPARE(page.resultId(), QStringLiteral("r1"));
    QCOMPARE(page.rowCount(), 42);
    QCOMPARE(page.totalRows(), 42);
    QVERIFY(page.done());
    QVERIFY(!page.capped());
    QVERIFY(!page.filtered());
    QVERIFY(page.error().isEmpty());
    QCOMPARE(changed.size(), 1);

    QCOMPARE(page.model()->resultId(), QStringLiteral("r1"));
    QCOMPARE(page.model()->rowCount(), 42);
    QCOMPARE(page.model()->columnCount(), 2);
    QCOMPARE(page.model()->columns().at(0).name, QStringLiteral("id"));
    QCOMPARE(page.model()->columns().at(1).type, QStringLiteral("varchar"));
    QCOMPARE(page.grid()->resultModel(), page.model());

    QCOMPARE(shown(page), static_cast<QWidget *>(page.grid()));
}

void TestResultPage::anErrorStateShowsTheMessageInsteadOfTheGrid()
{
    ResultPage page(QStringLiteral("select * from nope"));
    QJsonObject state = resultState(QStringLiteral("r1"), 0);
    state.insert(QStringLiteral("columns"), QJsonArray());
    state.insert(QStringLiteral("error"), QStringLiteral("Table 'nope' doesn't exist"));

    page.applyState(state);

    QCOMPARE(page.error(), QStringLiteral("Table 'nope' doesn't exist"));
    auto *summary = qobject_cast<QLabel *>(shown(page));
    QVERIFY(summary);
    QCOMPARE(summary->text(), QStringLiteral("Table 'nope' doesn't exist"));
}

void TestResultPage::aStatementWithoutRowsSaysSo()
{
    ResultPage page(QStringLiteral("update users set name = 'x'"));
    QJsonObject state = resultState(QStringLiteral("r1"), 0);
    state.insert(QStringLiteral("columns"), QJsonArray());

    page.applyState(state);

    // A statement with no result set still gets a tab, so the position of
    // every other tab keeps matching the script.
    auto *summary = qobject_cast<QLabel *>(shown(page));
    QVERIFY(summary);
    QCOMPARE(summary->text(), QStringLiteral("Statement executed. No result set."));
    QVERIFY(page.error().isEmpty());
}

void TestResultPage::streamingGrowthKeepsTheViewInPlace()
{
    ResultPage page(QStringLiteral("select 1"));
    page.applyState(resultState(QStringLiteral("r1"), 100, Progress::Streaming));
    QVERIFY(!page.done());

    QSignalSpy inserted(page.model(), &QAbstractItemModel::rowsInserted);
    QSignalSpy reset(page.model(), &QAbstractItemModel::modelReset);

    page.applyState(resultState(QStringLiteral("r1"), 250, Progress::Streaming));

    QCOMPARE(page.rowCount(), 250);
    QCOMPARE(page.model()->rowCount(), 250);
    // Growth is an insert and not a reset, so the view keeps its selection and
    // its scroll position while the result streams in.
    QCOMPARE(inserted.size(), 1);
    QCOMPARE(reset.size(), 0);

    QJsonObject last = resultState(QStringLiteral("r1"), 300);
    last.insert(QStringLiteral("capped"), true);
    page.applyState(last);

    QVERIFY(page.done());
    QVERIFY(page.capped());
}

void TestResultPage::columnsArrivingLateAreAdopted()
{
    ResultPage page(QStringLiteral("select 1"));
    QJsonObject first = resultState(QStringLiteral("r1"), 0, Progress::Streaming);
    first.insert(QStringLiteral("columns"), QJsonArray());

    // Run returns at column-metadata time, which for a streaming result can
    // still be before the columns are known.
    page.applyState(first);
    QCOMPARE(page.model()->columnCount(), 0);
    QCOMPARE(shown(page), static_cast<QWidget *>(page.grid()));

    page.applyState(resultState(QStringLiteral("r1"), 10, Progress::Streaming));

    QCOMPARE(page.model()->columnCount(), 2);
    QCOMPARE(page.model()->rowCount(), 10);
}

void TestResultPage::aHeaderClickIsRelayedOnlyOnceTheResultIsDone()
{
    ResultPage page(QStringLiteral("select 1"));
    QSignalSpy sort(&page, &ResultPage::sortRequested);
    QHeaderView *header = page.grid()->horizontalHeader();

    page.applyState(resultState(QStringLiteral("r1"), 10, Progress::Streaming));
    // Stands in for the click: the grid turns a section click into this.
    emit header->sectionClicked(1);
    QCOMPARE(sort.size(), 0);

    page.applyState(resultState(QStringLiteral("r1"), 10));
    emit header->sectionClicked(1);

    QCOMPARE(sort.size(), 1);
    QCOMPARE(sort.at(0).at(0).toInt(), 1);

    // Relayed, not acted on: the Sort call belongs to the editor tab, which
    // owns the guards and the sort indicator.
    QVERIFY(callsTo(m_backend, SortPath).isEmpty());
}

void TestResultPage::theModelsStagedCountAndErrorsReachTheTab()
{
    ResultPage page(QStringLiteral("select 1"));
    page.applyState(resultState(QStringLiteral("r1"), 10));

    QSignalSpy staged(&page, &ResultPage::stagedChanged);
    QSignalSpy raised(&page, &ResultPage::errorRaised);

    // The toolbar sizes Apply/Discard off this count.
    page.model()->stage(nameEdit(0, QStringLiteral("grace")));
    QCOMPARE(staged.size(), 1);
    QCOMPARE(staged.at(0).at(0).toInt(), 1);

    m_backend.replyWithError(QStringLiteral("result expired"));
    page.model()->data(page.model()->index(0, 0), Qt::DisplayRole); // drives the lazy fetch

    QVERIFY(waitUntil([&raised] { return !raised.isEmpty(); }));
    QCOMPARE(raised.at(0).at(0).toString(), QStringLiteral("result expired"));
}

void TestResultPage::closeResultReleasesTheBackendBuffer()
{
    ResultPage page(QStringLiteral("select 1"));
    page.applyState(resultState(QStringLiteral("r1"), 10));

    page.closeResult();

    QVERIFY(waitUntil([this] { return callsTo(m_backend, ClosePath).size() == 1; }));
    QCOMPARE(callsTo(m_backend, ClosePath).at(0).args, QJsonArray({QStringLiteral("r1")}));
    QVERIFY(page.resultId().isEmpty());

    // The buffer is gone already; releasing it twice would free whatever the
    // backend handed the id to next.
    page.closeResult();
    api()->flush(2000);
    QCOMPARE(callsTo(m_backend, ClosePath).size(), 1);
}

void TestResultPage::aDiscardedPageClosesItsResult()
{
    {
        ResultPage page(QStringLiteral("select 1"));
        page.applyState(resultState(QStringLiteral("r1"), 10));
    }

    // Closing the tab is the common way a result goes away, and nothing else
    // knows the buffer id by then.
    QVERIFY(waitUntil([this] { return callsTo(m_backend, ClosePath).size() == 1; }));
    QCOMPARE(callsTo(m_backend, ClosePath).at(0).args, QJsonArray({QStringLiteral("r1")}));
}

void TestResultPage::typingAFilterSendsTheTrimmedNeedle()
{
    ResultPage page(QStringLiteral("select 1"));
    page.applyState(resultState(QStringLiteral("r1"), 100));

    QJsonObject filtered = resultState(QStringLiteral("r1"), 3);
    filtered.insert(QStringLiteral("totalRows"), 100);
    m_backend.replyWithResult(filtered);

    QLineEdit *edit = filterEdit(page);
    QVERIFY(edit);
    edit->setText(QStringLiteral("  ada  "));

    QVERIFY(waitUntil([this] { return callsTo(m_backend, FilterPath).size() == 1; }));
    QCOMPARE(
        callsTo(m_backend, FilterPath).at(0).args,
        QJsonArray({QStringLiteral("r1"), QStringLiteral("ada")})
    );

    // Filter answers with the new ResultState, and the page folds it in: the
    // row count is now the filtered one, against the untouched total.
    QVERIFY(waitUntil([&page] { return page.filtered(); }));
    QCOMPARE(page.rowCount(), 3);
    QCOMPARE(page.totalRows(), 100);
    QCOMPARE(page.model()->rowCount(), 3);

    // The same needle once the whitespace is off: nothing to ask again.
    edit->setText(QStringLiteral("ada"));
    QTest::qWait(FilterDebounceMs * 3);
    QCOMPARE(callsTo(m_backend, FilterPath).size(), 1);
}

void TestResultPage::aFilterErrorIsRaisedAndTheViewIsUnchanged()
{
    ResultPage page(QStringLiteral("select 1"));
    page.applyState(resultState(QStringLiteral("r1"), 100));
    m_backend.replyWithError(QStringLiteral("filter needs a finished result"));

    QSignalSpy raised(&page, &ResultPage::errorRaised);
    QLineEdit *edit = filterEdit(page);
    QVERIFY(edit);
    edit->setText(QStringLiteral("ada"));

    QVERIFY(waitUntil([&raised] { return !raised.isEmpty(); }));
    QCOMPARE(raised.at(0).at(0).toString(), QStringLiteral("filter needs a finished result"));
    QCOMPARE(callsTo(m_backend, FilterPath).size(), 1);

    // The needle the backend refused is not the view the page is showing.
    QVERIFY(!page.filtered());
    QCOMPARE(page.rowCount(), 100);
}

void TestResultPage::filteringIsRefusedWhileEditsAreStaged()
{
    ResultPage page(QStringLiteral("select 1"));
    page.applyState(resultState(QStringLiteral("r1"), 100));
    page.model()->stage(nameEdit(0, QStringLiteral("grace")));

    QSignalSpy raised(&page, &ResultPage::errorRaised);
    QLineEdit *edit = filterEdit(page);
    QVERIFY(edit);
    edit->setText(QStringLiteral("ada"));

    // Filtering renumbers the rows the staged edits are keyed to, so the user
    // is sent back to Apply or Discard rather than losing them.
    QVERIFY(waitUntil([&raised] { return !raised.isEmpty(); }));
    QCOMPARE(
        raised.at(0).at(0).toString(),
        QStringLiteral("apply or discard the staged edits before filtering")
    );
    QVERIFY(callsTo(m_backend, FilterPath).isEmpty());
    QCOMPARE(page.model()->staged().size(), 1);
}

void TestResultPage::clearingTheFilterIsRefusedWhileEditsAreStaged()
{
    ResultPage page(QStringLiteral("select 1"));
    page.applyState(resultState(QStringLiteral("r1"), 100));

    QJsonObject filtered = resultState(QStringLiteral("r1"), 3);
    filtered.insert(QStringLiteral("totalRows"), 100);
    m_backend.replyWithResult(filtered);

    QLineEdit *edit = filterEdit(page);
    QVERIFY(edit);
    edit->setText(QStringLiteral("ada"));
    QVERIFY(waitUntil([&page] { return page.filtered(); }));

    page.model()->stage(nameEdit(0, QStringLiteral("grace")));
    m_backend.clearRequests();
    QSignalSpy raised(&page, &ResultPage::errorRaised);

    QPushButton *close = filterClose(page);
    QVERIFY(close);
    close->click();

    // Dropping the filter refetches every window, which would orphan the
    // edits, so the bar keeps the needle it refused to clear.
    QCOMPARE(raised.size(), 1);
    QCOMPARE(
        raised.at(0).at(0).toString(),
        QStringLiteral("apply or discard the staged edits before clearing the filter")
    );
    QCOMPARE(edit->text(), QStringLiteral("ada"));
    QVERIFY(page.filtered());
    QVERIFY(callsTo(m_backend, FilterPath).isEmpty());
}

void TestResultPage::clearingTheFilterAsksForTheWholeResult()
{
    ResultPage page(QStringLiteral("select 1"));
    page.applyState(resultState(QStringLiteral("r1"), 100));

    QJsonObject filtered = resultState(QStringLiteral("r1"), 3);
    filtered.insert(QStringLiteral("totalRows"), 100);
    m_backend.replyWithResult(filtered);

    QLineEdit *edit = filterEdit(page);
    QVERIFY(edit);
    edit->setText(QStringLiteral("ada"));
    QVERIFY(waitUntil([&page] { return page.filtered(); }));

    m_backend.replyWithResult(resultState(QStringLiteral("r1"), 100));
    m_backend.clearRequests();

    QPushButton *close = filterClose(page);
    QVERIFY(close);
    close->click();

    // An empty needle is how the unfiltered view is asked for, and it goes out
    // now rather than a debounce later.
    QVERIFY(waitUntil([this] { return callsTo(m_backend, FilterPath).size() == 1; }));
    QCOMPARE(
        callsTo(m_backend, FilterPath).at(0).args, QJsonArray({QStringLiteral("r1"), QString()})
    );
    QVERIFY(waitUntil([&page] { return !page.filtered(); }));
    QCOMPARE(page.rowCount(), 100);
    QVERIFY(edit->text().isEmpty());
}

void TestResultPage::aStreamingResultIsNotFiltered()
{
    ResultPage page(QStringLiteral("select 1"));
    page.applyState(resultState(QStringLiteral("r1"), 10, Progress::Streaming));

    QLineEdit *edit = filterEdit(page);
    QVERIFY(edit);
    edit->setText(QStringLiteral("ada"));
    QTest::qWait(FilterDebounceMs * 3);

    // The backend filters a finished buffer; rows are still arriving into this
    // one.
    QVERIFY(callsTo(m_backend, FilterPath).isEmpty());
    QVERIFY(!page.filtered());
}

void TestResultPage::editInfoMakesTheNamedColumnsEditable()
{
    ResultPage page(QStringLiteral("select id, name from users"));
    page.applyState(resultState(QStringLiteral("r1"), 10));

    QJsonObject info;
    info.insert(QStringLiteral("editable"), true);
    info.insert(QStringLiteral("editableCols"), QJsonArray({QStringLiteral("name")}));
    info.insert(QStringLiteral("keyCols"), QJsonArray({QStringLiteral("id")}));
    info.insert(QStringLiteral("schema"), QStringLiteral("app"));
    info.insert(QStringLiteral("table"), QStringLiteral("users"));
    m_backend.replyWithResult(info);
    m_backend.clearRequests();
    QSignalSpy changed(&page, &ResultPage::stateChanged);

    page.resolveEditability();

    QVERIFY(waitUntil([&page] { return page.editable(); }));
    QCOMPARE(callsTo(m_backend, EditInfoPath).size(), 1);
    QCOMPARE(callsTo(m_backend, EditInfoPath).at(0).args, QJsonArray({QStringLiteral("r1")}));
    QCOMPARE(page.insertSchema(), QStringLiteral("app"));
    QCOMPARE(page.insertTable(), QStringLiteral("users"));
    QCOMPARE(changed.size(), 1);

    // EditInfo names its columns; the page has to turn those names into this
    // result's column order before the model can use them.
    QVERIFY(page.model()->editable());
    QVERIFY(page.model()->flags(page.model()->index(0, 1)) & Qt::ItemIsEditable);
    QVERIFY(!(page.model()->flags(page.model()->index(0, 0)) & Qt::ItemIsEditable));
}

void TestResultPage::anEditInfoRefusalLeavesThePageReadOnly()
{
    ResultPage page(QStringLiteral("select 1 + 1"));
    page.applyState(resultState(QStringLiteral("r1"), 10));

    QJsonObject info;
    info.insert(QStringLiteral("editable"), false);
    m_backend.replyWithResult(info);
    QSignalSpy changed(&page, &ResultPage::stateChanged);
    QSignalSpy raised(&page, &ResultPage::errorRaised);

    page.resolveEditability();
    api()->flush(2000);
    QTest::qWait(50);

    QVERIFY(!page.editable());
    QVERIFY(!page.model()->editable());
    QVERIFY(page.insertTable().isEmpty());
    QCOMPARE(changed.size(), 0);

    m_backend.replyWithError(QStringLiteral("no such result"));
    page.resolveEditability();
    api()->flush(2000);
    QTest::qWait(50);

    // A result that maps to no single table is the normal case, not a failure
    // worth putting in front of the user.
    QVERIFY(!page.editable());
    QCOMPARE(changed.size(), 0);
    QVERIFY(raised.isEmpty());
}

void TestResultPage::aFailedStatementIsNeverAskedAboutEdits()
{
    ResultPage page(QStringLiteral("select * from nope"));
    QJsonObject state = resultState(QStringLiteral("r1"), 0);
    state.insert(QStringLiteral("columns"), QJsonArray());
    state.insert(QStringLiteral("error"), QStringLiteral("Table 'nope' doesn't exist"));
    page.applyState(state);
    m_backend.clearRequests();

    page.resolveEditability();
    api()->flush(2000);

    QVERIFY(callsTo(m_backend, EditInfoPath).isEmpty());
    QVERIFY(!page.editable());
}

QTEST_MAIN(TestResultPage)

#include "tst_resultpage.moc"
