#include "editor/editortab.h"

#include "app/api.h"
#include "app/stubbackend.h"
#include "app/theme.h"
#include "editor/resultgrid.h"
#include "editor/resultmodel.h"
#include "editor/resultpage.h"
#include "editor/sqleditor.h"

#include <QAbstractItemModel>
#include <QComboBox>
#include <QFontMetricsF>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QLabel>
#include <QLatin1String>
#include <QLineEdit>
#include <QList>
#include <QObject>
#include <QPalette>
#include <QPushButton>
#include <QSignalSpy>
#include <QString>
#include <QTabWidget>
#include <QTest>
#include <QTextCursor>
#include <QTextEdit>
#include <QVariant>
#include <QWidget>
#include <functional>

namespace
{

constexpr auto ConnID = "c1";
constexpr auto TabID = "t1";
constexpr auto ResultID = "r1";

constexpr auto RunPath = "/rpc/query/Run";
constexpr auto SortPath = "/rpc/query/Sort";
constexpr auto CancelPath = "/rpc/query/Cancel";
constexpr auto PreviewPath = "/rpc/query/PreviewEdits";
constexpr auto ApplyPath = "/rpc/query/ApplyEdits";

// What a run fetches while the limit dropdown sits on "Use Global Limit" and
// no preference has narrowed it.
constexpr int DefaultRowLimit = 50000;

// The editor emits documentSettled 300ms after the buffer changes and the tab
// lints on it, so a slot has to let that one call past before the request list
// can stand for what its own action sent.
constexpr int SettleMs = 400;

// Three statements, so the queue has somewhere to go after the first.
constexpr auto Script = "select 1;\nselect 2;\nselect 3";

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

// Run answers {state: ResultState} and the State poll behind it answers a bare
// ResultState. One canned body carries both shapes, so a whole run round-trip
// needs no timing on the stub's side.
QJsonObject runReply(const QJsonObject &state)
{
    QJsonObject reply = state;
    reply.insert(QStringLiteral("state"), state);
    return reply;
}

QJsonArray runArgs(const QString &stmt, int limit)
{
    return {QString::fromLatin1(ConnID), QString::fromLatin1(TabID), stmt, limit};
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

// The calls that went to one RPC method. A connected tab also lints, loads its
// completion schema and fetches row windows, so every assertion picks out the
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

// Everything on the button strip is a direct child of the widget the editor
// shares. The result pages below carry buttons and labels of their own, so
// every lookup starts here to leave them out.
QWidget *topPane(const EditorTab &tab)
{
    SqlEditor *editor = tab.findChild<SqlEditor *>();
    return editor ? editor->parentWidget() : nullptr;
}

// Positional, because the text is no handle: Apply renames itself with the
// staged count and Explain Analyze renames itself while it is armed.
enum class Button
{
    Run,
    RunStatement,
    Explain,
    Analyze,
    Format,
    Cancel,
    Export,
    Apply,
    Discard
};

QPushButton *button(const EditorTab &tab, Button which)
{
    QWidget *pane = topPane(tab);
    return pane ? pane->findChildren<QPushButton *>(QString(), Qt::FindDirectChildrenOnly)
                      .value(int(which))
                : nullptr;
}

// Same ordering rule: the keyboard hint and the row count, then the note, then
// the error strip under the bar.
enum class Readout
{
    Hint,
    Rows,
    Note,
    Error
};

QLabel *label(const EditorTab &tab, Readout which)
{
    QWidget *pane = topPane(tab);
    return pane ? pane->findChildren<QLabel *>(QString(), Qt::FindDirectChildrenOnly)
                      .value(int(which))
                : nullptr;
}

QString errorText(const EditorTab &tab)
{
    QLabel *strip = label(tab, Readout::Error);
    return strip ? strip->text() : QString();
}

// The note is elided to the width the strip can spare, so the tooltip is the
// only place the whole message survives.
QString noteText(const EditorTab &tab)
{
    QLabel *note = label(tab, Readout::Note);
    return note ? note->toolTip() : QString();
}

QTabWidget *resultTabs(const EditorTab &tab)
{
    return tab.findChild<QTabWidget *>();
}

// Runs the buffer and waits for the queue to drain. Returns the page the
// toolbar acts on, or nullptr if the run never finished.
ResultPage *runToCompletion(EditorTab &tab, const StubBackend &backend)
{
    QPushButton *run = button(tab, Button::Run);
    QTabWidget *tabs = resultTabs(tab);
    if (!run || !tabs)
    {
        return nullptr;
    }
    run->click();
    const bool finished =
        waitUntil([&backend, run]
                  { return !callsTo(backend, RunPath).isEmpty() && run->isEnabled(); });
    return finished ? qobject_cast<ResultPage *>(tabs->currentWidget()) : nullptr;
}

} // namespace

// The tab owns everything a single result page cannot: the statement queue and
// its Run/State conversation, the write path (the edit payload, the sort and
// the guards in front of both), the row-count readout, and the client-side
// placeholder lint.
class TestEditorTab : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void init();
    void cleanup();

