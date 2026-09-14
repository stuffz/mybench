#include "views/panelbase.h"

#include "app/api.h"
#include "app/stubbackend.h"
#include "app/theme.h"

#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonValue>
#include <QLabel>
#include <QLatin1String>
#include <QLayoutItem>
#include <QLineEdit>
#include <QList>
#include <QObject>
#include <QPushButton>
#include <QString>
#include <QTest>
#include <QVBoxLayout>
#include <QWidget>

// PanelBase is chrome and one hook: it builds the title row, owns the error
// strip, and calls refresh() when the button is pressed. It reports nothing
// about itself, so everything below is asserted through subclasses that record
// what the base called and through the layouts the base filled.
namespace
{

constexpr auto Title = "Client Connections";
constexpr auto ConnID = "conn-1";
constexpr auto Placeholder = "Filter variables…";
constexpr auto Message = "table is gone";
constexpr auto RefreshText = "Refresh";

// The objectName panelbase.cpp gives the title row, matched by the
// border-bottom rule in theme.cpp.
constexpr auto HeaderName = "PanelHeader";

constexpr auto Service = "admin";
constexpr auto Method = "Panel";
constexpr auto Path = "/rpc/admin/Panel";

constexpr auto Gruvbox = "gruvbox";
constexpr int FontSize = 13;
constexpr int FlushMs = 5000;

// addFilter's own minimum, wide enough for a table name.
constexpr int FilterMinWidth = 220;

// The header layouts and the error strip are protected because subclasses are
// meant to use them, which is the contract under test: reopen them here rather
// than reach around the base.
class OpenPanel : public PanelBase
{
public:
    using PanelBase::PanelBase;

    using PanelBase::addFilter;
    using PanelBase::addHeaderRow;
    using PanelBase::addHeaderStretch;
    using PanelBase::addHeaderWidget;
    using PanelBase::setBody;
    using PanelBase::showError;

    using PanelBase::m_connID;
    using PanelBase::m_error;
    using PanelBase::m_header;
    using PanelBase::m_root;
};

class RecordingPanel final : public OpenPanel
{
public:
    RecordingPanel() : OpenPanel(QString::fromLatin1(Title), QString::fromLatin1(ConnID)) {}

    int refreshes = 0;

protected:
    void refresh() override { ++refreshes; }
};

// The shape every real panel's refresh() has: one call carrying the connection
// id the base holds, and whatever error comes back straight to the strip.
class BackendPanel final : public OpenPanel
{
public:
    BackendPanel() : OpenPanel(QString::fromLatin1(Title), QString::fromLatin1(ConnID)) {}

    int replies = 0;

protected:
    void refresh() override
    {
        api()->call(
            QString::fromLatin1(Service), QString::fromLatin1(Method), {m_connID}, this,
            [this](const QJsonValue &, const QString &err)
            {
                ++replies;
                showError(err);
            }
        );
    }
};

QPushButton *refreshButton(const QWidget &panel)
{
    const QList<QPushButton *> buttons = panel.findChildren<QPushButton *>();
    for (QPushButton *button : buttons)
    {
        if (button->text() == QLatin1String(RefreshText))
        {
            return button;
        }
    }
    return nullptr;
}

// The panel is never shown in a test, and a child of a hidden parent is never
// isVisible(): ask whether the strip would come up with the panel instead.
bool errorShown(const OpenPanel &panel)
{
    return panel.m_error->isVisibleTo(&panel);
}

int spacersIn(const QHBoxLayout *row)
{
    int found = 0;
    for (int i = 0; i < row->count(); ++i)
    {
        if (row->itemAt(i)->spacerItem())
        {
            ++found;
        }
    }
    return found;
}

} // namespace

class TestPanelBase : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanup();

    void everyPanelGetsATitleAndARefreshButton();
    void theBaseNeverRefreshesOnItsOwn();
    void theRefreshButtonCallsTheSubclass();

    void theErrorStripStartsHidden();
    void showErrorRevealsTheMessage();
    void anEmptyMessageHidesTheStripAgain();
    void aThemeSwitchRecoloursTheErrorStrip();

    void aBackendErrorReachesTheErrorStrip();
    void aLaterRefreshClearsAnErrorThatIsGone();

    void addFilterPutsTheBoxInTheHeaderRow();
    void headerWidgetsKeepTheOrderTheyWereAddedIn();
    void setBodyStretchesTheTitleRowOnce();
    void addHeaderStretchKeepsSetBodyFromStretchingAgain();
    void theSecondHeaderRowIsStretchedToo();
    void setBodyPutsTheBodyUnderTheErrorStrip();
};

