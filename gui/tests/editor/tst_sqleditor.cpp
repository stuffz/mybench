#include "editor/sqleditor.h"

#include "app/theme.h"

#include <QAbstractItemModel>
#include <QAbstractItemView>
#include <QColor>
#include <QCompleter>
#include <QCoreApplication>
#include <QEvent>
#include <QFontMetricsF>
#include <QHash>
#include <QHelpEvent>
#include <QKeyEvent>
#include <QObject>
#include <QPalette>
#include <QPoint>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QString>
#include <QStringList>
#include <QTest>
#include <QTextCursor>
#include <QTextEdit>
#include <QToolTip>
#include <QVector>

// The editor keeps its completer, its row model and its gutter private, but the
// completer is a child object, so the rows the popup is about to show can be
// read back through it, with no accessor the application itself does not need.
// Everything else here goes through QPlainTextEdit's own accessors and the
// editor's signals.
//
// The gutter is painted, not reported: paintLineNumbers draws through a
// QPainter and nothing records what it drew, so only the width it lays out for
// is asserted below.
namespace
{

// apply() wants a slider value; nothing here depends on the size.
constexpr int BaseFontPx = 13;

// A second editor theme, only to have colours that differ from the applied
// one; any other theme would do.
constexpr auto OtherEditorTheme = "onedark";

// The row's insert text and its right-hand detail, both private roles in
// sqleditor.cpp.
constexpr int InsertRole = Qt::UserRole + 2;
constexpr int DetailRole = Qt::UserRole + 1;

// The gutter's padding and its two-digit minimum, spelled out again here.
constexpr int GutterPadding = 10;
constexpr int MinimumDigits = 2;

// The completion schema, and the same one the scan's own test uses: two tables
// in one schema, sharing a column name so the pool's dedupe has something to
// fold.
QHash<QString, QStringList> testSchema()
{
    return {
        {QStringLiteral("mydb.users"), {QStringLiteral("id"), QStringLiteral("name")}},
        {QStringLiteral("mydb.orders"), {QStringLiteral("id"), QStringLiteral("user_id")}},
    };
}

// More tables than the popup will ever show, to reach the cap.
QHash<QString, QStringList> wideSchema(int tables)
{
    QHash<QString, QStringList> schema;
    for (int i = 0; i < tables; ++i)
    {
        schema.insert(QStringLiteral("s.t%1").arg(i, 2, 10, QLatin1Char('0')), QStringList());
    }
    return schema;
}

QString numberedLines(int count)
{
    QStringList lines;
    for (int i = 0; i < count; ++i)
    {
        lines << QStringLiteral("SELECT %1;").arg(i);
    }
    return lines.join(QLatin1Char('\n'));
}

QCompleter *completerOf(const SqlEditor &editor)
{
    return editor.findChild<QCompleter *>();
}

QAbstractItemView *popupOf(const SqlEditor &editor)
{
    return completerOf(editor)->popup();
}

// One role read off every row the popup would show, in the order the editor
// appended them.
QStringList rowData(const SqlEditor &editor, int role)
{
    QAbstractItemModel *model = completerOf(editor)->model();
    QStringList out;
    for (int row = 0; row < model->rowCount(); ++row)
    {
        out << model->index(row, 0).data(role).toString();
    }
    return out;
}

QStringList labels(const SqlEditor &editor)
{
    return rowData(editor, Qt::DisplayRole);
}

QStringList inserts(const SqlEditor &editor)
{
    return rowData(editor, InsertRole);
}

QStringList details(const SqlEditor &editor)
{
    return rowData(editor, DetailRole);
}

void placeCursor(SqlEditor &editor, int at)
{
    QTextCursor cursor = editor.textCursor();
    cursor.setPosition(at);
    editor.setTextCursor(cursor);
}

void selectRange(SqlEditor &editor, int from, int to)
{
    QTextCursor cursor = editor.textCursor();
    cursor.setPosition(from);
    cursor.setPosition(to, QTextCursor::KeepAnchor);
    editor.setTextCursor(cursor);
}

// keyPressEvent is protected and the editor is never mapped under offscreen, so
// the event goes straight to it rather than through the window system.
void sendKey(SqlEditor &editor, int key, Qt::KeyboardModifiers modifiers = Qt::NoModifier)
{
    QKeyEvent press(QEvent::KeyPress, key, modifiers);
    QCoreApplication::sendEvent(&editor, &press);
}

// Qt's key code for a printable ASCII character is the upper-cased character
// itself, which is what the bracket and quote handling reads back out of
// QKeyEvent::text().
void typeText(SqlEditor &editor, const QString &text)
{
    for (const QChar &ch : text)
    {
        QKeyEvent press(QEvent::KeyPress, int(ch.toUpper().unicode()), Qt::NoModifier, QString(ch));
        QCoreApplication::sendEvent(&editor, &press);
    }
}

bool sendToolTip(SqlEditor &editor)
{
    QHelpEvent tip(QEvent::ToolTip, QPoint(1, 1), QPoint(1, 1));
    return QCoreApplication::sendEvent(&editor, &tip);
}

} // namespace

