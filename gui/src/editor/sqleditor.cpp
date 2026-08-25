#include "editor/sqleditor.h"

#include "app/theme.h"
#include "editor/sqlhighlighter.h"
#include "editor/sqlscan.h"

#include <QAbstractItemView>
#include <QCompleter>
#include <QCoreApplication>
#include <QKeyEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QResizeEvent>
#include <QScrollBar>
#include <QStandardItemModel>
#include <QStyledItemDelegate>
#include <QTextBlock>
#include <QTimer>
#include <QToolTip>
#include <algorithm>

// A thin child widget whose painting the editor does — the standard Qt
// code-editor arrangement, so scrolling and block geometry come for free.
class LineNumberArea : public QWidget
{
public:
    explicit LineNumberArea(SqlEditor *editor) : QWidget(editor), m_editor(editor) {}

    QSize sizeHint() const override { return {m_editor->lineNumberAreaWidth(), 0}; }

protected:
    void paintEvent(QPaintEvent *event) override { m_editor->paintLineNumbers(event); }

private:
    SqlEditor *m_editor;
};

namespace
{

// What accepting a row inserts. Its own role: in a QStandardItemModel,
// EditRole and DisplayRole are the same cell, so a qualified insert text
// ("so.customer_id") stored there would be overwritten by the display label.
constexpr int kInsertRole = Qt::UserRole + 2;

// Draws "label            detail" the way the CodeMirror popup did.
class CompletionDelegate : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void paint(QPainter *p, const QStyleOptionViewItem &opt, const QModelIndex &ix) const override
    {
        QStyleOptionViewItem o(opt);
        initStyleOption(&o, ix);
        const EditorPalette &pal = theme::currentEditor();
        p->save();
        p->fillRect(o.rect, (o.state & QStyle::State_Selected) ? pal.sel : pal.panel);
        const QRect r = o.rect.adjusted(6, 0, -6, 0);
        p->setPen(pal.fg);
        p->drawText(r, Qt::AlignLeft | Qt::AlignVCenter, ix.data(Qt::DisplayRole).toString());
        const QString detail = ix.data(Qt::UserRole + 1).toString();
        if (!detail.isEmpty())
        {
            p->setPen(pal.comment);
            p->drawText(r, Qt::AlignRight | Qt::AlignVCenter, detail);
        }
        p->restore();
    }

    QSize sizeHint(const QStyleOptionViewItem &opt, const QModelIndex &ix) const override
    {
        QSize s = QStyledItemDelegate::sizeHint(opt, ix);
        const QString detail = ix.data(Qt::UserRole + 1).toString();
        if (!detail.isEmpty())
        {
            s.setWidth(s.width() + opt.fontMetrics.horizontalAdvance(detail) + 32);
        }
        return s;
    }
};

} // namespace

