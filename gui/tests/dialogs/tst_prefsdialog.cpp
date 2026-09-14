#include "dialogs/prefsdialog.h"

#include "app/api.h"
#include "app/stubbackend.h"
#include "app/theme.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QLabel>
#include <QLatin1String>
#include <QLayoutItem>
#include <QList>
#include <QObject>
#include <QPushButton>
#include <QSignalSpy>
#include <QSlider>
#include <QSpinBox>
#include <QString>
#include <QStringList>
#include <QTest>
#include <QTimer>
#include <QVariant>
#include <QVector>
#include <QWidget>
#include <functional>

namespace
{

constexpr auto StatusPath = "/rpc/mcp/Status";
constexpr auto ConfigurePath = "/rpc/mcp/Configure";
constexpr auto RegeneratePath = "/rpc/mcp/RegenerateToken";
constexpr auto HistoryStatsPath = "/rpc/query/HistoryStats";
constexpr auto SetHistoryLimitPath = "/rpc/query/SetHistoryLimit";

// prefsdialog.cpp's own fallback for each key it reads.
constexpr int DefaultUiFont = 13;
constexpr int DefaultEditorFont = 13;
constexpr int DefaultTabSize = 4;
constexpr int DefaultHistoryKeep = 10000;
constexpr int DefaultRowLimit = 50000;

// The fallback that applies a size change no slider handle was released on.
constexpr int ApplyMs = 350;

// Whatever the backend reports; nothing in the dialog picks a port of its own.
constexpr int McpPort = 7337;
constexpr int OtherMcpPort = 7400;

// Obvious nonsense on purpose: the MCP token is a bearer credential, so
// nothing that would open a real endpoint may be written down here.
constexpr auto Token = "placeholder-not-a-token";
constexpr auto FreshToken = "placeholder-not-that-token-either";

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

// The calls that went to one RPC method. Every dialog asks after the history
// size and the MCP listener as it is built, so an assertion about a Configure
// picks out its own conversation.
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

// The label column holds either a plain mutedLabel or the tippedLabel wrapper
// (the name beside an info glyph), so the text is looked for one level down as
// well.
bool labelReads(QWidget *column, const QString &text)
{
    if (auto *plain = qobject_cast<QLabel *>(column))
    {
        return plain->text() == text;
    }
    for (QLabel *inner : column->findChildren<QLabel *>())
    {
        if (inner->text() == text)
        {
            return true;
        }
    }
    return false;
}

// Every setting hangs off the one form, the MCP rows included.
QWidget *fieldAt(const PrefsDialog &dlg, const QString &label)
{
    auto *form = dlg.findChild<QFormLayout *>();
    if (!form)
    {
        return nullptr;
    }
    for (int row = 0; row < form->rowCount(); ++row)
    {
        QLayoutItem *name = form->itemAt(row, QFormLayout::LabelRole);
        QWidget *column = name ? name->widget() : nullptr;
        if (!column || !labelReads(column, label))
        {
            continue;
        }
        QLayoutItem *field = form->itemAt(row, QFormLayout::FieldRole);
        return field ? field->widget() : nullptr;
    }
    return nullptr;
}

QComboBox *combo(const PrefsDialog &dlg, const QString &label)
{
    return qobject_cast<QComboBox *>(fieldAt(dlg, label));
}

QSpinBox *spin(const PrefsDialog &dlg, const QString &label)
{
    return qobject_cast<QSpinBox *>(fieldAt(dlg, label));
}

// SwitchBox is a QCheckBox that paints itself as a pill, so the toggles are
// found as what they are.
QCheckBox *toggle(const PrefsDialog &dlg, const QString &label)
{
    return qobject_cast<QCheckBox *>(fieldAt(dlg, label));
}

// The size rows pair a slider with its readout inside a plain wrapper widget.
QSlider *slider(const PrefsDialog &dlg, const QString &label)
{
    QWidget *field = fieldAt(dlg, label);
    return field ? field->findChild<QSlider *>() : nullptr;
}

QLabel *readout(const PrefsDialog &dlg, const QString &label)
{
    QWidget *field = fieldAt(dlg, label);
    return field ? field->findChild<QLabel *>() : nullptr;
}

QPushButton *button(const PrefsDialog &dlg, const QString &text)
{
    for (QPushButton *b : dlg.findChildren<QPushButton *>())
    {
        if (b->text() == text)
        {
            return b;
        }
    }
    return nullptr;
}

// The MCP error line is the only label wearing the destructive tone.
QLabel *errorLabel(const PrefsDialog &dlg)
{
    for (QLabel *l : dlg.findChildren<QLabel *>())
    {
        if (l->property("tone").toString() == QLatin1String("destructive"))
        {
            return l;
        }
    }
    return nullptr;
}

// The endpoint URL is the only label the sheet sizes down.
QLabel *urlLabel(const PrefsDialog &dlg)
{
    return dlg.findChild<QLabel *>(QStringLiteral("smallText"));
}

// The deferred apply, picked out by its interval: the dialog's other
// deferrals are QTimer::singleShot calls, which park their helper on the
// event dispatcher rather than on the dialog.
QTimer *applyTimer(const PrefsDialog &dlg)
{
    for (QTimer *t : dlg.findChildren<QTimer *>())
    {
        if (t->isSingleShot() && t->interval() == ApplyMs)
        {
            return t;
        }
    }
    return nullptr;
}

QJsonObject mcpStatus(bool enabled, int port, const QString &error = {})
{
    return QJsonObject{
        {"enabled", enabled},
        {"port", port},
        {"token", QString::fromLatin1(Token)},
        {"url", QStringLiteral("http://127.0.0.1:%1/mcp").arg(port)},
        {"error", error},
    };
}

// Every key emitPrefs writes, on a dialog opened with nothing. Preferences
// persist, so a key that disappears or is renamed here silently resets that
// setting for everyone whose workspace already holds it.
QJsonObject defaultPrefs()
{
    return QJsonObject{
        {"appTheme", theme::defaultApp},      {"editorTheme", theme::defaultEditor},
        {"uiFontSize", DefaultUiFont},        {"editorFontSize", DefaultEditorFont},
        {"tabSize", DefaultTabSize},          {"hideDefaultDBs", true},
        {"historyKeep", DefaultHistoryKeep},  {"copySeparator", "\t"},
        {"defaultRowLimit", DefaultRowLimit},
    };
}

// The same keys, none of them on its default, so a field that is read into the
// wrong widget shows up as the other one's value.
QJsonObject storedPrefs()
{
    return QJsonObject{
        {"appTheme", "nord"},   {"editorTheme", "onedark"},
        {"uiFontSize", 17},     {"editorFontSize", 20},
        {"tabSize", 8},         {"hideDefaultDBs", false},
        {"historyKeep", 250},   {"copySeparator", ";"},
        {"defaultRowLimit", 0},
    };
}

} // namespace

