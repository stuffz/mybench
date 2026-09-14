#include "app/theme.h"

#include <QApplication>
#include <QColor>
#include <QFont>
#include <QObject>
#include <QPalette>
#include <QRegularExpression>
#include <QSet>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QString>
#include <QStringList>
#include <QTest>
#include <QVector>

namespace
{

// The slider value the sizing tests start from: far enough above the 8px floor
// that scaledPx only clamps where the clamp is what is being asserted.
constexpr int BaseFontPx = 13;

// Both palettes are plain structs, so the members have to be listed by hand.
// The static_assert is what keeps the list honest: a colour added to the
// struct and forgotten in one theme table reads back as an invalid QColor,
// which is exactly what the callers below look for.
QVector<QColor> colours(const AppPalette &p)
{
    static_assert(sizeof(AppPalette) == 20 * sizeof(QColor), "list every AppPalette colour below");
    return {p.background,  p.foreground, p.card,    p.cardFg,  p.primary,
            p.primaryFg,   p.secondary,  p.muted,   p.mutedFg, p.accent,
            p.destructive, p.warning,    p.success, p.info,    p.special,
            p.border,      p.input,      p.ring,    p.sidebar, p.sidebarBorder};
}

QVector<QColor> colours(const EditorPalette &p)
{
    static_assert(
        sizeof(EditorPalette) == 12 * sizeof(QColor), "list every EditorPalette colour below"
    );
    return {p.bg,      p.fg,      p.caret,  p.sel,    p.line, p.panel,
            p.comment, p.keyword, p.string, p.number, p.func, p.type};
}

} // namespace

class TestTheme : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();

    void appThemesAreUniquelyIdentified();
    void editorThemesAreUniquelyIdentified();
    void everyAppThemeFillsItsWholePalette();
    void everyEditorThemeFillsItsWholePalette();

    void lookupReturnsTheThemesOwnPalette();
    void anUnknownIdFallsBackToTheDefault();
    void currentEditorFollowsTheIdLastSet();

    void connAccentKeepsAProfilesColour();
    void monoFamilyResolvesTheEmbeddedFont();

    void applyInstallsThePaletteAndSaysSo();
    void applyLeavesNoStylesheetTokenUnfilled();
    void fontSizeFollowsTheSliderThroughTheDpi();
    void scaledPxClampsBeforeItScales();
    void dpiPxMatchesTheFontsOwnCorrection();
};

void TestTheme::initTestCase()
{
    // apply() renders the dropdown chevron into the cache dir; test mode keeps
    // this run's copies out of the real one.
    QStandardPaths::setTestModeEnabled(true);

    // Nothing reads a sensible uiFontSize() until the first apply(): the
    // file-static starts at the raw slider default, uncorrected for DPI.
    theme::apply(theme::defaultApp, BaseFontPx);
}

void TestTheme::appThemesAreUniquelyIdentified()
{
    const QVector<AppTheme> &themes = theme::appThemes();
    QVERIFY(!themes.isEmpty());

    QSet<QString> ids;
    for (const AppTheme &t : themes)
    {
        QVERIFY(!t.id.isEmpty());
        QVERIFY(!t.label.isEmpty());

        // A repeated id leaves the second entry unreachable through app(),
        // while still offering it in the Preferences combo.
        QVERIFY2(!ids.contains(t.id), qPrintable(t.id));
        ids.insert(t.id);
    }

    QVERIFY(ids.contains(QString::fromLatin1(theme::defaultApp)));
}

void TestTheme::editorThemesAreUniquelyIdentified()
{
    const QVector<EditorTheme> &themes = theme::editorThemes();
    QVERIFY(!themes.isEmpty());

    QSet<QString> ids;
    for (const EditorTheme &t : themes)
    {
        QVERIFY(!t.id.isEmpty());
        QVERIFY(!t.label.isEmpty());
        QVERIFY2(!ids.contains(t.id), qPrintable(t.id));
        ids.insert(t.id);
    }

    QVERIFY(ids.contains(QString::fromLatin1(theme::defaultEditor)));
}

