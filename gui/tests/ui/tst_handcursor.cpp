// Item views and tab bars paint their rows and tabs rather than building a
// widget for each, so AppStyle::polish never sees them. Setting the cursor on
// the view itself would also claim the empty space past the last row, which is
// not clickable and must keep the arrow.
#include "ui/handcursor.h"

#include <QListWidget>
#include <QMouseEvent>
#include <QTabBar>
#include <QTest>

namespace
{

constexpr int ViewWidth = 200;
constexpr int ViewHeight = 300;

// Well past two short rows, and past the tabs in a bar of two.
constexpr int EmptyY = 260;
constexpr int EmptyX = 900;

void moveTo(QWidget *target, const QPoint &pos)
{
    QMouseEvent move(
        QEvent::MouseMove, QPointF(pos), QPointF(target->mapToGlobal(pos)), Qt::NoButton,
        Qt::NoButton, Qt::NoModifier
    );
    QCoreApplication::sendEvent(target, &move);
}

} // namespace

class TestHandCursor : public QObject
{
    Q_OBJECT
private slots:
    void aRowUnderThePointerGetsTheHand();
    void theEmptySpacePastTheRowsKeepsTheArrow();
    void leavingTheViewPutsTheCursorBack();
    void aTabUnderThePointerGetsTheHand();
    void theStripPastTheLastTabKeepsTheArrow();
    void watchingTheSameViewTwiceLeavesOneFilter();
};

void TestHandCursor::aRowUnderThePointerGetsTheHand()
{
    QListWidget list;
    list.addItems({QStringLiteral("Users"), QStringLiteral("Variables")});
    list.resize(ViewWidth, ViewHeight);
    handCursorOnRows(&list);

    const QRect row = list.visualItemRect(list.item(0));
    QVERIFY(row.isValid());
    moveTo(list.viewport(), row.center());
    QCOMPARE(list.viewport()->cursor().shape(), Qt::PointingHandCursor);
}

void TestHandCursor::theEmptySpacePastTheRowsKeepsTheArrow()
{
    QListWidget list;
    list.addItems({QStringLiteral("Users"), QStringLiteral("Variables")});
    list.resize(ViewWidth, ViewHeight);
    handCursorOnRows(&list);

    moveTo(list.viewport(), list.visualItemRect(list.item(0)).center());
    QCOMPARE(list.viewport()->cursor().shape(), Qt::PointingHandCursor);

    moveTo(list.viewport(), QPoint(ViewWidth / 2, EmptyY));
    QVERIFY2(
        list.viewport()->cursor().shape() != Qt::PointingHandCursor,
        "the blank area below the last row is not clickable"
    );
}

void TestHandCursor::leavingTheViewPutsTheCursorBack()
{
    QListWidget list;
    list.addItems({QStringLiteral("Users")});
    list.resize(ViewWidth, ViewHeight);
    handCursorOnRows(&list);

    moveTo(list.viewport(), list.visualItemRect(list.item(0)).center());
    QCOMPARE(list.viewport()->cursor().shape(), Qt::PointingHandCursor);

    QEvent leave(QEvent::Leave);
    QCoreApplication::sendEvent(list.viewport(), &leave);
    QVERIFY(list.viewport()->cursor().shape() != Qt::PointingHandCursor);
}

void TestHandCursor::aTabUnderThePointerGetsTheHand()
{
    QTabBar bar;
    bar.addTab(QStringLiteral("Query 1"));
    bar.addTab(QStringLiteral("Query 2"));
    bar.resize(ViewWidth, 30);
    handCursorOnTabs(&bar);

    moveTo(&bar, bar.tabRect(0).center());
    QCOMPARE(bar.cursor().shape(), Qt::PointingHandCursor);
}

void TestHandCursor::theStripPastTheLastTabKeepsTheArrow()
{
    QTabBar bar;
    bar.addTab(QStringLiteral("Query 1"));
    bar.resize(1000, 30);
    handCursorOnTabs(&bar);

    moveTo(&bar, bar.tabRect(0).center());
    QCOMPARE(bar.cursor().shape(), Qt::PointingHandCursor);

    moveTo(&bar, QPoint(EmptyX, 15));
    QVERIFY2(
        bar.cursor().shape() != Qt::PointingHandCursor,
        "the strip to the right of the tabs opens nothing"
    );
}

void TestHandCursor::watchingTheSameViewTwiceLeavesOneFilter()
{
    // AppStyle::polish() reaches a dropdown's popup again on every theme
    // change, so a second call has to be a no-op rather than another filter.
    QListWidget list;
    list.addItems({QStringLiteral("Users")});
    list.resize(ViewWidth, ViewHeight);

    handCursorOnRows(&list);
    const qsizetype watchers = list.viewport()->children().size();
    handCursorOnRows(&list);
    QCOMPARE(list.viewport()->children().size(), watchers);

    moveTo(list.viewport(), list.visualItemRect(list.item(0)).center());
    QCOMPARE(list.viewport()->cursor().shape(), Qt::PointingHandCursor);
}

QTEST_MAIN(TestHandCursor)

#include "tst_handcursor.moc"
