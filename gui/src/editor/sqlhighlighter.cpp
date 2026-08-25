#include "editor/sqlhighlighter.h"

#include "app/theme.h"

#include <QSet>

namespace
{

// Function-local statics: file-scope QStringList globals would run dynamic
// initialisers before main (cert-err58).
const QStringList &keywordList()
{
    static const QStringList list = {
        "ADD",
        "ALL",
        "ALTER",
        "ANALYZE",
        "AND",
        "AS",
        "ASC",
        "AUTO_INCREMENT",
        "BEFORE",
        "BEGIN",
        "BETWEEN",
        "BY",
        "CASCADE",
        "CASE",
        "CHANGE",
        "CHARACTER",
        "CHECK",
        "COLLATE",
        "COLUMN",
        "COMMIT",
        "CONSTRAINT",
        "CONVERT",
        "CREATE",
        "CROSS",
        "CURRENT_DATE",
        "CURRENT_TIME",
        "CURRENT_TIMESTAMP",
        "DATABASE",
        "DATABASES",
        "DEFAULT",
        "DELAYED",
        "DELETE",
        "DESC",
        "DESCRIBE",
        "DISTINCT",
        "DROP",
        "DUPLICATE",
        "ELSE",
        "END",
        "ENGINE",
        "EXISTS",
        "EXPLAIN",
        "FALSE",
        "FOR",
        "FORCE",
        "FOREIGN",
        "FROM",
        "FULL",
        "FULLTEXT",
        "GRANT",
        "GROUP",
        "HAVING",
        "IF",
        "IGNORE",
        "IN",
        "INDEX",
        "INNER",
        "INSERT",
        "INTERVAL",
        "INTO",
        "IS",
        "JOIN",
        "KEY",
        "KEYS",
        "KILL",
        "LEFT",
        "LIKE",
        "LIMIT",
        "LOCK",
        "MATCH",
        "MODIFY",
        "NATURAL",
        "NOT",
        "NULL",
        "OFFSET",
        "ON",
        "OPTIMIZE",
        "OR",
        "ORDER",
        "OUTER",
        "PARTITION",
        "PRIMARY",
        "PROCEDURE",
        "REFERENCES",
        "REGEXP",
        "RENAME",
        "REPLACE",
        "RESTRICT",
        "REVOKE",
        "RIGHT",
        "ROLLBACK",
        "ROW",
        "SCHEMA",
        "SELECT",
        "SET",
        "SHOW",
        "STRAIGHT_JOIN",
        "TABLE",
        "TABLES",
        "THEN",
        "TO",
        "TRANSACTION",
        "TRIGGER",
        "TRUE",
        "TRUNCATE",
        "UNION",
        "UNIQUE",
        "UNLOCK",
        "UNSIGNED",
        "UPDATE",
        "USE",
        "USING",
        "VALUES",
        "VIEW",
        "WHEN",
        "WHERE",
        "WITH",
        "ZEROFILL",
        "ANALYSE",
    };
    return list;
}

const QStringList &typeList()
{
    static const QStringList list = {
        "BIGINT",     "BINARY",   "BIT",     "BLOB",     "BOOL",      "BOOLEAN",    "CHAR",
        "DATE",       "DATETIME", "DECIMAL", "DOUBLE",   "ENUM",      "FLOAT",      "GEOMETRY",
        "INT",        "INTEGER",  "JSON",    "LONGBLOB", "LONGTEXT",  "MEDIUMBLOB", "MEDIUMINT",
        "MEDIUMTEXT", "NUMERIC",  "REAL",    "SET",      "SMALLINT",  "TEXT",       "TIME",
        "TIMESTAMP",  "TINYBLOB", "TINYINT", "TINYTEXT", "VARBINARY", "VARCHAR",    "YEAR",
    };
    return list;
}

// highlightBlock runs per word per block; a linear QStringList scan there is
// the hottest lookup in the editor.
const QSet<QString> &keywordSet()
{
    static const QSet<QString> set(keywordList().begin(), keywordList().end());
    return set;
}

const QSet<QString> &typeSet()
{
    static const QSet<QString> set(typeList().begin(), typeList().end());
    return set;
}

} // namespace