void TestPanelBase::initTestCase()
{
    // The error strip's stylesheet is built from theme::current(); apply() is
    // what installs it.
    theme::apply(theme::defaultApp, FontSize);
}

void TestPanelBase::cleanup()
{
    // One slot switches the theme, and the rest expect the default back.
    theme::apply(theme::defaultApp, FontSize);
}

void TestPanelBase::everyPanelGetsATitleAndARefreshButton()
{
    RecordingPanel panel;
    QVERIFY(refreshButton(panel));

    QWidget *header = panel.m_root->itemAt(0)->widget();
    QVERIFY(header);

    // Renaming the wrap, or dropping the styled-background attribute, takes
    // the rule in theme.cpp with it and the line under every panel goes.
    QCOMPARE(header->objectName(), QString::fromLatin1(HeaderName));
    QVERIFY(header->testAttribute(Qt::WA_StyledBackground));

    bool titled = false;
    const QList<QLabel *> labels = panel.findChildren<QLabel *>();
    for (const QLabel *label : labels)
    {
        if (label->text() == QLatin1String(Title))
        {
            titled = true;
        }
    }
    QVERIFY2(titled, "the title a panel is built with has to reach a label");
}

void TestPanelBase::theBaseNeverRefreshesOnItsOwn()
{
    // refresh() is pure virtual, so a call from the base constructor would not
    // reach the subclass at all: each panel loads itself once it is built.
    RecordingPanel panel;
    QCOMPARE(panel.refreshes, 0);
}

void TestPanelBase::theRefreshButtonCallsTheSubclass()
{
    RecordingPanel panel;
    QPushButton *button = refreshButton(panel);
    QVERIFY(button);

    button->click();
    QCOMPARE(panel.refreshes, 1);

    button->click();
    QCOMPARE(panel.refreshes, 2);
}

void TestPanelBase::theErrorStripStartsHidden()
{
    RecordingPanel panel;
    QVERIFY(!errorShown(panel));
    QVERIFY(panel.m_error->text().isEmpty());

    // A backend message can run long; unwrapped it would widen the panel.
    QVERIFY(panel.m_error->wordWrap());
}

void TestPanelBase::showErrorRevealsTheMessage()
{
    RecordingPanel panel;
    panel.showError(QString::fromLatin1(Message));

    QVERIFY(errorShown(panel));
    QCOMPARE(panel.m_error->text(), QString::fromLatin1(Message));
}

void TestPanelBase::anEmptyMessageHidesTheStripAgain()
{
    RecordingPanel panel;
    panel.showError(QString::fromLatin1(Message));
    panel.showError({});

    // showError({}) is how every panel clears itself after a good reply.
    QVERIFY2(!errorShown(panel), "a cleared error must not leave the strip standing");
    QVERIFY(panel.m_error->text().isEmpty());
}

void TestPanelBase::aThemeSwitchRecoloursTheErrorStrip()
{
    RecordingPanel panel;
    panel.showError(QString::fromLatin1(Message));

    const QString black = theme::app(theme::defaultApp).destructive.name();
    QVERIFY(panel.m_error->styleSheet().contains(black));

    // The colour is baked into a per-widget stylesheet, which the application
    // sheet does not reach: without the rebuild the strip keeps the old red.
    theme::apply(Gruvbox, FontSize);
    QVERIFY(panel.m_error->styleSheet().contains(theme::app(Gruvbox).destructive.name()));
    QVERIFY(!panel.m_error->styleSheet().contains(black));
}

void TestPanelBase::aBackendErrorReachesTheErrorStrip()
{
    StubBackend backend;
    QVERIFY(backend.listening());
    backend.replyWithError(QString::fromLatin1(Message));
    api()->setEndpoint(backend.base(), QString());

    BackendPanel panel;
    QPushButton *button = refreshButton(panel);
    QVERIFY(button);
    button->click();
    api()->flush(FlushMs);

    QCOMPARE(panel.replies, 1);
    QCOMPARE(backend.requests().size(), 1);
    QCOMPARE(backend.requests().at(0).path, QString::fromLatin1(Path));

    // The connection id the base was built with is what the panel asks about.
    QCOMPARE(backend.requests().at(0).args, QJsonArray({QString::fromLatin1(ConnID)}));

    QVERIFY2(errorShown(panel), "a failed refresh must say so, not read as an empty panel");
    QCOMPARE(panel.m_error->text(), QString::fromLatin1(Message));
}