SqlEditor::SqlEditor(QWidget *parent) : QPlainTextEdit(parent)
{
    setPlaceholderText(QStringLiteral("SELECT …"));
    setLineWrapMode(QPlainTextEdit::NoWrap);
    setFrameShape(QFrame::NoFrame);
    document()->setDocumentMargin(6);

    m_hl = new SqlHighlighter(document());

    m_gutter = new LineNumberArea(this);
    connect(this, &QPlainTextEdit::blockCountChanged, this, [this]() { updateGutterWidth(); });
    connect(
        this, &QPlainTextEdit::updateRequest, this,
        [this](const QRect &rect, int dy)
        {
            if (dy != 0)
            {
                m_gutter->scroll(0, dy);
            }
            else
            {
                m_gutter->update(0, rect.y(), m_gutter->width(), rect.height());
            }
        }
    );
    updateGutterWidth();

    m_model = new QStandardItemModel(this);
    m_completer = new QCompleter(m_model, this);
    // We rank and filter ourselves (fuzzy, not prefix), so the popup must
    // show the model as given.
    m_completer->setCompletionMode(QCompleter::UnfilteredPopupCompletion);
    m_completer->setCaseSensitivity(Qt::CaseInsensitive);
    m_completer->setCompletionRole(Qt::DisplayRole);
    m_completer->setWidget(this);
    m_completer->popup()->setItemDelegate(new CompletionDelegate(m_completer->popup()));
    // activated(QString) hands back the display text, so go via the index and
    // read the insert text out of its role.
    connect(
        m_completer, QOverload<const QModelIndex &>::of(&QCompleter::activated), this,
        [this](const QModelIndex &ix) { insertCompletion(ix.data(kInsertRole).toString()); }
    );

    m_changeTimer = new QTimer(this);
    m_changeTimer->setSingleShot(true);
    m_changeTimer->setInterval(300);
    connect(
        m_changeTimer, &QTimer::timeout, this, [this]() { emit documentSettled(toPlainText()); }
    );
    connect(
        this, &QPlainTextEdit::textChanged, this,
        [this]()
        {
            m_changeTimer->start();
            if (m_completer->popup()->isVisible())
            {
                updateCompletions();
            }
        }
    );
    // Repaint the gutter so the current-line number follows the cursor.
    connect(this, &QPlainTextEdit::cursorPositionChanged, this, [this]() { m_gutter->update(); });

    setEditorPalette(theme::currentEditor());
    setEditorFontSize(13);
    setTabWidthChars(4);
}

void SqlEditor::setEditorPalette(const EditorPalette &p)
{
    m_p = p;
    m_hl->setPalette(p);
    QPalette q = palette();
    q.setColor(QPalette::Base, p.bg);
    q.setColor(QPalette::Text, p.fg);
    q.setColor(QPalette::Highlight, p.sel);
    q.setColor(QPalette::HighlightedText, p.fg);
    q.setColor(QPalette::PlaceholderText, p.comment);
    setPalette(q);
    if (m_gutter)
    {
        m_gutter->update();
    }
    restyle();
}

void SqlEditor::setEditorFontSize(int px)
{
    // Same logical-DPI correction the UI font gets in theme::apply, so the
    // two sliders mean the same physical size.
    m_fontPx = theme::dpiPx(px);
    // setFont alone is not enough — the app stylesheet's QWidget font rule
    // overrides it — but the painting code (gutter, tab stops) reads font(),
    // so both the widget font and the stylesheet have to carry the size.
    QFont f = font();
    f.setFamily(theme::monoFamily());
    f.setPixelSize(m_fontPx);
    setFont(f);
    restyle();
    setTabWidthChars(m_tabChars);
    updateGutterWidth();
}

// The stylesheet has a global QWidget rule; be explicit so the editor keeps
// its own themed background and its own font size (the editor slider, not
// the UI one).
void SqlEditor::restyle()
{
    setStyleSheet(QString("QPlainTextEdit { background: %1; color: %2; border: 1px solid %3; "
                          "border-radius: 6px; font-family: \"%4\"; font-size: %5px; }")
                      .arg(m_p.bg.name(), m_p.fg.name(), m_p.sel.name(), theme::monoFamily())
                      .arg(m_fontPx));
}

void SqlEditor::setTabWidthChars(int chars)
{
    m_tabChars = qMax(1, chars);
    setTabStopDistance(QFontMetricsF(font()).horizontalAdvance(QLatin1Char(' ')) * m_tabChars);
}

int SqlEditor::lineNumberAreaWidth() const
{
    // Count the rows the viewport can show, not just the ones with text: the
    // gutter numbers past the last line, so a two-line buffer in a tall editor
    // still needs room for two digits.
    const int rowH = qMax(1, fontMetrics().height());
    const int visibleRows = viewport() ? viewport()->height() / rowH + 1 : 0;
    int digits = 1;
    for (int lines = qMax(qMax(1, blockCount()), visibleRows); lines >= 10; lines /= 10)
    {
        ++digits;
    }
    // Two digits minimum so a short buffer does not shift as it grows past 9.
    return 10 + fontMetrics().horizontalAdvance(QLatin1Char('9')) * qMax(2, digits);
}