    void runStatementSendsTheStatementUnderTheCursor();
    void runSendsEveryStatementInTheBufferInOrder();
    void aSelectionNarrowsTheRunToWhatIsSelected();
    void aDisconnectedTabSendsNothing();
    void theRowLimitFollowsTheDropdownThenThePreference();
    void aFailedStatementStopsTheRestOfTheScript();
    void cancelStopsTheStatementInFlight();
    void cancelLeavesTheTabRunnableAgain();
    void theRowCountTextNamesWhatTheResultIs();
    void aFilteredResultCountsAgainstTheTotal();
    void stagingAnEditOffersApplyAndDiscard();
    void theEditPayloadNamesTheKeyTheColumnAndTheValue();
    void discardDropsTheEditsWithoutTellingTheServer();
    void aHeaderClickSortsAndTheNextOneReversesIt();
    void sortingIsRefusedWhileEditsAreStaged();
    void aPlaceholderIsFlaggedWithoutAskingTheServer();
    void aQuestionMarkInAStringOrACommentIsNotAPlaceholder();
    void editorPrefsReachTheEditor();

private:
    StubBackend m_backend;
};

void TestEditorTab::initTestCase()
{
    // The tab renders its button icons and its error strip from theme::current()
    // in the constructor, so a palette has to be installed before the first one.
    theme::apply(theme::defaultApp, 13);
}

void TestEditorTab::init()
{
    QVERIFY(m_backend.listening());
    api()->setEndpoint(m_backend.base(), QString());
    // A finished 42-row result, for Run and for the State poll behind it.
    m_backend.replyWithResult(runReply(resultState(QString::fromLatin1(ResultID), 42)));
    m_backend.clearRequests();
}

void TestEditorTab::cleanup()
{
    // A tab destroyed as its slot returns posts CloseResult and CloseTab on the
    // way out; draining them here keeps them out of the next slot's requests.
    api()->flush(2000);
}

void TestEditorTab::runStatementSendsTheStatementUnderTheCursor()
{
    EditorTab tab(
        QString::fromLatin1(ConnID), QString::fromLatin1(TabID), QString::fromLatin1(Script)
    );
    tab.setConnected(true);
    SqlEditor *editor = tab.findChild<SqlEditor *>();
    QVERIFY(editor);

    const int insideThirdStatement =
        int(editor->toPlainText().indexOf(QStringLiteral("select 3"))) + 3;
    QTextCursor cursor = editor->textCursor();
    cursor.setPosition(insideThirdStatement);
    editor->setTextCursor(cursor);
    m_backend.clearRequests();

    QPushButton *runStmt = button(tab, Button::RunStatement);
    QVERIFY(runStmt);
    runStmt->click();

    QVERIFY(waitUntil([this] { return callsTo(m_backend, RunPath).size() == 1; }));
    // The delimiter and the surrounding blanks are shaved off, and the two
    // statements the cursor is not on are not sent at all.
    QCOMPARE(
        callsTo(m_backend, RunPath).at(0).args, runArgs(QStringLiteral("select 3"), DefaultRowLimit)
    );
    QCOMPARE(resultTabs(tab)->count(), 1);
}