// The dialog is the whole preference contract: what it reads out of the stored
// blob, and what it writes back into it. It applies nothing itself: every
// change leaves as one prefsChanged, and MainWindow is what turns that into
// theme::apply, theme::setCurrentEditor and the per-tab editor settings, so
// the payload is pinned key by key here.
//
// The MCP rows are the exception: those settings live in the backend's SQLite,
// not in the blob, and travel as mcp.Configure instead.
class TestPrefsDialog : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void init();
    void cleanup();

    void anEmptyPrefsObjectOpensOnTheDefaults();
    void theThemeMenusMirrorTheThemeTables();
    void everyStoredPreferenceReachesItsField();
    void anUnknownThemeIdFallsBackToTheFirstEntry();
    void whatIsReadInIsWhatIsWrittenOut();
    void eachEditedFieldLandsInTheEmittedPrefs();

    void theSizeSlidersHoldTheirRange_data();
    void theSizeSlidersHoldTheirRange();
    void aStoredSizeOutsideTheRangeIsClamped_data();
    void aStoredSizeOutsideTheRangeIsClamped();
    void theNumberBoxesHoldTheirRange();
    void theCopySeparatorOffersFourCharacters();

    void draggingASliderDefersTheApplyUntilRelease();
    void aSizeChangeWithNoDragAppliesOnTheTimer();
    void aDragCancelsAPendingApply();
    void theDialogEmitsRatherThanApplyingTheTheme();

    void theHistoryLimitIsPushedOnlyWhenItChanges();
    void everySettingIsLiveSoThereIsNoOkOrCancel();

    void theMcpStatusFillsTheRowsAndLeavesThemDead();
    void theMcpSwitchConfiguresTheListener();
    void aPortChangeAppliesOnlyWhenItChanged();
    void aStatusErrorIsShownWithItsPrefix();
    void aConfigureFailureShowsTheRawError();
    void regeneratingTheTokenReplacesIt();
    void theCopyButtonsHandOutTheEndpoint();

private:
    StubBackend m_backend;
    int m_baseFontPx = 0;
};

void TestPrefsDialog::initTestCase()
{
    // The info glyphs and the row heights are measured from theme::current()
    // as the dialog is constructed, so a palette has to be installed before
    // the first one.
    theme::apply(theme::defaultApp, DefaultUiFont);
    // What an untouched dialog must leave alone: the dialog emits, it does not
    // apply, so this stays put however far the font slider is dragged.
    m_baseFontPx = theme::uiFontSize();
}

void TestPrefsDialog::init()
{
    QVERIFY(m_backend.listening());
    api()->setEndpoint(m_backend.base(), QString());
    m_backend.clearRequests();
    // The reply both constructor calls get. query.HistoryStats reads no key it
    // carries, so it lands as "nothing stored yet" and only writes a tooltip; a
    // slot that wants another MCP status sets it before it builds its dialog.
    m_backend.replyWithResult(mcpStatus(false, McpPort));
}

void TestPrefsDialog::cleanup()
{
    // A dialog destroyed as its slot returns can still have a reply in flight;
    // draining it here keeps it out of the next slot's requests.
    api()->flush(2000);
}

