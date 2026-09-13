#pragma once
// The results table. QTableView + ResultModel replaces the hand-rolled
// virtualiser: sorting, column drag-resize and cell editing are the view's
// job here. What this class adds is the parts the web Grid had that Qt does
// not ship: a two-line header (name over type), one-shot width auto-fit from
// the first window, staged-edit tinting, and the copy menu.
#include <QHeaderView>
#include <QStyledItemDelegate>
#include <QTableView>

class ResultModel;

// Header that draws the column name with its MySQL type underneath, the way
// the web grid did.
class TypedHeader : public QHeaderView
{
    Q_OBJECT
public:
    explicit TypedHeader(QWidget *parent = nullptr);
    QSize sizeHint() const override;

protected:
    void paintSection(QPainter *p, const QRect &rect, int index) const override;
};

// Paints NULL in italic muted text and staged edits in the warning tone.
class CellDelegate : public QStyledItemDelegate
{
    Q_OBJECT
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    void paint(QPainter *p, const QStyleOptionViewItem &opt, const QModelIndex &ix) const override;
    QWidget *createEditor(QWidget *parent, const QStyleOptionViewItem &opt, const QModelIndex &ix)
        const override;
    void setEditorData(QWidget *editor, const QModelIndex &ix) const override;
    void updateEditorGeometry(
        QWidget *editor, const QStyleOptionViewItem &opt, const QModelIndex &ix
    ) const override;
};

class ResultGrid : public QTableView
{
    Q_OBJECT
public:
    explicit ResultGrid(QWidget *parent = nullptr);

    void setResultModel(ResultModel *m);

    ResultModel *resultModel() const { return m_model; }

    // Enables "Copy Row as INSERT" when the result maps to one table.
    void setInsertTarget(const QString &schema, const QString &table);
    void clearInsertTarget();

    void setSortable(bool on) { m_sortable = on; }

    // Widths land in two stages, as the web grid did: a name-based guess the
    // moment the columns are known, refined once from the first window.
    void resetFit();

    // The cell separator every copy action uses (tab unless the preference
    // says otherwise). Static: one preference, every grid.
    static void setCopySeparator(const QString &sep);

signals:
    void sortRequested(int column);
    // A copy that would have invented data, and why. ResultModel::cell()
    // answers invalid for a window that has not arrived, which every copy path
    // renders as the literal NULL.
    void copyRefused(const QString &reason);

private slots:
    void onWindowArrived();
    void onHeaderClicked(int section);
    void showContextMenu(const QPoint &pos);

protected:
    // Right-click inside the selection keeps it (the menu acts on all of it);
    // Ctrl+C copies the selected cells.
    void mousePressEvent(QMouseEvent *e) override;
    void keyPressEvent(QKeyEvent *e) override;

private:
    bool selectionIsComplete();
    void autoFit();
    void applyHeaderWidths();
    QString rowAsInsert(int row) const;
    void openValueDialog(const QModelIndex &ix);
    QString selectionAsText(bool withHeader) const;
    QString selectionAsInsert() const;

    ResultModel *m_model = nullptr;
    bool m_fitted = false;
    bool m_sortable = true;
    QString m_insertSchema, m_insertTable;
};
