#include "ui/tableutil.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QFontMetrics>
#include <QHeaderView>
#include <QList>
#include <QObject>
#include <QPair>
#include <QString>
#include <QStringList>
#include <QTableWidget>
#include <QTest>
#include <QVariant>
#include <QVector>

#include <memory>

namespace
{

// makeTable's own row height, which fitHeight then sums over.
constexpr int kRowHeight = 24;

// fitHeight's own floor and the padding it adds to the header line.
constexpr int kHeightFloor = 32;
constexpr int kHeaderPad = 4;

constexpr int kTallCap = 400;
constexpr int kShortCap = 100;

constexpr int kMinChars = 8;
constexpr int kMaxChars = 20;

// makeTable hands back a bare pointer, so every table here owns itself for the
// length of one slot.
using TablePtr = std::unique_ptr<QTableWidget>;

// Row order is whatever the live sort makes of it, so the key/value assertions
// look rows up by key rather than by the order they were handed over.
int rowOfKey(const QTableWidget *t, const QString &key)
{
    for (int r = 0; r < t->rowCount(); ++r)
    {
        const QTableWidgetItem *item = t->item(r, 0);
        if (item != nullptr && item->text() == key)
        {
            return r;
        }
    }
    return -1;
}

} // namespace

// These build real widgets, so the run needs a platform plugin: CTest sets
// QT_QPA_PLATFORM=offscreen for it.
class TestTableUtil : public QObject
{
    Q_OBJECT

private slots:
    void makeTableLabelsTheColumnsInOrder();
    void makeTableConfiguresTheViewForReadOnlyRows();

    void numItemsSortByValueAndNotByText();
    void formattedNumItemsShowTextAndSortByValue();

    void putRowAppendsCellsWithTooltips();
    void putRowTakesTheItemsItIsGiven();
    void putRowStopsAtTheLastColumn();
    void putRowDestroysItemsItCannotPlace();

    void putKVUpdatesInPlaceWhenTheKeysComeBack();
    void putKVRebuildsWhenTheKeySetDiffers();
    void putKVRebuildsWhenAKeyIsRepeated();

    void sortPauseHoldsRowsWhereTheyWerePut();

    void fitColumnsClampsToTheCharacterBounds();
    void fitHeightOnAnEmptyTableIsOneHeaderLine();
    void fitHeightStopsAtTheCap();
    void fitHeightHonoursACapBelowTheFloor();
};

void TestTableUtil::makeTableLabelsTheColumnsInOrder()
{
    const TablePtr t(
        makeTable({QStringLiteral("Id"), QStringLiteral("User"), QStringLiteral("Query")})
    );

    QCOMPARE(t->columnCount(), 3);
    QCOMPARE(t->rowCount(), 0);
    QCOMPARE(t->horizontalHeaderItem(0)->text(), QStringLiteral("Id"));
    QCOMPARE(t->horizontalHeaderItem(1)->text(), QStringLiteral("User"));
    QCOMPARE(t->horizontalHeaderItem(2)->text(), QStringLiteral("Query"));

    const TablePtr none(makeTable({}));
    QCOMPARE(none->columnCount(), 0);
}

void TestTableUtil::makeTableConfiguresTheViewForReadOnlyRows()
{
    const TablePtr t(makeTable({QStringLiteral("Id"), QStringLiteral("Query")}));

    QCOMPARE(t->selectionBehavior(), QAbstractItemView::SelectRows);
    QCOMPARE(t->selectionMode(), QAbstractItemView::SingleSelection);
    QCOMPARE(t->editTriggers(), QAbstractItemView::NoEditTriggers);
    QVERIFY(!t->showGrid());
    QVERIFY(!t->alternatingRowColors());
    QVERIFY(!t->wordWrap());
    QVERIFY(t->isSortingEnabled());
    QCOMPARE(t->textElideMode(), Qt::ElideRight);
    QCOMPARE(t->verticalHeader()->defaultSectionSize(), kRowHeight);

    const QHeaderView *head = t->horizontalHeader();
    // Qt centres header labels; these tables read as columns of text.
    QCOMPARE(head->defaultAlignment(), Qt::AlignLeft | Qt::AlignVCenter);
    QCOMPARE(head->sectionResizeMode(0), QHeaderView::Interactive);
    QVERIFY(head->stretchLastSection());

    // Every widget starts out hidden, so the row numbers being gone is only
    // visible once the table is: the horizontal header is the control.
    t->show();
    QVERIFY(!t->verticalHeader()->isVisible());
    QVERIFY(t->horizontalHeader()->isVisible());
}