void TestPrefsDialog::anEmptyPrefsObjectOpensOnTheDefaults()
{
    PrefsDialog dlg({});

    QCOMPARE(dlg.windowTitle(), QStringLiteral("Preferences"));
    QCOMPARE(
        combo(dlg, QStringLiteral("App Theme"))->currentData().toString(),
        QString::fromLatin1(theme::defaultApp)
    );
    QCOMPARE(
        combo(dlg, QStringLiteral("Editor Theme"))->currentData().toString(),
        QString::fromLatin1(theme::defaultEditor)
    );
    QCOMPARE(slider(dlg, QStringLiteral("UI Font Size"))->value(), DefaultUiFont);
    QCOMPARE(slider(dlg, QStringLiteral("Editor Font Size"))->value(), DefaultEditorFont);
    QCOMPARE(slider(dlg, QStringLiteral("Tab Size"))->value(), DefaultTabSize);
    QVERIFY(toggle(dlg, QStringLiteral("Hide Default Databases"))->isChecked());
    QCOMPARE(spin(dlg, QStringLiteral("History Per Server"))->value(), DefaultHistoryKeep);
    QCOMPARE(combo(dlg, QStringLiteral("Copy Separator"))->currentData().toString(), "\t");
    QCOMPARE(spin(dlg, QStringLiteral("Default Row Limit"))->value(), DefaultRowLimit);

    // The readouts spell out the same numbers; only the sizes carry the unit.
    QCOMPARE(readout(dlg, QStringLiteral("UI Font Size"))->text(), QStringLiteral("13px"));
    QCOMPARE(readout(dlg, QStringLiteral("Editor Font Size"))->text(), QStringLiteral("13px"));
    QCOMPARE(readout(dlg, QStringLiteral("Tab Size"))->text(), QStringLiteral("4"));

    // The opening values are what the backend already has, so nothing is
    // pushed back at it for being read.
    QVERIFY(waitUntil([this] { return callsTo(m_backend, StatusPath).size() == 1; }));
    QVERIFY(callsTo(m_backend, SetHistoryLimitPath).isEmpty());
    QCOMPARE(callsTo(m_backend, HistoryStatsPath).size(), 1);
    QCOMPARE(callsTo(m_backend, HistoryStatsPath).at(0).args, QJsonArray());
}

void TestPrefsDialog::theThemeMenusMirrorTheThemeTables()
{
    PrefsDialog dlg({});
    QComboBox *appTheme = combo(dlg, QStringLiteral("App Theme"));
    QComboBox *editorTheme = combo(dlg, QStringLiteral("Editor Theme"));
    QVERIFY(appTheme);
    QVERIFY(editorTheme);

    // Adding a palette to theme.cpp is meant to add a row here for free, so
    // the menus are compared against the tables rather than to a copy of them.
    QCOMPARE(appTheme->count(), int(theme::appThemes().size()));
    for (int i = 0; i < appTheme->count(); ++i)
    {
        QCOMPARE(appTheme->itemData(i).toString(), theme::appThemes().at(i).id);
        QCOMPARE(appTheme->itemText(i), theme::appThemes().at(i).label);
    }

    QCOMPARE(editorTheme->count(), int(theme::editorThemes().size()));
    for (int i = 0; i < editorTheme->count(); ++i)
    {
        QCOMPARE(editorTheme->itemData(i).toString(), theme::editorThemes().at(i).id);
        QCOMPARE(editorTheme->itemText(i), theme::editorThemes().at(i).label);
    }
}

void TestPrefsDialog::everyStoredPreferenceReachesItsField()
{
    PrefsDialog dlg(storedPrefs());

    QCOMPARE(combo(dlg, QStringLiteral("App Theme"))->currentData().toString(), "nord");
    QCOMPARE(combo(dlg, QStringLiteral("Editor Theme"))->currentData().toString(), "onedark");
    QCOMPARE(slider(dlg, QStringLiteral("UI Font Size"))->value(), 17);
    QCOMPARE(slider(dlg, QStringLiteral("Editor Font Size"))->value(), 20);
    QCOMPARE(slider(dlg, QStringLiteral("Tab Size"))->value(), 8);
    QVERIFY(!toggle(dlg, QStringLiteral("Hide Default Databases"))->isChecked());
    QCOMPARE(spin(dlg, QStringLiteral("History Per Server"))->value(), 250);
    QCOMPARE(combo(dlg, QStringLiteral("Copy Separator"))->currentData().toString(), ";");
    QCOMPARE(spin(dlg, QStringLiteral("Default Row Limit"))->value(), 0);

    QCOMPARE(readout(dlg, QStringLiteral("UI Font Size"))->text(), QStringLiteral("17px"));
    QCOMPARE(readout(dlg, QStringLiteral("Editor Font Size"))->text(), QStringLiteral("20px"));
    QCOMPARE(readout(dlg, QStringLiteral("Tab Size"))->text(), QStringLiteral("8"));

    // Zero is not a small limit, so the box says what it means instead.
    QCOMPARE(
        spin(dlg, QStringLiteral("Default Row Limit"))->specialValueText(),
        QStringLiteral("Unlimited")
    );
}