void SqlEditor::updateGutterWidth()
{
    setViewportMargins(lineNumberAreaWidth(), 0, 0, 0);
    placeGutter();
}

// contentsRect(), not the widget rect: the stylesheet gives this editor a 1px
// border and a radius, which insets the viewport. Block geometry is reported in
// viewport coordinates, so a gutter pinned to the widget's own origin drew every
// number offset by the frame — and came up short of the full height.
void SqlEditor::placeGutter()
{
    const QRect cr = contentsRect();
    m_gutter->setGeometry(QRect(cr.left(), cr.top(), lineNumberAreaWidth(), cr.height()));
}

void SqlEditor::resizeEvent(QResizeEvent *e)
{
    QPlainTextEdit::resizeEvent(e);
    // A taller editor can mean more phantom rows and so a wider gutter, which
    // is a viewport margin change — not just a reposition.
    updateGutterWidth();
}

void SqlEditor::paintLineNumbers(QPaintEvent *event)
{
    QPainter p(m_gutter);
    p.fillRect(event->rect(), m_p.bg);

    QTextBlock block = firstVisibleBlock();
    int number = block.blockNumber();
    qreal top = blockBoundingGeometry(block).translated(contentOffset()).top();
    const int current = textCursor().blockNumber();

    p.setFont(font());
    const int lineHeight = fontMetrics().height();
    const int gutterRight = m_gutter->width() - 6;
    // Row pitch for the phantom rows below the text. Measured as the distance
    // between consecutive block tops: the LAST block's bounding rect carries the
    // document's bottom margin, so inheriting its height spaced them far too
    // widely. Falls back to the font's line spacing for a single-line buffer.
    qreal rowHeight = fontMetrics().lineSpacing();
    qreal previousTop = top;
    bool pitchMeasured = false;

    while (block.isValid() && top <= event->rect().bottom())
    {
        if (!pitchMeasured && top > previousTop)
        {
            rowHeight = top - previousTop;
            pitchMeasured = true;
        }
        previousTop = top;
        const qreal bottom = top + blockBoundingRect(block).height();
        if (block.isVisible() && bottom >= event->rect().top())
        {
            // The cursor's line reads at full contrast, the rest muted — the
            // gutter should be legible without competing with the SQL.
            p.setPen(number == current ? m_p.fg : m_p.comment);
            // One line height from the block top, not centred in the block: a
            // wrapped block is taller than a line, and centring made the number
            // drift away from the row it labels.
            p.drawText(
                QRect(0, int(top), gutterRight, lineHeight), Qt::AlignRight | Qt::AlignVCenter,
                QString::number(number + 1)
            );
        }
        block = block.next();
        top = bottom;
        ++number;
    }

    // Past the last line, keep numbering to the bottom of the gutter so a short
    // buffer still reads as a numbered editor instead of a couple of digits
    // floating in the dark. Dimmer than the real ones, since there is no line
    // there yet.
    QColor phantom = m_p.comment;
    phantom.setAlphaF(0.45);
    p.setPen(phantom);
    // Continue from the last block's top plus one pitch, not from where its
    // bounding rect ended: that rect includes the document's bottom margin,
    // which showed up as a wider gap at the boundary.
    if (pitchMeasured || rowHeight > 0)
    {
        top = previousTop + rowHeight;
    }
    while (top <= event->rect().bottom() && rowHeight > 0)
    {
        p.drawText(
            QRect(0, int(top), gutterRight, lineHeight), Qt::AlignRight | Qt::AlignVCenter,
            QString::number(number + 1)
        );
        top += rowHeight;
        ++number;
    }
}