void TestEditorTab::runSendsEveryStatementInTheBufferInOrder()
{
    EditorTab tab(
        QString::fromLatin1(ConnID), QString::fromLatin1(TabID), QString::fromLatin1(Script)
    );
    tab.setConnected(true);
    QSignalSpy status(&tab, &EditorTab::statusMessage);
    m_backend.clearRequests();

    QVERIFY(runToCompletion(tab, m_backend));

    // One session, so USE and SET carry: the statements go out one after the
    // other rather than at once, each on the same connID+tabID.
    const QList<StubBackend::Request> runs = callsTo(m_backend, RunPath);
    QCOMPARE(runs.size(), 3);
    QCOMPARE(runs.at(0).args, runArgs(QStringLiteral("select 1"), DefaultRowLimit));
    QCOMPARE(runs.at(1).args, runArgs(QStringLiteral("select 2"), DefaultRowLimit));
    QCOMPARE(runs.at(2).args, runArgs(QStringLiteral("select 3"), DefaultRowLimit));

    // Numbered to match the script, so a tab position still names a statement.
    QCOMPARE(resultTabs(tab)->count(), 3);
    QCOMPARE(resultTabs(tab)->tabText(0), QStringLiteral("Result 1"));
    QCOMPARE(resultTabs(tab)->tabText(2), QStringLiteral("Result 3"));
    QCOMPARE(resultTabs(tab)->tabToolTip(1), QStringLiteral("select 2"));

    QCOMPARE(noteText(tab), QStringLiteral("ran 3 statements"));
    QCOMPARE(status.size(), 3);
    QCOMPARE(status.at(2).at(0).toString(), QStringLiteral("42 rows"));
}

void TestEditorTab::aSelectionNarrowsTheRunToWhatIsSelected()
{
    EditorTab tab(
        QString::fromLatin1(ConnID), QString::fromLatin1(TabID), QString::fromLatin1(Script)
    );
    tab.setConnected(true);
    SqlEditor *editor = tab.findChild<SqlEditor *>();
    QVERIFY(editor);

    const QString selected = QStringLiteral("select 2");
    const int from = int(editor->toPlainText().indexOf(selected));
    QTextCursor cursor = editor->textCursor();
    cursor.setPosition(from);
    cursor.setPosition(from + int(selected.size()), QTextCursor::KeepAnchor);
    editor->setTextCursor(cursor);
    m_backend.clearRequests();

    QVERIFY(runToCompletion(tab, m_backend));

    // Run means the selection, or the whole buffer when there is none. A
    // selection that ran the rest of the script would be a disaster here.
    QCOMPARE(callsTo(m_backend, RunPath).size(), 1);
    QCOMPARE(
        callsTo(m_backend, RunPath).at(0).args, runArgs(QStringLiteral("select 2"), DefaultRowLimit)
    );
    QCOMPARE(noteText(tab), QStringLiteral("ran 1 statement"));
}

void TestEditorTab::aDisconnectedTabSendsNothing()
{
    EditorTab tab(
        QString::fromLatin1(ConnID), QString::fromLatin1(TabID), QString::fromLatin1(Script)
    );
    m_backend.clearRequests();

    QPushButton *run = button(tab, Button::Run);
    QVERIFY(run);
    run->click();
    api()->flush(2000);
    QTest::qWait(50);

    // There is no session to run on, so the queue never starts and no result
    // tab is opened for a statement that was never sent.
    QVERIFY(callsTo(m_backend, RunPath).isEmpty());
    QCOMPARE(resultTabs(tab)->count(), 0);
}

void TestEditorTab::theRowLimitFollowsTheDropdownThenThePreference()
{
    EditorTab tab(
        QString::fromLatin1(ConnID), QString::fromLatin1(TabID), QStringLiteral("select 1")
    );
    tab.setConnected(true);
    QComboBox *limit = tab.findChild<QComboBox *>();
    QVERIFY(limit);
    QCOMPARE(limit->currentData().toInt(), 0); // "Use Global Limit"

    limit->setCurrentIndex(1); // "Limit 100"
    m_backend.clearRequests();
    QVERIFY(runToCompletion(tab, m_backend));
    QCOMPARE(callsTo(m_backend, RunPath).size(), 1);
    QCOMPARE(callsTo(m_backend, RunPath).at(0).args.at(3).toInt(), 100);

    limit->setCurrentIndex(limit->count() - 1);
    tab.setDefaultRowLimit(0);
    m_backend.clearRequests();
    QVERIFY(runToCompletion(tab, m_backend));

    // A global limit of 0 means unlimited, and -1 is how the backend is told
    // to fetch without a cap. Passing the 0 straight through would fetch
    // nothing at all.
    QCOMPARE(callsTo(m_backend, RunPath).size(), 1);
    QCOMPARE(callsTo(m_backend, RunPath).at(0).args.at(3).toInt(), -1);
}

