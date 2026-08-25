#pragma once
// Widget-free SQL text scanning: statement boundaries for run-at-cursor, the
// fuzzy matcher, and the token walk completion uses to learn which tables a
// statement references. Deliberately not a parser — SQL mid-edit is
// unparseable more often than not, so everything here reads a comment- and
// string-aware token stream and degrades to "no answer" instead of erroring.
#include <QHash>
#include <QString>
#include <QStringList>
#include <QVector>

// fzf-style subsequence match, shared with the graph view's finder: every
// query char must appear in order; word starts and runs score higher, gaps
// cost. Returns false when the query does not match at all.
bool fuzzyScore(const QString &query, const QString &target, int *score);

// The semicolon-delimited statement containing `pos` (string- and
// comment-aware). No DELIMITER support, same as the web build; the backend
// still executes exactly what it is handed.
QString statementAt(const QString &text, int pos);

// Every non-empty statement in the text, in order. Same scan; used to run a
// whole script into one result tab each.
QStringList splitStatements(const QString &text);

// One statement's exact place in the buffer: [start, end) with surrounding
// blanks and the trailing ';' shaved off, so positions inside `text` map
// straight onto editor offsets. Feeds the live syntax check.
struct StatementSpan
{
    int start = 0;
    int end = 0;
    QString text;
};

QVector<StatementSpan> statementSpans(const QString &text);

// Offsets of every '?' parameter placeholder outside strings and comments.
// Placeholders PREPARE cleanly (so the live lint passes them) but are fatal
// to plain execution — the editor warns on them before Run trips over it.
QVector<int> placeholderOffsets(const QString &text);

// Start of the completion prefix: one identifier leftwards from the end of
// `line` (the text left of the cursor), and when it sits behind a dot, the
// qualifier before it too — "mydb.us" is one prefix, so qualified candidates
// rank on the whole path and accepting one replaces the qualifier instead of
// duplicating it.
int completionStart(const QString &line);

// The tables one statement references (FROM/JOIN/UPDATE/INTO targets,
// including inside subqueries), resolved against the completion schema.
struct StatementTables
{
    // Lowercased alias, bare table name and qualified path, each mapped to
    // the schema-map key whose columns it stands for.
    QHash<QString, QString> byName;
    // Schema-map keys in statement order, deduped — feeds the bare-Ctrl+Space
    // column list.
    QStringList keys;
};

// `schema` is the "schema.table" → column names map completion works from.
StatementTables scanStatementTables(const QString &stmt, const QHash<QString, QStringList> &schema);