void SqlEditor::rebuildPool(const QHash<QString, QStringList> &schema)
{
    m_pool.clear();
    QSet<QString> schemas;
    // Same dedupe as lib/sqlComplete.ts: one entry per column name, the first
    // location as detail, so wide prod schemas do not flood the pool.
    QHash<QString, QPair<QString, int>> columns;

    for (auto it = schema.constBegin(); it != schema.constEnd(); ++it)
    {
        const QString &key = it.key();
        const qsizetype dot = key.indexOf('.');
        if (dot < 0)
        {
            continue; // qualified keys are canonical; bare ones duplicate them
        }
        const QString sch = key.left(dot);
        const QString table = key.mid(dot + 1);
        if (!schemas.contains(sch))
        {
            schemas.insert(sch);
            m_pool.append({sch, QStringLiteral("schema"), 2});
        }
        m_pool.append({table, sch, 1});
        m_pool.append({key, {}, -1});
        for (const QString &col : it.value())
        {
            auto cur = columns.find(col);
            if (cur == columns.end())
            {
                columns.insert(col, {key, 1});
            }
            else
            {
                cur->second++;
            }
        }
    }
    for (auto it = columns.constBegin(); it != columns.constEnd(); ++it)
    {
        const QString detail = it->second > 1
                                   ? it->first + " +" + QString::number(it->second - 1) + " more"
                                   : it->first;
        m_pool.append({it.key(), detail, -2});
    }
    for (const QString &kw : SqlHighlighter::keywords())
    {
        m_pool.append({kw, QStringLiteral("keyword"), -3});
    }
}

void SqlEditor::setCompletionSchema(const QHash<QString, QStringList> &schema)
{
    m_schema = schema;
    rebuildPool(schema);
}

QString SqlEditor::wordUnderCursor() const
{
    QTextCursor c = textCursor();
    c.select(QTextCursor::WordUnderCursor);
    return c.selectedText();
}

void SqlEditor::updateCompletions(bool manual)
{
    // The prefix is what sits immediately left of the cursor — WordUnderCursor
    // would also grab the tail of the word we are inside.
    QTextCursor c = textCursor();
    const QString line = c.block().text().left(c.positionInBlock());
    const QString prefix = line.mid(completionStart(line));

    // What the popup will show: label is displayed, insert replaces the whole
    // prefix (so a qualified insert keeps the alias the user typed).
    struct Row
    {
        QString label;
        QString insert;
        QString detail;
    };

    QVector<Row> rows;

    const StatementTables ctx =
        scanStatementTables(statementAt(toPlainText(), c.position()), m_schema);

    const qsizetype dot = prefix.lastIndexOf('.');
    const QString tableKey = dot > 0 ? ctx.byName.value(prefix.left(dot).toLower()) : QString();

    if (!tableKey.isEmpty())
    {
        // "alias." / "table.": that table's columns only.
        const QString part = prefix.mid(dot + 1);
        const QString qual = prefix.left(dot + 1);

        struct Scored
        {
            int score;
            const QString *col;
        };

        QVector<Scored> hits;
        const QStringList cols = m_schema.value(tableKey);
        for (const QString &col : cols)
        {
            int s = 0;
            if (!part.isEmpty() && !fuzzyScore(part, col, &s))
            {
                continue;
            }
            hits.append({s, &col});
        }
        std::stable_sort(
            hits.begin(), hits.end(),
            [](const Scored &a, const Scored &b) { return a.score > b.score; }
        );
        for (const Scored &h : hits)
        {
            rows.append({*h.col, qual + *h.col, tableKey});
        }
    }
    else if (prefix.isEmpty())
    {
        // Bare Ctrl+Space: the columns of the tables the statement references
        // ("… WHERE <Ctrl+Space>"), or schemas and tables when it references
        // none yet. Typing retriggers always carry a prefix, so only the
        // explicit key lands here.
        if (!manual)
        {
            m_completer->popup()->hide();
            return;
        }
        QSet<QString> seen;
        for (const QString &key : ctx.keys)
        {
            const QStringList cols = m_schema.value(key);
            for (const QString &col : cols)
            {
                if (seen.contains(col))
                {
                    continue;
                }
                seen.insert(col);
                rows.append({col, col, key});
            }
        }
        if (rows.isEmpty())
        {
            for (int boost : {2, 1}) // schemas first, then tables
            {
                QVector<Row> group;
                for (const Candidate &cand : m_pool)
                {
                    if (cand.boost == boost)
                    {
                        group.append({cand.label, cand.label, cand.detail});
                    }
                }
                std::sort(
                    group.begin(), group.end(),
                    [](const Row &a, const Row &b) { return a.label < b.label; }
                );
                rows += group;
            }
            if (rows.size() > 50)
            {
                rows.resize(50);
            }
        }
    }
    else
    {
        struct Scored
        {
            int score;
            const Candidate *c;
        };

        QVector<Scored> hits;
        hits.reserve(256);
        for (const Candidate &cand : m_pool)
        {
            int s = 0;
            if (!fuzzyScore(prefix, cand.label, &s))
            {
                continue;
            }
            hits.append({s + cand.boost * 2, &cand});
        }
        std::partial_sort(
            hits.begin(), hits.begin() + qMin<qsizetype>(hits.size(), 50), hits.end(),
            [](const Scored &a, const Scored &b) { return a.score > b.score; }
        );
        hits.resize(qMin<qsizetype>(hits.size(), 50));
        for (const Scored &h : hits)
        {
            rows.append({h.c->label, h.c->label, h.c->detail});
        }
    }

    if (rows.isEmpty())
    {
        m_completer->popup()->hide();
        return;
    }

    m_model->clear();
    for (const Row &row : rows)
    {
        auto *item = new QStandardItem;
        item->setData(row.insert, kInsertRole);
        item->setData(row.label, Qt::DisplayRole);
        item->setData(row.detail, Qt::UserRole + 1);
        m_model->appendRow(item);
    }
    m_completer->setCompletionPrefix(QString()); // unfiltered: we did the work
    QRect r = cursorRect();
    r.setWidth(
        m_completer->popup()->sizeHintForColumn(0) +
        m_completer->popup()->verticalScrollBar()->sizeHint().width() + 24
    );
    m_completer->complete(r);
    m_completer->popup()->setCurrentIndex(m_model->index(0, 0));
}