void TestPrefsDialog::anUnknownThemeIdFallsBackToTheFirstEntry()
{
    // A palette that was dropped, or one a newer build wrote. findData misses
    // and the menu falls back to index 0 rather than to no selection at all,
    // which would send an empty id back out on the next change.
    PrefsDialog dlg(QJsonObject{{"appTheme", "dracula"}, {"editorTheme", "dracula"}});

    QCOMPARE(
        combo(dlg, QStringLiteral("App Theme"))->currentData().toString(),
        theme::appThemes().first().id
    );
    QCOMPARE(
        combo(dlg, QStringLiteral("Editor Theme"))->currentData().toString(),
        theme::editorThemes().first().id
    );
}

void TestPrefsDialog::whatIsReadInIsWhatIsWrittenOut()
{
    // A key the dialog knows nothing about. The blob is the workspace's, not
    // the dialog's, so opening Preferences must not strip anything from it.
    QJsonObject stored = storedPrefs();
    stored.insert("someKeyThisDialogNeverHeardOf", "kept");

    PrefsDialog dlg(stored);
    QSignalSpy changed(&dlg, &PrefsDialog::prefsChanged);

    // The timeout is the one way to make the dialog emit without editing
    // anything, which is what leaves the comparison an identity.
    QVERIFY(QMetaObject::invokeMethod(applyTimer(dlg), "timeout"));

    QCOMPARE(changed.size(), 1);
    QCOMPARE(changed.at(0).at(0).toJsonObject(), stored);
}

void TestPrefsDialog::eachEditedFieldLandsInTheEmittedPrefs()
{
    PrefsDialog dlg({});
    QSignalSpy changed(&dlg, &PrefsDialog::prefsChanged);

    QComboBox *appTheme = combo(dlg, QStringLiteral("App Theme"));
    QComboBox *editorTheme = combo(dlg, QStringLiteral("Editor Theme"));
    QComboBox *copySep = combo(dlg, QStringLiteral("Copy Separator"));
    appTheme->setCurrentIndex(appTheme->findData(QStringLiteral("solarized")));
    editorTheme->setCurrentIndex(editorTheme->findData(QStringLiteral("nord")));
    copySep->setCurrentIndex(copySep->findData(QStringLiteral("|")));
    slider(dlg, QStringLiteral("UI Font Size"))->setValue(22);
    slider(dlg, QStringLiteral("Editor Font Size"))->setValue(9);
    slider(dlg, QStringLiteral("Tab Size"))->setValue(2);
    toggle(dlg, QStringLiteral("Hide Default Databases"))->setChecked(false);
    spin(dlg, QStringLiteral("History Per Server"))->setValue(1000);
    spin(dlg, QStringLiteral("Default Row Limit"))->setValue(100);

    QVERIFY(QMetaObject::invokeMethod(applyTimer(dlg), "timeout"));
    QVERIFY(!changed.isEmpty());

    // Each field overwrites its own key and nobody else's: a slider wired to
    // the wrong key would show up as the other slider's number.
    QJsonObject want = defaultPrefs();
    want.insert("appTheme", "solarized");
    want.insert("editorTheme", "nord");
    want.insert("copySeparator", "|");
    want.insert("uiFontSize", 22);
    want.insert("editorFontSize", 9);
    want.insert("tabSize", 2);
    want.insert("hideDefaultDBs", false);
    want.insert("historyKeep", 1000);
    want.insert("defaultRowLimit", 100);
    QCOMPARE(changed.last().at(0).toJsonObject(), want);
}

void TestPrefsDialog::theSizeSlidersHoldTheirRange_data()
{
    QTest::addColumn<QString>("label");
    QTest::addColumn<int>("lo");
    QTest::addColumn<int>("hi");
    QTest::addColumn<QString>("suffix");

    QTest::newRow("ui font size") << QStringLiteral("UI Font Size") << 10 << 22
                                  << QStringLiteral("px");
    QTest::newRow("editor font size")
        << QStringLiteral("Editor Font Size") << 9 << 24 << QStringLiteral("px");
    QTest::newRow("tab size") << QStringLiteral("Tab Size") << 2 << 8 << QString();
}

void TestPrefsDialog::theSizeSlidersHoldTheirRange()
{
    QFETCH(QString, label);
    QFETCH(int, lo);
    QFETCH(int, hi);
    QFETCH(QString, suffix);

    PrefsDialog dlg({});
    QSlider *size = slider(dlg, label);
    QLabel *shown = readout(dlg, label);
    QVERIFY(size);
    QVERIFY(shown);

    QCOMPARE(size->minimum(), lo);
    QCOMPARE(size->maximum(), hi);

    // Past either end the slider clamps rather than wrapping, and the readout
    // follows the value that was actually kept.
    size->setValue(hi + 5);
    QCOMPARE(size->value(), hi);
    QCOMPARE(shown->text(), QString::number(hi) + suffix);

    size->setValue(lo - 5);
    QCOMPARE(size->value(), lo);
    QCOMPARE(shown->text(), QString::number(lo) + suffix);
}

