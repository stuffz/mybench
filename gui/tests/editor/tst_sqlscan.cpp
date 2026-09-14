#include "editor/sqlscan.h"

#include <QHash>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTest>
#include <QVector>

namespace
{

// The completion schema the scan resolves against. Two tables in one schema is
// enough to exercise exact, bare and case-insensitive lookup.
QHash<QString, QStringList> testSchema()
{
    return {
        {QStringLiteral("mydb.users"), {QStringLiteral("id"), QStringLiteral("name")}},
        {QStringLiteral("mydb.orders"), {QStringLiteral("id"), QStringLiteral("user_id")}},
    };
}

} // namespace

class TestSqlScan : public QObject
{
    Q_OBJECT

private slots:
    void fuzzyRejectsWhatIsNotASubsequence();
    void fuzzyIgnoresCaseAndAcceptsANullScore();
    void fuzzyRanksTighterMatchesHigher();

    void statementAtFindsTheOneUnderTheCursor();
    void statementAtWalksBackOverTheDelimiter();
    void statementAtFallsForwardWhenEverythingBehindIsEmpty();
    void statementAtHandlesAnEmptyBuffer();

    void splitDropsEmptyStatements();
    void boundariesIgnoreQuotesAndComments_data();
    void boundariesIgnoreQuotesAndComments();

    void spansMapOntoBufferOffsets();
    void spansSkipRangesThatAreOnlyBlanks();
    void spansTrimBlanksOnBothSidesOfTheDelimiter();

    void placeholdersAreFoundOutsideQuotesAndComments_data();
    void placeholdersAreFoundOutsideQuotesAndComments();

    void completionStartStopsAtTheIdentifier();
    void completionStartReachesBackOverAQualifier();

    void tablesResolveQualifiedBareAndAliased();
    void tablesRefuseAClauseKeywordAsAnAlias();
    void tablesCollectJoinsInOrderAndDedupe();
    void tablesReadCommaListsOnlyWhereTheyAreLegal();
    void tablesReadUpdateAndInsertTargets();
    void tablesSkipTheDerivedTableButNotItsBody();
    void tablesIgnoreUnknownNamesAndStringContents();
    void tablesReadAnEscapedBacktickInAName();
    void tablesStepOverABlockComment();
};

void TestSqlScan::fuzzyRejectsWhatIsNotASubsequence()
{
    int score = 0;
    QVERIFY(!fuzzyScore(QStringLiteral("xyz"), QStringLiteral("users"), &score));

    // In order, not merely present: "sru" has the same letters as "usr".
    QVERIFY(!fuzzyScore(QStringLiteral("sru"), QStringLiteral("users"), &score));
    QVERIFY(fuzzyScore(QStringLiteral("usr"), QStringLiteral("users"), &score));
}

void TestSqlScan::fuzzyIgnoresCaseAndAcceptsANullScore()
{
    int score = 0;
    QVERIFY(fuzzyScore(QStringLiteral("US"), QStringLiteral("users"), &score));
    QVERIFY(fuzzyScore(QStringLiteral("us"), QStringLiteral("USERS"), &score));

    // The finder calls this just to filter, with nowhere to put a score.
    QVERIFY(fuzzyScore(QStringLiteral("u"), QStringLiteral("users"), nullptr));

    // An empty query matches anything, and scores nothing.
    QVERIFY(fuzzyScore(QString(), QStringLiteral("users"), &score));
    QCOMPARE(score, 0);
}

void TestSqlScan::fuzzyRanksTighterMatchesHigher()
{
    int atBoundary = 0;
    int midWord = 0;
    QVERIFY(fuzzyScore(QStringLiteral("users"), QStringLiteral("db.users"), &atBoundary));
    QVERIFY(fuzzyScore(QStringLiteral("users"), QStringLiteral("xusers"), &midWord));
    QVERIFY2(atBoundary > midWord, "a match after '.' should outrank one mid-word");

    int consecutive = 0;
    int gapped = 0;
    QVERIFY(fuzzyScore(QStringLiteral("us"), QStringLiteral("users"), &consecutive));
    QVERIFY(fuzzyScore(QStringLiteral("us"), QStringLiteral("uxs"), &gapped));
    QVERIFY2(consecutive > gapped, "adjacent characters should outrank a gapped match");
}

