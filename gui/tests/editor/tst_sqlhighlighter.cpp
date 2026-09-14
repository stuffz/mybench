#include "editor/sqlhighlighter.h"

#include "app/theme.h"

#include <QColor>
#include <QList>
#include <QObject>
#include <QSet>
#include <QStandardPaths>
#include <QString>
#include <QStringList>
#include <QTest>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextLayout>

// highlightBlock is protected and returns nothing, so the only way in from
// outside is the document it writes to: the formats land on each block's
// layout, and the colour a character ended up with is what every assertion
// below reads back.
namespace
{

// apply() wants a slider value; nothing here depends on the size.
constexpr int BaseFontPx = 13;

// SqlHighlighter::State is private, so the two values it leaves in the block
// user state are spelled out again here.
constexpr int NormalState = 0;
constexpr int InBlockCommentState = 1;

// A second editor theme, only to have colours that differ from the applied
// one; any other theme would do.
constexpr auto OtherEditorTheme = "onedark";

// One colour per character, invalid where the highlighter left the text alone.
// Neighbouring characters that share a format are merged into one range and
// split again when they stop sharing it, so reading per character keeps the
// expectations about the text rather than about the run layout.
//
// The formats only reach the block layout once rehighlight() has run: with no
// view laying the document out, the contentsChange path QSyntaxHighlighter
// normally rides on leaves layout()->formats() empty.
QList<QColor> foregrounds(const QTextDocument &doc, int blockNumber)
{
    const QTextBlock block = doc.findBlockByNumber(blockNumber);
    QList<QColor> colours(block.text().size(), QColor());
    for (const QTextLayout::FormatRange &range : block.layout()->formats())
    {
        for (int i = range.start; i < range.start + range.length; ++i)
        {
            colours[i] = range.format.foreground().color();
        }
    }
    return colours;
}

// Three blocks: the comment opens mid-line on the first, owns the second
// whole, and closes at the head of the third.
QString spanningComment()
{
    return QStringLiteral("SELECT 1 /* open\nstill inside\n*/ SELECT 2");
}

} // namespace

class TestSqlHighlighter : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();

    void theWordListsAreStaticAndFreeOfDuplicates();
    void theWordListsHoldWhatTheyClaim_data();
    void theWordListsHoldWhatTheyClaim();

    void eachTokenKindTakesItsColour_data();
    void eachTokenKindTakesItsColour();

    void anOpenBlockCommentCarriesToTheNextLine();
    void theCloseReturnsTheRestOfTheLineToCode();
    void closingTheCommentRepaintsTheLinesBelow();

    void aNewPaletteRecoloursTheDocument();
};

void TestSqlHighlighter::initTestCase()
{
    // apply() renders the dropdown chevron into the cache dir; test mode keeps
    // this run's copies out of the real one.
    QStandardPaths::setTestModeEnabled(true);

    // The app themes before it opens an editor, and the highlighter reads
    // theme::currentEditor() in its constructor.
    theme::apply(theme::defaultApp, BaseFontPx);
}

void TestSqlHighlighter::theWordListsAreStaticAndFreeOfDuplicates()
{
    const QStringList &keywords = SqlHighlighter::keywords();
    const QStringList &types = SqlHighlighter::types();

    QVERIFY(!keywords.isEmpty());
    QVERIFY(!types.isEmpty());

    // Both are handed out by reference and the completion pool holds on to
    // one, so a second call has to be the same list rather than a fresh copy.
    QCOMPARE(&SqlHighlighter::keywords(), &keywords);
    QCOMPARE(&SqlHighlighter::types(), &types);

    // A repeated entry would offer the same completion twice.
    QCOMPARE(QSet<QString>(keywords.begin(), keywords.end()).size(), keywords.size());
    QCOMPARE(QSet<QString>(types.begin(), types.end()).size(), types.size());

    // highlightBlock looks words up upper-cased, so a lower-case entry here
    // would never colour anything.
    for (const QString &word : keywords + types)
    {
        QCOMPARE(word, word.toUpper());
    }
}

