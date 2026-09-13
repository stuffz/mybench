#include "editor/resultgrid.h"

#include "app/api.h"
#include "app/stubbackend.h"
#include "app/theme.h"
#include "editor/resultmodel.h"

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QCoreApplication>
#include <QEvent>
#include <QFontMetrics>
#include <QHeaderView>
#include <QImage>
#include <QItemSelectionModel>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QKeyEvent>
#include <QLineEdit>
#include <QList>
#include <QMenu>
#include <QModelIndex>
#include <QMouseEvent>
#include <QObject>
#include <QPainter>
#include <QPixmap>
#include <QPoint>
#include <QSignalSpy>
#include <QString>
#include <QStyleOptionViewItem>
#include <QTest>
#include <QTimer>
#include <QVariant>
#include <QVector>
#include <QWidget>

namespace
{

constexpr int GridWidth = 900;
constexpr int GridHeight = 600;

// A key column, a text column, and one that carries the NULLs.
QVector<ColumnMeta> testColumns()
{
    return {
        {QStringLiteral("id"), QStringLiteral("int")},
        {QStringLiteral("name"), QStringLiteral("varchar")},
        {QStringLiteral("note"), QStringLiteral("text")},
    };
}

// Two rows, one of them holding a SQL NULL.
QVector<QVector<QJsonValue>> testRows()
{
    return {
        {QJsonValue(QStringLiteral("1")), QJsonValue(QStringLiteral("ada")),
         QJsonValue(QJsonValue::Null)},
        {QJsonValue(QStringLiteral("2")), QJsonValue(QStringLiteral("grace")),
         QJsonValue(QStringLiteral("x"))},
    };
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

// Copying, editing and the context menu are all gated on rowLoaded(), so a
// grid is only worth driving once the stub has answered its first window.
bool loadGrid(
    ResultGrid &grid, ResultModel &model, StubBackend &backend, const QVector<ColumnMeta> &cols,
    const QVector<QVector<QJsonValue>> &rows
)
{
    api()->setEndpoint(backend.base(), QString());
    backend.replyWithResult(rowsPayload(rows));
    grid.resize(GridWidth, GridHeight);
    grid.setResultModel(&model);
    model.setResult(QStringLiteral("r1"), cols, int(rows.size()));
    model.data(model.index(0, 0), Qt::DisplayRole); // drives the lazy fetch
    return waitForWindow(model, int(rows.size()) - 1);
}

// A result taller than the window the stub answers with: rows past the first
// three exist in the model but their data has not arrived.
bool loadGridWithTail(ResultGrid &grid, ResultModel &model, StubBackend &backend, int totalRows)
{
    api()->setEndpoint(backend.base(), QString());
    backend.replyWithResult(rowsPayload({{1, QStringLiteral("one")}}));
    grid.resize(GridWidth, GridHeight);
    grid.setResultModel(&model);
    model.setResult(QStringLiteral("r1"), testColumns(), totalRows);
    model.data(model.index(0, 0), Qt::DisplayRole);
    return waitForWindow(model, 0);
}

void selectCells(ResultGrid &grid, const QList<QModelIndex> &cells)
{
    grid.selectionModel()->clearSelection();
    for (const QModelIndex &cell : cells)
    {
        grid.selectionModel()->select(cell, QItemSelectionModel::Select);
    }
}

} // namespace

// A view has little to say about itself, so what follows drives the grid the
// way the app does, through its model, its selection and its context menu,
// and reads back the clipboard, the model and the column widths. Nothing here
// asserts a pixel: the painted checks compare two renders of one size, and
// the width checks compare columns against each other rather than against
// numbers that follow the font.
class TestResultGrid : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();

    void setResultModelHandsTheGridItsModel();
    void switchingModelsTakesTheViewWithIt();

    void theHeaderIsTallerThanAPlainOne();
    void theHeaderDrawsTheNameAndTheType();
    void theHeaderDrawsItsSortArrow();

    void editingACellStartsFromItsCurrentText();
    void editingANullCellStartsFromEmpty();