void TestTableUtil::numItemsSortByValueAndNotByText()
{
    const std::unique_ptr<QTableWidgetItem> nine(numItem(9));
    const std::unique_ptr<QTableWidgetItem> kilo(numItem(1024));

    QCOMPARE(nine->text(), QStringLiteral("9"));

    // The display role stays a number. That is the whole mechanism: it is what
    // makes the inherited comparison numeric instead of lexical.
    QCOMPARE(nine->data(Qt::DisplayRole).typeId(), int(QMetaType::LongLong));
    QCOMPARE(nine->data(Qt::DisplayRole).toLongLong(), 9LL);

    QVERIFY(kilo->text() < nine->text());
    QVERIFY2(*nine < *kilo, "9 must sort below 1024 even though its text does not");
    QVERIFY(!(*kilo < *nine));
}

void TestTableUtil::formattedNumItemsShowTextAndSortByValue()
{
    const std::unique_ptr<QTableWidgetItem> bytes(numItem(9, QStringLiteral("9 B")));
    const std::unique_ptr<QTableWidgetItem> kib(numItem(1024, QStringLiteral("1.0 KiB")));

    QCOMPARE(bytes->text(), QStringLiteral("9 B"));
    QCOMPARE(bytes->toolTip(), QStringLiteral("9 B"));
    QCOMPARE(bytes->data(Qt::UserRole).toLongLong(), 9LL);
    QCOMPARE(kib->data(Qt::UserRole).toLongLong(), 1024LL);

    QVERIFY(kib->text() < bytes->text());
    QVERIFY2(*bytes < *kib, "the raw value orders these, not the rendering");
    QVERIFY(!(*kib < *bytes));
}

void TestTableUtil::putRowAppendsCellsWithTooltips()
{
    const TablePtr t(makeTable({QStringLiteral("Host"), QStringLiteral("State")}));
    // Insertion order is what is under test here, so keep the sort out of it.
    t->setSortingEnabled(false);

    putRow(t.get(), {QStringLiteral("db1"), QStringLiteral("Sleep")});
    putRow(t.get(), {QStringLiteral("db2"), QStringLiteral("Query")});

    QCOMPARE(t->rowCount(), 2);
    QCOMPARE(t->item(0, 0)->text(), QStringLiteral("db1"));
    QCOMPARE(t->item(0, 1)->text(), QStringLiteral("Sleep"));
    QCOMPARE(t->item(1, 0)->text(), QStringLiteral("db2"));
    QCOMPARE(t->item(1, 1)->text(), QStringLiteral("Query"));

    // The cells elide, so the tooltip is the only place the whole value is.
    QCOMPARE(t->item(1, 1)->toolTip(), QStringLiteral("Query"));
}

void TestTableUtil::putRowTakesTheItemsItIsGiven()
{
    const TablePtr t(makeTable({QStringLiteral("Id"), QStringLiteral("Rows")}));
    t->setSortingEnabled(false);

    QTableWidgetItem *const id = numItem(7);
    QTableWidgetItem *const rows = numItem(1024, QStringLiteral("1,024"));
    // Spelled out: a braced list of two pointers also reads as an iterator
    // pair, which makes the overload ambiguous.
    const QList<QTableWidgetItem *> items{id, rows};
    putRow(t.get(), items);

    QCOMPARE(t->rowCount(), 1);
    // The items themselves go in, so the value ordering they carry survives.
    QCOMPARE(t->item(0, 0), id);
    QCOMPARE(t->item(0, 1), rows);
    QCOMPARE(t->item(0, 1)->text(), QStringLiteral("1,024"));
}

void TestTableUtil::putKVUpdatesInPlaceWhenTheKeysComeBack()
{
    const TablePtr t(makeTable({QStringLiteral("Key"), QStringLiteral("Value")}));

    putKV(
        t.get(), {{QStringLiteral("Uptime"), QStringLiteral("1h")},
                  {QStringLiteral("Threads"), QStringLiteral("8")},
                  {QStringLiteral("Queries"), QStringLiteral("10")}}
    );
    QCOMPARE(t->rowCount(), 3);

    const int threads = rowOfKey(t.get(), QStringLiteral("Threads"));
    QVERIFY(threads >= 0);
    QTableWidgetItem *const value = t->item(threads, 1);
    QCOMPARE(value->text(), QStringLiteral("8"));

    t->selectRow(threads);

    putKV(
        t.get(), {{QStringLiteral("Uptime"), QStringLiteral("2h")},
                  {QStringLiteral("Threads"), QStringLiteral("9")},
                  {QStringLiteral("Queries"), QStringLiteral("20")}}
    );

    QCOMPARE(t->rowCount(), 3);
    const int stillThreads = rowOfKey(t.get(), QStringLiteral("Threads"));
    QVERIFY(stillThreads >= 0);

    // The same cell object, not a replacement: rebuilding a table that
    // refreshes twice a second throws away the selection and the scroll
    // position, which makes a row impossible to read.
    QCOMPARE(t->item(stillThreads, 1), value);
    QCOMPARE(value->text(), QStringLiteral("9"));
    QCOMPARE(t->item(rowOfKey(t.get(), QStringLiteral("Uptime")), 1)->text(), QStringLiteral("2h"));

    const QList<QTableWidgetItem *> selected = t->selectedItems();
    QCOMPARE(selected.size(), 2);
    for (const QTableWidgetItem *item : selected)
    {
        QCOMPARE(item->row(), stillThreads);
    }
}