void TestPrefsDialog::aStoredSizeOutsideTheRangeIsClamped_data()
{
    QTest::addColumn<QString>("key");
    QTest::addColumn<QString>("label");
    QTest::addColumn<int>("stored");
    QTest::addColumn<int>("kept");

    QTest::newRow("ui font size above the range")
        << QStringLiteral("uiFontSize") << QStringLiteral("UI Font Size") << 99 << 22;
    QTest::newRow("ui font size below the range")
        << QStringLiteral("uiFontSize") << QStringLiteral("UI Font Size") << 4 << 10;
    QTest::newRow("editor font size above the range")
        << QStringLiteral("editorFontSize") << QStringLiteral("Editor Font Size") << 40 << 24;
    QTest::newRow("editor font size below the range")
        << QStringLiteral("editorFontSize") << QStringLiteral("Editor Font Size") << 0 << 9;
    QTest::newRow("tab size above the range")
        << QStringLiteral("tabSize") << QStringLiteral("Tab Size") << 16 << 8;
    QTest::newRow("tab size below the range")
        << QStringLiteral("tabSize") << QStringLiteral("Tab Size") << 0 << 2;
}

void TestPrefsDialog::aStoredSizeOutsideTheRangeIsClamped()
{
    QFETCH(QString, key);
    QFETCH(QString, label);
    QFETCH(int, stored);
    QFETCH(int, kept);

    PrefsDialog dlg(QJsonObject{{key, stored}});
    QCOMPARE(slider(dlg, label)->value(), kept);

    // And the clamped value is what goes back out, so the out-of-range number
    // is gone from the blob rather than lying in wait for a wider slider.
    QSignalSpy changed(&dlg, &PrefsDialog::prefsChanged);
    QVERIFY(QMetaObject::invokeMethod(applyTimer(dlg), "timeout"));
    QCOMPARE(changed.size(), 1);
    QCOMPARE(changed.at(0).at(0).toJsonObject().value(key).toInt(), kept);
}

void TestPrefsDialog::theNumberBoxesHoldTheirRange()
{
    PrefsDialog dlg({});
    QSpinBox *history = spin(dlg, QStringLiteral("History Per Server"));
    QSpinBox *rows = spin(dlg, QStringLiteral("Default Row Limit"));
    QVERIFY(history);
    QVERIFY(rows);

    // A hundred statements is the floor: below that the history stops being
    // usable at all, and there is no "off".
    QCOMPARE(history->minimum(), 100);
    QCOMPARE(history->maximum(), 1000000);
    history->setValue(0);
    QCOMPARE(history->value(), 100);
    history->setValue(9999999);
    QCOMPARE(history->value(), 1000000);

    QCOMPARE(rows->minimum(), 0);
    QCOMPARE(rows->maximum(), 100000000);
    rows->setValue(-1);
    QCOMPARE(rows->value(), 0);
    QCOMPARE(rows->text(), QStringLiteral("Unlimited"));

    // Typed into rather than stepped, so no thousands separator comes between
    // the digits and reads as a syntax error.
    QVERIFY(!history->isGroupSeparatorShown());
    QVERIFY(!rows->isGroupSeparatorShown());
}

void TestPrefsDialog::theCopySeparatorOffersFourCharacters()
{
    PrefsDialog dlg({});
    QComboBox *sep = combo(dlg, QStringLiteral("Copy Separator"));
    QVERIFY(sep);

    // The data, not the label, is what ends up between two copied cells.
    QCOMPARE(sep->count(), 4);
    QCOMPARE(sep->itemData(0).toString(), "\t");
    QCOMPARE(sep->itemData(1).toString(), ",");
    QCOMPARE(sep->itemData(2).toString(), ";");
    QCOMPARE(sep->itemData(3).toString(), "|");
    QCOMPARE(sep->itemText(0), QStringLiteral("Tab"));
    QCOMPARE(sep->itemText(1), QStringLiteral("Comma (,)"));
    QCOMPARE(sep->itemText(2), QStringLiteral("Semicolon (;)"));
    QCOMPARE(sep->itemText(3), QStringLiteral("Pipe (|)"));

    // A separator no longer offered falls back to the tab rather than to
    // nothing, which would paste every row as one run-on line.
    PrefsDialog stale(QJsonObject{{"copySeparator", "~"}});
    QCOMPARE(combo(stale, QStringLiteral("Copy Separator"))->currentData().toString(), "\t");
}

void TestPrefsDialog::draggingASliderDefersTheApplyUntilRelease()
{
    PrefsDialog dlg({});
    QSlider *size = slider(dlg, QStringLiteral("UI Font Size"));
    QTimer *timer = applyTimer(dlg);
    QVERIFY(size);
    QVERIFY(timer);
    QSignalSpy changed(&dlg, &PrefsDialog::prefsChanged);

    // Applying a font size re-lays out this dialog too, so a mid-drag apply
    // would move the handle out from under the pointer. The readout follows
    // every step; nothing else does.
    size->setSliderDown(true);
    size->setValue(18);
    size->setValue(20);
    QCOMPARE(readout(dlg, QStringLiteral("UI Font Size"))->text(), QStringLiteral("20px"));
    QVERIFY(!timer->isActive());
    QVERIFY(changed.isEmpty());

    // Letting go emits once, with where the handle ended up.
    size->setSliderDown(false);
    QCOMPARE(changed.size(), 1);
    QCOMPARE(changed.at(0).at(0).toJsonObject().value("uiFontSize").toInt(), 20);
    QVERIFY(!timer->isActive());
}

