#include "editor/resultgrid.h"

#include "app/theme.h"
#include "editor/resultmodel.h"

#include <QApplication>
#include <QClipboard>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFontMetrics>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPlainTextEdit>
#include <QScrollBar>
#include <QSet>
#include <QVBoxLayout>
#include <algorithm>

namespace
{

constexpr int RowHeight = 28;
// Width bounds in characters — same values the web grid used. The header-only
// guess is capped tighter than the sampled fit.
constexpr int FitSample = 200;
constexpr int FitMinCh = 12;
constexpr int FitMaxCh = 60;
constexpr int HeaderMaxCh = 44;
// Matches the inset CellDelegate::paint uses; column widths have to allow for
// it or the last characters of a full-width value are elided.
constexpr int CellPad = 6;

// SQL literal for a generated INSERT. Mirrors internal/sqlesc.Value and the
// sqlLit() helper the web grid carried — keep the three in sync.
QString sqlLit(const QVariant &v)
{
    if (!v.isValid())
    {
        return QStringLiteral("NULL");
    }
    QString s = v.toString();
    s.replace('\\', QStringLiteral("\\\\"));
    s.replace('\'', QStringLiteral("''"));
    s.replace(QChar(0), QStringLiteral("\\0"));
    s.replace('\n', QStringLiteral("\\n"));
    s.replace('\r', QStringLiteral("\\r"));
    s.replace(QChar(0x1a), QStringLiteral("\\Z"));
    return "'" + s + "'";
}

QString quoteId(const QString &id)
{
    QString s = id;
    s.replace('`', QStringLiteral("``"));
    return "`" + s + "`";
}

// Function-local static: a namespace-scope QString would be a throwing
// static initialiser (cert-err58-cpp).
QString &copySep()
{
    static QString sep = QStringLiteral("\t");
    return sep;
}

} // namespace

void ResultGrid::setCopySeparator(const QString &sep)
{
    copySep() = sep.isEmpty() ? QStringLiteral("\t") : sep;
}

// ---------------------------------------------------------------- header ---

TypedHeader::TypedHeader(QWidget *parent) : QHeaderView(Qt::Horizontal, parent)
{
    setSectionsClickable(true);
    setSectionsMovable(false);
    setSectionResizeMode(QHeaderView::Interactive);
    setHighlightSections(false);
    setStretchLastSection(false);
    setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
}

QSize TypedHeader::sizeHint() const
{
    const QFontMetrics fm(font());
    return {QHeaderView::sizeHint().width(), fm.height() * 2 + 8};
}

void TypedHeader::paintSection(QPainter *p, const QRect &rect, int index) const
{
    if (!model())
    {
        return;
    }
    const AppPalette &pal = theme::current();
    p->save();
    p->fillRect(rect, pal.background);
    p->setPen(pal.border);
    p->drawLine(rect.bottomLeft(), rect.bottomRight());
    p->drawLine(rect.topRight(), rect.bottomRight());

    const QString name = model()->headerData(index, Qt::Horizontal, Qt::DisplayRole).toString();
    const QString type = model()->headerData(index, Qt::Horizontal, Qt::UserRole).toString();
    const bool sorted = isSortIndicatorShown() && sortIndicatorSection() == index;
    const QString arrow = sorted
                              ? (sortIndicatorOrder() == Qt::AscendingOrder ? QStringLiteral(" ↑")
                                                                            : QStringLiteral(" ↓"))
                              : QString();

    QFontMetrics fm(p->font());
    const QRect inner = rect.adjusted(6, 2, -4, -2);
    const int lineH = fm.height();

    QFont bold = p->font();
    bold.setWeight(QFont::Medium);
    p->setFont(bold);
    p->setPen(pal.foreground);
    p->drawText(
        QRect(inner.x(), inner.y(), inner.width(), lineH), Qt::AlignLeft | Qt::AlignVCenter,
        fm.elidedText(name + arrow, Qt::ElideRight, inner.width())
    );

    QFont small = p->font();
    small.setWeight(QFont::Normal);
    small.setPixelSize(qMax(9, int(p->font().pixelSize() * 0.8)));
    p->setFont(small);
    p->setPen(pal.mutedFg);
    const QFontMetrics sfm(small);
    p->drawText(
        QRect(inner.x(), inner.y() + lineH, inner.width(), lineH), Qt::AlignLeft | Qt::AlignVCenter,
        sfm.elidedText(type, Qt::ElideRight, inner.width())
    );
    p->restore();
}