class TestSqlEditor : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();

    void ctrlSpaceOffersSchemasThenTablesWhenNoTableIsNamed();
    void ctrlSpaceOffersTheColumnsOfTheStatementsTables();
    void withNoSchemaThereIsNothingToOffer();
    void typingRetriggersAndAnEmptyPrefixClosesThePopup();
    void aPrefixRanksTheTableOverItsQualifiedPath();
    void aQualifiedPrefixOffersOnlyThatTablesColumns();
    void theOfferedListStopsAtFiftyRows();
    void everyCandidateCarriesWhereItCameFrom();

    void acceptingWithEnterReplacesTheWholePrefix();
    void acceptingWithTabKeepsTheAliasAndTheRestOfTheLine();
    void escapeClosesThePopupAndTypesNothing();
    void theArrowKeysGoToThePopupNotTheCaret();
    void ctrlEnterRunsEvenWithThePopupOpen();

    void statementToRunFollowsTheCursor();
    void statementToRunPrefersTheSelection();
    void theRunAndFormatKeysEmitAndTypeNothing();

    void bracketsAndQuotesCloseThemselves_data();
    void bracketsAndQuotesCloseThemselves();
    void aSelectionIsReplacedRatherThanWrapped();
    void insertSnippetLandsAtTheCursor();

    void diagnosticsSquiggleInTheirOwnTone();
    void diagnosticsClampToTheDocumentAndClear();
    void diagnosticsFollowLaterEdits();
    void aTooltipShowsOnlyOverADiagnostic();

    void theDocumentSettlesOnceAfterABurstOfEdits();

    void theFontSizeRidesInTheSheetAsWellAsTheWidget();
    void theTabStopFollowsTheFontAndTheCharCount();
    void thePaletteReachesTheWidgetAndTheSheet();
    void theGutterKeepsRoomForTwoDigitsAndGrows();
};

void TestSqlEditor::initTestCase()
{
    // apply() renders the dropdown chevron into the cache dir; test mode keeps
    // this run's copies out of the real one.
    QStandardPaths::setTestModeEnabled(true);

    // The editor sizes its font against theme::dpiPx and styles itself from
    // theme::currentEditor() in its constructor; apply() is what installs both.
    theme::apply(theme::defaultApp, BaseFontPx);
}

void TestSqlEditor::ctrlSpaceOffersSchemasThenTablesWhenNoTableIsNamed()
{
    SqlEditor editor;
    editor.setCompletionSchema(testSchema());
    editor.setPlainText(QStringLiteral("SELECT "));
    placeCursor(editor, 7);

    QVERIFY2(completerOf(editor), "the completer is the only way in to the rows the popup shows");
    sendKey(editor, Qt::Key_Space, Qt::ControlModifier);

    // Nothing is referenced yet, so the offer is what a statement can start
    // from: schemas first, then tables, each group in its own alphabetical
    // order.
    QVERIFY(popupOf(editor)->isVisible());
    QCOMPARE(
        labels(editor),
        QStringList({QStringLiteral("mydb"), QStringLiteral("orders"), QStringLiteral("users")})
    );
    QCOMPARE(
        details(editor),
        QStringList({QStringLiteral("schema"), QStringLiteral("mydb"), QStringLiteral("mydb")})
    );

    // Nothing here is qualified, so accepting a row inserts what it shows.
    QCOMPARE(inserts(editor), labels(editor));
}