void TestPanelBase::aLaterRefreshClearsAnErrorThatIsGone()
{
    StubBackend backend;
    QVERIFY(backend.listening());
    backend.replyWithError(QString::fromLatin1(Message));
    api()->setEndpoint(backend.base(), QString());

    BackendPanel panel;
    QPushButton *button = refreshButton(panel);
    QVERIFY(button);
    button->click();
    api()->flush(FlushMs);
    QVERIFY(errorShown(panel));

    backend.replyWithResult(QJsonArray());
    button->click();
    api()->flush(FlushMs);

    QCOMPARE(panel.replies, 2);
    QVERIFY2(!errorShown(panel), "the strip would otherwise outlive the failure it reported");
}

void TestPanelBase::addFilterPutsTheBoxInTheHeaderRow()
{
    RecordingPanel panel;
    QLineEdit *filter = panel.addFilter(QString::fromLatin1(Placeholder));
    QVERIFY(filter);
    QCOMPARE(filter->placeholderText(), QString::fromLatin1(Placeholder));
    QVERIFY(filter->isClearButtonEnabled());
    QCOMPARE(filter->minimumWidth(), FilterMinWidth);

    // Third in the title row, after the title and the Refresh button, and
    // inside the header wrap rather than in the body.
    QCOMPARE(panel.m_header->indexOf(filter), 2);
    QVERIFY(filter->parentWidget());
    QCOMPARE(filter->parentWidget()->objectName(), QString::fromLatin1(HeaderName));
}

void TestPanelBase::headerWidgetsKeepTheOrderTheyWereAddedIn()
{
    RecordingPanel panel;
    auto *first = new QWidget;
    auto *second = new QWidget;
    panel.addHeaderWidget(first);
    panel.addHeaderWidget(second);

    QCOMPARE(panel.m_header->indexOf(first), 2);
    QCOMPARE(panel.m_header->indexOf(second), 3);
    QVERIFY(first->parentWidget());
    QCOMPARE(first->parentWidget()->objectName(), QString::fromLatin1(HeaderName));
}

void TestPanelBase::setBodyStretchesTheTitleRowOnce()
{
    RecordingPanel panel;
    panel.setBody(new QWidget);

    // Without the trailing stretch the title row spreads its controls across
    // the whole width.
    QCOMPARE(spacersIn(panel.m_header), 1);
    QVERIFY(panel.m_header->itemAt(panel.m_header->count() - 1)->spacerItem());
}

void TestPanelBase::addHeaderStretchKeepsSetBodyFromStretchingAgain()
{
    RecordingPanel panel;
    auto *right = new QWidget;
    panel.addHeaderStretch();
    panel.addHeaderWidget(right);
    panel.setBody(new QWidget);

    // One stretch, and it stays ahead of the widget: a second one from setBody
    // would pull that widget back off the right edge.
    QCOMPARE(spacersIn(panel.m_header), 1);
    QVERIFY(panel.m_header->itemAt(2)->spacerItem());
    QCOMPARE(panel.m_header->indexOf(right), 3);
}

void TestPanelBase::theSecondHeaderRowIsStretchedToo()
{
    RecordingPanel panel;
    QHBoxLayout *row = panel.addHeaderRow();
    QVERIFY(row);
    QVERIFY(row != panel.m_header);

    auto *control = new QWidget;
    row->addWidget(control);
    panel.setBody(new QWidget);

    QCOMPARE(row->indexOf(control), 0);
    QCOMPARE(spacersIn(row), 1);
    QVERIFY(row->itemAt(row->count() - 1)->spacerItem());
}

void TestPanelBase::setBodyPutsTheBodyUnderTheErrorStrip()
{
    RecordingPanel panel;
    auto *body = new QWidget;
    panel.setBody(body);

    QCOMPARE(panel.m_root->count(), 3);
    QVERIFY(panel.m_root->itemAt(1)->widget() == panel.m_error);
    QCOMPARE(panel.m_root->itemAt(2)->widget(), body);

    // The body is the only row that grows: an error strip that took the slack
    // would leave a one-line message holding half the panel.
    QCOMPARE(panel.m_root->stretch(2), 1);
    QCOMPARE(panel.m_root->stretch(1), 0);
}

QTEST_MAIN(TestPanelBase)

#include "tst_panelbase.moc"
