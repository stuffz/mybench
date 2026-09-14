#include "shell/servertabbar.h"

#include "app/theme.h"

#include <QColor>
#include <QCoreApplication>
#include <QEvent>
#include <QHash>
#include <QLabel>
#include <QLayout>
#include <QList>
#include <QMouseEvent>
#include <QObject>
#include <QPair>
#include <QPointF>
#include <QPointer>
#include <QPushButton>
#include <QSignalSpy>
#include <QString>
#include <QTest>
#include <QVector>
#include <QWidget>

// The row of open servers. It owns no state of its own beyond the three inputs
// it was last handed, and it answers with two signals, so what a test can reach
// is the tabs in its layout, the text and properties they carry, and the
// signals a click produces.
//
// The colours themselves are the theme's business and are not asserted: what
// belongs to this row is that a connection's accent follows its id, and that
// the tab in front does not look like the ones behind it.
namespace
{

constexpr auto FirstID = "prod-eu";
constexpr auto FirstName = "Prod EU";
constexpr auto SecondID = "dev-local";
constexpr auto SecondName = "Dev";

constexpr auto Gruvbox = "gruvbox";
constexpr int FontSize = 13;

// The property servertabbar.cpp tags each tab with, which is how a click finds
// the connection it belongs to.
constexpr auto ConnProp = "connID";

// The tab's layout: accent dot, name, close. Read by position rather than by
// type, because the dot is a QLabel too.
constexpr int DotIndex = 0;
constexpr int NameIndex = 1;

constexpr auto CloseTip = "Disconnect";

using Conns = QVector<QPair<QString, QString>>;

Conns twoConns()
{
    return {
        {QString::fromLatin1(FirstID), QString::fromLatin1(FirstName)},
        {QString::fromLatin1(SecondID), QString::fromLatin1(SecondName)},
    };
}

// The colours MainWindow hands the row for connections with no colour of their
// own (mainwindow.cpp's connColor falls through to this).
QHash<QString, QColor> accents()
{
    QHash<QString, QColor> out;
    out.insert(QString::fromLatin1(FirstID), theme::connAccent(QString::fromLatin1(FirstID)));
    out.insert(QString::fromLatin1(SecondID), theme::connAccent(QString::fromLatin1(SecondID)));
    return out;
}

// The row as it stands now. A rebuild hands the old tabs to deleteLater, so
// they outlive it until the event loop runs and a findChildren() sweep would
// still return them: the layout is the row.
QList<QWidget *> tabsOf(const ServerTabBar &bar)
{
    QList<QWidget *> out;
    QLayout *row = bar.layout();
    if (!row)
    {
        return out;
    }
    for (int i = 0; i < row->count(); ++i)
    {
        if (QWidget *tab = row->itemAt(i)->widget())
        {
            out.append(tab);
        }
    }
    return out;
}

QWidget *partAt(QWidget *tab, int index)
{
    QLayout *lay = tab ? tab->layout() : nullptr;
    if (!lay || lay->count() <= index)
    {
        return nullptr;
    }
    return lay->itemAt(index)->widget();
}

QLabel *dotOf(QWidget *tab)
{
    return qobject_cast<QLabel *>(partAt(tab, DotIndex));
}

QLabel *nameOf(QWidget *tab)
{
    return qobject_cast<QLabel *>(partAt(tab, NameIndex));
}

QPushButton *closeOf(QWidget *tab)
{
    return tab ? tab->findChild<QPushButton *>() : nullptr;
}

// Empty where the tab has lost the part being asked about, which fails the
// comparison rather than the process.
QString textOf(QWidget *tab)
{
    const QLabel *name = nameOf(tab);
    return name ? name->text() : QString();
}

QString nameSheet(QWidget *tab)
{
    const QLabel *name = nameOf(tab);
    return name ? name->styleSheet() : QString();
}

QString dotSheet(QWidget *tab)
{
    const QLabel *dot = dotOf(tab);
    return dot ? dot->styleSheet() : QString();
}

// The event filter reads the type of the event and the object it was sent to,
// and nothing else: where the press landed is not part of what it decides.
void pressOn(QWidget *target)
{
    QMouseEvent press(
        QEvent::MouseButtonPress, QPointF(), QPointF(), Qt::LeftButton, Qt::LeftButton,
        Qt::NoModifier
    );
    QCoreApplication::sendEvent(target, &press);
}

} // namespace