void TestEditorTab::aFailedStatementStopsTheRestOfTheScript()
{
    EditorTab tab(
        QString::fromLatin1(ConnID), QString::fromLatin1(TabID), QString::fromLatin1(Script)
    );
    tab.setConnected(true);
    m_backend.replyWithError(QStringLiteral("Table 'nope' doesn't exist"));
    m_backend.clearRequests();

    QPushButton *run = button(tab, Button::Run);
    QVERIFY(run);
    run->click();
    QVERIFY(waitUntil([this, run]
                      { return !callsTo(m_backend, RunPath).isEmpty() && run->isEnabled(); }));

    // Later statements in a script usually assume the earlier ones worked, so
    // the queue stops where it broke instead of running on.
    QCOMPARE(callsTo(m_backend, RunPath).size(), 1);
    QCOMPARE(resultTabs(tab)->count(), 1);
    QCOMPARE(errorText(tab), QStringLiteral("Table 'nope' doesn't exist"));
    QCOMPARE(noteText(tab), QStringLiteral("stopped at statement 1 of 3"));

    QPushButton *cancel = button(tab, Button::Cancel);
    QVERIFY(cancel);
    QVERIFY(!cancel->isVisibleTo(&tab));
}

void TestEditorTab::cancelStopsTheStatementInFlight()
{
    EditorTab tab(
        QString::fromLatin1(ConnID), QString::fromLatin1(TabID), QString::fromLatin1(Script)
    );
    tab.setConnected(true);
    // Rows are still arriving, which is the only state Cancel exists for.
    m_backend.replyWithResult(
        runReply(resultState(QString::fromLatin1(ResultID), 10, Progress::Streaming))
    );
    m_backend.clearRequests();

    QPushButton *run = button(tab, Button::Run);
    QPushButton *cancel = button(tab, Button::Cancel);
    QVERIFY(run && cancel);
    QVERIFY(!cancel->isVisibleTo(&tab));

    run->click();
    QVERIFY(waitUntil(
        [&tab, this]
        {
            ResultPage *page = qobject_cast<ResultPage *>(resultTabs(tab)->currentWidget());
            return !callsTo(m_backend, RunPath).isEmpty() && page && !page->resultId().isEmpty();
        }
    ));
    QVERIFY(!run->isEnabled());
    QVERIFY(cancel->isVisibleTo(&tab));

    m_backend.clearRequests();
    cancel->click();

    // Cancel names the buffer the statement is filling; the queue behind it is
    // dropped rather than carried on with.
    QVERIFY(waitUntil([this] { return callsTo(m_backend, CancelPath).size() == 1; }));
    QCOMPARE(
        callsTo(m_backend, CancelPath).at(0).args, QJsonArray({QString::fromLatin1(ResultID)})
    );
}

void TestEditorTab::cancelLeavesTheTabRunnableAgain()
{
    // Cancel used to clear the queue and leave m_running standing, so Run
    // stayed disabled and Cancel stayed on screen with no way back. Recovery
    // depended on the next State poll, and the Cancel call's own failure is
    // never checked, so a tab could be stranded for good.
    EditorTab tab(
        QString::fromLatin1(ConnID), QString::fromLatin1(TabID), QString::fromLatin1(Script)
    );
    tab.setConnected(true);
    m_backend.replyWithResult(
        runReply(resultState(QString::fromLatin1(ResultID), 10, Progress::Streaming))
    );
    m_backend.clearRequests();

    QPushButton *run = button(tab, Button::Run);
    QPushButton *cancel = button(tab, Button::Cancel);
    QVERIFY(run && cancel);

    run->click();
    QVERIFY(waitUntil(
        [&tab, this]
        {
            ResultPage *page = qobject_cast<ResultPage *>(resultTabs(tab)->currentWidget());
            return !callsTo(m_backend, RunPath).isEmpty() && page && !page->resultId().isEmpty();
        }
    ));
    QVERIFY(!run->isEnabled());
    QVERIFY(cancel->isVisibleTo(&tab));

    cancel->click();

    QVERIFY2(run->isEnabled(), "a cancelled tab has to be runnable without waiting for a poll");
    QVERIFY2(
        !cancel->isVisibleTo(&tab), "nothing is in flight, so there is nothing left to cancel"
    );

    // And it says it was cancelled rather than claiming the statement ran.
    QVERIFY2(!noteText(tab).contains(QStringLiteral("ran")), qPrintable(noteText(tab)));
    QCOMPARE(noteText(tab), QStringLiteral("cancelled"));
}