    void ctrlCCopiesTheSelectedCells();
    void aHoleInTheSelectionCopiesAsEmpty();
    void copyWithHeadersNamesTheSelectedColumns();
    void theCopySeparatorIsProcessWide();
    void copyingOverAnUnloadedWindowIsRefused();
    void copyingAsInsertOverAnUnloadedWindowIsRefused();
    void copyCellWithHeaderTakesOneColumn();
    void copyRowTakesEveryColumnOfTheRow();
    void copyRowAsJsonMarksNullAsJsonNull();
    void copyRowAsInsertEscapesEveryValue();
    void copyRowAsInsertQuotesTheTarget();
    void copySelectionAsInsertFillsEveryHole();
    void withoutATargetTheInsertActionIsGone();

    void clickingASortableHeaderAsksForASort();
    void anUnsortableHeaderAsksForNothing();

    void widthsLandInTwoStages();
    void theSampledFitRunsOnlyOnce();
    void resetFitReturnsToTheHeaderGuess();
    void theSampledFitStopsAtItsCap();
};

void TestResultGrid::initTestCase()
{
    // The header and the delegate read theme::current() on every paint, and
    // apply() is what installs it.
    theme::apply(theme::defaultApp, 13);
}

void TestResultGrid::setResultModelHandsTheGridItsModel()
{
    ResultModel model;
    ResultGrid grid;
    grid.setResultModel(&model);
    model.setResult(QStringLiteral("r1"), testColumns(), 5);

    QCOMPARE(grid.resultModel(), &model);
    QCOMPARE(grid.model(), &model);
    QCOMPARE(grid.horizontalHeader()->count(), model.columnCount());
    QVERIFY(grid.selectionModel());

    // The two pieces the grid adds to a plain table view.
    QVERIFY(qobject_cast<TypedHeader *>(grid.horizontalHeader()));
    QVERIFY(qobject_cast<CellDelegate *>(grid.itemDelegateForIndex(model.index(0, 0))));

    // Rows are numbered by the result, not by the view.
    QVERIFY(grid.verticalHeader()->isHidden());
}

void TestResultGrid::switchingModelsTakesTheViewWithIt()
{
    ResultModel first;
    first.setResult(QStringLiteral("r1"), testColumns(), 5);
    ResultModel second;
    second.setResult(QStringLiteral("r2"), {{QStringLiteral("only"), QStringLiteral("int")}}, 2);

    ResultGrid grid;
    grid.setResultModel(&first);
    QCOMPARE(grid.horizontalHeader()->count(), 3);

    grid.setResultModel(&second);
    QCOMPARE(grid.resultModel(), &second);
    QCOMPARE(grid.model(), &second);
    QCOMPARE(grid.horizontalHeader()->count(), 1);

    // The view is off the old model for good: re-running that query cannot
    // reshape the grid showing the new one.
    first.setResult(
        QStringLiteral("r3"),
        {{QStringLiteral("a"), QStringLiteral("int")}, {QStringLiteral("b"), QStringLiteral("int")}
        },
        4
    );
    QCOMPARE(grid.model(), &second);
    QCOMPARE(grid.horizontalHeader()->count(), 1);
}

namespace
{

constexpr int SectionWidth = 160;

// paintSection is protected and the grid never hands out its painter, so a
// subclass is the only way to run it.
class ProbeHeader : public TypedHeader
{
public:
    using TypedHeader::TypedHeader;

    QImage paintSectionImage(int section) const
    {
        QPixmap canvas(SectionWidth, sizeHint().height());
        canvas.fill(Qt::transparent);
        QPainter painter(&canvas);
        paintSection(&painter, canvas.rect(), section);
        painter.end();
        return canvas.toImage();
    }
};

// The canvas starts transparent, so an image that comes back unchanged means
// paintSection drew nothing at all.
bool drewSomething(const QImage &image)
{
    if (image.isNull())
    {
        return false;
    }
    QImage untouched(image.size(), image.format());
    untouched.fill(Qt::transparent);
    return image != untouched;
}

} // namespace