// -------------------------------------------------------------- delegate ---

void CellDelegate::paint(QPainter *p, const QStyleOptionViewItem &opt, const QModelIndex &ix) const
{
    const AppPalette &pal = theme::current();
    QStyleOptionViewItem o(opt);
    initStyleOption(&o, ix);

    const bool staged = ix.data(Qt::UserRole + 1).toBool();
    const bool isNull = ix.data(Qt::UserRole + 2).toBool();

    p->save();
    if (o.state & QStyle::State_Selected)
    {
        p->fillRect(o.rect, pal.muted);
    }
    else if (staged)
    {
        p->fillRect(o.rect, QColor(pal.warning.red(), pal.warning.green(), pal.warning.blue(), 38));
    }
    p->setPen(QColor(pal.border.red(), pal.border.green(), pal.border.blue(), 110));
    p->drawLine(o.rect.bottomLeft(), o.rect.bottomRight());

    QFont f = o.font;
    if (isNull)
    {
        f.setItalic(true);
    }
    p->setFont(f);
    p->setPen(staged ? pal.warning : (isNull ? pal.mutedFg : pal.foreground));
    const QFontMetrics fm(f);
    const QRect r = o.rect.adjusted(CellPad, 0, -CellPad, 0);
    p->drawText(
        r, Qt::AlignLeft | Qt::AlignVCenter,
        fm.elidedText(ix.data(Qt::DisplayRole).toString(), Qt::ElideRight, r.width())
    );
    p->restore();
}

QWidget *CellDelegate::createEditor(
    QWidget *parent, const QStyleOptionViewItem &opt, const QModelIndex &ix
) const
{
    QWidget *e = QStyledItemDelegate::createEditor(parent, opt, ix);
    // The cell itself becomes the input, visually unchanged: same background,
    // no border, text at CellPad like the painted value. The min/max-height
    // resets defeat the app stylesheet's global control-height rule, which
    // shrank the editor into a floating box inside the cell.
    if (auto *le = qobject_cast<QLineEdit *>(e))
    {
        const AppPalette &pal = theme::current();
        le->setStyleSheet(QString("QLineEdit { background: %1; color: %2; border: none; "
                                  "border-radius: 0; padding: 0 %3px; margin: 0; "
                                  "min-height: 0px; max-height: 10000px; }")
                              .arg(pal.background.name(), pal.foreground.name())
                              .arg(CellPad));
    }
    return e;
}

void CellDelegate::updateEditorGeometry(
    QWidget *editor, const QStyleOptionViewItem &opt, const QModelIndex &ix
) const
{
    Q_UNUSED(ix);
    // The full cell, minus the row divider the delegate paints on the bottom
    // edge — the grid lines stay put while editing.
    editor->setGeometry(opt.rect.adjusted(0, 0, 0, -1));
}

void CellDelegate::setEditorData(QWidget *editor, const QModelIndex &ix) const
{
    // The base implementation reads EditRole, which renders NULL as the
    // string "NULL" — confirming that edit would stage the literal string.
    // Read the raw value instead: editing a NULL cell starts from empty, and
    // NULL is staged explicitly from the context menu.
    if (auto *e = qobject_cast<QLineEdit *>(editor))
    {
        const QVariant raw = ix.data(Qt::UserRole);
        e->setText(raw.isValid() ? raw.toString() : QString());
        return;
    }
    QStyledItemDelegate::setEditorData(editor, ix);
}

// ------------------------------------------------------------------ grid ---

ResultGrid::ResultGrid(QWidget *parent) : QTableView(parent)
{
    setHorizontalHeader(new TypedHeader(this));
    setItemDelegate(new CellDelegate(this));
    verticalHeader()->setVisible(false);
    verticalHeader()->setDefaultSectionSize(RowHeight);
    verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    setShowGrid(false);
    setSelectionBehavior(QAbstractItemView::SelectItems);
    setSelectionMode(QAbstractItemView::ExtendedSelection);
    setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed);
    setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    setContextMenuPolicy(Qt::CustomContextMenu);
    setWordWrap(false);
    setTextElideMode(Qt::ElideRight);
    // No indicator until a sort has actually been applied server-side.
    horizontalHeader()->setSortIndicatorShown(false);

    connect(this, &QWidget::customContextMenuRequested, this, &ResultGrid::showContextMenu);
    connect(horizontalHeader(), &QHeaderView::sectionClicked, this, &ResultGrid::onHeaderClicked);
    // Single click only selects; the full-value viewer moved to the context
    // menu, so selecting a cell no longer pops an editor in your face.
}