class TestServerTabBar : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanup();

    void theRowStaysHiddenUntilAConnectionOpens();
    void everyConnectionGetsATabInTheOrderGiven();
    void aTabKeepsALongNameWhole();

    void clickingATabActivatesThatConnection();
    void clickingTheNameActivatesItToo();
    void theCloseButtonAsksToDisconnectThatTab();

    void theActiveTabIsStyledApartFromTheRest();
    void theSameConnectionKeepsItsAccent();
    void connectionsWithNoColourShareOneFallback();

    void applyThemeRebuildsTheSameRow();
    void rebuildingTheRowDestroysTheOldTabs();
    void theRowHidesAgainWhenTheLastTabGoes();

private:
    // A parent the bar can be hidden and shown inside, so setVisible() never
    // puts a window on the screen and isVisibleTo() has something to answer
    // against.
    QWidget m_host;
};

void TestServerTabBar::initTestCase()
{
    // Every tab bakes theme::current() into a stylesheet of its own, and the
    // close glyph is sized from the base font; apply() installs both.
    theme::apply(theme::defaultApp, FontSize);
}

void TestServerTabBar::cleanup()
{
    // One slot switches the theme, and the rest expect the default back.
    theme::apply(theme::defaultApp, FontSize);
}

void TestServerTabBar::theRowStaysHiddenUntilAConnectionOpens()
{
    ServerTabBar bar(&m_host);

    // With nothing open the row is a band of chrome with nothing in it.
    QVERIFY(!bar.isVisibleTo(&m_host));
    QVERIFY(tabsOf(bar).isEmpty());

    bar.setConnections(twoConns(), accents(), QString::fromLatin1(FirstID));
    QVERIFY(bar.isVisibleTo(&m_host));
}

void TestServerTabBar::everyConnectionGetsATabInTheOrderGiven()
{
    ServerTabBar bar(&m_host);
    bar.setConnections(twoConns(), accents(), QString::fromLatin1(FirstID));

    const QList<QWidget *> tabs = tabsOf(bar);
    QCOMPARE(tabs.size(), 2);

    // The name is what the tab shows; the id is what a click on it reports.
    QCOMPARE(textOf(tabs.at(0)), QString::fromLatin1(FirstName));
    QCOMPARE(tabs.at(0)->property(ConnProp).toString(), QString::fromLatin1(FirstID));
    QCOMPARE(textOf(tabs.at(1)), QString::fromLatin1(SecondName));
    QCOMPARE(tabs.at(1)->property(ConnProp).toString(), QString::fromLatin1(SecondID));

    // The parts inside carry the id too, so a click that lands on the text or
    // the dot rather than on the tab around them still knows which server it
    // means.
    const QLabel *name = nameOf(tabs.at(0));
    const QLabel *dot = dotOf(tabs.at(0));
    QVERIFY(name);
    QVERIFY(dot);
    QCOMPARE(name->property(ConnProp).toString(), QString::fromLatin1(FirstID));
    QCOMPARE(dot->property(ConnProp).toString(), QString::fromLatin1(FirstID));
}

void TestServerTabBar::aTabKeepsALongNameWhole()
{
    constexpr auto LongName = "production-eu-west-1 replica (read only)";

    ServerTabBar bar(&m_host);
    bar.setConnections(
        {{QString::fromLatin1(FirstID), QString::fromLatin1(LongName)}}, accents(),
        QString::fromLatin1(FirstID)
    );

    // Nothing here shortens the name: the tab takes the width the profile's
    // name needs rather than cutting it down to something that could read the
    // same as its neighbour.
    const QList<QWidget *> tabs = tabsOf(bar);
    QCOMPARE(tabs.size(), 1);
    QCOMPARE(textOf(tabs.at(0)), QString::fromLatin1(LongName));
}

void TestServerTabBar::clickingATabActivatesThatConnection()
{
    ServerTabBar bar(&m_host);
    bar.setConnections(twoConns(), accents(), QString::fromLatin1(FirstID));

    QSignalSpy activated(&bar, &ServerTabBar::activated);
    QSignalSpy closed(&bar, &ServerTabBar::closeRequested);
    QVERIFY(activated.isValid());
    QVERIFY(closed.isValid());

    const QList<QWidget *> tabs = tabsOf(bar);
    QCOMPARE(tabs.size(), 2);
    pressOn(tabs.at(1));

    QCOMPARE(activated.size(), 1);
    QCOMPARE(activated.at(0).at(0).toString(), QString::fromLatin1(SecondID));

    // Switching servers must not also take one down.
    QCOMPARE(closed.size(), 0);
}

