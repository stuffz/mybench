#pragma once
// The SQL editor. This is the one component with no off-the-shelf Qt
// equivalent, so it reimplements what CodeMirror gave us: syntax colouring
// (SqlHighlighter), statement-aware fuzzy completion, bracket closing and the
// Ctrl+Enter / Ctrl+Shift+F keys. The widget-free text scanning it builds on
// (statement boundaries, fuzzy matcher, table references) lives in sqlscan.h.
#include <QHash>
#include <QPlainTextEdit>
#include <QStringList>
#include <QVector>

#include "app/theme.h"

class LineNumberArea;
class QCompleter;
class QStandardItemModel;
class QTimer;
class SqlHighlighter;

// One live-lint finding: [from, to) buffer offsets plus the message the
// tooltip shows. Warnings (a '?' placeholder) squiggle in the warning tone,
// errors in the destructive one.
struct EditorDiagnostic
{
    int from = 0;
    int to = 0;
    QString message;
    bool warning = false;
};

class SqlEditor : public QPlainTextEdit
{
    Q_OBJECT
public:
    explicit SqlEditor(QWidget *parent = nullptr);

    // "schema.table" → column names, exactly the map AdminService.SchemaMap
    // returns; also the completion pool.
    void setCompletionSchema(const QHash<QString, QStringList> &schema);

    // The selection if there is one, else the semicolon-delimited statement
    // under the cursor (string- and comment-aware).
    QString statementToRun() const;

    void insertSnippet(const QString &sql);
    // Squiggles the given ranges; an empty list clears them. Ranges track
    // subsequent edits via their cursors until the next lint pass lands.
    void setDiagnostics(const QVector<EditorDiagnostic> &diags);
    void setEditorPalette(const EditorPalette &p);
    void setEditorFontSize(int px);
    void setTabWidthChars(int chars);

    // Painted by the gutter child widget.
    void paintLineNumbers(QPaintEvent *event);
    int lineNumberAreaWidth() const;

signals:
    // Ctrl+Enter: the statement at the cursor. Ctrl+Shift+Enter: the whole
    // script. Both exist because the buttons do, and muscle memory reaches for
    // the keyboard.
    void runRequested();
    void runScriptRequested();
    void formatRequested();
    // Debounced, feeds workspace persistence like the CodeMirror listener did.
    void documentSettled(const QString &sql);

protected:
    void keyPressEvent(QKeyEvent *e) override;
    void resizeEvent(QResizeEvent *e) override;
    bool event(QEvent *e) override; // diagnostic tooltips

private:
    struct Candidate
    {
        QString label;
        QString detail;
        int boost = 0;
    };

    void rebuildPool(const QHash<QString, QStringList> &schema);
    // `manual` is the explicit Ctrl+Space press, which also completes on an
    // empty prefix (columns of the statement's tables); typing retriggers
    // never do.
    void updateCompletions(bool manual = false);
    void insertCompletion(const QString &text);
    QString wordUnderCursor() const;
    void updateGutterWidth();
    void placeGutter();

    LineNumberArea *m_gutter = nullptr;
    QCompleter *m_completer = nullptr;
    QStandardItemModel *m_model = nullptr;
    SqlHighlighter *m_hl = nullptr;
    QTimer *m_changeTimer = nullptr;
    QVector<Candidate> m_pool;
    QVector<EditorDiagnostic> m_diags;
    // The raw schema map, kept for table-scoped completion ("alias.").
    QHash<QString, QStringList> m_schema;
    EditorPalette m_p;
    int m_tabChars = 4;
    int m_fontPx = 13;
    // Rebuilds the per-widget stylesheet: colours AND font, because the app
    // stylesheet's global QWidget font-size rule overrides plain setFont().
    void restyle();
};