void TestResultGrid::theHeaderIsTallerThanAPlainOne()
{
    ResultModel model;
    model.setResult(QStringLiteral("r1"), testColumns(), 0);

    ProbeHeader typed;
    typed.setModel(&model);
    QHeaderView plain(Qt::Horizontal);
    plain.setModel(&model);

    // Two lines of text where a plain header asks for one.
    QVERIFY(plain.sizeHint().height() > 0);
    QVERIFY(typed.sizeHint().height() > plain.sizeHint().height());
    QVERIFY(typed.sizeHint().height() >= 2 * QFontMetrics(typed.font()).height());
}

void TestResultGrid::theHeaderDrawsTheNameAndTheType()
{
    // Two columns of the same name and one sharing the first one's type: a
    // section that differs from both can only differ by the line they do not
    // share.
    ResultModel model;
    model.setResult(
        QStringLiteral("r1"),
        {{QStringLiteral("id"), QStringLiteral("int")},
         {QStringLiteral("id"), QStringLiteral("bigint")},
         {QStringLiteral("ref"), QStringLiteral("int")}},
        0
    );

    ProbeHeader header;
    header.setModel(&model);

    const QImage first = header.paintSectionImage(0);
    QVERIFY(drewSomething(first));
    QVERIFY2(first != header.paintSectionImage(1), "the type belongs on the second line");
    QVERIFY2(first != header.paintSectionImage(2), "the name belongs on the first line");
}

void TestResultGrid::theHeaderDrawsItsSortArrow()
{
    ResultModel model;
    model.setResult(QStringLiteral("r1"), testColumns(), 0);

    ProbeHeader header;
    header.setModel(&model);
    const QImage unsorted = header.paintSectionImage(0);
    const QImage neighbour = header.paintSectionImage(1);

    header.setSortIndicatorShown(true);
    header.setSortIndicator(0, Qt::AscendingOrder);
    const QImage ascending = header.paintSectionImage(0);
    QVERIFY(ascending != unsorted);

    header.setSortIndicator(0, Qt::DescendingOrder);
    QVERIFY(header.paintSectionImage(0) != ascending);

    // Only the sorted column carries an arrow.
    QCOMPARE(header.paintSectionImage(1), neighbour);
}

void TestResultGrid::editingACellStartsFromItsCurrentText()
{
    StubBackend backend;
    ResultModel model;
    ResultGrid grid;
    QVERIFY(loadGrid(grid, model, backend, testColumns(), testRows()));
    model.setEditable(true, {1}, {{QStringLiteral("id"), 0}});

    const QModelIndex ix = model.index(0, 1);
    QVERIFY(model.flags(ix) & Qt::ItemIsEditable);

    auto *delegate = qobject_cast<CellDelegate *>(grid.itemDelegateForIndex(ix));
    QVERIFY(delegate);

    QStyleOptionViewItem option;
    option.rect = grid.visualRect(ix);
    QVERIFY(option.rect.height() > 1);

    auto *editor = qobject_cast<QLineEdit *>(delegate->createEditor(grid.viewport(), option, ix));
    QVERIFY(editor);

    delegate->setEditorData(editor, ix);
    QCOMPARE(editor->text(), QStringLiteral("ada"));

    // The delegate paints the row divider along the cell's bottom edge, and
    // the editor stops short of it.
    delegate->updateEditorGeometry(editor, option, ix);
    QCOMPARE(editor->geometry(), option.rect.adjusted(0, 0, 0, -1));
}