void TestPrefsDialog::aSizeChangeWithNoDragAppliesOnTheTimer()
{
    PrefsDialog dlg({});
    QTimer *timer = applyTimer(dlg);
    QVERIFY(timer);
    QSignalSpy changed(&dlg, &PrefsDialog::prefsChanged);

    // An arrow key or a mouse wheel produces no release to apply on, so the
    // timer is the fallback. All three sliders share it.
    slider(dlg, QStringLiteral("UI Font Size"))->setValue(16);
    slider(dlg, QStringLiteral("Editor Font Size"))->setValue(11);
    slider(dlg, QStringLiteral("Tab Size"))->setValue(2);
    QVERIFY(timer->isActive());
    QVERIFY(changed.isEmpty());

    QVERIFY(QMetaObject::invokeMethod(timer, "timeout"));

    // One apply carrying all three, not one per step.
    QCOMPARE(changed.size(), 1);
    const QJsonObject sent = changed.at(0).at(0).toJsonObject();
    QCOMPARE(sent.value("uiFontSize").toInt(), 16);
    QCOMPARE(sent.value("editorFontSize").toInt(), 11);
    QCOMPARE(sent.value("tabSize").toInt(), 2);
}

void TestPrefsDialog::aDragCancelsAPendingApply()
{
    PrefsDialog dlg({});
    QSlider *size = slider(dlg, QStringLiteral("UI Font Size"));
    QTimer *timer = applyTimer(dlg);
    QVERIFY(size);
    QVERIFY(timer);
    QSignalSpy changed(&dlg, &PrefsDialog::prefsChanged);

    size->setValue(16);
    QVERIFY(timer->isActive());

    // Grabbing the handle before the fallback fires takes the pending apply
    // back off: the release is what will apply it now.
    size->setSliderDown(true);
    size->setValue(17);
    QVERIFY(!timer->isActive());
    QVERIFY(changed.isEmpty());

    size->setSliderDown(false);
    QCOMPARE(changed.size(), 1);
    QCOMPARE(changed.at(0).at(0).toJsonObject().value("uiFontSize").toInt(), 17);
}

void TestPrefsDialog::theDialogEmitsRatherThanApplyingTheTheme()
{
    PrefsDialog dlg({});
    QSignalSpy changed(&dlg, &PrefsDialog::prefsChanged);
    QSignalSpy applied(theme::notifier(), &Notifier::changed);

    QComboBox *appTheme = combo(dlg, QStringLiteral("App Theme"));
    QComboBox *editorTheme = combo(dlg, QStringLiteral("Editor Theme"));
    appTheme->setCurrentIndex(appTheme->findData(QStringLiteral("gruvbox")));
    editorTheme->setCurrentIndex(editorTheme->findData(QStringLiteral("solarized")));
    slider(dlg, QStringLiteral("UI Font Size"))->setValue(22);
    QVERIFY(QMetaObject::invokeMethod(applyTimer(dlg), "timeout"));

    // The id, not the label, is what MainWindow hands to theme::apply and
    // theme::setCurrentEditor, and the raw slider value is what it hands to
    // apply beside it.
    QVERIFY(!changed.isEmpty());
    const QJsonObject sent = changed.last().at(0).toJsonObject();
    QCOMPARE(sent.value("appTheme").toString(), QStringLiteral("gruvbox"));
    QCOMPARE(sent.value("editorTheme").toString(), QStringLiteral("solarized"));
    QCOMPARE(sent.value("uiFontSize").toInt(), 22);

    // And the dialog itself applies none of it. theme::apply is what notifies
    // and what moves the base font, so a dialog that had reached past its own
    // signal would show up in both.
    QCOMPARE(applied.size(), 0);
    QCOMPARE(theme::uiFontSize(), m_baseFontPx);
}

void TestPrefsDialog::theHistoryLimitIsPushedOnlyWhenItChanges()
{
    PrefsDialog dlg({});
    QVERIFY(waitUntil([this] { return callsTo(m_backend, StatusPath).size() == 1; }));

    // Retention lives backend-side, so it travels as its own call rather than
    // inside the blob, which Go treats as opaque.
    m_backend.clearRequests();
    spin(dlg, QStringLiteral("History Per Server"))->setValue(500);
    QVERIFY(waitUntil([this] { return callsTo(m_backend, SetHistoryLimitPath).size() == 1; }));
    QCOMPARE(callsTo(m_backend, SetHistoryLimitPath).at(0).args, QJsonArray({500}));

    // A later change to something else emits the whole blob again, retention
    // included, but must not re-push it: an unrelated drag would otherwise
    // prune the history once per step.
    QComboBox *sep = combo(dlg, QStringLiteral("Copy Separator"));
    sep->setCurrentIndex(sep->findData(QStringLiteral(",")));
    api()->flush(2000);
    QTest::qWait(50);
    QCOMPARE(callsTo(m_backend, SetHistoryLimitPath).size(), 1);
}