void SqlEditor::insertCompletion(const QString &text)
{
    QTextCursor c = textCursor();
    const QString line = c.block().text().left(c.positionInBlock());
    const int start = completionStart(line);
    c.setPosition(c.block().position() + start, QTextCursor::MoveAnchor);
    c.setPosition(c.block().position() + int(line.size()), QTextCursor::KeepAnchor);
    c.insertText(text);
    setTextCursor(c);
}

QString SqlEditor::statementToRun() const
{
    const QTextCursor c = textCursor();
    if (c.hasSelection())
    {
        return c.selectedText().replace(QChar(0x2029), '\n').trimmed();
    }
    return statementAt(toPlainText(), c.position());
}

void SqlEditor::setDiagnostics(const QVector<EditorDiagnostic> &diags)
{
    m_diags = diags;
    const int docEnd = qMax(0, int(document()->characterCount()) - 1);
    QList<QTextEdit::ExtraSelection> sels;
    for (const EditorDiagnostic &d : m_diags)
    {
        QTextEdit::ExtraSelection sel;
        sel.cursor = textCursor();
        sel.cursor.setPosition(qBound(0, d.from, docEnd));
        sel.cursor.setPosition(qBound(0, d.to, docEnd), QTextCursor::KeepAnchor);
        sel.format.setUnderlineStyle(QTextCharFormat::WaveUnderline);
        sel.format.setUnderlineColor(
            d.warning ? theme::current().warning : theme::current().destructive
        );
        sels.append(sel);
    }
    setExtraSelections(sels);
}

bool SqlEditor::event(QEvent *e)
{
    if (e->type() == QEvent::ToolTip)
    {
        auto *he = static_cast<QHelpEvent *>(e);
        const int pos = cursorForPosition(viewport()->mapFrom(this, he->pos())).position();
        for (const EditorDiagnostic &d : m_diags)
        {
            if (pos >= d.from && pos <= d.to)
            {
                QToolTip::showText(he->globalPos(), d.message, this);
                return true;
            }
        }
        QToolTip::hideText();
        return true;
    }
    return QPlainTextEdit::event(e);
}