void ResultGrid::mousePressEvent(QMouseEvent *e)
{
    // Right-click inside the current selection must not collapse it to the
    // cell under the cursor — the menu is about to act on the selection.
    if (e->button() == Qt::RightButton)
    {
        const QModelIndex ix = indexAt(e->pos());
        if (ix.isValid() && selectionModel() && selectionModel()->isSelected(ix))
        {
            return;
        }
    }
    QTableView::mousePressEvent(e);
}

void ResultGrid::keyPressEvent(QKeyEvent *e)
{
    if (e->matches(QKeySequence::Copy) && m_model && selectionModel() &&
        !selectionModel()->selectedIndexes().isEmpty())
    {
        QApplication::clipboard()->setText(selectionAsText(false));
        return;
    }
    QTableView::keyPressEvent(e);
}

namespace
{

// The selection, rectangularised: the rows and columns actually present in
// it, sorted, plus the exact cell set so Ctrl-click holes stay detectable.
struct SelShape
{
    QList<int> rows, cols;
    QSet<qint64> cells;

    bool has(int row, int col) const { return cells.contains(qint64(row) << 32 | quint32(col)); }
};

SelShape shapeOf(const QModelIndexList &sel)
{
    SelShape out;
    QSet<int> rowSet, colSet;
    for (const QModelIndex &ix : sel)
    {
        rowSet.insert(ix.row());
        colSet.insert(ix.column());
        out.cells.insert(qint64(ix.row()) << 32 | quint32(ix.column()));
    }
    out.rows = rowSet.values();
    out.cols = colSet.values();
    std::sort(out.rows.begin(), out.rows.end());
    std::sort(out.cols.begin(), out.cols.end());
    return out;
}

} // namespace

// The selection as separator-joined lines; Ctrl-click holes stay empty. The
// optional header row carries only the selected columns' names.
QString ResultGrid::selectionAsText(bool withHeader) const
{
    const SelShape shape = shapeOf(selectionModel()->selectedIndexes());
    const QList<int> &rows = shape.rows;
    const QList<int> &cols = shape.cols;

    QStringList lines;
    if (withHeader)
    {
        QStringList names;
        for (int col : cols)
        {
            names << m_model->columns().at(col).name;
        }
        lines << names.join(copySep());
    }
    for (int row : rows)
    {
        QStringList line;
        for (int col : cols)
        {
            if (!shape.has(row, col))
            {
                line << QString();
                continue;
            }
            const QVariant v = m_model->cell(row, col);
            line << (v.isValid() ? v.toString() : QStringLiteral("NULL"));
        }
        lines << line.join(copySep());
    }
    return lines.join('\n');
}

// One multi-row INSERT from the selection: the selected columns, one VALUES
// tuple per selected row. Ctrl-click holes become NULL — the statement has to
// say something for every column it names.
QString ResultGrid::selectionAsInsert() const
{
    const SelShape shape = shapeOf(selectionModel()->selectedIndexes());
    QStringList names;
    for (int col : shape.cols)
    {
        names << quoteId(m_model->columns().at(col).name);
    }
    QStringList tuples;
    for (int row : shape.rows)
    {
        QStringList vals;
        for (int col : shape.cols)
        {
            vals
                << (shape.has(row, col) ? sqlLit(m_model->cell(row, col)) : QStringLiteral("NULL"));
        }
        tuples << "(" + vals.join(", ") + ")";
    }
    return "INSERT INTO " + quoteId(m_insertSchema) + "." + quoteId(m_insertTable) + " (" +
           names.join(", ") + ") VALUES\n  " + tuples.join(",\n  ") + ";";
}