const QStringList &SqlHighlighter::keywords()
{
    return keywordList();
}

const QStringList &SqlHighlighter::types()
{
    return typeList();
}

SqlHighlighter::SqlHighlighter(QTextDocument *doc)
    : QSyntaxHighlighter(doc), m_word("[A-Za-z_][A-Za-z0-9_$]*")
{
    setPalette(theme::currentEditor());
}

void SqlHighlighter::setPalette(const EditorPalette &p)
{
    m_p = p;
    m_keyword.setForeground(p.keyword);
    m_keyword.setFontWeight(QFont::Medium);
    m_type.setForeground(p.type);
    m_string.setForeground(p.string);
    m_number.setForeground(p.number);
    m_comment.setForeground(p.comment);
    m_comment.setFontItalic(true);
    m_func.setForeground(p.func);
    m_ident.setForeground(p.type);
    rehighlight();
}

void SqlHighlighter::highlightBlock(const QString &text)
{
    const qsizetype n = text.size();
    qsizetype i = 0;

    if (previousBlockState() == InBlockComment)
    {
        const qsizetype end = text.indexOf("*/");
        if (end < 0)
        {
            setFormat(0, int(n), m_comment);
            setCurrentBlockState(InBlockComment);
            return;
        }
        setFormat(0, int(end + 2), m_comment);
        i = end + 2;
    }
    setCurrentBlockState(Normal);

    while (i < n)
    {
        const QChar c = text.at(i);

        // Line comments: -- (SQL), # (MySQL)
        if ((c == '-' && i + 1 < n && text.at(i + 1) == '-') || c == '#')
        {
            setFormat(int(i), int(n - i), m_comment);
            return;
        }
        // Block comment
        if (c == '/' && i + 1 < n && text.at(i + 1) == '*')
        {
            const qsizetype end = text.indexOf("*/", i + 2);
            if (end < 0)
            {
                setFormat(int(i), int(n - i), m_comment);
                setCurrentBlockState(InBlockComment);
                return;
            }
            setFormat(int(i), int(end + 2 - i), m_comment);
            i = end + 2;
            continue;
        }
        // Quoted: '…' and "…" are strings, `…` an identifier. Backslash
        // escapes apply to the first two only, matching MySQL's default.
        if (c == '\'' || c == '"' || c == '`')
        {
            const QChar q = c;
            qsizetype j = i + 1;
            while (j < n)
            {
                if (text.at(j) == '\\' && q != '`')
                {
                    j += 2;
                    continue;
                }
                if (text.at(j) == q)
                {
                    break;
                }
                ++j;
            }
            const qsizetype len = qMin(j + 1, n) - i;
            setFormat(int(i), int(len), q == '`' ? m_ident : m_string);
            i += len;
            continue;
        }
        // Numbers
        if (c.isDigit())
        {
            qsizetype j = i;
            while (j < n && (text.at(j).isDigit() || text.at(j) == '.'))
            {
                ++j;
            }
            setFormat(int(i), int(j - i), m_number);
            i = j;
            continue;
        }
        // Words: keyword, type, or a function call when followed by '('
        if (c.isLetter() || c == '_')
        {
            const auto m = m_word.match(
                text, i, QRegularExpression::NormalMatch,
                QRegularExpression::AnchorAtOffsetMatchOption
            );
            if (!m.hasMatch())
            {
                ++i;
                continue;
            }
            const QString w = m.captured();
            const QString up = w.toUpper();
            const qsizetype j = i + w.size();
            qsizetype k = j;
            while (k < n && text.at(k).isSpace())
            {
                ++k;
            }
            const bool isCall = k < n && text.at(k) == '(';

            if (keywordSet().contains(up))
            {
                setFormat(int(i), int(w.size()), m_keyword);
            }
            else if (typeSet().contains(up))
            {
                setFormat(int(i), int(w.size()), m_type);
            }
            else if (isCall)
            {
                setFormat(int(i), int(w.size()), m_func);
            }
            i = j;
            continue;
        }
        ++i;
    }
}