void TestResultGrid::editingANullCellStartsFromEmpty()
{
    StubBackend backend;
    ResultModel model;
    ResultGrid grid;
    QVERIFY(loadGrid(grid, model, backend, testColumns(), testRows()));
    model.setEditable(true, {2}, {{QStringLiteral("id"), 0}});

    const QModelIndex ix = model.index(0, 2);
    auto *delegate = qobject_cast<CellDelegate *>(grid.itemDelegateForIndex(ix));
    QVERIFY(delegate);

    QStyleOptionViewItem option;
    option.rect = grid.visualRect(ix);
    auto *editor = qobject_cast<QLineEdit *>(delegate->createEditor(grid.viewport(), option, ix));
    QVERIFY(editor);

    // EditRole is the word the grid paints, which the base delegate would
    // have typed into the editor for the user to commit back as a string.
    QCOMPARE(ix.data(Qt::EditRole).toString(), QStringLiteral("NULL"));

    delegate->setEditorData(editor, ix);
    QVERIFY(editor->text().isEmpty());
}

namespace
{

// The grid is never mapped under offscreen, so the keystroke goes straight to
// it rather than through the window system.
QString copyWithCtrlC(ResultGrid &grid)
{
    QApplication::clipboard()->clear();
    QKeyEvent copy(QEvent::KeyPress, Qt::Key_C, Qt::ControlModifier, QStringLiteral("c"));
    QCoreApplication::sendEvent(&grid, &copy);
    return QApplication::clipboard()->text();
}

// showContextMenu builds its menu on the stack and blocks in exec(), so an
// action can only be reached from inside that loop. The clipboard is cleared
// first: an action the menu never offered leaves it empty.
QString copyViaMenu(ResultGrid &grid, const QModelIndex &ix, const QString &label)
{
    QApplication::clipboard()->clear();
    QTimer::singleShot(
        0, &grid,
        [&grid, label]()
        {
            QMenu *menu = grid.findChild<QMenu *>();
            if (!menu)
            {
                return;
            }
            for (QAction *action : menu->actions())
            {
                if (action->text() == label)
                {
                    action->trigger();
                }
            }
            menu->close();
        }
    );

    // The real path: the window system delivers the event to the viewport,
    // which hands the grid a position indexAt() can resolve.
    const QPoint at = grid.visualRect(ix).center();
    QContextMenuEvent request(QContextMenuEvent::Mouse, at, grid.viewport()->mapToGlobal(at));
    QCoreApplication::sendEvent(grid.viewport(), &request);
    return QApplication::clipboard()->text();
}

// The separator is one process-wide preference, so a test that changes it has
// to put it back even when an assertion returns early.
struct SeparatorGuard
{
    ~SeparatorGuard() { ResultGrid::setCopySeparator(QStringLiteral("\t")); }
};

} // namespace

void TestResultGrid::ctrlCCopiesTheSelectedCells()
{
    StubBackend backend;
    ResultModel model;
    ResultGrid grid;
    QVERIFY(loadGrid(grid, model, backend, testColumns(), testRows()));

    selectCells(grid, {model.index(0, 0), model.index(0, 2), model.index(1, 0), model.index(1, 2)});

    // The selected columns only, tab-separated, one line per selected row,
    // and NULL spelled out so it stays apart from an empty string.
    QCOMPARE(copyWithCtrlC(grid), QStringLiteral("1\tNULL\n2\tx"));
}

void TestResultGrid::aHoleInTheSelectionCopiesAsEmpty()
{
    StubBackend backend;
    ResultModel model;
    ResultGrid grid;
    QVERIFY(loadGrid(grid, model, backend, testColumns(), testRows()));

    selectCells(grid, {model.index(0, 0), model.index(0, 2), model.index(1, 0)});

    // The copy stays rectangular; the cell nobody picked contributes nothing.
    QCOMPARE(
        copyViaMenu(grid, model.index(0, 0), QStringLiteral("Copy Selection (3 Cells)")),
        QStringLiteral("1\tNULL\n2\t")
    );
}

void TestResultGrid::copyWithHeadersNamesTheSelectedColumns()
{
    StubBackend backend;
    ResultModel model;
    ResultGrid grid;
    QVERIFY(loadGrid(grid, model, backend, testColumns(), testRows()));

    selectCells(grid, {model.index(0, 0), model.index(0, 2), model.index(1, 0), model.index(1, 2)});

    QCOMPARE(
        copyViaMenu(grid, model.index(0, 0), QStringLiteral("Copy Selection with Headers")),
        QStringLiteral("id\tnote\n1\tNULL\n2\tx")
    );
}