void TestSqlEditor::ctrlSpaceOffersTheColumnsOfTheStatementsTables()
{
    SqlEditor editor;
    editor.setCompletionSchema(testSchema());
    const QString sql =
        QStringLiteral("SELECT * FROM mydb.users u JOIN mydb.orders o ON o.user_id = u.id WHERE ");
    editor.setPlainText(sql);
    placeCursor(editor, int(sql.size()));

    sendKey(editor, Qt::Key_Space, Qt::ControlModifier);

    // Both tables in statement order, and the column they share is offered
    // once, under the table that named it first.
    QVERIFY(popupOf(editor)->isVisible());
    QCOMPARE(
        labels(editor),
        QStringList({QStringLiteral("id"), QStringLiteral("name"), QStringLiteral("user_id")})
    );
    const QString users = QStringLiteral("mydb.users");
    QCOMPARE(details(editor), QStringList({users, users, QStringLiteral("mydb.orders")}));
    QCOMPARE(inserts(editor), labels(editor));
}

void TestSqlEditor::withNoSchemaThereIsNothingToOffer()
{
    // No setCompletionSchema call: the pool is empty, keywords included, so
    // every branch has to come up with nothing rather than an empty popup.
    SqlEditor editor;
    editor.setPlainText(QStringLiteral("SELECT "));
    placeCursor(editor, 7);
    sendKey(editor, Qt::Key_Space, Qt::ControlModifier);
    QVERIFY(!popupOf(editor)->isVisible());

    editor.setPlainText(QStringLiteral("SELECT * FROM us"));
    placeCursor(editor, 16);
    sendKey(editor, Qt::Key_Space, Qt::ControlModifier);
    QVERIFY(!popupOf(editor)->isVisible());
}

void TestSqlEditor::typingRetriggersAndAnEmptyPrefixClosesThePopup()
{
    SqlEditor editor;
    editor.setCompletionSchema(testSchema());
    editor.setPlainText(QStringLiteral("SELECT * FROM "));
    placeCursor(editor, 14);

    typeText(editor, QStringLiteral("u"));
    QCOMPARE(editor.toPlainText(), QStringLiteral("SELECT * FROM u"));
    QVERIFY(popupOf(editor)->isVisible());
    QVERIFY(labels(editor).contains(QStringLiteral("users")));

    // The space ends the word: a typing retrigger never falls back to the bare
    // list the way Ctrl+Space does, so the popup closes instead.
    typeText(editor, QStringLiteral(" "));
    QCOMPARE(editor.toPlainText(), QStringLiteral("SELECT * FROM u "));
    QVERIFY(!popupOf(editor)->isVisible());
}

void TestSqlEditor::aPrefixRanksTheTableOverItsQualifiedPath()
{
    SqlEditor editor;
    editor.setCompletionSchema(testSchema());
    editor.setPlainText(QStringLiteral("SELECT * FROM user"));
    placeCursor(editor, 18);

    sendKey(editor, Qt::Key_Space, Qt::ControlModifier);
    const QStringList shown = labels(editor);

    // The pool's boosts ride on top of the fuzzy score: a table (+1) outranks
    // the column that only contains the same letters (-2), which in turn
    // outranks the qualified path (-1) the match starts five characters into.
    QCOMPARE(shown.value(0), QStringLiteral("users"));
    QVERIFY(shown.contains(QStringLiteral("user_id")));
    QVERIFY(shown.contains(QStringLiteral("mydb.users")));
    QVERIFY(shown.indexOf(QStringLiteral("user_id")) < shown.indexOf(QStringLiteral("mydb.users")));
}

void TestSqlEditor::aQualifiedPrefixOffersOnlyThatTablesColumns()
{
    SqlEditor editor;
    editor.setCompletionSchema(testSchema());
    const QString sql = QStringLiteral("SELECT * FROM mydb.users u WHERE u.");
    editor.setPlainText(sql);
    placeCursor(editor, int(sql.size()));

    sendKey(editor, Qt::Key_Space, Qt::ControlModifier);

    // The alias resolves to its table, so the offer is that table's columns in
    // schema order, with no keywords and no other table's columns, and each
    // row carries the alias into the text it inserts.
    QCOMPARE(labels(editor), QStringList({QStringLiteral("id"), QStringLiteral("name")}));
    QCOMPARE(inserts(editor), QStringList({QStringLiteral("u.id"), QStringLiteral("u.name")}));
    QCOMPARE(
        details(editor), QStringList({QStringLiteral("mydb.users"), QStringLiteral("mydb.users")})
    );

    // Past the dot the prefix filters the same list.
    typeText(editor, QStringLiteral("na"));
    QCOMPARE(labels(editor), QStringList({QStringLiteral("name")}));
    QCOMPARE(inserts(editor), QStringList({QStringLiteral("u.name")}));
}

