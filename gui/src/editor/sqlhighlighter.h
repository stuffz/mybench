#pragma once
// MySQL syntax highlighting for the editor. Replaces @codemirror/lang-sql:
// a lexer-shaped QSyntaxHighlighter rather than a real grammar, which is
// what SQL colouring actually needs — strings, comments, numbers, keywords,
// function calls and backquoted identifiers.
#include <QRegularExpression>
#include <QSyntaxHighlighter>
#include <QTextCharFormat>

#include "app/theme.h"

class SqlHighlighter : public QSyntaxHighlighter
{
    Q_OBJECT
public:
    explicit SqlHighlighter(QTextDocument *doc);
    void setPalette(const EditorPalette &p);

    // Every MySQL keyword we colour; also the completion keyword pool.
    static const QStringList &keywords();
    static const QStringList &types();

protected:
    void highlightBlock(const QString &text) override;

private:
    enum State
    {
        Normal = 0,
        InBlockComment = 1
    };

    EditorPalette m_p;
    QTextCharFormat m_keyword, m_type, m_string, m_number, m_comment, m_func, m_ident;
    QRegularExpression m_word;
};