void TestTableUtil::putKVRebuildsWhenTheKeySetDiffers()
{
    const TablePtr t(makeTable({QStringLiteral("Key"), QStringLiteral("Value")}));

    putKV(
        t.get(), {{QStringLiteral("Uptime"), QStringLiteral("1h")},
                  {QStringLiteral("Threads"), QStringLiteral("8")}}
    );
    t->selectRow(rowOfKey(t.get(), QStringLiteral("Uptime")));

    putKV(
        t.get(), {{QStringLiteral("Uptime"), QStringLiteral("1h")},
                  {QStringLiteral("Version"), QStringLiteral("8.4.0")}}
    );

    QCOMPARE(t->rowCount(), 2);
    QCOMPARE(rowOfKey(t.get(), QStringLiteral("Threads")), -1);
    QCOMPARE(
        t->item(rowOfKey(t.get(), QStringLiteral("Version")), 1)->text(), QStringLiteral("8.4.0")
    );

    // Only the in-place path keeps a selection; this one is why it exists.
    QVERIFY(t->selectedItems().isEmpty());

    // A different row count is the cheaper half of the same check.
    putKV(t.get(), {{QStringLiteral("Uptime"), QStringLiteral("3h")}});
    QCOMPARE(t->rowCount(), 1);
    QCOMPARE(t->item(0, 0)->text(), QStringLiteral("Uptime"));
    QCOMPARE(t->item(0, 1)->text(), QStringLiteral("3h"));
}

void TestTableUtil::sortPauseHoldsRowsWhereTheyWerePut()
{
    const TablePtr t(makeTable({QStringLiteral("Rows")}));
    // Pin the sort the restore will run, rather than relying on whichever
    // column the header points at by default.
    t->sortByColumn(0, Qt::AscendingOrder);

    putRow(t.get(), {numItem(3)});
    putRow(t.get(), {numItem(1)});
    QCOMPARE(t->item(0, 0)->text(), QStringLiteral("1"));

    {
        const SortPause pause(t.get());
        QVERIFY(!t->isSortingEnabled());

        const int row = t->rowCount();
        t->insertRow(row);
        t->setItem(row, 0, numItem(2));

        // With sorting live this row would be carried off mid-insert and the
        // cells still to come would land on whatever row took its index.
        QCOMPARE(t->item(row, 0)->text(), QStringLiteral("2"));
        QCOMPARE(t->item(0, 0)->text(), QStringLiteral("1"));
        QCOMPARE(t->item(1, 0)->text(), QStringLiteral("3"));
    }

    QVERIFY(t->isSortingEnabled());
    QCOMPARE(t->item(0, 0)->text(), QStringLiteral("1"));
    QCOMPARE(t->item(1, 0)->text(), QStringLiteral("2"));
    QCOMPARE(t->item(2, 0)->text(), QStringLiteral("3"));
}

void TestTableUtil::fitColumnsClampsToTheCharacterBounds()
{
    // The last column is the stretched one, so the values under test sit left
    // of it where nothing but the clamp decides their width.
    const TablePtr t(
        makeTable({QStringLiteral("Key"), QStringLiteral("Value"), QStringLiteral("Tail")})
    );
    t->setSortingEnabled(false);
    putRow(t.get(), {QStringLiteral("x"), QString(400, QLatin1Char('W')), QString()});

    fitColumns(t.get(), kMinChars, kMaxChars);

    const int ch = qMax(1, QFontMetrics(t->font()).horizontalAdvance(QLatin1Char('0')));
    QVERIFY(t->columnWidth(0) >= kMinChars * ch);
    QVERIFY2(t->columnWidth(1) <= kMaxChars * ch, "a long value must not take the viewport");
    QVERIFY(t->columnWidth(1) > t->columnWidth(0));
}

