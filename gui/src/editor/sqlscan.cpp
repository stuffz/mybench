#include "editor/sqlscan.h"

#include <QSet>
#include <QVector>

bool fuzzyScore(const QString &query, const QString &target, int *score)
{
    const QString q = query.toLower();
    const QString t = target.toLower();
    int s = 0;
    qsizetype from = 0, last = -2;
    for (const QChar &ch : q)
    {
        const qsizetype idx = t.indexOf(ch, from);
        if (idx < 0)
        {
            return false;
        }
        const bool boundary = idx == 0 || t.at(idx - 1) == '.' || t.at(idx - 1) == '_';
        const bool consecutive = idx == last + 1;
        s += (boundary ? 3 : 0) + (consecutive ? 2 : 0) - int(idx - from);
        last = idx;
        from = idx + 1;
    }
    if (score)
    {
        *score = s;
    }
    return true;
}

namespace
{

// Statement start offsets: 0, then just past every top-level ';'. Quoted
// strings, backquoted identifiers and comments are skipped so a semicolon
// inside them is not a boundary.
QVector<int> statementBounds(const QString &text)
{
    QVector<int> bounds{0};
    const int n = int(text.size());
    int i = 0;
    while (i < n)
    {
        const QChar c = text.at(i);
        if (c == '\'' || c == '"' || c == '`')
        {
            const QChar q = c;
            ++i;
            while (i < n && text.at(i) != q)
            {
                i += (text.at(i) == '\\' && q != '`') ? 2 : 1;
            }
            ++i;
        }
        else if ((c == '-' && i + 1 < n && text.at(i + 1) == '-') || c == '#')
        {
            // Both line-comment forms skip to the newline.
            while (i < n && text.at(i) != '\n')
            {
                ++i;
            }
        }
        else if (c == '/' && i + 1 < n && text.at(i + 1) == '*')
        {
            i += 2;
            while (i < n && !(text.at(i) == '*' && i + 1 < n && text.at(i + 1) == '/'))
            {
                ++i;
            }
            i += 2;
        }
        else
        {
            if (c == ';')
            {
                bounds.append(i + 1);
            }
            ++i;
        }
    }
    bounds.append(n);
    return bounds;
}

// One statement range, trailing delimiter and blanks removed.
QString statementIn(const QString &text, const QVector<int> &bounds, int b)
{
    QString stmt = text.mid(bounds.at(b), bounds.at(b + 1) - bounds.at(b));
    qsizetype end = stmt.size();
    while (end > 0 && stmt.at(end - 1).isSpace())
    {
        --end;
    }
    if (end > 0 && stmt.at(end - 1) == ';')
    {
        --end;
    }
    return stmt.left(end).trimmed();
}

} // namespace

QString statementAt(const QString &text, int pos)
{
    const QVector<int> bounds = statementBounds(text);
    // The range the cursor is in, walking back past empty ones: right after
    // typing "SELECT 1;" the cursor sits past the delimiter, where the range
    // is empty — the statement meant is the one before it.
    for (int b = int(bounds.size()) - 2; b >= 0; --b)
    {
        if (pos < bounds.at(b))
        {
            continue;
        }
        const QString stmt = statementIn(text, bounds, b);
        if (!stmt.isEmpty())
        {
            return stmt;
        }
    }
    // Cursor before every statement (leading blank lines): take the first
    // non-empty one rather than the whole buffer.
    for (int b = 0; b + 1 < bounds.size(); ++b)
    {
        const QString stmt = statementIn(text, bounds, b);
        if (!stmt.isEmpty())
        {
            return stmt;
        }
    }
    return {};
}

QStringList splitStatements(const QString &text)
{
    const QVector<int> bounds = statementBounds(text);
    QStringList out;
    for (int b = 0; b + 1 < bounds.size(); ++b)
    {
        const QString stmt = statementIn(text, bounds, b);
        if (!stmt.isEmpty())
        {
            out << stmt;
        }
    }
    return out;
}

