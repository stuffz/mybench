#include "ui/switchbox.h"
#include "ui/widgets.h"

#include "app/theme.h"

#include <QAbstractButton>
#include <QApplication>
#include <QFont>
#include <QFontMetrics>
#include <QFrame>
#include <QLabel>
#include <QObject>
#include <QSignalSpy>
#include <QSize>
#include <QString>
#include <QTest>
#include <QVariant>

#include <memory>

namespace
{

constexpr int kUiFontSize = 13;

// switchbox.cpp's own numbers: the pill height and its 15/8 aspect, and the
// gap it leaves between the pill and its label.
constexpr double kPillFactor = 1.4;
constexpr int kPillMinPx = 14;
constexpr int kPillWidthNum = 15;
constexpr int kPillWidthDen = 8;
constexpr double kGapFactor = 0.5;

// Any theme whose border is not the default's.
constexpr auto kOtherTheme = "gruvbox";

// The makers hand back bare pointers with no parent, so every widget here
// owns itself for the length of one slot.
using LabelPtr = std::unique_ptr<QLabel>;
using FramePtr = std::unique_ptr<QFrame>;

} // namespace

// The chrome the views, the panels and the dialogs are assembled from. What
// the labels set is not decoration: the stylesheet selects on the object name
// and on the muted property, so a typo in either unstyles the widget and
// nothing else fails.
//
// These build real widgets, so the run needs a platform plugin: CTest sets
// QT_QPA_PLATFORM=offscreen for it.
class TestWidgets : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();

    void mutedLabelsCarryTheMutedProperty();
    void smallLabelsAreMutedAndNamedForTheSheet();
    void weightedLabelsKeepTheWeightTheyWereGiven_data();
    void weightedLabelsKeepTheWeightTheyWereGiven();

    void hairlinesAreAOnePixelHorizontalRule();
    void vhairlinesAreAOnePixelVerticalRule();
    void rulesBakeTheirColourAtConstruction();

    // hitButton() and paintEvent() are reachable only through a mouse press
    // at a coordinate and through a paint, so what is covered here is the
    // state and the signals the callers wire to.
    void aSwitchStartsOffAndKeepsItsText();
    void clickingASwitchTogglesItAndReports();
    void aProgrammaticSetIsNotAClick();
    void theSwitchAsksForItsPillAndItsLabel();
};

void TestWidgets::initTestCase()
{
    // The rules bake theme::current().border and the switch sizes itself with
    // theme::scaledPx(); apply() is what installs both.
    theme::apply(theme::defaultApp, kUiFontSize);
}

void TestWidgets::mutedLabelsCarryTheMutedProperty()
{
    const LabelPtr l(mutedLabel(QStringLiteral("Threads Running")));

    QCOMPARE(l->text(), QStringLiteral("Threads Running"));
    // The sheet selects on QLabel[muted="true"], so the name and the value
    // are both part of the contract.
    QCOMPARE(l->property("muted"), QVariant(true));
    // Only smallLabel() names itself, and the name is what sizes it down.
    QVERIFY(l->objectName().isEmpty());

    const LabelPtr blank(mutedLabel());
    QVERIFY(blank->text().isEmpty());
    QCOMPARE(blank->property("muted"), QVariant(true));
}

void TestWidgets::smallLabelsAreMutedAndNamedForTheSheet()
{
    const LabelPtr l(smallLabel(QStringLiteral("of 200")));

    QCOMPARE(l->text(), QStringLiteral("of 200"));
    QCOMPARE(l->property("muted"), QVariant(true));
    QCOMPARE(l->objectName(), QStringLiteral("smallText"));

    const LabelPtr blank(smallLabel());
    QVERIFY(blank->text().isEmpty());
    QCOMPARE(blank->objectName(), QStringLiteral("smallText"));
}

void TestWidgets::weightedLabelsKeepTheWeightTheyWereGiven_data()
{
    QTest::addColumn<QFont::Weight>("weight");

    QTest::newRow("normal") << QFont::Normal;
    QTest::newRow("medium") << QFont::Medium;
    QTest::newRow("demibold") << QFont::DemiBold;
    QTest::newRow("bold") << QFont::Bold;
}

void TestWidgets::weightedLabelsKeepTheWeightTheyWereGiven()
{
    QFETCH(QFont::Weight, weight);

    const LabelPtr l(weightedLabel(QStringLiteral("Connections"), weight));

    QCOMPARE(l->text(), QStringLiteral("Connections"));
    QCOMPARE(l->font().weight(), weight);
    // It copies the label's own font and changes the one property the sheet
    // does not own. A size of its own here would survive the prefs slider.
    QCOMPARE(l->font().pixelSize(), QApplication::font().pixelSize());
    QVERIFY(!l->property("muted").isValid());
    QVERIFY(l->objectName().isEmpty());
}

void TestWidgets::hairlinesAreAOnePixelHorizontalRule()
{
    const FramePtr rule(hairline());

    QCOMPARE(rule->frameShape(), QFrame::HLine);
    QCOMPARE(rule->minimumHeight(), 1);
    QCOMPARE(rule->maximumHeight(), 1);
    // Fixed in one direction only: a rule between stacked rows spans them.
    QCOMPARE(rule->maximumWidth(), QWIDGETSIZE_MAX);
}