void TestSqlEditor::theOfferedListStopsAtFiftyRows()
{
    SqlEditor editor;
    editor.setCompletionSchema(wideSchema(60));

    editor.setPlainText(QStringLiteral("SELECT "));
    placeCursor(editor, 7);
    sendKey(editor, Qt::Key_Space, Qt::ControlModifier);
    QCOMPARE(labels(editor).size(), 50);

    // The same cap on the fuzzy branch, which every table and every qualified
    // path matches.
    editor.setPlainText(QStringLiteral("SELECT * FROM t"));
    placeCursor(editor, 15);
    sendKey(editor, Qt::Key_Space, Qt::ControlModifier);
    QCOMPARE(labels(editor).size(), 50);
}

void TestSqlEditor::everyCandidateCarriesWhereItCameFrom()
{
    SqlEditor editor;
    editor.setCompletionSchema(testSchema());

    editor.setPlainText(QStringLiteral("sele"));
    placeCursor(editor, 4);
    sendKey(editor, Qt::Key_Space, Qt::ControlModifier);
    const qsizetype keywordRow = labels(editor).indexOf(QStringLiteral("SELECT"));
    QVERIFY(keywordRow >= 0);
    QCOMPARE(details(editor).at(keywordRow), QStringLiteral("keyword"));

    // A column in one table names it; a column both tables carry names one and
    // counts the rest, and which one it names follows the schema map's own
    // order.
    editor.setPlainText(QStringLiteral("nam"));
    placeCursor(editor, 3);
    sendKey(editor, Qt::Key_Space, Qt::ControlModifier);
    const qsizetype nameRow = labels(editor).indexOf(QStringLiteral("name"));
    QVERIFY(nameRow >= 0);
    QCOMPARE(details(editor).at(nameRow), QStringLiteral("mydb.users"));

    editor.setPlainText(QStringLiteral("id"));
    placeCursor(editor, 2);
    sendKey(editor, Qt::Key_Space, Qt::ControlModifier);
    const qsizetype idRow = labels(editor).indexOf(QStringLiteral("id"));
    QVERIFY(idRow >= 0);
    QVERIFY(details(editor).at(idRow).endsWith(QStringLiteral(" +1 more")));
}

void TestSqlEditor::acceptingWithEnterReplacesTheWholePrefix()
{
    SqlEditor editor;
    editor.setCompletionSchema(testSchema());
    editor.setPlainText(QStringLiteral("SELECT * FROM mydb.us"));
    placeCursor(editor, 21);

    sendKey(editor, Qt::Key_Space, Qt::ControlModifier);

    // "mydb.us" is one prefix, so the qualifier is part of the match and the
    // only candidate is the qualified path.
    QCOMPARE(labels(editor), QStringList({QStringLiteral("mydb.users")}));
    QVERIFY(popupOf(editor)->isVisible());
    QCOMPARE(popupOf(editor)->currentIndex().row(), 0);

    sendKey(editor, Qt::Key_Return);

    // The qualifier is replaced rather than kept, which is what stops the
    // insert from reading "mydb.mydb.users".
    QCOMPARE(editor.toPlainText(), QStringLiteral("SELECT * FROM mydb.users"));
    QVERIFY(!popupOf(editor)->isVisible());
}

void TestSqlEditor::acceptingWithTabKeepsTheAliasAndTheRestOfTheLine()
{
    SqlEditor editor;
    editor.setCompletionSchema(testSchema());
    const QString sql = QStringLiteral("SELECT * FROM mydb.users u WHERE u.na = 1");
    editor.setPlainText(sql);
    placeCursor(editor, int(sql.indexOf(QStringLiteral(" = 1"))));

    sendKey(editor, Qt::Key_Space, Qt::ControlModifier);
    QVERIFY(popupOf(editor)->isVisible());

    sendKey(editor, Qt::Key_Tab);

    // Only the prefix left of the cursor is replaced, and the alias the user
    // typed rides along in the insert text.
    QCOMPARE(editor.toPlainText(), QStringLiteral("SELECT * FROM mydb.users u WHERE u.name = 1"));
    QVERIFY(!popupOf(editor)->isVisible());
}

