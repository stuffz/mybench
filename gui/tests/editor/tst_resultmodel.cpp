#include "editor/resultmodel.h"

#include "app/api.h"
#include "app/stubbackend.h"

#include <QAbstractItemModel>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QModelIndex>
#include <QObject>
#include <QPair>
#include <QSet>
#include <QSignalSpy>
#include <QString>
#include <QTest>
#include <QVariant>
#include <QVector>

namespace
{

// One key column and one editable column: the shape the edit path needs.
QVector<ColumnMeta> testColumns()
{
    return {
        {QStringLiteral("id"), QStringLiteral("int")},
        {QStringLiteral("name"), QStringLiteral("varchar")},
    };
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

// A window of rows as the backend sends them: {"rows": [[cell, ...], ...]},
// with JSON null for a SQL NULL.
QJsonObject rowsPayload(const QVector<QVector<QJsonValue>> &rows)
{
    QJsonArray out;
    for (const QVector<QJsonValue> &row : rows)
    {
        QJsonArray cells;
        for (const QJsonValue &cell : row)
        {
            cells.append(cell);
        }
        out.append(cells);
    }
    QJsonObject payload;
    payload.insert(QStringLiteral("rows"), out);
    return payload;
}

// Spins the event loop until the model has the window, or gives up.
bool waitForWindow(const ResultModel &model, int row)
{
    for (int waited = 0; waited < 5000; waited += 10)
    {
        if (model.rowLoaded(row))
        {
            return true;
        }
        QTest::qWait(10);
    }
    return false;
}

} // namespace

// No endpoint is ever set on the Api singleton, so every window fetch fails
// synchronously with "backend not ready" and no window is ever cached. That
// leaves rowLoaded() false throughout and nothing waiting on a reply, which is
// the point: what is under test is the model's own bookkeeping — window
// accounting, counts, staged edits — and not the transport.
class TestResultModel : public QObject
{
    Q_OBJECT

private slots:
    void setResultPublishesTheGridShape();
    void headersCarryTheColumnNameAndType();
    void streamingGrowthInsertsTheNewRows();
    void anUnchangedRowCountSignalsNothing();
    void aShrinkingRowCountResetsTheModel();
    void windowsAreTwoHundredRowsWide();
    void cellsOutsideTheGridFetchNothing();
    void aResultWithoutAnIdIsNeverFetched();
    void invalidateWindowsRepaintsTheWholeGrid();
    void invalidateWindowsKeepsQuietOnAnEmptyGrid();
    void clearResultReturnsTheModelToEmpty();
    void flagsMarkOnlyTheEditableColumns();
    void flagsOnAnInvalidIndexAreEmpty();
    void setDataRefusesWhatIsNotEditable();
    void setDataRefusesARowThatIsNotLoaded();
    void stagedValuesShadowTheBuffer();
    void stagingTheSameCellTwiceReplacesIt();
    void discardStagedClearsEveryEditOnce();
    void stageNullNeedsALoadedRow();
    void setResultDropsTheStagedEdits();
    void clearResultAnnouncesTheDiscardedEdits();
    void aShrinkAnnouncesTheDiscardedEdits();