void TestTableUtil::fitHeightOnAnEmptyTableIsOneHeaderLine()
{
    const TablePtr t(makeTable({QStringLiteral("Key"), QStringLiteral("Value")}));

    fitHeight(t.get(), kTallCap);
    const int empty = t->height();

    // One header line plus its padding, raised only by the floor: an empty
    // list must not leave a screen of blank grid behind.
    QVERIFY(empty <= qMax(kHeightFloor, t->horizontalHeader()->height() + kHeaderPad));

    for (int i = 0; i < 5; ++i)
    {
        putRow(t.get(), {QString::number(i), QStringLiteral("v")});
    }
    fitHeight(t.get(), kTallCap);

    QVERIFY2(t->height() > empty, "rows have to add height");
    QVERIFY(t->height() <= kTallCap);
}

void TestTableUtil::fitHeightStopsAtTheCap()
{
    const TablePtr t(makeTable({QStringLiteral("Key")}));
    // 200 rows of 24px is far past any cap the panels pass in.
    t->setRowCount(200);

    fitHeight(t.get(), kShortCap);

    QCOMPARE(t->height(), kShortCap);
    // Fixed, not merely resized: a layout must not stretch it back out.
    QCOMPARE(t->minimumHeight(), kShortCap);
    QCOMPARE(t->maximumHeight(), kShortCap);
}

void TestTableUtil::putKVRebuildsWhenAKeyIsRepeated()
{
    const std::unique_ptr<QTableWidget> t(makeTable({QStringLiteral("Key"), QStringLiteral("Value")}
    ));
    putKV(
        t.get(),
        {{QStringLiteral("a"), QStringLiteral("1")}, {QStringLiteral("b"), QStringLiteral("2")}}
    );
    QCOMPARE(t->rowCount(), 2);

    // Same row count and every incoming key already present, but "a" twice
    // and no "b": the in-place path would write "a" over itself and leave
    // "b" showing a value that is no longer in the data.
    putKV(
        t.get(),
        {{QStringLiteral("a"), QStringLiteral("9")}, {QStringLiteral("a"), QStringLiteral("9")}}
    );

    QCOMPARE(t->rowCount(), 2);
    for (int r = 0; r < t->rowCount(); ++r)
    {
        QCOMPARE(t->item(r, 0)->text(), QStringLiteral("a"));
        QCOMPARE(t->item(r, 1)->text(), QStringLiteral("9"));
    }
}

void TestTableUtil::putRowStopsAtTheLastColumn()
{
    const std::unique_ptr<QTableWidget> t(makeTable({QStringLiteral("One"), QStringLiteral("Two")})
    );

    // A third cell has no column to land in. setItem ignores an out-of-range
    // column without adopting the item, so anything built for it must not be
    // built, or it is leaked.
    putRow(t.get(), {QStringLiteral("a"), QStringLiteral("b"), QStringLiteral("c")});

    QCOMPARE(t->rowCount(), 1);
    QCOMPARE(t->columnCount(), 2);
    QCOMPARE(t->item(0, 0)->text(), QStringLiteral("a"));
    QCOMPARE(t->item(0, 1)->text(), QStringLiteral("b"));
}

void TestTableUtil::fitHeightHonoursACapBelowTheFloor()
{
    const std::unique_ptr<QTableWidget> t(makeTable({QStringLiteral("Key"), QStringLiteral("Value")}
    ));
    putRow(t.get(), {QStringLiteral("a"), QStringLiteral("1")});

    // The floor exists so an empty table is not a sliver; the cap is what the
    // caller's layout actually has room for. When they disagree the cap has
    // to win, and clamping between crossed bounds is undefined behaviour.
    fitHeight(t.get(), 20);
    QCOMPARE(t->height(), 20);
}

void TestTableUtil::putRowDestroysItemsItCannotPlace()
{
    const std::unique_ptr<QTableWidget> t(makeTable({QStringLiteral("One"), QStringLiteral("Two")})
    );

    // The item overload is handed ownership, so an item with no column to go
    // in has to be destroyed here or nothing will ever destroy it.
    putRow(t.get(), {numItem(1), numItem(2), numItem(3)});

    QCOMPARE(t->rowCount(), 1);
    QCOMPARE(t->columnCount(), 2);
    QCOMPARE(t->item(0, 0)->text(), QStringLiteral("1"));
    QCOMPARE(t->item(0, 1)->text(), QStringLiteral("2"));
}

QTEST_MAIN(TestTableUtil)

#include "tst_tableutil.moc"