void TestSqlEditor::escapeClosesThePopupAndTypesNothing()
{
    SqlEditor editor;
    editor.setCompletionSchema(testSchema());
    editor.setPlainText(QStringLiteral("SELECT * FROM mydb.us"));
    placeCursor(editor, 21);

    sendKey(editor, Qt::Key_Space, Qt::ControlModifier);
    QVERIFY(popupOf(editor)->isVisible());

    sendKey(editor, Qt::Key_Escape);
    QVERIFY(!popupOf(editor)->isVisible());
    QCOMPARE(editor.toPlainText(), QStringLiteral("SELECT * FROM mydb.us"));
}

void TestSqlEditor::theArrowKeysGoToThePopupNotTheCaret()
{
    SqlEditor editor;
    editor.setCompletionSchema(testSchema());
    editor.setPlainText(QStringLiteral("SELECT * FROM mydb.us\nSELECT 2"));
    placeCursor(editor, 21);

    // Control: with no popup open, Down is the caret's own key.
    sendKey(editor, Qt::Key_Down);
    QCOMPARE(editor.textCursor().blockNumber(), 1);

    placeCursor(editor, 21);
    sendKey(editor, Qt::Key_Space, Qt::ControlModifier);
    QVERIFY(popupOf(editor)->isVisible());

    // Now the same key belongs to the list, so the caret must not move with it.
    sendKey(editor, Qt::Key_Down);
    QCOMPARE(editor.textCursor().position(), 21);
    QVERIFY(popupOf(editor)->isVisible());
}

void TestSqlEditor::ctrlEnterRunsEvenWithThePopupOpen()
{
    SqlEditor editor;
    editor.setCompletionSchema(testSchema());
    editor.setPlainText(QStringLiteral("SELECT * FROM mydb.us"));
    placeCursor(editor, 21);
    sendKey(editor, Qt::Key_Space, Qt::ControlModifier);
    QVERIFY(popupOf(editor)->isVisible());

    QSignalSpy run(&editor, &SqlEditor::runRequested);
    sendKey(editor, Qt::Key_Return, Qt::ControlModifier);

    // Ctrl skips the popup's keys entirely, so Run wins over accepting a
    // candidate: the text is untouched.
    QCOMPARE(run.size(), 1);
    QCOMPARE(editor.toPlainText(), QStringLiteral("SELECT * FROM mydb.us"));
}

void TestSqlEditor::statementToRunFollowsTheCursor()
{
    SqlEditor editor;
    QCOMPARE(editor.statementToRun(), QString());

    editor.setPlainText(QStringLiteral("SELECT 1;\nSELECT 2;"));
    placeCursor(editor, 0);
    QCOMPARE(editor.statementToRun(), QStringLiteral("SELECT 1"));

    placeCursor(editor, 12);
    QCOMPARE(editor.statementToRun(), QStringLiteral("SELECT 2"));
}

void TestSqlEditor::statementToRunPrefersTheSelection()
{
    SqlEditor editor;
    editor.setPlainText(QStringLiteral("SELECT 1;\nSELECT 2;"));

    // A selection runs as written, delimiters and all: it is not cut back to
    // the statement under the cursor.
    selectRange(editor, 0, 19);
    const QString whole = editor.statementToRun();
    QCOMPARE(whole, QStringLiteral("SELECT 1;\nSELECT 2;"));
    QVERIFY2(
        !whole.contains(QChar(0x2029)),
        "selectedText hands back paragraph separators, which the server would never see"
    );

    // Blanks on either side of the selection are shaved.
    selectRange(editor, 9, 19);
    QCOMPARE(editor.statementToRun(), QStringLiteral("SELECT 2;"));
}

void TestSqlEditor::theRunAndFormatKeysEmitAndTypeNothing()
{
    SqlEditor editor;
    QSignalSpy run(&editor, &SqlEditor::runRequested);
    QSignalSpy script(&editor, &SqlEditor::runScriptRequested);
    QSignalSpy format(&editor, &SqlEditor::formatRequested);

    sendKey(editor, Qt::Key_Return, Qt::ControlModifier);
    QCOMPARE(run.size(), 1);
    QCOMPARE(script.size(), 0);

    // The keypad's Enter is the same key to muscle memory.
    sendKey(editor, Qt::Key_Enter, Qt::ControlModifier);
    QCOMPARE(run.size(), 2);

    sendKey(editor, Qt::Key_Return, Qt::ControlModifier | Qt::ShiftModifier);
    QCOMPARE(script.size(), 1);
    QCOMPARE(run.size(), 2);

    sendKey(editor, Qt::Key_F, Qt::ControlModifier | Qt::ShiftModifier);
    QCOMPARE(format.size(), 1);

    QVERIFY2(editor.toPlainText().isEmpty(), "a key the editor answers must not also be typed");
}