    void aFetchedWindowFillsItsCells();
    void aNullCellArrivesAsAnInvalidVariant();
    void arrivalRepaintsOnlyTheRowsItFilled();
    void anEditCapturesTheKeyColumnsCurrentValues();
    void aWindowForAnOldResultIsDropped();
    void aBackendErrorIsRelayed();
};

void TestResultModel::setResultPublishesTheGridShape()
{
    ResultModel model;
    QSignalSpy reset(&model, &QAbstractItemModel::modelReset);
    QSignalSpy staged(&model, &ResultModel::stagedChanged);

    model.setResult(QStringLiteral("r1"), testColumns(), 42);

    QCOMPARE(model.resultId(), QStringLiteral("r1"));
    QCOMPARE(model.rowCount(), 42);
    QCOMPARE(model.columnCount(), 2);
    QCOMPARE(model.columns().size(), 2);
    QCOMPARE(model.columns().at(0).name, QStringLiteral("id"));
    QCOMPARE(reset.size(), 1);

    // The grid is flat: an index has no children to count.
    QCOMPARE(model.rowCount(model.index(0, 0)), 0);
    QCOMPARE(model.columnCount(model.index(0, 0)), 0);

    // A new result reports a clean edit count whether or not the one it
    // replaced had any, so the Apply button cannot survive the swap.
    QCOMPARE(staged.size(), 1);
    QCOMPARE(staged.at(0).at(0).toInt(), 0);
}

void TestResultModel::headersCarryTheColumnNameAndType()
{
    ResultModel model;
    model.setResult(QStringLiteral("r1"), testColumns(), 1);

    QCOMPARE(
        model.headerData(1, Qt::Horizontal, Qt::DisplayRole).toString(), QStringLiteral("name")
    );
    QCOMPARE(
        model.headerData(1, Qt::Horizontal, Qt::UserRole).toString(), QStringLiteral("varchar")
    );
    QCOMPARE(
        model.headerData(1, Qt::Horizontal, Qt::ToolTipRole).toString(),
        QStringLiteral("name (varchar)")
    );

    QVERIFY(!model.headerData(2, Qt::Horizontal, Qt::DisplayRole).isValid());
    QVERIFY(!model.headerData(-1, Qt::Horizontal, Qt::DisplayRole).isValid());
    QVERIFY(!model.headerData(0, Qt::Horizontal, Qt::DecorationRole).isValid());

    // The grid hides its vertical header, so there is nothing to label there.
    QVERIFY(!model.headerData(0, Qt::Vertical, Qt::DisplayRole).isValid());
}

void TestResultModel::streamingGrowthInsertsTheNewRows()
{
    ResultModel model;
    model.setResult(QStringLiteral("r1"), testColumns(), 100);
    QSignalSpy inserted(&model, &QAbstractItemModel::rowsInserted);
    QSignalSpy reset(&model, &QAbstractItemModel::modelReset);

    model.setRowCount(250);

    QCOMPARE(model.rowCount(), 250);
    QCOMPARE(inserted.size(), 1);
    QVERIFY(!inserted.at(0).at(0).value<QModelIndex>().isValid());
    QCOMPARE(inserted.at(0).at(1).toInt(), 100);
    QCOMPARE(inserted.at(0).at(2).toInt(), 249);

    // Growth is an insert and not a reset, so the view keeps its selection and
    // its scroll position while the result streams in.
    QCOMPARE(reset.size(), 0);
}

void TestResultModel::anUnchangedRowCountSignalsNothing()
{
    ResultModel model;
    model.setResult(QStringLiteral("r1"), testColumns(), 100);
    QSignalSpy inserted(&model, &QAbstractItemModel::rowsInserted);
    QSignalSpy reset(&model, &QAbstractItemModel::modelReset);

    // Streaming polls the same count over and over; each repeat has to be free.
    model.setRowCount(100);

    QCOMPARE(model.rowCount(), 100);
    QCOMPARE(inserted.size(), 0);
    QCOMPARE(reset.size(), 0);
}

void TestResultModel::aShrinkingRowCountResetsTheModel()
{
    ResultModel model;
    model.setResult(QStringLiteral("r1"), testColumns(), 100);
    model.setEditable(true, {1}, {{QStringLiteral("id"), 0}});
    model.stage(nameEdit(0, QStringLiteral("x")));

    QSignalSpy reset(&model, &QAbstractItemModel::modelReset);
    QSignalSpy removed(&model, &QAbstractItemModel::rowsRemoved);

    model.setRowCount(10);

    QCOMPARE(model.rowCount(), 10);
    QCOMPARE(reset.size(), 1);
    QCOMPARE(removed.size(), 0);

    // The rows the staged edits point at may be gone or renumbered, so the
    // edits go with them.
    QVERIFY(model.staged().isEmpty());
}

void TestResultModel::windowsAreTwoHundredRowsWide()
{
    ResultModel model;
    model.setResult(QStringLiteral("r1"), testColumns(), 450);
    QSignalSpy failed(&model, &ResultModel::error);

    QCOMPARE(ResultModel::Window, 200);

    // Row 0 and row 199 share the first window, row 200 opens the second.
    // Asking whether a row is loaded never fetches anything.
    QVERIFY(!model.rowLoaded(0));
    QVERIFY(!model.rowLoaded(ResultModel::Window - 1));
    QVERIFY(!model.rowLoaded(ResultModel::Window));
    QCOMPARE(failed.size(), 0);

    // Reading a cell does fetch, and with no backend the window never arrives.
    QVERIFY(!model.cell(0, 0).isValid());
    QCOMPARE(failed.size(), 1);
    failed.clear();

    const QModelIndex ix = model.index(0, 0);
    QCOMPARE(model.data(ix, Qt::DisplayRole).toString(), QStringLiteral("…"));
    QCOMPARE(failed.size(), 1);

    // A window in flight is not a row of NULLs: the raw value is invalid, but
    // the cell must not be painted as SQL NULL until its window is here.
    QVERIFY(!model.data(ix, Qt::UserRole).isValid());
    QVERIFY(!model.data(ix, Qt::UserRole + 1).toBool());
    QVERIFY(!model.data(ix, Qt::UserRole + 2).toBool());
}

void TestResultModel::cellsOutsideTheGridFetchNothing()
{
    ResultModel model;
    model.setResult(QStringLiteral("r1"), testColumns(), 10);
    QSignalSpy failed(&model, &ResultModel::error);

    QVERIFY(!model.cell(-1, 0).isValid());
    QVERIFY(!model.cell(10, 0).isValid());
    QVERIFY(!model.cell(0, -1).isValid());
    QVERIFY(!model.cell(0, 2).isValid());
    QVERIFY(!model.data(QModelIndex(), Qt::DisplayRole).isValid());

    QCOMPARE(failed.size(), 0);
}

void TestResultModel::aResultWithoutAnIdIsNeverFetched()
{
    ResultModel model;
    model.setResult(QString(), testColumns(), 10);
    QSignalSpy failed(&model, &ResultModel::error);

    // There is no buffer to ask for, so the fetch stops at the guard rather
    // than posting a call with an empty id.
    QVERIFY(!model.cell(0, 0).isValid());
    QCOMPARE(failed.size(), 0);
}

void TestResultModel::invalidateWindowsRepaintsTheWholeGrid()
{
    ResultModel model;
    model.setResult(QStringLiteral("r1"), testColumns(), 30);
    QSignalSpy changed(&model, &QAbstractItemModel::dataChanged);

    model.invalidateWindows();

    QCOMPARE(changed.size(), 1);
    QCOMPARE(changed.at(0).at(0).value<QModelIndex>(), model.index(0, 0));
    QCOMPARE(changed.at(0).at(1).value<QModelIndex>(), model.index(29, 1));

    // A sort or an applied edit rewrites the rows in place: same shape, stale
    // contents.
    QCOMPARE(model.rowCount(), 30);
    QCOMPARE(model.columnCount(), 2);
    QCOMPARE(model.resultId(), QStringLiteral("r1"));
}

void TestResultModel::invalidateWindowsKeepsQuietOnAnEmptyGrid()
{
    ResultModel model;
    QSignalSpy changed(&model, &QAbstractItemModel::dataChanged);

    model.invalidateWindows();
    QCOMPARE(changed.size(), 0);

    // Columns without rows, then rows without columns: neither spans a valid
    // index range to repaint.
    model.setResult(QStringLiteral("r1"), testColumns(), 0);
    model.invalidateWindows();
    QCOMPARE(changed.size(), 0);

    model.setResult(QStringLiteral("r2"), {}, 5);
    model.invalidateWindows();
    QCOMPARE(changed.size(), 0);
}

void TestResultModel::clearResultReturnsTheModelToEmpty()
{
    ResultModel model;
    model.setResult(QStringLiteral("r1"), testColumns(), 100);
    model.setEditable(true, {1}, {{QStringLiteral("id"), 0}});
    model.stage(nameEdit(0, QStringLiteral("x")));

    QSignalSpy reset(&model, &QAbstractItemModel::modelReset);
    model.clearResult();

    QCOMPARE(reset.size(), 1);
    QVERIFY(model.resultId().isEmpty());
    QCOMPARE(model.rowCount(), 0);
    QCOMPARE(model.columnCount(), 0);
    QVERIFY(model.columns().isEmpty());
    QVERIFY(model.staged().isEmpty());
}

void TestResultModel::flagsMarkOnlyTheEditableColumns()
{
    ResultModel model;
    model.setResult(QStringLiteral("r1"), testColumns(), 10);
    const QModelIndex id = model.index(0, 0);
    const QModelIndex name = model.index(0, 1);

    // Nothing is editable until the backend has said which columns are.
    QVERIFY(!model.editable());
    QCOMPARE(model.flags(name), Qt::ItemIsSelectable | Qt::ItemIsEnabled);

    model.setEditable(true, {1}, {{QStringLiteral("id"), 0}});

    QVERIFY(model.editable());
    QCOMPARE(model.flags(name), Qt::ItemIsSelectable | Qt::ItemIsEnabled | Qt::ItemIsEditable);
    QCOMPARE(model.flags(id), Qt::ItemIsSelectable | Qt::ItemIsEnabled);
}

void TestResultModel::setDataRefusesWhatIsNotEditable()
{
    ResultModel model;
    model.setResult(QStringLiteral("r1"), testColumns(), 10);
    QSignalSpy failed(&model, &ResultModel::error);

    QVERIFY(!model.setData(model.index(0, 1), QStringLiteral("x"), Qt::EditRole));

    model.setEditable(true, {1}, {{QStringLiteral("id"), 0}});
    QVERIFY(!model.setData(model.index(0, 0), QStringLiteral("x"), Qt::EditRole));
    QVERIFY(!model.setData(model.index(0, 1), QStringLiteral("x"), Qt::DisplayRole));

    // A cell the user was never offered an editor for refuses quietly; only a
    // refusal they could have expected to work is worth an error.
    QCOMPARE(failed.size(), 0);
    QVERIFY(model.staged().isEmpty());
}

void TestResultModel::setDataRefusesARowThatIsNotLoaded()
{
    ResultModel model;
    model.setResult(QStringLiteral("r1"), testColumns(), 10);
    model.setEditable(true, {1}, {{QStringLiteral("id"), 0}});
    QSignalSpy failed(&model, &ResultModel::error);
    QSignalSpy staged(&model, &ResultModel::stagedChanged);

    // The WHERE clause comes from the key columns' buffered values, so an edit
    // against a window that never arrived has no identity to carry.
    QVERIFY(!model.setData(model.index(0, 1), QStringLiteral("x"), Qt::EditRole));

    QVERIFY(model.staged().isEmpty());
    QCOMPARE(staged.size(), 0);
    QCOMPARE(failed.size(), 1);
    QVERIFY(failed.at(0).at(0).toString().contains(QStringLiteral("not loaded")));
}

void TestResultModel::stagedValuesShadowTheBuffer()
{
    ResultModel model;
    model.setResult(QStringLiteral("r1"), testColumns(), 10);
    model.setEditable(true, {1}, {{QStringLiteral("id"), 0}});
    QSignalSpy changed(&model, &QAbstractItemModel::dataChanged);
    QSignalSpy staged(&model, &ResultModel::stagedChanged);

    model.stage(nameEdit(0, QStringLiteral("zed")));

    QCOMPARE(model.staged().size(), 1);
    QCOMPARE(staged.size(), 1);
    QCOMPARE(staged.at(0).at(0).toInt(), 1);
    QCOMPARE(changed.size(), 1);
    QCOMPARE(changed.at(0).at(0).value<QModelIndex>(), model.index(0, 1));
    QCOMPARE(changed.at(0).at(1).value<QModelIndex>(), model.index(0, 1));

    // A staged value shows even though its window never arrived: it is what
    // the user just typed, not something to wait for.
    const QModelIndex ix = model.index(0, 1);
    QCOMPARE(model.data(ix, Qt::DisplayRole).toString(), QStringLiteral("zed"));
    QCOMPARE(model.data(ix, Qt::EditRole).toString(), QStringLiteral("zed"));
    QCOMPARE(model.data(ix, Qt::ToolTipRole).toString(), QStringLiteral("zed"));
    QCOMPARE(model.data(ix, Qt::UserRole).toString(), QStringLiteral("zed"));
    QVERIFY(model.data(ix, Qt::UserRole + 1).toBool());

    // The neighbouring cell is untouched by the edit next to it.
    QVERIFY(!model.data(model.index(0, 0), Qt::UserRole + 1).toBool());

    const StagedEdit &e = *model.staged().constBegin();
    QCOMPARE(e.row, 0);
    QCOMPARE(e.col, 1);
    QCOMPARE(e.colName, QStringLiteral("name"));
    QCOMPARE(e.key.value(QStringLiteral("id")).toString(), QStringLiteral("0"));

    // An invalid value is SQL NULL, and reads back as the word rather than as
    // an empty string.
    model.stage(nameEdit(1, QVariant()));
    QCOMPARE(model.data(model.index(1, 1), Qt::DisplayRole).toString(), QStringLiteral("NULL"));
    QVERIFY(!model.data(model.index(1, 1), Qt::UserRole).isValid());
}

void TestResultModel::stagingTheSameCellTwiceReplacesIt()
{
    ResultModel model;
    model.setResult(QStringLiteral("r1"), testColumns(), 10);
    model.setEditable(true, {1}, {{QStringLiteral("id"), 0}});
    QSignalSpy staged(&model, &ResultModel::stagedChanged);

    model.stage(nameEdit(0, QStringLiteral("first")));
    model.stage(nameEdit(0, QStringLiteral("second")));

    // One edit per cell: the count on the Apply button is cells, not commits.
    QCOMPARE(model.staged().size(), 1);
    QCOMPARE(model.data(model.index(0, 1), Qt::DisplayRole).toString(), QStringLiteral("second"));
    QCOMPARE(staged.size(), 2);
    QCOMPARE(staged.at(1).at(0).toInt(), 1);

    model.stage(nameEdit(1, QStringLiteral("other")));
    QCOMPARE(model.staged().size(), 2);
    QCOMPARE(staged.at(2).at(0).toInt(), 2);
}

void TestResultModel::discardStagedClearsEveryEditOnce()
{
    ResultModel model;
    model.setResult(QStringLiteral("r1"), testColumns(), 10);
    model.setEditable(true, {1}, {{QStringLiteral("id"), 0}});
    model.stage(nameEdit(0, QStringLiteral("a")));
    model.stage(nameEdit(1, QStringLiteral("b")));

    QSignalSpy changed(&model, &QAbstractItemModel::dataChanged);
    QSignalSpy staged(&model, &ResultModel::stagedChanged);

    model.discardStaged();

    QVERIFY(model.staged().isEmpty());
    QCOMPARE(staged.size(), 1);
    QCOMPARE(staged.at(0).at(0).toInt(), 0);
    QCOMPARE(changed.size(), 1);
    QCOMPARE(changed.at(0).at(0).value<QModelIndex>(), model.index(0, 0));
    QCOMPARE(changed.at(0).at(1).value<QModelIndex>(), model.index(9, 1));

    // Nothing staged, nothing to tell anyone.
    model.discardStaged();
    QCOMPARE(staged.size(), 1);
    QCOMPARE(changed.size(), 1);
}

void TestResultModel::stageNullNeedsALoadedRow()
{
    ResultModel model;
    model.setResult(QStringLiteral("r1"), testColumns(), 10);
    model.setEditable(true, {1}, {{QStringLiteral("id"), 0}});
    QSignalSpy staged(&model, &ResultModel::stagedChanged);

    // Same identity rule as setData, but from the context menu, where there is
    // no editor to refuse: it stages nothing and says nothing.
    model.stageNull(0, 1);

    QVERIFY(model.staged().isEmpty());
    QCOMPARE(staged.size(), 0);
}

void TestResultModel::setResultDropsTheStagedEdits()
{
    ResultModel model;
    model.setResult(QStringLiteral("r1"), testColumns(), 10);
    model.setEditable(true, {1}, {{QStringLiteral("id"), 0}});
    model.stage(nameEdit(0, QStringLiteral("a")));

    QSignalSpy staged(&model, &ResultModel::stagedChanged);
    model.setResult(QStringLiteral("r2"), testColumns(), 5);

    QVERIFY(model.staged().isEmpty());
    QCOMPARE(staged.size(), 1);
    QCOMPARE(staged.at(0).at(0).toInt(), 0);
    QCOMPARE(model.data(model.index(0, 1), Qt::DisplayRole).toString(), QStringLiteral("…"));

    // Editability belongs to the previous result's table, so the new one has
    // to be asked about again before anything is editable.
    QVERIFY(!model.editable());
    QCOMPARE(model.flags(model.index(0, 1)), Qt::ItemIsSelectable | Qt::ItemIsEnabled);
}

void TestResultModel::flagsOnAnInvalidIndexAreEmpty()
{
    ResultModel model;
    model.setResult(QStringLiteral("r1"), {{QStringLiteral("id"), QStringLiteral("int")}}, 1);

    // Qt's model contract: an invalid index carries no flags. Returning
    // Selectable|Enabled for one is what makes QAbstractItemModelTester
    // refuse this model.
    QCOMPARE(model.flags(QModelIndex()), Qt::NoItemFlags);
}

void TestResultModel::aFetchedWindowFillsItsCells()
{
    StubBackend backend;
    api()->setEndpoint(backend.base(), QString());
    backend.replyWithResult(
        rowsPayload({{QJsonValue(QStringLiteral("1")), QJsonValue(QStringLiteral("ada"))}})
    );

    ResultModel model;
    model.setResult(QStringLiteral("r1"), testColumns(), 1);

    QVERIFY(!model.rowLoaded(0));
    model.data(model.index(0, 0), Qt::DisplayRole); // drives the lazy fetch
    QVERIFY(waitForWindow(model, 0));

    QCOMPARE(model.cell(0, 0), QVariant(QStringLiteral("1")));
    QCOMPARE(model.cell(0, 1), QVariant(QStringLiteral("ada")));
    QCOMPARE(model.data(model.index(0, 1), Qt::DisplayRole).toString(), QStringLiteral("ada"));

    QCOMPARE(backend.requests().size(), 1);
    QCOMPARE(backend.requests().at(0).path, QStringLiteral("/rpc/query/Rows"));
    QCOMPARE(
        backend.requests().at(0).args, QJsonArray({QStringLiteral("r1"), 0, ResultModel::Window})
    );
}

void TestResultModel::aNullCellArrivesAsAnInvalidVariant()
{
    StubBackend backend;
    api()->setEndpoint(backend.base(), QString());
    backend.replyWithResult(
        rowsPayload({{QJsonValue(QStringLiteral("1")), QJsonValue(QJsonValue::Null)}})
    );

    ResultModel model;
    model.setResult(QStringLiteral("r1"), testColumns(), 1);
    model.data(model.index(0, 0), Qt::DisplayRole);
    QVERIFY(waitForWindow(model, 0));

    // A SQL NULL has to stay distinguishable from the empty string, which is
    // what the grid paints in italics.
    QVERIFY(!model.cell(0, 1).isValid());
    QVERIFY(model.cell(0, 0).isValid());
}

void TestResultModel::arrivalRepaintsOnlyTheRowsItFilled()
{
    StubBackend backend;
    api()->setEndpoint(backend.base(), QString());
    backend.replyWithResult(
        rowsPayload({{QJsonValue(QStringLiteral("1")), QJsonValue(QStringLiteral("ada"))}})
    );

    ResultModel model;
    model.setResult(QStringLiteral("r1"), testColumns(), 1);
    QSignalSpy changed(&model, &QAbstractItemModel::dataChanged);
    QSignalSpy arrived(&model, &ResultModel::windowArrived);

    model.data(model.index(0, 0), Qt::DisplayRole);
    QVERIFY(waitForWindow(model, 0));

    QCOMPARE(arrived.size(), 1);
    QCOMPARE(changed.size(), 1);
    const QModelIndex topLeft = changed.at(0).at(0).toModelIndex();
    const QModelIndex bottomRight = changed.at(0).at(1).toModelIndex();
    QCOMPARE(topLeft.row(), 0);
    QCOMPARE(topLeft.column(), 0);
    QCOMPARE(bottomRight.row(), 0);
    QCOMPARE(bottomRight.column(), testColumns().size() - 1);
}

void TestResultModel::anEditCapturesTheKeyColumnsCurrentValues()
{
    StubBackend backend;
    api()->setEndpoint(backend.base(), QString());
    backend.replyWithResult(
        rowsPayload({{QJsonValue(QStringLiteral("7")), QJsonValue(QStringLiteral("ada"))}})
    );

    ResultModel model;
    model.setResult(QStringLiteral("r1"), testColumns(), 1);
    model.setEditable(true, {1}, {{QStringLiteral("id"), 0}});
    model.data(model.index(0, 0), Qt::DisplayRole);
    QVERIFY(waitForWindow(model, 0));

    QVERIFY(model.setData(model.index(0, 1), QStringLiteral("grace"), Qt::EditRole));

    // The captured key is what the backend turns into the UPDATE's WHERE
    // clause, so it has to be the row's value as loaded, not the new one.
    QCOMPARE(model.staged().size(), 1);
    const StagedEdit &edit = *model.staged().constBegin();
    QCOMPARE(edit.colName, QStringLiteral("name"));
    QCOMPARE(edit.value.toString(), QStringLiteral("grace"));
    QCOMPARE(edit.key.value(QStringLiteral("id")), QVariant(QStringLiteral("7")));
}

void TestResultModel::aWindowForAnOldResultIsDropped()
{
    StubBackend backend;
    api()->setEndpoint(backend.base(), QString());
    backend.replyWithResult(
        rowsPayload({{QJsonValue(QStringLiteral("1")), QJsonValue(QStringLiteral("ada"))}})
    );

    ResultModel model;
    model.setResult(QStringLiteral("r1"), testColumns(), 1);
    model.data(model.index(0, 0), Qt::DisplayRole);

    // Re-running the query while the first window is still in flight: the
    // reply that lands belongs to a result nobody is showing any more.
    model.setResult(QStringLiteral("r2"), testColumns(), 1);
    api()->flush(2000);
    QTest::qWait(50);

    QVERIFY2(!model.rowLoaded(0), "a window from the previous result must not be cached");
}

void TestResultModel::aBackendErrorIsRelayed()
{
    StubBackend backend;
    api()->setEndpoint(backend.base(), QString());
    backend.replyWithError(QStringLiteral("result expired"));

    ResultModel model;
    model.setResult(QStringLiteral("r1"), testColumns(), 1);
    QSignalSpy failed(&model, &ResultModel::error);

    model.data(model.index(0, 0), Qt::DisplayRole);
    for (int waited = 0; failed.isEmpty() && waited < 5000; waited += 10)
    {
        QTest::qWait(10);
    }

    QCOMPARE(failed.size(), 1);
    QCOMPARE(failed.at(0).at(0).toString(), QStringLiteral("result expired"));
    QVERIFY(!model.rowLoaded(0));
}

void TestResultModel::clearResultAnnouncesTheDiscardedEdits()
{
    ResultModel model;
    model.setResult(QStringLiteral("r1"), testColumns(), 1);
    model.stage(nameEdit(0, QStringLiteral("grace")));

    QSignalSpy staged(&model, &ResultModel::stagedChanged);
    model.clearResult();

    // The toolbar sizes Apply/Discard off this signal, so edits vanishing
    // without one leaves the buttons offering work that no longer exists.
    QVERIFY(model.staged().isEmpty());
    QCOMPARE(staged.size(), 1);
    QCOMPARE(staged.at(0).at(0).toInt(), 0);
}

void TestResultModel::aShrinkAnnouncesTheDiscardedEdits()
{
    ResultModel model;
    model.setResult(QStringLiteral("r1"), testColumns(), 10);
    model.stage(nameEdit(0, QStringLiteral("grace")));

    QSignalSpy staged(&model, &ResultModel::stagedChanged);
    model.setRowCount(2);

    QVERIFY(model.staged().isEmpty());
    QCOMPARE(staged.size(), 1);
    QCOMPARE(staged.at(0).at(0).toInt(), 0);
}

QTEST_GUILESS_MAIN(TestResultModel)

#include "tst_resultmodel.moc"