void TestResultGrid::copyingOverAnUnloadedWindowIsRefused()
{
    // ResultModel::cell() answers invalid for a window that has not arrived and
    // every copy path renders invalid as the literal NULL. Copying a selection
    // that spans one would put rows of fabricated NULLs on the clipboard.
    StubBackend backend;
    QVERIFY(backend.listening());
    ResultGrid grid;
    ResultModel model;
    const int total = 200;
    QVERIFY(loadGridWithTail(grid, model, backend, total));

    const int unloaded = total - 1;
    QVERIFY2(!model.rowLoaded(unloaded), "the tail of this result must still be missing");

    QSignalSpy refused(&grid, &ResultGrid::copyRefused);
    selectCells(grid, {model.index(0, 0), model.index(unloaded, 0)});

    QCOMPARE(copyWithCtrlC(grid), QString());
    QCOMPARE(refused.count(), 1);
    QVERIFY(!refused.at(0).at(0).toString().isEmpty());
}

void TestResultGrid::copyingAsInsertOverAnUnloadedWindowIsRefused()
{
    // The same hole, but this one generates SQL that would be run elsewhere.
    StubBackend backend;
    QVERIFY(backend.listening());
    ResultGrid grid;
    ResultModel model;
    const int total = 200;
    QVERIFY(loadGridWithTail(grid, model, backend, total));
    grid.setInsertTarget(QStringLiteral("app"), QStringLiteral("users"));

    const int unloaded = total - 1;
    QSignalSpy refused(&grid, &ResultGrid::copyRefused);
    selectCells(grid, {model.index(0, 0), model.index(unloaded, 0)});

    QCOMPARE(
        copyViaMenu(grid, model.index(0, 0), QStringLiteral("Copy Selection as INSERT")), QString()
    );
    QCOMPARE(refused.count(), 1);
}

void TestResultGrid::theCopySeparatorIsProcessWide()
{
    StubBackend backend;
    ResultModel model;
    ResultGrid grid;
    QVERIFY(loadGrid(grid, model, backend, testColumns(), testRows()));

    selectCells(grid, {model.index(0, 0), model.index(0, 2), model.index(1, 0), model.index(1, 2)});

    const SeparatorGuard guard;
    ResultGrid::setCopySeparator(QStringLiteral(","));
    QCOMPARE(copyWithCtrlC(grid), QStringLiteral("1,NULL\n2,x"));

    // An empty preference is not a separator; the tab comes back.
    ResultGrid::setCopySeparator(QString());
    QCOMPARE(copyWithCtrlC(grid), QStringLiteral("1\tNULL\n2\tx"));
}

void TestResultGrid::copyCellWithHeaderTakesOneColumn()
{
    StubBackend backend;
    ResultModel model;
    ResultGrid grid;
    QVERIFY(loadGrid(grid, model, backend, testColumns(), testRows()));

    selectCells(grid, {model.index(0, 2)});

    QCOMPARE(
        copyViaMenu(grid, model.index(0, 2), QStringLiteral("Copy Cell with Header")),
        QStringLiteral("note\nNULL")
    );
}

void TestResultGrid::copyRowTakesEveryColumnOfTheRow()
{
    StubBackend backend;
    ResultModel model;
    ResultGrid grid;
    QVERIFY(loadGrid(grid, model, backend, testColumns(), testRows()));

    // One cell selected, but the row action reaches past the selection.
    selectCells(grid, {model.index(0, 1)});

    QCOMPARE(
        copyViaMenu(grid, model.index(0, 1), QStringLiteral("Copy Row")),
        QStringLiteral("1\tada\tNULL")
    );
}