void TestServerTabBar::clickingTheNameActivatesItToo()
{
    ServerTabBar bar(&m_host);
    bar.setConnections(twoConns(), accents(), QString::fromLatin1(SecondID));

    QSignalSpy activated(&bar, &ServerTabBar::activated);
    QVERIFY(activated.isValid());

    // The whole tab is the target, not a hot spot inside it: the name fills
    // most of the tab, so a press that lands on it has to count.
    const QList<QWidget *> tabs = tabsOf(bar);
    QCOMPARE(tabs.size(), 2);
    QLabel *name = nameOf(tabs.at(0));
    QVERIFY(name);
    pressOn(name);

    QCOMPARE(activated.size(), 1);
    QCOMPARE(activated.at(0).at(0).toString(), QString::fromLatin1(FirstID));
}

void TestServerTabBar::theCloseButtonAsksToDisconnectThatTab()
{
    ServerTabBar bar(&m_host);
    bar.setConnections(twoConns(), accents(), QString::fromLatin1(FirstID));

    QSignalSpy activated(&bar, &ServerTabBar::activated);
    QSignalSpy closed(&bar, &ServerTabBar::closeRequested);
    QVERIFY(activated.isValid());
    QVERIFY(closed.isValid());

    const QList<QWidget *> tabs = tabsOf(bar);
    QCOMPARE(tabs.size(), 2);
    QPushButton *close = closeOf(tabs.at(1));
    QVERIFY(close);

    // The glyph is an icon with no text of its own, so the tooltip is all a
    // reader has to go on.
    QCOMPARE(close->toolTip(), QString::fromLatin1(CloseTip));

    close->click();

    // The row only asks: whoever owns the connections decides what closing
    // one means, and the tab is still standing until they say so.
    QCOMPARE(closed.size(), 1);
    QCOMPARE(closed.at(0).at(0).toString(), QString::fromLatin1(SecondID));
    QCOMPARE(tabsOf(bar).size(), 2);

    // Closing a tab that is not in front must not pull it forward on the way
    // out.
    QCOMPARE(activated.size(), 0);
}

void TestServerTabBar::theActiveTabIsStyledApartFromTheRest()
{
    ServerTabBar bar(&m_host);
    bar.setConnections(twoConns(), accents(), QString::fromLatin1(FirstID));

    QList<QWidget *> tabs = tabsOf(bar);
    QCOMPARE(tabs.size(), 2);
    const QString frontTab = tabs.at(0)->styleSheet();
    const QString frontName = nameSheet(tabs.at(0));
    const QString backName = nameSheet(tabs.at(1));
    QVERIFY2(frontName != backName, "the server being looked at has to read as the one in front");

    bar.setConnections(twoConns(), accents(), QString::fromLatin1(SecondID));
    tabs = tabsOf(bar);
    QCOMPARE(tabs.size(), 2);

    // Same connection, same accent, no longer in front: its tab has to stop
    // looking like the one that is, and the other has to take that look on.
    QVERIFY(tabs.at(0)->styleSheet() != frontTab);
    QCOMPARE(nameSheet(tabs.at(0)), backName);
    QCOMPARE(nameSheet(tabs.at(1)), frontName);
}

void TestServerTabBar::theSameConnectionKeepsItsAccent()
{
    ServerTabBar bar(&m_host);
    bar.setConnections(twoConns(), accents(), QString::fromLatin1(FirstID));

    QList<QWidget *> tabs = tabsOf(bar);
    QCOMPARE(tabs.size(), 2);
    const QString firstDot = dotSheet(tabs.at(0));
    const QString secondDot = dotSheet(tabs.at(1));

    // The accents are what keeps prod and dev apart at a glance, so two open
    // servers sharing one would defeat the whole row.
    QVERIFY2(firstDot != secondDot, "two connections must not wear the same accent");

    // Reordered, and the other one in front: the accent belongs to the id,
    // not to the tab's place in the row or to which one is being looked at.
    const Conns conns = twoConns();
    bar.setConnections({conns.at(1), conns.at(0)}, accents(), QString::fromLatin1(SecondID));
    tabs = tabsOf(bar);
    QCOMPARE(tabs.size(), 2);
    QCOMPARE(dotSheet(tabs.at(0)), secondDot);
    QCOMPARE(dotSheet(tabs.at(1)), firstDot);
}