void ResultGrid::setResultModel(ResultModel *m)
{
    m_model = m;
    setModel(m);
    m_fitted = false;
    connect(m, &ResultModel::windowArrived, this, &ResultGrid::onWindowArrived);
    connect(
        m, &ResultModel::modelReset, this,
        [this]()
        {
            m_fitted = false;
            applyHeaderWidths();
        }
    );
}

void ResultGrid::resetFit()
{
    m_fitted = false;
    horizontalHeader()->setSortIndicatorShown(false);
    applyHeaderWidths();
}

// Stage one: size every column to its name, so a fresh result is readable
// before any row has arrived. Without this the columns sit at the view's
// uniform default until the sampled fit lands — or forever, if the result
// has no rows to sample.
void ResultGrid::applyHeaderWidths()
{
    if (!m_model || m_model->columnCount() == 0)
    {
        return;
    }
    const QFontMetrics fm(font());
    const int ch = qMax(1, fm.horizontalAdvance(QLatin1Char('0')));
    for (int c = 0; c < m_model->columnCount(); ++c)
    {
        const int chars =
            qBound(FitMinCh, int(m_model->columns().at(c).name.size()) + 4, HeaderMaxCh);
        setColumnWidth(c, chars * ch + 2 * CellPad);
    }
}

void ResultGrid::setInsertTarget(const QString &schema, const QString &table)
{
    m_insertSchema = schema;
    m_insertTable = table;
}

void ResultGrid::clearInsertTarget()
{
    m_insertSchema.clear();
    m_insertTable.clear();
}

void ResultGrid::onWindowArrived()
{
    if (!m_fitted)
    {
        autoFit();
    }
}

void ResultGrid::autoFit()
{
    if (!m_model || m_model->rowCount() == 0 || m_model->columnCount() == 0)
    {
        return;
    }
    const QFontMetrics fm(font());
    const int ch = qMax(1, fm.horizontalAdvance(QLatin1Char('0')));
    const int sample = qMin(m_model->rowCount(), FitSample);
    // Only fit once the sampled window is actually here, so widths are not
    // computed from placeholder cells.
    if (!m_model->rowLoaded(0))
    {
        return;
    }
    m_fitted = true;

    for (int c = 0; c < m_model->columnCount(); ++c)
    {
        int content = 4; // "NULL"
        for (int r = 0; r < sample; ++r)
        {
            if (!m_model->rowLoaded(r))
            {
                break;
            }
            const QVariant v = m_model->cell(r, c);
            const int len = v.isValid() ? int(v.toString().size()) : 4;
            content = qMax(content, len);
        }
        const int nameLen = int(m_model->columns().at(c).name.size()) + 4;
        const int chars = qBound(FitMinCh, qMax(nameLen, content + 2), FitMaxCh);
        setColumnWidth(c, chars * ch + 2 * CellPad);
    }
}

void ResultGrid::onHeaderClicked(int section)
{
    if (m_sortable)
    {
        emit sortRequested(section);
    }
}

QString ResultGrid::rowAsInsert(int row) const
{
    QStringList cols, vals;
    for (int c = 0; c < m_model->columnCount(); ++c)
    {
        cols << quoteId(m_model->columns().at(c).name);
        vals << sqlLit(m_model->cell(row, c));
    }
    return "INSERT INTO " + quoteId(m_insertSchema) + "." + quoteId(m_insertTable) + " (" +
           cols.join(", ") + ") VALUES (" + vals.join(", ") + ");";
}