void TestSqlScan::statementAtFindsTheOneUnderTheCursor()
{
    const QString text = QStringLiteral("SELECT 1; SELECT 2;");
    QCOMPARE(statementAt(text, 0), QStringLiteral("SELECT 1"));
    QCOMPARE(statementAt(text, 8), QStringLiteral("SELECT 1"));
    QCOMPARE(statementAt(text, 12), QStringLiteral("SELECT 2"));
}

void TestSqlScan::statementAtWalksBackOverTheDelimiter()
{
    // Right after typing the ';' the cursor sits in an empty range; the
    // statement meant is the one before it, not nothing.
    QCOMPARE(statementAt(QStringLiteral("SELECT 1;"), 9), QStringLiteral("SELECT 1"));
    QCOMPARE(statementAt(QStringLiteral("SELECT 1;\n\n"), 11), QStringLiteral("SELECT 1"));
}

void TestSqlScan::statementAtFallsForwardWhenEverythingBehindIsEmpty()
{
    // Cursor inside a run of stray delimiters: there is nothing to run behind
    // it, so the first real statement ahead is the answer.
    QCOMPARE(statementAt(QStringLiteral("   ;  SELECT 1"), 2), QStringLiteral("SELECT 1"));
}

void TestSqlScan::statementAtHandlesAnEmptyBuffer()
{
    QCOMPARE(statementAt(QString(), 0), QString());
    QCOMPARE(statementAt(QStringLiteral("   ;;;   "), 4), QString());
}

void TestSqlScan::splitDropsEmptyStatements()
{
    QCOMPARE(
        splitStatements(QStringLiteral("SELECT 1;\n\nSELECT 2;\n")),
        QStringList({QStringLiteral("SELECT 1"), QStringLiteral("SELECT 2")})
    );

    // A missing final delimiter still closes the last statement.
    QCOMPARE(
        splitStatements(QStringLiteral("SELECT 1; SELECT 2")),
        QStringList({QStringLiteral("SELECT 1"), QStringLiteral("SELECT 2")})
    );

    QCOMPARE(splitStatements(QStringLiteral("  ;;;  ")), QStringList());
    QCOMPARE(splitStatements(QString()), QStringList());
}

void TestSqlScan::boundariesIgnoreQuotesAndComments_data()
{
    QTest::addColumn<QString>("sql");
    QTest::addColumn<int>("statements");

    // A ';' inside any of these is content, not a statement boundary.
    QTest::newRow("single quotes") << QStringLiteral("SELECT ';' AS x") << 1;
    QTest::newRow("double quotes") << QStringLiteral("SELECT \";\" AS x") << 1;
    QTest::newRow("backquoted identifier") << QStringLiteral("SELECT `a;b` FROM t") << 1;
    QTest::newRow("block comment") << QStringLiteral("SELECT /* ; */ 1") << 1;
    QTest::newRow("dash comment") << QStringLiteral("SELECT 1 -- ; still a comment\n") << 1;
    QTest::newRow("hash comment") << QStringLiteral("SELECT 1 # ; still a comment\n") << 1;

    // An escaped quote does not end the string, so the ';' after it is still
    // inside one.
    QTest::newRow("escaped quote") << QStringLiteral("SELECT 'it\\'s ; fine' AS x") << 1;

    QTest::newRow("a real boundary still splits") << QStringLiteral("SELECT 1; SELECT 2") << 2;
}

void TestSqlScan::boundariesIgnoreQuotesAndComments()
{
    QFETCH(QString, sql);
    QFETCH(int, statements);
    QCOMPARE(splitStatements(sql).size(), statements);
}

void TestSqlScan::spansMapOntoBufferOffsets()
{
    const QString text = QStringLiteral("SELECT 1;\nSELECT 2;");
    const QVector<StatementSpan> spans = statementSpans(text);
    QCOMPARE(spans.size(), 2);

    // The contract the live syntax check depends on: a span's text is exactly
    // what sits at its offsets in the buffer.
    for (const StatementSpan &span : spans)
    {
        QCOMPARE(text.mid(span.start, span.end - span.start), span.text);
    }

    QCOMPARE(spans.at(0).start, 0);
    QCOMPARE(spans.at(0).end, 8);
    QCOMPARE(spans.at(0).text, QStringLiteral("SELECT 1"));
    QCOMPARE(spans.at(1).start, 10);
    QCOMPARE(spans.at(1).end, 18);
    QCOMPARE(spans.at(1).text, QStringLiteral("SELECT 2"));
}