void TestSqlEditor::bracketsAndQuotesCloseThemselves_data()
{
    QTest::addColumn<QString>("text");
    QTest::addColumn<int>("at");
    QTest::addColumn<QString>("typed");
    QTest::addColumn<QString>("expected");
    QTest::addColumn<int>("expectedAt");

    QTest::newRow("paren on an empty line")
        << QString() << 0 << QStringLiteral("(") << QStringLiteral("()") << 1;
    QTest::newRow("bracket") << QString() << 0 << QStringLiteral("[") << QStringLiteral("[]") << 1;
    QTest::newRow("quote") << QString() << 0 << QStringLiteral("'") << QStringLiteral("''") << 1;
    QTest::newRow("double quote") << QString() << 0 << QStringLiteral("\"")
                                  << QStringLiteral("\"\"") << 1;
    QTest::newRow("backquote") << QString() << 0 << QStringLiteral("`") << QStringLiteral("``")
                               << 1;

    // Closing in the middle of a word is never wanted.
    QTest::newRow("before a word")
        << QStringLiteral("abc") << 0 << QStringLiteral("(") << QStringLiteral("(abc") << 1;
    QTest::newRow("before a number")
        << QStringLiteral("1") << 0 << QStringLiteral("(") << QStringLiteral("(1") << 1;
    QTest::newRow("before a blank")
        << QStringLiteral("a ") << 1 << QStringLiteral("(") << QStringLiteral("a() ") << 2;
    QTest::newRow("before a closer")
        << QStringLiteral("()") << 1 << QStringLiteral("(") << QStringLiteral("(())") << 2;

    // A quote right after a word is an apostrophe or a closing quote; pairing
    // it would double it.
    QTest::newRow("quote after a word")
        << QStringLiteral("don") << 3 << QStringLiteral("'") << QStringLiteral("don'") << 4;
    QTest::newRow("quote after an underscore")
        << QStringLiteral("a_") << 2 << QStringLiteral("'") << QStringLiteral("a_'") << 3;

    // Typing the closer we inserted steps past it instead of adding a second:
    // without this, typing COUNT(*) leaves COUNT(*)).
    QTest::newRow("type-over") << QStringLiteral("()") << 1 << QStringLiteral(")")
                               << QStringLiteral("()") << 2;
    QTest::newRow("closer with nothing ahead")
        << QString() << 0 << QStringLiteral(")") << QStringLiteral(")") << 1;
}

void TestSqlEditor::bracketsAndQuotesCloseThemselves()
{
    QFETCH(QString, text);
    QFETCH(int, at);
    QFETCH(QString, typed);
    QFETCH(QString, expected);
    QFETCH(int, expectedAt);

    SqlEditor editor;
    editor.setPlainText(text);
    placeCursor(editor, at);

    typeText(editor, typed);

    QCOMPARE(editor.toPlainText(), expected);
    QCOMPARE(editor.textCursor().position(), expectedAt);
}

void TestSqlEditor::aSelectionIsReplacedRatherThanWrapped()
{
    SqlEditor editor;
    editor.setPlainText(QStringLiteral("abc"));
    selectRange(editor, 0, 3);

    typeText(editor, QStringLiteral("("));

    // Auto-closing only runs on an empty cursor, so a selection takes the plain
    // editing path.
    QCOMPARE(editor.toPlainText(), QStringLiteral("("));
    QCOMPARE(editor.textCursor().position(), 1);
}

void TestSqlEditor::insertSnippetLandsAtTheCursor()
{
    SqlEditor editor;
    editor.setPlainText(QStringLiteral("SELECT  FROM t"));
    placeCursor(editor, 7);

    editor.insertSnippet(QStringLiteral("1"));
    QCOMPARE(editor.toPlainText(), QStringLiteral("SELECT 1 FROM t"));
    QCOMPARE(editor.textCursor().position(), 8);

    // A snippet over a selection replaces it, the way typing would.
    selectRange(editor, 0, 15);
    editor.insertSnippet(QStringLiteral("SHOW TABLES"));
    QCOMPARE(editor.toPlainText(), QStringLiteral("SHOW TABLES"));
}