// The full value in a big box — editable (staging on accept) when the cell
// is, a read-only viewer otherwise.
void ResultGrid::openValueDialog(const QModelIndex &ix)
{
    const QString col = m_model->columns().value(ix.column()).name;
    const QVariant v = ix.data(Qt::UserRole);
    const bool editable = m_model->editable() && (m_model->flags(ix) & Qt::ItemIsEditable);

    QDialog dlg(this);
    dlg.setWindowTitle(col);
    auto *lay = new QVBoxLayout(&dlg);
    auto *body = new QPlainTextEdit(v.isValid() ? v.toString() : QString());
    // The editor palette, not the dialog's: the app stylesheet leaves text
    // boxes transparent, which made the value invisible against the dialog.
    {
        const EditorPalette &ep = theme::currentEditor();
        body->setStyleSheet(
            QString("QPlainTextEdit { background: %1; color: %2; "
                    "border: 1px solid %3; border-radius: 6px; padding: 4px; "
                    "selection-background-color: %4; selection-color: %2; }")
                .arg(ep.bg.name(), ep.fg.name(), theme::current().input.name(), ep.sel.name())
        );
    }
    if (!v.isValid())
    {
        body->setPlaceholderText(QStringLiteral("NULL"));
    }
    body->setReadOnly(!editable);
    body->setMinimumSize(560, 260);
    lay->addWidget(body);
    auto *box = new QDialogButtonBox;
    if (editable)
    {
        box->addButton(tr("Stage Edit"), QDialogButtonBox::AcceptRole);
        box->addButton(QDialogButtonBox::Cancel);
    }
    else
    {
        box->addButton(QDialogButtonBox::Close);
    }
    connect(box, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(box, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    lay->addWidget(box);
    if (dlg.exec() != QDialog::Accepted || !editable)
    {
        return;
    }
    // setData treats an unchanged value as a no-op, so accepting without
    // touching the text stays clean.
    m_model->setData(ix, body->toPlainText(), Qt::EditRole);
}

void ResultGrid::showContextMenu(const QPoint &pos)
{
    if (!m_model)
    {
        return;
    }
    const QModelIndex ix = indexAt(pos);
    if (!ix.isValid() || !m_model->rowLoaded(ix.row()))
    {
        return;
    }
    const int row = ix.row();

    QMenu menu(this);
    auto *clip = QApplication::clipboard();

    // Multi-select shows only whole-selection actions: the per-cell and
    // per-row entries act on one cell and would be ambiguous over many.
    const int selCount = int(selectionModel()->selectedIndexes().size());
    if (selCount > 1)
    {
        menu.addAction(
            tr("Copy Selection (%1 Cells)").arg(selCount), this,
            [this, clip]() { clip->setText(selectionAsText(false)); }
        );
        menu.addAction(
            tr("Copy Selection with Headers"), this,
            [this, clip]() { clip->setText(selectionAsText(true)); }
        );
        if (!m_insertTable.isEmpty())
        {
            menu.addAction(
                tr("Copy Selection as INSERT"), this,
                [this, clip]() { clip->setText(selectionAsInsert()); }
            );
        }
        menu.exec(viewport()->mapToGlobal(pos));
        return;
    }

    const bool cellEditable = m_model->editable() && (m_model->flags(ix) & Qt::ItemIsEditable);
    menu.addAction(
        cellEditable ? tr("Edit Value…") : tr("View Value…"), this,
        [this, ix]() { openValueDialog(ix); }
    );
    menu.addSeparator();

    menu.addAction(
        tr("Copy Cell"), this, [this, clip]() { clip->setText(selectionAsText(false)); }
    );
    menu.addAction(
        tr("Copy Cell with Header"), this, [this, clip]() { clip->setText(selectionAsText(true)); }
    );
    menu.addAction(
        tr("Copy Row"), this,
        [this, row, clip]()
        {
            QStringList out;
            for (int c = 0; c < m_model->columnCount(); ++c)
            {
                const QVariant v = m_model->cell(row, c);
                out << (v.isValid() ? v.toString() : QStringLiteral("NULL"));
            }
            clip->setText(out.join(copySep()));
        }
    );
    menu.addAction(
        tr("Copy Row as JSON"), this,
        [this, row, clip]()
        {
            QJsonObject o;
            for (int c = 0; c < m_model->columnCount(); ++c)
            {
                const QVariant v = m_model->cell(row, c);
                o.insert(
                    m_model->columns().at(c).name,
                    v.isValid() ? QJsonValue(v.toString()) : QJsonValue()
                );
            }
            clip->setText(QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Indented)));
        }
    );
    if (!m_insertTable.isEmpty())
    {
        menu.addAction(
            tr("Copy Row as INSERT"), this, [this, row, clip]() { clip->setText(rowAsInsert(row)); }
        );
    }
    if (m_model->editable() && (m_model->flags(ix) & Qt::ItemIsEditable))
    {
        menu.addSeparator();
        menu.addAction(
            tr("Stage NULL"), this, [this, ix]() { m_model->stageNull(ix.row(), ix.column()); }
        );
    }
    menu.exec(viewport()->mapToGlobal(pos));
}