void TestSqlScan::spansSkipRangesThatAreOnlyBlanks()
{
    const QString text = QStringLiteral("\n\n  SELECT 1;\n\n\n");
    const QVector<StatementSpan> spans = statementSpans(text);
    QCOMPARE(spans.size(), 1);
    QCOMPARE(spans.at(0).text, QStringLiteral("SELECT 1"));
    QCOMPARE(text.mid(spans.at(0).start, spans.at(0).end - spans.at(0).start), spans.at(0).text);

    QCOMPARE(statementSpans(QStringLiteral("   ;;;   ")).size(), 0);
}

void TestSqlScan::placeholdersAreFoundOutsideQuotesAndComments_data()
{
    QTest::addColumn<QString>("sql");
    QTest::addColumn<QVector<int>>("offsets");

    QTest::newRow("two placeholders")
        << QStringLiteral("SELECT * FROM t WHERE a=? AND b=?") << QVector<int>({24, 32});

    // A '?' that is content must not be reported: the editor would warn about
    // a placeholder the server will never see.
    QTest::newRow("single quotes") << QStringLiteral("SELECT '?' AS q") << QVector<int>();
    QTest::newRow("double quotes") << QStringLiteral("SELECT \"?\" AS q") << QVector<int>();
    QTest::newRow("backquotes") << QStringLiteral("SELECT `?` FROM t") << QVector<int>();
    QTest::newRow("block comment") << QStringLiteral("SELECT /* ? */ 1") << QVector<int>();
    QTest::newRow("dash comment") << QStringLiteral("SELECT 1 -- ?\n") << QVector<int>();
    QTest::newRow("hash comment") << QStringLiteral("SELECT 1 # ?\n") << QVector<int>();
    QTest::newRow("empty") << QString() << QVector<int>();
}

void TestSqlScan::placeholdersAreFoundOutsideQuotesAndComments()
{
    QFETCH(QString, sql);
    QFETCH(QVector<int>, offsets);
    QCOMPARE(placeholderOffsets(sql), offsets);

    // The offsets must land on actual '?' characters in the buffer.
    for (const int at : offsets)
    {
        QCOMPARE(sql.at(at), QChar('?'));
    }
}

void TestSqlScan::completionStartStopsAtTheIdentifier()
{
    QCOMPARE(completionStart(QStringLiteral("SELECT * FROM us")), 14);
    QCOMPARE(completionStart(QStringLiteral("SELECT * FROM ")), 14);
    QCOMPARE(completionStart(QString()), 0);

    // Digits, underscores and '$' are identifier characters here.
    QCOMPARE(completionStart(QStringLiteral("a_b$c1")), 0);
}

void TestSqlScan::completionStartReachesBackOverAQualifier()
{
    // "mydb.us" is one prefix, so accepting a candidate replaces the qualifier
    // instead of duplicating it.
    QCOMPARE(completionStart(QStringLiteral("SELECT * FROM mydb.us")), 14);
    QCOMPARE(completionStart(QStringLiteral("SELECT * FROM mydb.")), 14);
    QCOMPARE(completionStart(QStringLiteral("t.")), 0);

    // Only one qualifier deep: the scan stops at the second dot.
    QCOMPARE(completionStart(QStringLiteral("a.b.c")), 2);
}

void TestSqlScan::tablesResolveQualifiedBareAndAliased()
{
    const QHash<QString, QStringList> schema = testSchema();

    const StatementTables qualified =
        scanStatementTables(QStringLiteral("SELECT * FROM mydb.users u"), schema);
    QCOMPARE(qualified.keys, QStringList({QStringLiteral("mydb.users")}));
    QCOMPARE(qualified.byName.value(QStringLiteral("mydb.users")), QStringLiteral("mydb.users"));
    QCOMPARE(qualified.byName.value(QStringLiteral("users")), QStringLiteral("mydb.users"));
    QCOMPARE(qualified.byName.value(QStringLiteral("u")), QStringLiteral("mydb.users"));

    // A bare name takes any schema's table of that name.
    const StatementTables bare = scanStatementTables(QStringLiteral("SELECT * FROM users"), schema);
    QCOMPARE(bare.keys, QStringList({QStringLiteral("mydb.users")}));

    const StatementTables mixedCase =
        scanStatementTables(QStringLiteral("SELECT * FROM MyDB.Users AS U"), schema);
    QCOMPARE(mixedCase.keys, QStringList({QStringLiteral("mydb.users")}));
    QCOMPARE(mixedCase.byName.value(QStringLiteral("u")), QStringLiteral("mydb.users"));

    // A backquoted name is always an alias, keyword or not.
    const StatementTables quoted =
        scanStatementTables(QStringLiteral("SELECT * FROM `mydb`.`users` `order`"), schema);
    QCOMPARE(quoted.keys, QStringList({QStringLiteral("mydb.users")}));
    QCOMPARE(quoted.byName.value(QStringLiteral("order")), QStringLiteral("mydb.users"));
}