void TestSqlEditor::diagnosticsSquiggleInTheirOwnTone()
{
    SqlEditor editor;
    editor.setPlainText(QStringLiteral("SELECT ?;"));

    EditorDiagnostic warning;
    warning.from = 7;
    warning.to = 8;
    warning.message = QStringLiteral("a '?' placeholder");
    warning.warning = true;
    editor.setDiagnostics({warning});

    QList<QTextEdit::ExtraSelection> sels = editor.extraSelections();
    QCOMPARE(sels.size(), 1);
    QCOMPARE(sels.at(0).cursor.selectionStart(), 7);
    QCOMPARE(sels.at(0).cursor.selectionEnd(), 8);
    QCOMPARE(sels.at(0).format.underlineStyle(), QTextCharFormat::WaveUnderline);
    QCOMPARE(sels.at(0).format.underlineColor(), theme::current().warning);

    EditorDiagnostic error;
    error.from = 0;
    error.to = 6;
    error.message = QStringLiteral("You have an error in your SQL syntax");
    editor.setDiagnostics({error});

    sels = editor.extraSelections();
    QCOMPARE(sels.size(), 1);
    QCOMPARE(sels.at(0).format.underlineColor(), theme::current().destructive);
}

void TestSqlEditor::diagnosticsClampToTheDocumentAndClear()
{
    SqlEditor editor;
    editor.setPlainText(QStringLiteral("SELECT 1"));

    // The lint runs against text the editor may already have moved past, so a
    // range off the end has to land inside the document rather than nowhere.
    EditorDiagnostic stale;
    stale.from = -5;
    stale.to = 200;
    stale.message = QStringLiteral("from an older revision");
    editor.setDiagnostics({stale});

    // The last position a cursor can take: one short of the character count,
    // which counts the block separator the document always ends on.
    const int docEnd = int(editor.document()->characterCount()) - 1;
    const QList<QTextEdit::ExtraSelection> sels = editor.extraSelections();
    QCOMPARE(sels.size(), 1);
    QCOMPARE(sels.at(0).cursor.selectionStart(), 0);
    QCOMPARE(sels.at(0).cursor.selectionEnd(), docEnd);

    editor.setDiagnostics({});
    QVERIFY(editor.extraSelections().isEmpty());
}

void TestSqlEditor::diagnosticsFollowLaterEdits()
{
    SqlEditor editor;
    editor.setPlainText(QStringLiteral("SELECT ?;"));

    EditorDiagnostic warning;
    warning.from = 7;
    warning.to = 8;
    warning.message = QStringLiteral("a '?' placeholder");
    warning.warning = true;
    editor.setDiagnostics({warning});

    QTextCursor edit = editor.textCursor();
    edit.setPosition(0);
    edit.insertText(QStringLiteral("-- x\n"));

    // The squiggle rides on cursors, so it stays on the '?' until the next lint
    // pass lands instead of marking whatever moved under it.
    const QList<QTextEdit::ExtraSelection> sels = editor.extraSelections();
    QCOMPARE(sels.size(), 1);
    QCOMPARE(sels.at(0).cursor.selectionStart(), 12);
    QCOMPARE(sels.at(0).cursor.selectionEnd(), 13);
    QCOMPARE(editor.toPlainText().mid(12, 1), QStringLiteral("?"));
}

void TestSqlEditor::aTooltipShowsOnlyOverADiagnostic()
{
    SqlEditor editor;
    editor.setPlainText(QStringLiteral("?"));

    // The editor answers every tooltip itself, so the default one never shows.
    QVERIFY(sendToolTip(editor));
    QVERIFY2(QToolTip::text().isEmpty(), "nothing is squiggled yet, so there is nothing to say");

    EditorDiagnostic warning;
    warning.from = 0;
    warning.to = 1;
    warning.message = QStringLiteral("a '?' placeholder");
    warning.warning = true;
    editor.setDiagnostics({warning});

    QVERIFY(sendToolTip(editor));
    QCOMPARE(QToolTip::text(), warning.message);
    QToolTip::hideText();
}