void TestServerTabBar::connectionsWithNoColourShareOneFallback()
{
    ServerTabBar bar(&m_host);
    bar.setConnections(twoConns(), {}, QString::fromLatin1(FirstID));

    QList<QWidget *> tabs = tabsOf(bar);
    QCOMPARE(tabs.size(), 2);
    const QString fallback = dotSheet(tabs.at(0));

    // A connection the caller has no colour for falls back to one shared
    // default, so a missing entry reads as "no colour", not as an accent of
    // its own that happens to repeat.
    QCOMPARE(dotSheet(tabs.at(1)), fallback);

    bar.setConnections(twoConns(), accents(), QString::fromLatin1(FirstID));
    tabs = tabsOf(bar);
    QCOMPARE(tabs.size(), 2);
    QVERIFY2(
        dotSheet(tabs.at(0)) != fallback,
        "a connection with an accent must not look like one without"
    );
}

void TestServerTabBar::applyThemeRebuildsTheSameRow()
{
    ServerTabBar bar(&m_host);
    bar.setConnections(twoConns(), accents(), QString::fromLatin1(SecondID));
    const QString border = bar.styleSheet();
    QCOMPARE(tabsOf(bar).size(), 2);
    const QString frontName = nameSheet(tabsOf(bar).at(1));

    theme::apply(Gruvbox, FontSize);
    bar.applyTheme();

    // The row's separator and every tab's colours are baked into per-widget
    // stylesheets, which the application sheet does not reach: without the
    // rebuild they would all keep the old theme.
    QVERIFY2(bar.styleSheet() != border, "the row's own separator has to follow the theme");

    // And the rebuild is from the inputs the row was last given, so the tabs,
    // their order and the one in front all survive it.
    const QList<QWidget *> tabs = tabsOf(bar);
    QCOMPARE(tabs.size(), 2);
    QVERIFY(nameSheet(tabs.at(1)) != frontName);
    QCOMPARE(tabs.at(0)->property(ConnProp).toString(), QString::fromLatin1(FirstID));
    QCOMPARE(textOf(tabs.at(0)), QString::fromLatin1(FirstName));
    QCOMPARE(tabs.at(1)->property(ConnProp).toString(), QString::fromLatin1(SecondID));
    QCOMPARE(textOf(tabs.at(1)), QString::fromLatin1(SecondName));
    QVERIFY(nameSheet(tabs.at(0)) != nameSheet(tabs.at(1)));
    QVERIFY(dotSheet(tabs.at(0)) != dotSheet(tabs.at(1)));
    QVERIFY(bar.isVisibleTo(&m_host));
}

void TestServerTabBar::rebuildingTheRowDestroysTheOldTabs()
{
    ServerTabBar bar(&m_host);
    bar.setConnections(twoConns(), accents(), QString::fromLatin1(FirstID));

    const QPointer<QWidget> old = tabsOf(bar).value(0);
    QVERIFY(!old.isNull());

    bar.setConnections(
        {{QString::fromLatin1(SecondID), QString::fromLatin1(SecondName)}}, accents(),
        QString::fromLatin1(SecondID)
    );
    const QList<QWidget *> tabs = tabsOf(bar);
    QCOMPARE(tabs.size(), 1);
    QVERIFY(!tabs.contains(old.data()));

    // The row is rebuilt on every connect, disconnect and theme change, and
    // the old tabs go through deleteLater: a deferred delete posted outside an
    // event loop only runs when it is asked for by name.
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QVERIFY2(old.isNull(), "the tabs of the last row have to go, not just leave the layout");
}

void TestServerTabBar::theRowHidesAgainWhenTheLastTabGoes()
{
    ServerTabBar bar(&m_host);
    bar.setConnections(twoConns(), accents(), QString::fromLatin1(FirstID));
    QVERIFY(bar.isVisibleTo(&m_host));

    bar.setConnections({}, {}, QString());

    QVERIFY(tabsOf(bar).isEmpty());
    QVERIFY2(
        !bar.isVisibleTo(&m_host), "an emptied row would leave a strip of chrome above the editor"
    );
}

QTEST_MAIN(TestServerTabBar)

#include "tst_servertabbar.moc"