void TestResultGrid::copyRowAsJsonMarksNullAsJsonNull()
{
    StubBackend backend;
    ResultModel model;
    ResultGrid grid;
    QVERIFY(loadGrid(grid, model, backend, testColumns(), testRows()));

    selectCells(grid, {model.index(0, 0)});
    const QString copied = copyViaMenu(grid, model.index(0, 0), QStringLiteral("Copy Row as JSON"));
    const QJsonObject row = QJsonDocument::fromJson(copied.toUtf8()).object();

    QCOMPARE(row.size(), 3);
    QCOMPARE(row.value(QStringLiteral("id")).toString(), QStringLiteral("1"));
    QCOMPARE(row.value(QStringLiteral("name")).toString(), QStringLiteral("ada"));

    // A NULL is a JSON null, not the four letters the grid paints.
    QVERIFY(row.value(QStringLiteral("note")).isNull());
}

void TestResultGrid::copyRowAsInsertEscapesEveryValue()
{
    StubBackend backend;
    ResultModel model;
    ResultGrid grid;
    QVERIFY(loadGrid(
        grid, model, backend, testColumns(),
        {
            {QJsonValue(QStringLiteral("1")), QJsonValue(QStringLiteral("ada")),
             QJsonValue(QJsonValue::Null)},
            {QJsonValue(QStringLiteral("2")), QJsonValue(QStringLiteral("it's")),
             QJsonValue(QStringLiteral("a\nb"))},
            {QJsonValue(QStringLiteral("3")), QJsonValue(QStringLiteral("c:\\path")),
             QJsonValue(QStringLiteral("x"))},
        }
    ));
    grid.setInsertTarget(QStringLiteral("shop"), QStringLiteral("orders"));

    QCOMPARE(
        copyViaMenu(grid, model.index(0, 0), QStringLiteral("Copy Row as INSERT")),
        QStringLiteral("INSERT INTO `shop`.`orders` (`id`, `name`, `note`) "
                       "VALUES ('1', 'ada', NULL);")
    );

    // A quote doubles and a newline becomes the two characters MySQL reads
    // back as one.
    QCOMPARE(
        copyViaMenu(grid, model.index(1, 0), QStringLiteral("Copy Row as INSERT")),
        QStringLiteral("INSERT INTO `shop`.`orders` (`id`, `name`, `note`) "
                       "VALUES ('2', 'it''s', 'a\\nb');")
    );

    // A backslash doubles, and doubles before the newline escape is inserted
    // rather than after it.
    QCOMPARE(
        copyViaMenu(grid, model.index(2, 0), QStringLiteral("Copy Row as INSERT")),
        QStringLiteral("INSERT INTO `shop`.`orders` (`id`, `name`, `note`) "
                       "VALUES ('3', 'c:\\\\path', 'x');")
    );
}

void TestResultGrid::copyRowAsInsertQuotesTheTarget()
{
    StubBackend backend;
    ResultModel model;
    ResultGrid grid;
    QVERIFY(loadGrid(grid, model, backend, testColumns(), testRows()));
    grid.setInsertTarget(QStringLiteral("my`db"), QStringLiteral("or`ders"));

    // Every identifier is backquoted and every backquote inside one doubled.
    QCOMPARE(
        copyViaMenu(grid, model.index(0, 0), QStringLiteral("Copy Row as INSERT")),
        QStringLiteral("INSERT INTO `my``db`.`or``ders` (`id`, `name`, `note`) "
                       "VALUES ('1', 'ada', NULL);")
    );
}

void TestResultGrid::copySelectionAsInsertFillsEveryHole()
{
    StubBackend backend;
    ResultModel model;
    ResultGrid grid;
    QVERIFY(loadGrid(grid, model, backend, testColumns(), testRows()));
    grid.setInsertTarget(QStringLiteral("shop"), QStringLiteral("orders"));

    selectCells(grid, {model.index(0, 0), model.index(0, 2), model.index(1, 0), model.index(1, 2)});
    QCOMPARE(
        copyViaMenu(grid, model.index(0, 0), QStringLiteral("Copy Selection as INSERT")),
        QStringLiteral("INSERT INTO `shop`.`orders` (`id`, `note`) VALUES\n"
                       "  ('1', NULL),\n"
                       "  ('2', 'x');")
    );

    // The statement names the column for every row it writes, so an unpicked
    // cell has to be NULL rather than nothing.
    selectCells(grid, {model.index(0, 0), model.index(0, 2), model.index(1, 0)});
    QCOMPARE(
        copyViaMenu(grid, model.index(0, 0), QStringLiteral("Copy Selection as INSERT")),
        QStringLiteral("INSERT INTO `shop`.`orders` (`id`, `note`) VALUES\n"
                       "  ('1', NULL),\n"
                       "  ('2', NULL);")
    );
}