void TestPrefsDialog::everySettingIsLiveSoThereIsNoOkOrCancel()
{
    PrefsDialog dlg({});
    QSignalSpy changed(&dlg, &PrefsDialog::prefsChanged);

    // Nothing is held back for an OK, so there is nothing a Cancel could
    // discard: the only buttons in the dialog belong to the MCP endpoint row.
    QVERIFY(!dlg.findChild<QDialogButtonBox *>());
    QStringList labels;
    for (QPushButton *b : dlg.findChildren<QPushButton *>())
    {
        labels << b->text();
    }
    labels.sort();
    QCOMPARE(
        labels,
        QStringList(
            {QStringLiteral("Copy Token"), QStringLiteral("Copy URL"), QStringLiteral("Regenerate")}
        )
    );

    toggle(dlg, QStringLiteral("Hide Default Databases"))->setChecked(false);
    QCOMPARE(changed.size(), 1);
    QCOMPARE(dlg.result(), int(QDialog::Rejected));
}

void TestPrefsDialog::theMcpStatusFillsTheRowsAndLeavesThemDead()
{
    PrefsDialog dlg({});
    QCheckBox *sw = toggle(dlg, QStringLiteral("MCP Server"));
    QSpinBox *port = spin(dlg, QStringLiteral("MCP Port"));
    QWidget *endpoint = fieldAt(dlg, QStringLiteral("MCP Endpoint"));
    QVERIFY(sw);
    QVERIFY(port);
    QVERIFY(endpoint);

    QCOMPARE(port->minimum(), 1);
    QCOMPARE(port->maximum(), 65535);

    QVERIFY(waitUntil([port] { return port->value() == McpPort; }));
    QVERIFY(!sw->isChecked());
    QCOMPARE(urlLabel(dlg)->text(), QStringLiteral("http://127.0.0.1:%1/mcp").arg(McpPort));

    // The rows are greyed rather than hidden, because QFormLayout mislays
    // hidden rows, so the port the listener would come back on stays readable.
    QVERIFY(!port->isEnabled());
    QVERIFY(!endpoint->isEnabled());

    // Reflecting the state back into the switch must not echo a Configure:
    // opening Preferences would then bounce a listener somebody is using.
    api()->flush(2000);
    QTest::qWait(50);
    QVERIFY(callsTo(m_backend, ConfigurePath).isEmpty());
    QVERIFY(errorLabel(dlg)->isHidden());
}

void TestPrefsDialog::theMcpSwitchConfiguresTheListener()
{
    PrefsDialog dlg({});
    QSpinBox *port = spin(dlg, QStringLiteral("MCP Port"));
    QVERIFY(port);
    QVERIFY(waitUntil([port] { return port->value() == McpPort; }));

    m_backend.replyWithResult(mcpStatus(true, McpPort));
    m_backend.clearRequests();
    toggle(dlg, QStringLiteral("MCP Server"))->setChecked(true);

    QVERIFY(waitUntil([this] { return callsTo(m_backend, ConfigurePath).size() == 1; }));
    const StubBackend::Request req = callsTo(m_backend, ConfigurePath).at(0);
    QCOMPARE(req.verb, QStringLiteral("POST"));
    // The port the row is showing travels with the switch: enabling on its own
    // would leave the backend to guess which port was meant.
    QCOMPARE(req.args, QJsonArray({true, McpPort}));

    QVERIFY(waitUntil([port] { return port->isEnabled(); }));
    QVERIFY(fieldAt(dlg, QStringLiteral("MCP Endpoint"))->isEnabled());
}

void TestPrefsDialog::aPortChangeAppliesOnlyWhenItChanged()
{
    m_backend.replyWithResult(mcpStatus(true, McpPort));
    PrefsDialog dlg({});
    QSpinBox *port = spin(dlg, QStringLiteral("MCP Port"));
    QVERIFY(port);
    QVERIFY(waitUntil([port] { return port->value() == McpPort; }));

    // editingFinished also fires on plain focus loss, so leaving the field
    // untouched must not restart the listener under whoever is using it.
    m_backend.clearRequests();
    QVERIFY(QMetaObject::invokeMethod(port, "editingFinished"));
    api()->flush(2000);
    QTest::qWait(50);
    QVERIFY(callsTo(m_backend, ConfigurePath).isEmpty());

    m_backend.replyWithResult(mcpStatus(true, OtherMcpPort));
    port->setValue(OtherMcpPort);
    QVERIFY(QMetaObject::invokeMethod(port, "editingFinished"));
    QVERIFY(waitUntil([this] { return callsTo(m_backend, ConfigurePath).size() == 1; }));
    QCOMPARE(callsTo(m_backend, ConfigurePath).at(0).args, QJsonArray({true, OtherMcpPort}));

    // Typing a port while the listener is off changes nothing until the switch
    // is thrown: there is no listener to move.
    m_backend.replyWithResult(mcpStatus(false, OtherMcpPort));
    QCheckBox *sw = toggle(dlg, QStringLiteral("MCP Server"));
    QVERIFY(sw);
    sw->setChecked(false);
    QVERIFY(waitUntil([this] { return callsTo(m_backend, ConfigurePath).size() == 2; }));
    // Both replies have to land before the switch reads as off: each one
    // reflects a whole status back into it.
    api()->flush(2000);
    QVERIFY(!sw->isChecked());
    m_backend.clearRequests();
    port->setValue(McpPort);
    QVERIFY(QMetaObject::invokeMethod(port, "editingFinished"));
    api()->flush(2000);
    QTest::qWait(50);
    QVERIFY(callsTo(m_backend, ConfigurePath).isEmpty());
}