void SqlEditor::insertSnippet(const QString &sql)
{
    QTextCursor c = textCursor();
    c.insertText(sql);
    setTextCursor(c);
    setFocus();
}

void SqlEditor::keyPressEvent(QKeyEvent *e)
{
    QAbstractItemView *popup = m_completer->popup();
    if (popup->isVisible() && !e->modifiers().testFlag(Qt::ControlModifier))
    {
        switch (e->key())
        {
        case Qt::Key_Enter:
        case Qt::Key_Return:
        case Qt::Key_Tab:
            // Accept the highlighted completion.
            if (popup->currentIndex().isValid())
            {
                insertCompletion(popup->currentIndex().data(kInsertRole).toString());
                popup->hide();
                return;
            }
            break;
        case Qt::Key_Escape:
            popup->hide();
            return;
        case Qt::Key_Up:
        case Qt::Key_Down:
        case Qt::Key_PageUp:
        case Qt::Key_PageDown:
            QCoreApplication::sendEvent(popup, e);
            return;
        default:
            break;
        }
    }

    const bool ctrl = e->modifiers().testFlag(Qt::ControlModifier);
    const bool shift = e->modifiers().testFlag(Qt::ShiftModifier);

    if (ctrl && (e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter))
    {
        if (shift)
        {
            emit runScriptRequested();
        }
        else
        {
            emit runRequested();
        }
        return;
    }
    if (ctrl && shift && e->key() == Qt::Key_F)
    {
        emit formatRequested();
        return;
    }
    if (ctrl && e->key() == Qt::Key_Space)
    {
        updateCompletions(true);
        return;
    }

    // Bracket/quote closing, the closeBrackets() extension's job.
    static const QHash<QString, QString> pairs{
        {"(", ")"}, {"[", "]"}, {"'", "'"}, {"\"", "\""}, {"`", "`"}
    };
    static const QString closers = ")]'\"`";
    const QString t = e->text();

    // Type-over: typing the closing character when it is already there (because
    // we inserted it) should step past it, not add a second one. Without this,
    // typing COUNT(*) leaves COUNT(*)).
    if (!ctrl && t.size() == 1 && closers.contains(t) && !textCursor().hasSelection())
    {
        QTextCursor c = textCursor();
        const QString ahead = c.block().text().mid(c.positionInBlock(), 1);
        if (ahead == t)
        {
            c.movePosition(QTextCursor::Right);
            setTextCursor(c);
            return;
        }
    }

    if (!ctrl && pairs.contains(t) && !textCursor().hasSelection())
    {
        QTextCursor c = textCursor();
        // Only auto-close when what follows is whitespace, a closer, or the end
        // of the line — closing in the middle of a word is never wanted.
        const QString ahead = c.block().text().mid(c.positionInBlock(), 1);
        const bool wrapWord = !ahead.isEmpty() && (ahead.at(0).isLetterOrNumber() || ahead == "_");
        // A quote right after a word is an apostrophe or a closing quote
        // (don't, `col`); pairing it would double it.
        const QString behind =
            c.positionInBlock() > 0 ? c.block().text().mid(c.positionInBlock() - 1, 1) : QString();
        const bool quote = t == "'" || t == "\"" || t == "`";
        const bool afterWord =
            quote && !behind.isEmpty() && (behind.at(0).isLetterOrNumber() || behind == "_");
        QPlainTextEdit::keyPressEvent(e);
        if (!wrapWord && !afterWord)
        {
            QTextCursor ins = textCursor();
            ins.insertText(pairs.value(t));
            ins.movePosition(QTextCursor::Left);
            setTextCursor(ins);
        }
        return;
    }

    QPlainTextEdit::keyPressEvent(e);

    // Retrigger completion while typing a word.
    if (!ctrl && !t.isEmpty() && (t.at(0).isLetterOrNumber() || t.at(0) == '_' || t.at(0) == '.'))
    {
        updateCompletions();
    }
}