void TestResultGrid::withoutATargetTheInsertActionIsGone()
{
    StubBackend backend;
    ResultModel model;
    ResultGrid grid;
    QVERIFY(loadGrid(grid, model, backend, testColumns(), testRows()));
    selectCells(grid, {model.index(0, 0)});

    grid.setInsertTarget(QStringLiteral("shop"), QStringLiteral("orders"));
    QVERIFY(!copyViaMenu(grid, model.index(0, 0), QStringLiteral("Copy Row as INSERT")).isEmpty());

    // A result that maps to no single table has no table to insert into.
    grid.clearInsertTarget();
    QVERIFY(copyViaMenu(grid, model.index(0, 0), QStringLiteral("Copy Row as INSERT")).isEmpty());
}

namespace
{

// The header emits sectionClicked only when press and release land on the same
// section. Only the x coordinate picks that section for a horizontal header,
// and the events go to the viewport, which is where the window system would
// have delivered them.
void clickSection(ResultGrid &grid, int section)
{
    QHeaderView *header = grid.horizontalHeader();
    const QPointF at(
        header->sectionViewportPosition(section) + header->sectionSize(section) / 2.0, 0
    );
    QMouseEvent press(
        QEvent::MouseButtonPress, at, at, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier
    );
    QMouseEvent release(
        QEvent::MouseButtonRelease, at, at, Qt::LeftButton, Qt::NoButton, Qt::NoModifier
    );
    QCoreApplication::sendEvent(header->viewport(), &press);
    QCoreApplication::sendEvent(header->viewport(), &release);
}

} // namespace

void TestResultGrid::clickingASortableHeaderAsksForASort()
{
    ResultModel model;
    ResultGrid grid;
    grid.resize(GridWidth, GridHeight);
    grid.setResultModel(&model);
    model.setResult(QStringLiteral("r1"), testColumns(), 2);
    QSignalSpy sorted(&grid, &ResultGrid::sortRequested);

    clickSection(grid, 2);

    // The sort itself is the page's job; the grid only asks for it.
    QCOMPARE(sorted.size(), 1);
    QCOMPARE(sorted.at(0).at(0).toInt(), 2);
}

void TestResultGrid::anUnsortableHeaderAsksForNothing()
{
    ResultModel model;
    ResultGrid grid;
    grid.resize(GridWidth, GridHeight);
    grid.setResultModel(&model);
    model.setResult(QStringLiteral("r1"), testColumns(), 2);
    QSignalSpy sorted(&grid, &ResultGrid::sortRequested);

    // A result that is still streaming cannot be reordered underneath itself.
    grid.setSortable(false);
    clickSection(grid, 1);

    QCOMPARE(sorted.size(), 0);
}

namespace
{

constexpr int LongNameChars = 50;  // past the header guess's cap
constexpr int LongValueChars = 40; // past the floor, short of the sampled cap
constexpr int CappedValueChars = 100;
constexpr int WiderValueChars = 200;

// A name at the floor, a name at the floor whose values outgrow it, and a
// name long enough to reach the header guess's cap.
QVector<ColumnMeta> fitColumns()
{
    return {
        {QStringLiteral("id"), QStringLiteral("int")},
        {QStringLiteral("note"), QStringLiteral("text")},
        {QString(LongNameChars, QLatin1Char('c')), QStringLiteral("varchar")},
    };
}

QVector<QVector<QJsonValue>> fitRows(int noteChars)
{
    return {{
        QJsonValue(QStringLiteral("1")),
        QJsonValue(QString(noteChars, QLatin1Char('v'))),
        QJsonValue(QStringLiteral("x")),
    }};
}

} // namespace

