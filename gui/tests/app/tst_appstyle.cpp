// Qt Style Sheets have no cursor property — verified against Qt 6.8.2, where
// "cursor", "qproperty-cursor: PointingHandCursor" and "qproperty-cursor: 13"
// are all rejected with "Unknown property cursor". The cursor therefore has to
// be set on the widget, and QStyle::polish() is the hook Qt calls for every
// widget as it is styled, including the ones it builds for us.
#include "app/appstyle.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPushButton>
#include <QTest>

namespace
{

constexpr int PopupWidth = 200;
constexpr int PopupHeight = 100;

void moveTo(QWidget *target, const QPoint &pos)
{
    QMouseEvent move(
        QEvent::MouseMove, QPointF(pos), QPointF(target->mapToGlobal(pos)), Qt::NoButton,
        Qt::NoButton, Qt::NoModifier
    );
    QCoreApplication::sendEvent(target, &move);
}

} // namespace

class TestAppStyle : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase();

    void everyButtonGetsAHandCursor();
    void buttonsQtBuildsForUsGetItToo();
    void dropdownsGetTheHandToo();
    void theRowsOfAnOpenDropdownGetItAsWell();
    void widgetsThatAreNotButtonsKeepTheArrow();
    void tooltipsStillAppearInstantly();
};

void TestAppStyle::initTestCase()
{
    qApp->setStyle(new AppStyle);
}

void TestAppStyle::everyButtonGetsAHandCursor()
{
    QPushButton button(QStringLiteral("Run"));
    button.ensurePolished();
    QCOMPARE(button.cursor().shape(), Qt::PointingHandCursor);
}

void TestAppStyle::buttonsQtBuildsForUsGetItToo()
{
    // The query dialog's Close button is built by QDialogButtonBox, not by us,
    // which is why a button factory could never have covered it.
    QDialogButtonBox box;
    box.addButton(QDialogButtonBox::Close);
    QAbstractButton *close = box.buttons().at(0);
    close->ensurePolished();
    QCOMPARE(close->cursor().shape(), Qt::PointingHandCursor);
}

void TestAppStyle::dropdownsGetTheHandToo()
{
    // A combo is not a QAbstractButton, but the whole control opens the popup
    // when clicked, so it needs no hit test — unlike a list's rows.
    QComboBox combo;
    combo.addItem(QStringLiteral("Every 2 s"));
    combo.ensurePolished();
    QCOMPARE(combo.cursor().shape(), Qt::PointingHandCursor);
}

void TestAppStyle::theRowsOfAnOpenDropdownGetItAsWell()
{
    // The popup is a window of its own: it inherits no cursor from the combo,
    // so polishing the combo has to reach the rows separately.
    QComboBox combo;
    combo.addItems({QStringLiteral("Every 2 s"), QStringLiteral("Every 5 s")});
    combo.ensurePolished();

    QAbstractItemView *popup = combo.view();
    popup->resize(PopupWidth, PopupHeight);
    const QRect row = popup->visualRect(popup->model()->index(0, 0));
    QVERIFY(row.isValid());

    moveTo(popup->viewport(), row.center());
    QCOMPARE(popup->viewport()->cursor().shape(), Qt::PointingHandCursor);
}

void TestAppStyle::widgetsThatAreNotButtonsKeepTheArrow()
{
    // A text field has its own cursor and must not be handed a pointing hand.
    QLineEdit edit;
    edit.ensurePolished();
    QVERIFY(edit.cursor().shape() != Qt::PointingHandCursor);
}

void TestAppStyle::tooltipsStillAppearInstantly()
{
    // polish() is an addition; the reason this style exists must survive it.
    QCOMPARE(qApp->style()->styleHint(QStyle::SH_ToolTip_WakeUpDelay), 150);
    QCOMPARE(qApp->style()->styleHint(QStyle::SH_ToolTip_FallAsleepDelay), 0);
}

QTEST_MAIN(TestAppStyle)

#include "tst_appstyle.moc"