void TestSqlHighlighter::theWordListsHoldWhatTheyClaim_data()
{
    QTest::addColumn<QString>("word");
    QTest::addColumn<bool>("keyword");
    QTest::addColumn<bool>("type");

    QTest::newRow("select") << QStringLiteral("SELECT") << true << false;
    QTest::newRow("join") << QStringLiteral("JOIN") << true << false;
    QTest::newRow("varchar") << QStringLiteral("VARCHAR") << false << true;
    QTest::newRow("timestamp") << QStringLiteral("TIMESTAMP") << false << true;

    // SET is a statement keyword and a column type both.
    QTest::newRow("in both lists") << QStringLiteral("SET") << true << true;

    QTest::newRow("not sql at all") << QStringLiteral("BANANA") << false << false;
}

void TestSqlHighlighter::theWordListsHoldWhatTheyClaim()
{
    QFETCH(QString, word);
    QFETCH(bool, keyword);
    QFETCH(bool, type);

    QCOMPARE(SqlHighlighter::keywords().contains(word), keyword);
    QCOMPARE(SqlHighlighter::types().contains(word), type);
}

void TestSqlHighlighter::eachTokenKindTakesItsColour_data()
{
    const EditorPalette &p = theme::currentEditor();

    QTest::addColumn<QString>("sql");
    QTest::addColumn<int>("start");
    QTest::addColumn<int>("length");
    QTest::addColumn<QColor>("colour");

    QTest::newRow("keyword") << QStringLiteral("SELECT 1") << 0 << 6 << p.keyword;
    QTest::newRow("keyword in lower case") << QStringLiteral("select 1") << 0 << 6 << p.keyword;
    QTest::newRow("type") << QStringLiteral("id INT") << 3 << 3 << p.type;

    // The keyword lookup runs first, so the word both lists hold is a keyword.
    QTest::newRow("in both lists") << QStringLiteral("SET x = 1") << 0 << 3 << p.keyword;

    QTest::newRow("number") << QStringLiteral("SELECT 42") << 7 << 2 << p.number;
    QTest::newRow("decimal number") << QStringLiteral("SELECT 3.14") << 7 << 4 << p.number;

    QTest::newRow("single quoted string") << QStringLiteral("SELECT 'x'") << 7 << 3 << p.string;
    QTest::newRow("double quoted string") << QStringLiteral("SELECT \"x\"") << 7 << 3 << p.string;

    // The backslash keeps the string open past the quote it escapes.
    QTest::newRow("escaped quote") << QStringLiteral("SELECT 'a\\'b'") << 7 << 6 << p.string;

    // A quote left open takes the rest of the line rather than nothing.
    QTest::newRow("unterminated string") << QStringLiteral("SELECT 'x") << 7 << 2 << p.string;

    QTest::newRow("function call") << QStringLiteral("SELECT count(*)") << 7 << 5 << p.func;
    QTest::newRow("call with a space before the paren")
        << QStringLiteral("SELECT count (*)") << 7 << 5 << p.func;

    // A backquoted identifier deliberately shares the type colour.
    QTest::newRow("backquoted identifier") << QStringLiteral("SELECT `a b`") << 7 << 5 << p.type;

    QTest::newRow("dash comment") << QStringLiteral("SELECT 1 -- note") << 9 << 7 << p.comment;
    QTest::newRow("hash comment") << QStringLiteral("SELECT 1 # note") << 9 << 6 << p.comment;
    QTest::newRow("block comment") << QStringLiteral("SELECT /* c */ 1") << 7 << 7 << p.comment;
    QTest::newRow("code after a closed block comment")
        << QStringLiteral("SELECT /* c */ 1") << 15 << 1 << p.number;

    // A word that is none of the three keeps the editor's own foreground,
    // which the highlighter leaves alone.
    QTest::newRow("bare identifier") << QStringLiteral("SELECT x") << 7 << 1 << QColor();
}