void TestResultGrid::widthsLandInTwoStages()
{
    StubBackend backend;
    api()->setEndpoint(backend.base(), QString());
    backend.replyWithResult(rowsPayload(fitRows(LongValueChars)));

    ResultModel model;
    ResultGrid grid;
    grid.resize(GridWidth, GridHeight);
    grid.setResultModel(&model);
    model.setResult(QStringLiteral("r1"), fitColumns(), 1);

    // Stage one is the column names alone, so a fresh result is readable
    // before any row has arrived.
    QVERIFY(!model.rowLoaded(0));
    const int floorWidth = grid.columnWidth(0);
    QCOMPARE(grid.columnWidth(1), floorWidth);
    const int namedWidth = grid.columnWidth(2);
    QVERIFY(namedWidth > floorWidth);

    model.data(model.index(0, 0), Qt::DisplayRole);
    QVERIFY(waitForWindow(model, 0));

    // Stage two samples the first window: short values keep the guess, a long
    // value widens its column, and the long name widens too because the
    // sampled cap is looser than the header one.
    QCOMPARE(grid.columnWidth(0), floorWidth);
    QVERIFY(grid.columnWidth(1) > floorWidth);
    QVERIFY(grid.columnWidth(2) > namedWidth);
}

void TestResultGrid::theSampledFitRunsOnlyOnce()
{
    StubBackend backend;
    ResultModel model;
    ResultGrid grid;
    QVERIFY(loadGrid(grid, model, backend, fitColumns(), fitRows(LongValueChars)));
    const int fitted = grid.columnWidth(1);

    // Every later window leaves the widths where they are: the user may have
    // dragged them since.
    backend.replyWithResult(rowsPayload(fitRows(CappedValueChars)));
    model.invalidateWindows();
    model.data(model.index(0, 0), Qt::DisplayRole);
    QVERIFY(waitForWindow(model, 0));

    QCOMPARE(grid.columnWidth(1), fitted);
}

void TestResultGrid::resetFitReturnsToTheHeaderGuess()
{
    StubBackend backend;
    ResultModel model;
    ResultGrid grid;
    QVERIFY(loadGrid(grid, model, backend, fitColumns(), fitRows(LongValueChars)));
    const int fitted = grid.columnWidth(1);
    QVERIFY(fitted > grid.columnWidth(0));
    grid.horizontalHeader()->setSortIndicatorShown(true);

    grid.resetFit();

    // A new result is unsorted and unsampled: back to the names, and no
    // indicator over a column nobody has sorted yet.
    QVERIFY(!grid.horizontalHeader()->isSortIndicatorShown());
    QCOMPARE(grid.columnWidth(1), grid.columnWidth(0));

    model.invalidateWindows();
    model.data(model.index(0, 0), Qt::DisplayRole);
    QVERIFY(waitForWindow(model, 0));

    QCOMPARE(grid.columnWidth(1), fitted);
}

void TestResultGrid::theSampledFitStopsAtItsCap()
{
    StubBackend backend;
    ResultModel model;
    ResultGrid grid;
    QVERIFY(loadGrid(
        grid, model, backend,
        {{QStringLiteral("a"), QStringLiteral("text")},
         {QStringLiteral("b"), QStringLiteral("text")},
         {QStringLiteral("c"), QStringLiteral("text")}},
        {{
            QJsonValue(QString(CappedValueChars, QLatin1Char('v'))),
            QJsonValue(QString(WiderValueChars, QLatin1Char('v'))),
            QJsonValue(QStringLiteral("x")),
        }}
    ));

    // One value twice as long as the other buys no more width: past the cap a
    // column is elided, not widened off the screen.
    QCOMPARE(grid.columnWidth(0), grid.columnWidth(1));
    QVERIFY(grid.columnWidth(0) > grid.columnWidth(2));
}

QTEST_MAIN(TestResultGrid)

#include "tst_resultgrid.moc"