void TestSqlScan::tablesRefuseAClauseKeywordAsAnAlias()
{
    const QHash<QString, QStringList> schema = testSchema();

    const StatementTables where =
        scanStatementTables(QStringLiteral("SELECT * FROM mydb.users WHERE id = 1"), schema);
    QCOMPARE(where.keys, QStringList({QStringLiteral("mydb.users")}));
    QVERIFY(!where.byName.contains(QStringLiteral("where")));

    const StatementTables ordered =
        scanStatementTables(QStringLiteral("SELECT * FROM mydb.users ORDER BY id"), schema);
    QVERIFY(!ordered.byName.contains(QStringLiteral("order")));
}

void TestSqlScan::tablesCollectJoinsInOrderAndDedupe()
{
    const QHash<QString, QStringList> schema = testSchema();

    const StatementTables joined = scanStatementTables(
        QStringLiteral("SELECT * FROM mydb.users u JOIN mydb.orders o ON o.user_id = u.id"), schema
    );
    QCOMPARE(
        joined.keys, QStringList({QStringLiteral("mydb.users"), QStringLiteral("mydb.orders")})
    );
    QCOMPARE(joined.byName.value(QStringLiteral("o")), QStringLiteral("mydb.orders"));

    // The same table twice is one key, with both aliases pointing at it.
    const StatementTables selfJoin = scanStatementTables(
        QStringLiteral("SELECT * FROM mydb.users a JOIN mydb.users b ON a.id = b.id"), schema
    );
    QCOMPARE(selfJoin.keys, QStringList({QStringLiteral("mydb.users")}));
    QCOMPARE(selfJoin.byName.value(QStringLiteral("a")), QStringLiteral("mydb.users"));
    QCOMPARE(selfJoin.byName.value(QStringLiteral("b")), QStringLiteral("mydb.users"));
}

void TestSqlScan::tablesReadCommaListsOnlyWhereTheyAreLegal()
{
    const QHash<QString, QStringList> schema = testSchema();

    const StatementTables listed =
        scanStatementTables(QStringLiteral("SELECT * FROM mydb.users u, mydb.orders o"), schema);
    QCOMPARE(
        listed.keys, QStringList({QStringLiteral("mydb.users"), QStringLiteral("mydb.orders")})
    );

    // JOIN takes one table, so the comma after it ends the list rather than
    // continuing it.
    const StatementTables afterJoin = scanStatementTables(
        QStringLiteral("SELECT * FROM mydb.users u JOIN mydb.orders o, mydb.users x"), schema
    );
    QCOMPARE(
        afterJoin.keys, QStringList({QStringLiteral("mydb.users"), QStringLiteral("mydb.orders")})
    );
    QVERIFY(!afterJoin.byName.contains(QStringLiteral("x")));
}

void TestSqlScan::tablesReadUpdateAndInsertTargets()
{
    const QHash<QString, QStringList> schema = testSchema();

    const StatementTables updated =
        scanStatementTables(QStringLiteral("UPDATE mydb.users SET name = 'x'"), schema);
    QCOMPARE(updated.keys, QStringList({QStringLiteral("mydb.users")}));
    QVERIFY(!updated.byName.contains(QStringLiteral("set")));

    const StatementTables inserted = scanStatementTables(
        QStringLiteral("INSERT INTO mydb.users (id, name) VALUES (1, 'x')"), schema
    );
    QCOMPARE(inserted.keys, QStringList({QStringLiteral("mydb.users")}));

    const StatementTables deleted =
        scanStatementTables(QStringLiteral("DELETE FROM mydb.orders WHERE id = 1"), schema);
    QCOMPARE(deleted.keys, QStringList({QStringLiteral("mydb.orders")}));
}