void TestPrefsDialog::aStatusErrorIsShownWithItsPrefix()
{
    const QString failure =
        QStringLiteral("listen tcp 127.0.0.1:%1: bind: address already in use").arg(McpPort);
    m_backend.replyWithResult(mcpStatus(false, McpPort, failure));

    PrefsDialog dlg({});
    QLabel *error = errorLabel(dlg);
    QVERIFY(error);

    // The dialog is never shown, so the label's own hidden flag is what says
    // whether the message would be on screen. The row is added rather than
    // unhidden: QFormLayout still spends a hidden row's spacing, which opened
    // the dialog clipped.
    QVERIFY(error->isHidden());
    QVERIFY(waitUntil([error] { return !error->isHidden(); }));
    QCOMPARE(error->text(), QStringLiteral("MCP: %1").arg(failure));
}

void TestPrefsDialog::aConfigureFailureShowsTheRawError()
{
    PrefsDialog dlg({});
    QSpinBox *port = spin(dlg, QStringLiteral("MCP Port"));
    QVERIFY(port);
    QVERIFY(waitUntil([port] { return port->value() == McpPort; }));

    m_backend.replyWithError(QStringLiteral("mcp: port 80 needs root"));
    toggle(dlg, QStringLiteral("MCP Server"))->setChecked(true);

    QLabel *error = errorLabel(dlg);
    QVERIFY(error);
    QVERIFY(waitUntil([error] { return !error->isHidden(); }));
    // Straight from the RPC, without the "MCP: " the status path prepends.
    QCOMPARE(error->text(), QStringLiteral("mcp: port 80 needs root"));

    // Nothing came back to reflect, so the endpoint rows stay dead.
    QVERIFY(!port->isEnabled());
    QVERIFY(!fieldAt(dlg, QStringLiteral("MCP Endpoint"))->isEnabled());
}

void TestPrefsDialog::regeneratingTheTokenReplacesIt()
{
    m_backend.replyWithResult(mcpStatus(true, McpPort));
    PrefsDialog dlg({});
    QSpinBox *port = spin(dlg, QStringLiteral("MCP Port"));
    QVERIFY(port);
    QVERIFY(waitUntil([port] { return port->value() == McpPort; }));

    QPushButton *regen = button(dlg, QStringLiteral("Regenerate"));
    QVERIFY(regen);
    QCOMPARE(regen->toolTip(), QStringLiteral("Invalidates the current token"));

    QJsonObject fresh = mcpStatus(true, OtherMcpPort);
    fresh.insert("token", QString::fromLatin1(FreshToken));
    m_backend.replyWithResult(fresh);
    m_backend.clearRequests();
    regen->click();

    QVERIFY(waitUntil([this] { return callsTo(m_backend, RegeneratePath).size() == 1; }));
    QCOMPARE(callsTo(m_backend, RegeneratePath).at(0).args, QJsonArray());

    // The reply is a whole status, not just a token, so the whole endpoint row
    // is rebuilt from it; the moved port is what makes that observable.
    QVERIFY(waitUntil([port] { return port->value() == OtherMcpPort; }));
    QCOMPARE(urlLabel(dlg)->text(), QStringLiteral("http://127.0.0.1:%1/mcp").arg(OtherMcpPort));

    QPushButton *copyToken = button(dlg, QStringLiteral("Copy Token"));
    QVERIFY(copyToken);
    QApplication::clipboard()->clear();
    copyToken->click();
    QCOMPARE(QApplication::clipboard()->text(), QString::fromLatin1(FreshToken));
}

void TestPrefsDialog::theCopyButtonsHandOutTheEndpoint()
{
    m_backend.replyWithResult(mcpStatus(true, McpPort));
    PrefsDialog dlg({});
    QLabel *url = urlLabel(dlg);
    QVERIFY(url);
    QVERIFY(waitUntil([url] { return !url->text().isEmpty(); }));

    QPushButton *copyUrl = button(dlg, QStringLiteral("Copy URL"));
    QPushButton *copyToken = button(dlg, QStringLiteral("Copy Token"));
    QVERIFY(copyUrl);
    QVERIFY(copyToken);

    QApplication::clipboard()->clear();
    copyUrl->click();
    QCOMPARE(QApplication::clipboard()->text(), url->text());
    // The label says so for a second and a half; the restore is a timer this
    // slot deliberately outruns.
    QCOMPARE(copyUrl->text(), QStringLiteral("Copied"));

    QApplication::clipboard()->clear();
    copyToken->click();
    // The token is never on screen, only on the clipboard.
    QCOMPARE(QApplication::clipboard()->text(), QString::fromLatin1(Token));
    QVERIFY(!url->text().contains(QString::fromLatin1(Token)));
    QCOMPARE(copyToken->text(), QStringLiteral("Copied"));
}

QTEST_MAIN(TestPrefsDialog)

#include "tst_prefsdialog.moc"