QVector<StatementSpan> statementSpans(const QString &text)
{
    const QVector<int> bounds = statementBounds(text);
    QVector<StatementSpan> out;
    for (int b = 0; b + 1 < bounds.size(); ++b)
    {
        int from = bounds.at(b);
        int to = bounds.at(b + 1);
        while (from < to && text.at(from).isSpace())
        {
            ++from;
        }
        while (to > from && text.at(to - 1).isSpace())
        {
            --to;
        }
        if (to > from && text.at(to - 1) == ';')
        {
            --to;
        }
        while (to > from && text.at(to - 1).isSpace())
        {
            --to;
        }
        if (to > from)
        {
            out.append({from, to, text.mid(from, to - from)});
        }
    }
    return out;
}

QVector<int> placeholderOffsets(const QString &text)
{
    QVector<int> out;
    const int n = int(text.size());
    int i = 0;
    while (i < n)
    {
        const QChar c = text.at(i);
        if (c == '\'' || c == '"' || c == '`')
        {
            const QChar q = c;
            ++i;
            while (i < n && text.at(i) != q)
            {
                i += (text.at(i) == '\\' && q != '`') ? 2 : 1;
            }
            ++i;
        }
        else if ((c == '-' && i + 1 < n && text.at(i + 1) == '-') || c == '#')
        {
            while (i < n && text.at(i) != '\n')
            {
                ++i;
            }
        }
        else if (c == '/' && i + 1 < n && text.at(i + 1) == '*')
        {
            i += 2;
            while (i < n && !(text.at(i) == '*' && i + 1 < n && text.at(i + 1) == '/'))
            {
                ++i;
            }
            i += 2;
        }
        else
        {
            if (c == '?')
            {
                out.append(i);
            }
            ++i;
        }
    }
    return out;
}

namespace
{

bool isWordChar(QChar ch)
{
    return ch.isLetterOrNumber() || ch == '_' || ch == '$';
}

} // namespace

int completionStart(const QString &line)
{
    qsizetype start = line.size();
    while (start > 0 && isWordChar(line.at(start - 1)))
    {
        --start;
    }
    if (start > 0 && line.at(start - 1) == '.')
    {
        --start;
        while (start > 0 && isWordChar(line.at(start - 1)))
        {
            --start;
        }
    }
    return int(start);
}

namespace
{

enum class Tok
{
    Word,  // bare identifier or keyword, text as written
    Ident, // backquoted identifier, text unquoted — always a name, never a keyword
    Text,  // string literal or number; never a name
    Punct, // one character: . , ( ) and the rest
};

struct Token
{
    Tok kind;
    QString text;
};

// Comments and whitespace are dropped; strings and numbers survive only as
// Text so the walk steps over them without mistaking their content for names.
QVector<Token> tokenize(const QString &sql)
{
    QVector<Token> out;
    const int n = int(sql.size());
    int i = 0;
    while (i < n)
    {
        const QChar c = sql.at(i);
        if (c.isSpace())
        {
            ++i;
        }
        else if (c == '\'' || c == '"')
        {
            ++i;
            while (i < n && sql.at(i) != c)
            {
                i += (sql.at(i) == '\\') ? 2 : 1;
            }
            ++i;
            out.append({Tok::Text, {}});
        }
        else if (c == '`')
        {
            QString name;
            ++i;
            while (i < n)
            {
                if (sql.at(i) == '`')
                {
                    if (i + 1 < n && sql.at(i + 1) == '`') // `` escapes a backtick
                    {
                        name += '`';
                        i += 2;
                        continue;
                    }
                    ++i;
                    break;
                }
                name += sql.at(i++);
            }
            out.append({Tok::Ident, name});
        }
        else if ((c == '-' && i + 1 < n && sql.at(i + 1) == '-') || c == '#')
        {
            while (i < n && sql.at(i) != '\n')
            {
                ++i;
            }
        }
        else if (c == '/' && i + 1 < n && sql.at(i + 1) == '*')
        {
            i += 2;
            while (i < n && !(sql.at(i) == '*' && i + 1 < n && sql.at(i + 1) == '/'))
            {
                ++i;
            }
            i += 2;
        }
        else if (isWordChar(c))
        {
            const int start = i;
            while (i < n && isWordChar(sql.at(i)))
            {
                ++i;
            }
            out.append({c.isDigit() ? Tok::Text : Tok::Word, sql.mid(start, i - start)});
        }
        else
        {
            out.append({Tok::Punct, QString(c)});
            ++i;
        }
    }
    return out;
}

// Words that can follow a table reference but are never its alias.
const QSet<QString> &aliasStopWords()
{
    static const QSet<QString> set{
        QStringLiteral("as"),        QStringLiteral("on"),
        QStringLiteral("using"),     QStringLiteral("where"),
        QStringLiteral("set"),       QStringLiteral("join"),
        QStringLiteral("inner"),     QStringLiteral("left"),
        QStringLiteral("right"),     QStringLiteral("full"),
        QStringLiteral("outer"),     QStringLiteral("cross"),
        QStringLiteral("natural"),   QStringLiteral("group"),
        QStringLiteral("order"),     QStringLiteral("limit"),
        QStringLiteral("having"),    QStringLiteral("union"),
        QStringLiteral("values"),    QStringLiteral("select"),
        QStringLiteral("partition"), QStringLiteral("for"),
        QStringLiteral("lock"),      QStringLiteral("window"),
        QStringLiteral("force"),     QStringLiteral("use"),
        QStringLiteral("ignore"),    QStringLiteral("straight_join"),
    };
    return set;
}

// The schema-map key a referenced path stands for: exact first, then
// case-insensitive, and a bare name accepts any schema's table of that name.
QString resolveKey(const QString &path, const QHash<QString, QStringList> &schema)
{
    if (schema.contains(path))
    {
        return path;
    }
    const bool bare = !path.contains('.');
    for (auto it = schema.constBegin(); it != schema.constEnd(); ++it)
    {
        const QString &k = it.key();
        if (k.compare(path, Qt::CaseInsensitive) == 0 ||
            (bare && k.section('.', 1).compare(path, Qt::CaseInsensitive) == 0))
        {
            return k;
        }
    }
    return {};
}

} // namespace