void TestEditorTab::theRowCountTextNamesWhatTheResultIs()
{
    EditorTab tab(
        QString::fromLatin1(ConnID), QString::fromLatin1(TabID), QStringLiteral("select 1")
    );
    tab.setConnected(true);
    ResultPage *page = runToCompletion(tab, m_backend);
    QVERIFY(page);
    QLabel *rows = label(tab, Readout::Rows);
    QVERIFY(rows);

    QCOMPARE(rows->text(), QStringLiteral("42 rows"));

    // Still streaming: the count is what has arrived so far, and says so.
    page->applyState(resultState(QString::fromLatin1(ResultID), 90, Progress::Streaming));
    QCOMPARE(rows->text(), QStringLiteral("90 rows…"));

    QJsonObject capped = resultState(QString::fromLatin1(ResultID), 500);
    capped.insert(QStringLiteral("capped"), true);
    page->applyState(capped);
    // The query returned more than the run was allowed to fetch, so the number
    // is a cap and not a count.
    QCOMPARE(rows->text(), QStringLiteral("capped at 500"));
}

void TestEditorTab::aFilteredResultCountsAgainstTheTotal()
{
    EditorTab tab(
        QString::fromLatin1(ConnID), QString::fromLatin1(TabID), QStringLiteral("select 1")
    );
    tab.setConnected(true);
    m_backend.replyWithResult(runReply(resultState(QString::fromLatin1(ResultID), 100)));
    ResultPage *page = runToCompletion(tab, m_backend);
    QVERIFY(page);
    QLabel *rows = label(tab, Readout::Rows);
    QVERIFY(rows);
    QCOMPARE(rows->text(), QStringLiteral("100 rows"));

    QJsonObject filtered = resultState(QString::fromLatin1(ResultID), 3);
    filtered.insert(QStringLiteral("totalRows"), 100);
    m_backend.replyWithResult(filtered);

    QLineEdit *needle = page->findChild<QLineEdit *>();
    QVERIFY(needle);
    needle->setText(QStringLiteral("ada"));

    // Both numbers, because the filter hides rows the result still holds.
    QVERIFY(waitUntil([rows] { return rows->text() == QStringLiteral("3 of 100 rows"); }));
}

void TestEditorTab::stagingAnEditOffersApplyAndDiscard()
{
    EditorTab tab(
        QString::fromLatin1(ConnID), QString::fromLatin1(TabID),
        QStringLiteral("select id, name from users")
    );
    tab.setConnected(true);
    ResultPage *page = runToCompletion(tab, m_backend);
    QVERIFY(page);

    QPushButton *apply = button(tab, Button::Apply);
    QPushButton *discard = button(tab, Button::Discard);
    QVERIFY(apply && discard);
    QVERIFY(!apply->isVisibleTo(&tab));
    QVERIFY(!discard->isVisibleTo(&tab));

    page->model()->stage(nameEdit(0, QStringLiteral("grace")));

    // The count is on the button because it is what the user is about to write
    // to the database.
    QVERIFY(apply->isVisibleTo(&tab));
    QVERIFY(discard->isVisibleTo(&tab));
    QCOMPARE(apply->text(), QStringLiteral("Apply 1 Edit"));

    page->model()->stage(nameEdit(1, QStringLiteral("ada")));
    QCOMPARE(apply->text(), QStringLiteral("Apply 2 Edits"));
}