void TestWidgets::vhairlinesAreAOnePixelVerticalRule()
{
    const FramePtr rule(vhairline());

    QCOMPARE(rule->frameShape(), QFrame::VLine);
    QCOMPARE(rule->minimumWidth(), 1);
    QCOMPARE(rule->maximumWidth(), 1);
    QCOMPARE(rule->maximumHeight(), QWIDGETSIZE_MAX);
}

void TestWidgets::rulesBakeTheirColourAtConstruction()
{
    const QString before = theme::app(theme::defaultApp).border.name();
    const QString after = theme::app(kOtherTheme).border.name();
    QVERIFY2(before != after, "the two themes have to disagree for this to prove anything");

    const FramePtr baked(hairline());
    QVERIFY(baked->styleSheet().contains(before));

    theme::apply(kOtherTheme, kUiFontSize);

    // The documented caveat: the colour goes into the widget's own sheet once,
    // so a rule that outlives a theme switch keeps showing the old one and its
    // owner has to rebuild it.
    QVERIFY(baked->styleSheet().contains(before));
    QVERIFY(!baked->styleSheet().contains(after));

    const FramePtr rebuilt(vhairline());
    QVERIFY(rebuilt->styleSheet().contains(after));

    theme::apply(theme::defaultApp, kUiFontSize);
}

void TestWidgets::aSwitchStartsOffAndKeepsItsText()
{
    const SwitchBox plain;

    QVERIFY(plain.text().isEmpty());
    QVERIFY(!plain.isChecked());
    QVERIFY(plain.isCheckable());
    // The whole widget is the control, and it says so under the cursor.
    QCOMPARE(plain.cursor().shape(), Qt::PointingHandCursor);

    const SwitchBox labelled(QStringLiteral("Live"));

    QCOMPARE(labelled.text(), QStringLiteral("Live"));
    QVERIFY(!labelled.isChecked());
}

void TestWidgets::clickingASwitchTogglesItAndReports()
{
    SwitchBox box(QStringLiteral("Live"));
    QSignalSpy toggled(&box, &QAbstractButton::toggled);
    QSignalSpy clicked(&box, &QAbstractButton::clicked);

    box.click();

    QVERIFY(box.isChecked());
    QCOMPARE(toggled.size(), 1);
    QCOMPARE(toggled.takeFirst().at(0).toBool(), true);
    QCOMPARE(clicked.size(), 1);
    QCOMPARE(clicked.takeFirst().at(0).toBool(), true);

    box.click();

    QVERIFY(!box.isChecked());
    QCOMPARE(toggled.size(), 1);
    QCOMPARE(toggled.takeFirst().at(0).toBool(), false);
    QCOMPARE(clicked.size(), 1);
    QCOMPARE(clicked.takeFirst().at(0).toBool(), false);
}

void TestWidgets::aProgrammaticSetIsNotAClick()
{
    SwitchBox box;
    QSignalSpy toggled(&box, &QAbstractButton::toggled);
    QSignalSpy clicked(&box, &QAbstractButton::clicked);

    box.setChecked(true);

    QVERIFY(box.isChecked());
    // Every caller wires toggled, so a stored setting loaded with setChecked
    // reaches the same slot a click does; clicked() is what separates them.
    QCOMPARE(toggled.size(), 1);
    QCOMPARE(toggled.takeFirst().at(0).toBool(), true);
    QVERIFY(clicked.isEmpty());

    box.setChecked(true);
    QVERIFY(toggled.isEmpty());

    box.setChecked(false);

    QVERIFY(!box.isChecked());
    QCOMPARE(toggled.size(), 1);
    QCOMPARE(toggled.takeFirst().at(0).toBool(), false);
    QVERIFY(clicked.isEmpty());
}

void TestWidgets::theSwitchAsksForItsPillAndItsLabel()
{
    // Derived rather than written out: every size here follows the prefs font
    // slider and the display scaling apply() folds into it.
    const int pillHeight = theme::scaledPx(kPillFactor, kPillMinPx);
    const int pillWidth = pillHeight * kPillWidthNum / kPillWidthDen;

    const SwitchBox plain;

    QCOMPARE(plain.sizeHint(), QSize(pillWidth, pillHeight));
    // Nothing gives these room to shrink into, so the minimum is the hint.
    QCOMPARE(plain.minimumSizeHint(), plain.sizeHint());

    const QString text = QStringLiteral("Hide Sleeping");
    const SwitchBox labelled(text);
    const QFontMetrics fm(labelled.font());

    QCOMPARE(
        labelled.sizeHint().width(),
        pillWidth + theme::scaledPx(kGapFactor) + fm.horizontalAdvance(text)
    );
    // A label taller than the pill raises the row; a shorter one does not
    // shrink it.
    QCOMPARE(labelled.sizeHint().height(), qMax(pillHeight, fm.height()));
    QCOMPARE(labelled.minimumSizeHint(), labelled.sizeHint());
    QVERIFY(labelled.sizeHint().width() > plain.sizeHint().width());
}

QTEST_MAIN(TestWidgets)

#include "tst_widgets.moc"