void TestTheme::everyAppThemeFillsItsWholePalette()
{
    for (const AppTheme &t : theme::appThemes())
    {
        const QVector<QColor> all = colours(t.p);
        for (qsizetype i = 0; i < all.size(); ++i)
        {
            QVERIFY2(
                all.at(i).isValid(), qPrintable(QStringLiteral("%1 colour %2").arg(t.id).arg(i))
            );
        }
    }
}

void TestTheme::everyEditorThemeFillsItsWholePalette()
{
    for (const EditorTheme &t : theme::editorThemes())
    {
        const QVector<QColor> all = colours(t.p);
        for (qsizetype i = 0; i < all.size(); ++i)
        {
            QVERIFY2(
                all.at(i).isValid(), qPrintable(QStringLiteral("%1 colour %2").arg(t.id).arg(i))
            );
        }
    }
}

void TestTheme::lookupReturnsTheThemesOwnPalette()
{
    // Identity, not equality: what comes back is the table entry itself, so
    // nothing here has to restate the colours the table already holds.
    for (const AppTheme &t : theme::appThemes())
    {
        QCOMPARE(&theme::app(t.id), &t.p);
    }

    for (const EditorTheme &t : theme::editorThemes())
    {
        QCOMPARE(&theme::editor(t.id), &t.p);
    }
}

void TestTheme::anUnknownIdFallsBackToTheDefault()
{
    const QString missing = QStringLiteral("no-such-theme");

    QCOMPARE(&theme::app(missing), &theme::app(theme::defaultApp));
    QCOMPARE(&theme::editor(missing), &theme::editor(theme::defaultEditor));

    // Resolved by name, so the tables above can be reordered freely without
    // changing where a settings file holding a dropped theme id lands.

    for (const QColor &c : colours(theme::app(missing)))
    {
        QVERIFY(c.isValid());
    }

    for (const QColor &c : colours(theme::editor(missing)))
    {
        QVERIFY(c.isValid());
    }
}

void TestTheme::currentEditorFollowsTheIdLastSet()
{
    for (const EditorTheme &t : theme::editorThemes())
    {
        theme::setCurrentEditor(t.id);
        QCOMPARE(&theme::currentEditor(), &t.p);
    }

    // A stale id from the settings file must land on the default, not leave
    // the previous theme in place.
    theme::setCurrentEditor(QStringLiteral("no-such-theme"));
    QCOMPARE(&theme::currentEditor(), &theme::editor(theme::defaultEditor));

    theme::setCurrentEditor(QString::fromLatin1(theme::defaultEditor));
}

void TestTheme::connAccentKeepsAProfilesColour()
{
    const QString prod = QStringLiteral("prod");
    QCOMPARE(theme::connAccent(prod), theme::connAccent(prod));

    // The web build hashes ids with the same rolling hash, so the hue is
    // pinned: changing it gives one profile two different colours depending on
    // which UI is looking at it.
    QCOMPARE(theme::connAccent(prod).hslHue(), 167);
    QCOMPARE(theme::connAccent(QStringLiteral("dev")).hslHue(), 349);

    const QStringList ids{
        QStringLiteral("prod"),  QStringLiteral("dev"), QStringLiteral("staging"),
        QStringLiteral("local"), QStringLiteral("db1"), QStringLiteral("db2"),
    };
    QSet<int> hues;
    for (const QString &id : ids)
    {
        const QColor c = theme::connAccent(id);
        QVERIFY(c.isValid());

        // Only the hue carries the hash; saturation and lightness are fixed so
        // every accent reads with the same weight against the theme.
        QCOMPARE(c.hslSaturation(), 153);
        QCOMPARE(c.lightness(), 128);
        hues.insert(c.hslHue());
    }
    QCOMPARE(hues.size(), ids.size());
}

void TestTheme::monoFamilyResolvesTheEmbeddedFont()
{
    const QString family = theme::monoFamily();
    QVERIFY(!family.isEmpty());

    // The family is cached after the first load, so re-registering the same
    // four files must not rename the font out from under the stylesheet.
    QCOMPARE(theme::loadFonts(), family);
    QCOMPARE(theme::monoFamily(), family);
}