void TestEditorTab::theEditPayloadNamesTheKeyTheColumnAndTheValue()
{
    EditorTab tab(
        QString::fromLatin1(ConnID), QString::fromLatin1(TabID),
        QStringLiteral("select id, name from users")
    );
    tab.setConnected(true);
    ResultPage *page = runToCompletion(tab, m_backend);
    QVERIFY(page);
    page->model()->stage(nameEdit(0, QStringLiteral("grace")));

    // Refused on purpose: a preview that succeeds opens a modal dialog, and an
    // exec()'d dialog under the offscreen platform never returns. The payload
    // asserted below is the one ApplyEdits sends: both build it the same way,
    // with stagedEditsPayload().
    m_backend.replyWithError(QStringLiteral("result expired"));
    m_backend.clearRequests();
    QPushButton *apply = button(tab, Button::Apply);
    QVERIFY(apply);
    apply->click();

    QVERIFY(waitUntil([this] { return callsTo(m_backend, PreviewPath).size() == 1; }));
    QJsonObject key;
    key.insert(QStringLiteral("id"), QStringLiteral("0"));
    QJsonObject edit;
    edit.insert(QStringLiteral("key"), key);
    edit.insert(QStringLiteral("col"), QStringLiteral("name"));
    edit.insert(QStringLiteral("value"), QStringLiteral("grace"));
    QCOMPARE(
        callsTo(m_backend, PreviewPath).at(0).args,
        QJsonArray({QString::fromLatin1(ResultID), QJsonArray({edit})})
    );

    // A refused write keeps its edits: dropping them would lose changes the
    // database never took, with nothing left on screen to retry from.
    QVERIFY(waitUntil([&tab] { return errorText(tab) == QStringLiteral("result expired"); }));
    QCOMPARE(page->model()->staged().size(), 1);
    QCOMPARE(apply->text(), QStringLiteral("Apply 1 Edit"));
    QVERIFY(callsTo(m_backend, ApplyPath).isEmpty());
}

void TestEditorTab::discardDropsTheEditsWithoutTellingTheServer()
{
    EditorTab tab(
        QString::fromLatin1(ConnID), QString::fromLatin1(TabID),
        QStringLiteral("select id, name from users")
    );
    tab.setConnected(true);
    ResultPage *page = runToCompletion(tab, m_backend);
    QVERIFY(page);
    page->model()->stage(nameEdit(0, QStringLiteral("grace")));

    QTest::qWait(SettleMs);
    api()->flush(2000);
    m_backend.clearRequests();

    QPushButton *discard = button(tab, Button::Discard);
    QVERIFY(discard);
    discard->click();
    api()->flush(2000);
    QTest::qWait(50);

    // Nothing was written, so there is nothing to undo on the server: the
    // staged edits only ever lived in the model.
    QVERIFY(m_backend.requests().isEmpty());
    QVERIFY(page->model()->staged().isEmpty());
    QVERIFY(!discard->isVisibleTo(&tab));
    QVERIFY(!button(tab, Button::Apply)->isVisibleTo(&tab));
}

void TestEditorTab::aHeaderClickSortsAndTheNextOneReversesIt()
{
    EditorTab tab(
        QString::fromLatin1(ConnID), QString::fromLatin1(TabID),
        QStringLiteral("select id, name from users")
    );
    tab.setConnected(true);
    ResultPage *page = runToCompletion(tab, m_backend);
    QVERIFY(page);
    m_backend.clearRequests();
    QSignalSpy refetched(page->model(), &QAbstractItemModel::dataChanged);

    QHeaderView *header = page->grid()->horizontalHeader();
    // Stands in for the click: the grid turns a section click into this.
    emit header->sectionClicked(1);

    QVERIFY(waitUntil([this] { return callsTo(m_backend, SortPath).size() == 1; }));
    QCOMPARE(
        callsTo(m_backend, SortPath).at(0).args,
        QJsonArray({QString::fromLatin1(ResultID), 1, false})
    );

    // The indicator moves only once the backend has reordered the buffer, and
    // it is what the next click reads to pick the direction.
    QVERIFY(waitUntil([header] { return header->isSortIndicatorShown(); }));
    QCOMPARE(header->sortIndicatorSection(), 1);
    QCOMPARE(header->sortIndicatorOrder(), Qt::AscendingOrder);
    // Every cached window predates the new order.
    QVERIFY(!refetched.isEmpty());

    emit header->sectionClicked(1);

    QVERIFY(waitUntil([this] { return callsTo(m_backend, SortPath).size() == 2; }));
    QCOMPARE(
        callsTo(m_backend, SortPath).at(1).args,
        QJsonArray({QString::fromLatin1(ResultID), 1, true})
    );
}