StatementTables scanStatementTables(const QString &stmt, const QHash<QString, QStringList> &schema)
{
    const QVector<Token> toks = tokenize(stmt);
    StatementTables out;

    const auto isName = [&toks](qsizetype i)
    { return i < toks.size() && (toks.at(i).kind == Tok::Word || toks.at(i).kind == Tok::Ident); };
    const auto word = [&toks](qsizetype i)
    {
        return i < toks.size() && toks.at(i).kind == Tok::Word ? toks.at(i).text.toLower()
                                                               : QString();
    };
    const auto punct = [&toks](qsizetype i, QChar ch)
    { return i < toks.size() && toks.at(i).kind == Tok::Punct && toks.at(i).text == ch; };

    const auto learn = [&out](const QString &name, const QString &key)
    {
        if (!name.isEmpty() && !out.byName.contains(name))
        {
            out.byName.insert(name, key);
        }
    };

    qsizetype i = 0;
    while (i < toks.size())
    {
        const QString kw = word(i);
        // FROM and multi-table UPDATE take comma-separated lists; JOIN and
        // INTO take one table each.
        const bool listable = kw == QLatin1String("from") || kw == QLatin1String("update");
        if (!listable && kw != QLatin1String("join") && kw != QLatin1String("into"))
        {
            ++i;
            continue;
        }
        ++i;
        // A derived table ("FROM (SELECT …) x") has no schema columns, so it
        // stops the list here; the walk still reaches the subquery's own FROM
        // tokens and picks its tables up.
        while (isName(i))
        {
            QString path = toks.at(i).text;
            ++i;
            if (punct(i, '.') && isName(i + 1))
            {
                path += '.' + toks.at(i + 1).text;
                i += 2;
            }

            // "AS alias", or a bare word that is not the start of the next
            // clause. A backquoted name is always an alias.
            QString alias;
            if (word(i) == QLatin1String("as") && isName(i + 1))
            {
                alias = toks.at(i + 1).text;
                i += 2;
            }
            else if (isName(i) && (toks.at(i).kind == Tok::Ident ||
                                   !aliasStopWords().contains(toks.at(i).text.toLower())))
            {
                alias = toks.at(i).text;
                ++i;
            }

            const QString key = resolveKey(path, schema);
            if (!key.isEmpty())
            {
                if (!out.keys.contains(key))
                {
                    out.keys.append(key);
                }
                learn(path.toLower(), key);
                if (path.contains('.'))
                {
                    learn(path.section('.', 1).toLower(), key);
                }
                learn(alias.toLower(), key);
            }

            if (!(listable && punct(i, ',')))
            {
                break;
            }
            ++i;
        }
    }
    return out;
}