void TestTheme::applyInstallsThePaletteAndSaysSo()
{
    QSignalSpy spy(theme::notifier(), &Notifier::changed);

    for (const AppTheme &t : theme::appThemes())
    {
        theme::apply(t.id, BaseFontPx);
        QCOMPARE(&theme::current(), &t.p);

        // Painted widgets read QPalette rather than the sheet, so the two have
        // to be installed together or the grid keeps the old background.
        QCOMPARE(qApp->palette().color(QPalette::Window), t.p.background);
        QCOMPARE(qApp->palette().color(QPalette::WindowText), t.p.foreground);
        QCOMPARE(qApp->font().pixelSize(), theme::uiFontSize());
    }
    QCOMPARE(spy.count(), theme::appThemes().size());

    // Widgets that captured colours only reload on changed(), so an unknown id
    // still has to run the whole of apply() rather than return early.
    theme::apply(QStringLiteral("no-such-theme"), BaseFontPx);
    QCOMPARE(&theme::current(), &theme::app(theme::defaultApp));
    QCOMPARE(spy.count(), theme::appThemes().size() + 1);
}

void TestTheme::applyLeavesNoStylesheetTokenUnfilled()
{
    const QString nord = QStringLiteral("nord");
    theme::apply(nord, BaseFontPx);

    const QString sheet = qApp->styleSheet();
    QVERIFY(!sheet.isEmpty());
    QVERIFY(sheet.contains(theme::app(nord).background.name()));
    QVERIFY(sheet.contains(theme::monoFamily()));

    // A token added to the template but missed in the replace chain survives
    // as literal "%NAME%", and Qt drops the rule around it without a word.
    const QRegularExpressionMatch unfilled =
        QRegularExpression(QStringLiteral("%[A-Z]+%")).match(sheet);
    QVERIFY2(!unfilled.hasMatch(), qPrintable(unfilled.captured()));

    theme::apply(theme::defaultApp, BaseFontPx);
}

void TestTheme::fontSizeFollowsTheSliderThroughTheDpi()
{
    // The correction is the screen's logical DPI over 96, bounded to [1, 4],
    // so the slider value is a floor and four times it a ceiling. An exact
    // size here would pin the test to the DPI of whatever runs it.
    theme::apply(theme::defaultApp, BaseFontPx);
    const int base = theme::uiFontSize();
    QVERIFY(base >= BaseFontPx);
    QVERIFY(base <= BaseFontPx * 4);

    theme::apply(theme::defaultApp, BaseFontPx * 2);
    const int doubled = theme::uiFontSize();
    QVERIFY(doubled > base);
    QCOMPARE(qApp->font().pixelSize(), doubled);

    theme::apply(theme::defaultApp, BaseFontPx);
    QCOMPARE(theme::uiFontSize(), base);
}

void TestTheme::scaledPxClampsBeforeItScales()
{
    theme::apply(theme::defaultApp, BaseFontPx);
    const int base = theme::uiFontSize();

    QCOMPARE(theme::scaledPx(1.0), base);
    QCOMPARE(theme::scaledPx(2.0), base * 2);

    // Without the floor a caption at a small factor rounds away to nothing.
    QCOMPARE(theme::scaledPx(0.01), 8);
    QCOMPARE(theme::scaledPx(0.5, base * 4), base * 4);
    QCOMPARE(theme::scaledPx(0.0, 1), 1);
}

void TestTheme::dpiPxMatchesTheFontsOwnCorrection()
{
    theme::apply(theme::defaultApp, BaseFontPx);

    // The editor font comes from its own slider through dpiPx(), so the same
    // input has to land on the same size the UI font got, or the two panes
    // disagree about what one px is.
    QCOMPARE(theme::dpiPx(BaseFontPx), theme::uiFontSize());

    QVERIFY(theme::dpiPx(10) >= 10);
    QVERIFY(theme::dpiPx(10) <= 40);
    QCOMPARE(theme::dpiPx(0), 1);
}

QTEST_MAIN(TestTheme)

#include "tst_theme.moc"