void TestEditorTab::sortingIsRefusedWhileEditsAreStaged()
{
    EditorTab tab(
        QString::fromLatin1(ConnID), QString::fromLatin1(TabID),
        QStringLiteral("select id, name from users")
    );
    tab.setConnected(true);
    ResultPage *page = runToCompletion(tab, m_backend);
    QVERIFY(page);
    page->model()->stage(nameEdit(0, QStringLiteral("grace")));
    m_backend.clearRequests();

    emit page->grid()->horizontalHeader()->sectionClicked(1);

    // Sorting renumbers the rows the staged edits are keyed to, so the user is
    // sent back to Apply or Discard rather than losing them.
    QCOMPARE(errorText(tab), QStringLiteral("apply or discard the staged edits before sorting"));
    QVERIFY(callsTo(m_backend, SortPath).isEmpty());
    QCOMPARE(page->model()->staged().size(), 1);
}

void TestEditorTab::aPlaceholderIsFlaggedWithoutAskingTheServer()
{
    const QString sql = QStringLiteral("select * from users where id = ?");
    EditorTab tab(QString::fromLatin1(ConnID), QString::fromLatin1(TabID), sql);
    // The constructor reaps orphaned sessions on its way in; that POST has to
    // land before the request list can stand for what the lint sent.
    api()->flush(2000);
    m_backend.clearRequests();

    // Disconnected, which is exactly when the server-side lint cannot run: a
    // pasted-from-code placeholder PREPAREs cleanly anyway, so this finding is
    // the client's or it is nobody's.
    tab.setConnected(false);

    SqlEditor *editor = tab.findChild<SqlEditor *>();
    QVERIFY(editor);
    const QList<QTextEdit::ExtraSelection> marks = editor->extraSelections();
    QCOMPARE(marks.size(), 1);
    QCOMPARE(marks.at(0).cursor.selectionStart(), int(sql.indexOf(QLatin1Char('?'))));
    QCOMPARE(marks.at(0).cursor.selectionEnd(), int(sql.indexOf(QLatin1Char('?'))) + 1);
    // Warning tone, not the destructive one: the statement parses, it just
    // cannot be executed as it stands.
    QCOMPARE(marks.at(0).format.underlineColor(), theme::current().warning);

    api()->flush(2000);
    QVERIFY(m_backend.requests().isEmpty());
}

void TestEditorTab::aQuestionMarkInAStringOrACommentIsNotAPlaceholder()
{
    EditorTab tab(
        QString::fromLatin1(ConnID), QString::fromLatin1(TabID),
        QStringLiteral("select '?' from users -- ?\n/* ? */")
    );

    tab.setConnected(false);

    // A question mark inside a literal or a comment is data, not a parameter:
    // flagging one would teach the user to ignore the warning.
    SqlEditor *editor = tab.findChild<SqlEditor *>();
    QVERIFY(editor);
    QCOMPARE(editor->extraSelections().size(), 0);
}

void TestEditorTab::editorPrefsReachTheEditor()
{
    EditorTab tab(
        QString::fromLatin1(ConnID), QString::fromLatin1(TabID), QStringLiteral("select 1")
    );
    SqlEditor *editor = tab.findChild<SqlEditor *>();
    QVERIFY(editor);

    tab.applyEditorPrefs(QStringLiteral("nord"), 17, 8);

    QCOMPARE(editor->palette().color(QPalette::Base), theme::editor(QStringLiteral("nord")).bg);
    // The editor's own slider, through the same logical-DPI correction the UI
    // font gets, so the two sliders mean the same physical size.
    QCOMPARE(editor->font().pixelSize(), theme::dpiPx(17));
    const qreal space = QFontMetricsF(editor->font()).horizontalAdvance(QLatin1Char(' '));
    QCOMPARE(editor->tabStopDistance(), space * 8);
}

QTEST_MAIN(TestEditorTab)

#include "tst_editortab.moc"