void TestSqlHighlighter::eachTokenKindTakesItsColour()
{
    QFETCH(QString, sql);
    QFETCH(int, start);
    QFETCH(int, length);
    QFETCH(QColor, colour);

    QTextDocument doc;
    SqlHighlighter highlighter(&doc);
    doc.setPlainText(sql);
    highlighter.rehighlight();

    QCOMPARE(foregrounds(doc, 0).mid(start, length), QList<QColor>(length, colour));
}

void TestSqlHighlighter::anOpenBlockCommentCarriesToTheNextLine()
{
    QTextDocument doc;
    SqlHighlighter highlighter(&doc);
    doc.setPlainText(spanningComment());
    highlighter.rehighlight();

    const EditorPalette &p = theme::currentEditor();

    // The code before the "/*" keeps its colours; the comment owns the rest.
    QCOMPARE(foregrounds(doc, 0).mid(0, 6), QList<QColor>(6, p.keyword));
    QCOMPARE(foregrounds(doc, 0).mid(9), QList<QColor>(7, p.comment));
    QCOMPARE(doc.findBlockByNumber(0).userState(), InBlockCommentState);

    // Nothing on the second line opens a comment: it is coloured as one only
    // because the first line left the state open.
    const QTextBlock second = doc.findBlockByNumber(1);
    QCOMPARE(foregrounds(doc, 1), QList<QColor>(second.text().size(), p.comment));
    QCOMPARE(second.userState(), InBlockCommentState);
}

void TestSqlHighlighter::theCloseReturnsTheRestOfTheLineToCode()
{
    QTextDocument doc;
    SqlHighlighter highlighter(&doc);
    doc.setPlainText(spanningComment());
    highlighter.rehighlight();

    const EditorPalette &p = theme::currentEditor();
    const QList<QColor> third = foregrounds(doc, 2);

    QCOMPARE(third.mid(0, 2), QList<QColor>(2, p.comment));
    QCOMPARE(third.mid(3, 6), QList<QColor>(6, p.keyword));
    QCOMPARE(third.at(10), p.number);
    QCOMPARE(doc.findBlockByNumber(2).userState(), NormalState);
}

void TestSqlHighlighter::closingTheCommentRepaintsTheLinesBelow()
{
    QTextDocument doc;
    SqlHighlighter highlighter(&doc);
    doc.setPlainText(QStringLiteral("/* open\nSELECT 1"));
    highlighter.rehighlight();

    const EditorPalette &p = theme::currentEditor();
    const QTextBlock second = doc.findBlockByNumber(1);
    QCOMPARE(foregrounds(doc, 1), QList<QColor>(second.text().size(), p.comment));

    QTextCursor cursor(doc.findBlockByNumber(0));
    cursor.movePosition(QTextCursor::EndOfBlock);
    cursor.insertText(QStringLiteral(" */"));
    highlighter.rehighlight();

    // The edit touches the first line only: the second gets its colouring back
    // because the state it inherits changed, not because it was itself edited.
    QCOMPARE(foregrounds(doc, 1).mid(0, 6), QList<QColor>(6, p.keyword));
    QCOMPARE(doc.findBlockByNumber(1).userState(), NormalState);
}

void TestSqlHighlighter::aNewPaletteRecoloursTheDocument()
{
    QTextDocument doc;
    SqlHighlighter highlighter(&doc);
    doc.setPlainText(QStringLiteral("SELECT 1"));
    highlighter.rehighlight();

    const EditorPalette &applied = theme::currentEditor();
    QCOMPARE(foregrounds(doc, 0).at(0), applied.keyword);

    const EditorPalette &other = theme::editor(OtherEditorTheme);
    QVERIFY(other.keyword != applied.keyword);
    QVERIFY(other.number != applied.number);

    // Text already in the document is what a theme switch has to reach.
    highlighter.setPalette(other);
    QCOMPARE(foregrounds(doc, 0).at(0), other.keyword);
    QCOMPARE(foregrounds(doc, 0).at(7), other.number);
}

QTEST_MAIN(TestSqlHighlighter)

#include "tst_sqlhighlighter.moc"