void TestSqlEditor::theDocumentSettlesOnceAfterABurstOfEdits()
{
    SqlEditor editor;
    QSignalSpy settled(&editor, &SqlEditor::documentSettled);

    editor.setPlainText(QStringLiteral("SELECT 1"));
    editor.setPlainText(QStringLiteral("SELECT 2"));
    QCOMPARE(settled.size(), 0);

    // Workspace persistence rides on this, so a burst of typing has to come out
    // as one save of the final text rather than one per keystroke.
    QVERIFY(settled.wait());
    QCOMPARE(settled.size(), 1);
    QCOMPARE(settled.at(0).at(0).toString(), QStringLiteral("SELECT 2"));
}

void TestSqlEditor::theFontSizeRidesInTheSheetAsWellAsTheWidget()
{
    SqlEditor editor;
    editor.setEditorFontSize(20);

    // The slider value goes through the same logical-DPI correction the UI font
    // gets, so the two sliders mean the same physical size.
    const int px = theme::dpiPx(20);
    QCOMPARE(editor.font().pixelSize(), px);
    QCOMPARE(editor.font().family(), theme::monoFamily());

    // The app stylesheet's global QWidget font rule would override setFont() on
    // its own, so the size has to ride in the widget's own sheet as well: the
    // gutter and the tab stops read the widget font, the text reads the sheet.
    QVERIFY(editor.styleSheet().contains(QStringLiteral("font-size: %1px").arg(px)));
    QVERIFY(editor.styleSheet().contains(theme::monoFamily()));
}

void TestSqlEditor::theTabStopFollowsTheFontAndTheCharCount()
{
    SqlEditor editor;
    editor.setEditorFontSize(BaseFontPx);
    editor.setTabWidthChars(4);

    const qreal space = QFontMetricsF(editor.font()).horizontalAdvance(QLatin1Char(' '));
    QCOMPARE(editor.tabStopDistance(), space * 4);

    // A tab narrower than one character is not a tab.
    editor.setTabWidthChars(0);
    QCOMPARE(editor.tabStopDistance(), space);

    editor.setTabWidthChars(4);
    const qreal narrow = editor.tabStopDistance();
    editor.setEditorFontSize(2 * BaseFontPx);
    QVERIFY2(
        editor.tabStopDistance() > narrow, "the stop is a width in pixels, so it follows the font"
    );
}

void TestSqlEditor::thePaletteReachesTheWidgetAndTheSheet()
{
    SqlEditor editor;
    const EditorPalette &other = theme::editor(OtherEditorTheme);
    QVERIFY(other.bg != theme::currentEditor().bg);

    editor.setEditorPalette(other);

    QCOMPARE(editor.palette().color(QPalette::Base), other.bg);

    // Only Base is read back. setEditorPalette does set Text, Highlight,
    // HighlightedText and PlaceholderText, but the application stylesheet
    // shadows them for the life of the widget, shown or not, so reading them
    // back measures Qt's stylesheet resolution rather than this class. What
    // reaches the screen is the sheet below.

    // The sheet carries the same colours, because the app stylesheet would
    // otherwise paint this editor in the application's background.
    QVERIFY(editor.styleSheet().contains(other.bg.name()));
    QVERIFY(editor.styleSheet().contains(other.fg.name()));
    QVERIFY(editor.styleSheet().contains(other.sel.name()));
}

void TestSqlEditor::theGutterKeepsRoomForTwoDigitsAndGrows()
{
    SqlEditor editor;
    const int digit = editor.fontMetrics().horizontalAdvance(QLatin1Char('9'));

    // Two digits minimum, so a short buffer does not shift as it grows past 9.
    QCOMPARE(editor.lineNumberAreaWidth(), GutterPadding + digit * MinimumDigits);

    editor.setPlainText(numberedLines(1000));
    QCOMPARE(editor.lineNumberAreaWidth(), GutterPadding + digit * 4);

    // The numbers are painted in the editor's own font, so the room they need
    // follows the editor's font size rather than the UI one.
    const int narrow = editor.lineNumberAreaWidth();
    editor.setEditorFontSize(2 * BaseFontPx);
    const int wide = editor.fontMetrics().horizontalAdvance(QLatin1Char('9'));
    QCOMPARE(editor.lineNumberAreaWidth(), GutterPadding + wide * 4);
    QVERIFY(editor.lineNumberAreaWidth() > narrow);
}

QTEST_MAIN(TestSqlEditor)

#include "tst_sqleditor.moc"