void TestSqlScan::tablesSkipTheDerivedTableButNotItsBody()
{
    const QHash<QString, QStringList> schema = testSchema();

    // The derived table has no schema columns, so it contributes no key — but
    // the walk still reaches the subquery's own FROM.
    const StatementTables derived =
        scanStatementTables(QStringLiteral("SELECT * FROM (SELECT id FROM mydb.users) x"), schema);
    QCOMPARE(derived.keys, QStringList({QStringLiteral("mydb.users")}));
    QVERIFY(!derived.byName.contains(QStringLiteral("x")));
}

void TestSqlScan::tablesIgnoreUnknownNamesAndStringContents()
{
    const QHash<QString, QStringList> schema = testSchema();

    const StatementTables unknown =
        scanStatementTables(QStringLiteral("SELECT * FROM nope.nope n"), schema);
    QVERIFY(unknown.keys.isEmpty());
    QVERIFY(unknown.byName.isEmpty());

    // A table name appearing inside a string literal is content, not a
    // reference.
    const StatementTables quotedContent = scanStatementTables(
        QStringLiteral("SELECT * FROM mydb.users WHERE name = 'orders'"), schema
    );
    QCOMPARE(quotedContent.keys, QStringList({QStringLiteral("mydb.users")}));

    // Same for a name that only appears in a comment.
    const StatementTables commented = scanStatementTables(
        QStringLiteral("SELECT * FROM mydb.users -- join mydb.orders\n"), schema
    );
    QCOMPARE(commented.keys, QStringList({QStringLiteral("mydb.users")}));

    QVERIFY(scanStatementTables(QString(), schema).keys.isEmpty());
}

void TestSqlScan::spansTrimBlanksOnBothSidesOfTheDelimiter()
{
    // Whitespace between the statement and its ';', which is shaved after the
    // delimiter is dropped rather than before it.
    const QString spaced = QStringLiteral("SELECT 1 ;");
    const QVector<StatementSpan> before = statementSpans(spaced);
    QCOMPARE(before.size(), 1);
    QCOMPARE(before.at(0).text, QStringLiteral("SELECT 1"));
    QCOMPARE(
        spaced.mid(before.at(0).start, before.at(0).end - before.at(0).start), before.at(0).text
    );

    // Trailing whitespace with no delimiter at all: the last range runs to the
    // end of the buffer, so the blanks are trimmed on the way in.
    const QString trailing = QStringLiteral("SELECT 1  ");
    const QVector<StatementSpan> after = statementSpans(trailing);
    QCOMPARE(after.size(), 1);
    QCOMPARE(after.at(0).text, QStringLiteral("SELECT 1"));
    QCOMPARE(after.at(0).end, 8);
}

void TestSqlScan::tablesReadAnEscapedBacktickInAName()
{
    // `` inside a backquoted identifier is one literal backtick, so the schema
    // this names is my`db, not two broken tokens.
    const QHash<QString, QStringList> schema{
        {QStringLiteral("my`db.users"), {QStringLiteral("id")}},
    };
    const StatementTables tables =
        scanStatementTables(QStringLiteral("SELECT * FROM `my``db`.`users` u"), schema);

    QCOMPARE(tables.keys, QStringList({QStringLiteral("my`db.users")}));
    QCOMPARE(tables.byName.value(QStringLiteral("u")), QStringLiteral("my`db.users"));
}

void TestSqlScan::tablesStepOverABlockComment()
{
    // scanStatementTables tokenises separately from the statement splitter, so
    // its own comment handling needs covering rather than the splitter's.
    const QHash<QString, QStringList> schema = testSchema();
    const StatementTables tables = scanStatementTables(
        QStringLiteral("SELECT * FROM mydb.users /* JOIN mydb.orders o */ WHERE id = 1"), schema
    );

    QCOMPARE(tables.keys, QStringList({QStringLiteral("mydb.users")}));
    QVERIFY(!tables.byName.contains(QStringLiteral("o")));
}

QTEST_APPLESS_MAIN(TestSqlScan)

#include "tst_sqlscan.moc"
