#include "editor/resultmodel.h"

#include "app/api.h"

#include <QJsonArray>
#include <QJsonObject>
#include <utility>

ResultModel::ResultModel(QObject *parent) : QAbstractTableModel(parent) {}

void ResultModel::setResult(const QString &resultId, const QVector<ColumnMeta> &cols, int rowCount)
{
    beginResetModel();
    m_resultId = resultId;
    m_cols = cols;
    m_rows = rowCount;
    m_cache.clear();
    m_pending.clear();
    m_staged.clear();
    m_editable = false;
    m_editableCols.clear();
    m_keyCols.clear();
    endResetModel();
    emit stagedChanged(0);
}

void ResultModel::setRowCount(int rowCount)
{
    if (rowCount == m_rows)
    {
        return;
    }
    if (rowCount > m_rows)
    {
        beginInsertRows({}, m_rows, rowCount - 1);
        // The tail window may have been cached while still partial.
        const int tail = m_rows / Window;
        m_cache.remove(tail);
        m_rows = rowCount;
        endInsertRows();
    }
    else
    {
        beginResetModel();
        m_rows = rowCount;
        m_cache.clear();
        m_pending.clear();
        // Staged edits keyed by row would point at removed or shifted rows.
        m_staged.clear();
        endResetModel();
    }
}

void ResultModel::invalidateWindows()
{
    m_cache.clear();
    m_pending.clear();
    if (m_rows > 0 && !m_cols.isEmpty())
    {
        emit dataChanged(index(0, 0), index(m_rows - 1, int(m_cols.size()) - 1));
    }
}

void ResultModel::clearResult()
{
    beginResetModel();
    m_resultId.clear();
    m_cols.clear();
    m_rows = 0;
    m_cache.clear();
    m_pending.clear();
    m_staged.clear();
    endResetModel();
}

int ResultModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : m_rows;
}

int ResultModel::columnCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : int(m_cols.size());
}

void ResultModel::fetchWindow(int w) const
{
    if (m_resultId.isEmpty() || m_pending.contains(w))
    {
        return;
    }
    m_pending.insert(w);
    auto *self = const_cast<ResultModel *>(this);
    const QString id = m_resultId;
    api()->call(
        "query", "Rows", {id, w * Window, Window}, self,
        [self, w, id](const QJsonValue &res, const QString &err)
        {
            self->m_pending.remove(w);
            if (self->m_resultId != id)
            {
                return; // result swapped under us
            }
            if (!err.isEmpty())
            {
                emit self->error(err);
                return;
            }
            QVector<QVector<QVariant>> rows;
            const QJsonArray arr = res.toObject().value("rows").toArray();
            rows.reserve(arr.size());
            for (const auto &r : arr)
            {
                const QJsonArray cells = r.toArray();
                QVector<QVariant> row;
                row.reserve(cells.size());
                for (const auto &c : cells)
                {
                    row.append(c.isNull() ? QVariant() : QVariant(c.toString()));
                }
                rows.append(std::move(row));
            }
            self->m_cache.insert(w, rows);
            const int from = w * Window;
            const int to = qMin(from + Window, self->m_rows) - 1;
            if (to >= from && !self->m_cols.isEmpty())
            {
                emit self->dataChanged(
                    self->index(from, 0), self->index(to, int(self->m_cols.size()) - 1)
                );
            }
            emit self->windowArrived();
        }
    );
}

bool ResultModel::rowLoaded(int row) const
{
    const int w = row / Window;
    auto it = m_cache.constFind(w);
    if (it == m_cache.constEnd())
    {
        return false;
    }
    return (row - w * Window) < it->size();
}

QVariant ResultModel::cell(int row, int col) const
{
    if (row < 0 || row >= m_rows || col < 0 || col >= m_cols.size())
    {
        return {};
    }
    const int w = row / Window;
    auto it = m_cache.constFind(w);
    if (it == m_cache.constEnd())
    {
        fetchWindow(w);
        return {};
    }
    const int local = row - w * Window;
    if (local >= it->size())
    {
        // A tail window fetched mid-stream can be short; refetch once the
        // rows behind it exist.
        if (qsizetype(w) * Window + it->size() < m_rows)
        {
            const_cast<ResultModel *>(this)->m_cache.remove(w);
        }
        fetchWindow(w);
        return {};
    }
    const QVector<QVariant> &row_ = it->at(local);
    return col < row_.size() ? row_.at(col) : QVariant();
}

QVariant ResultModel::data(const QModelIndex &ix, int role) const
{
    if (!ix.isValid())
    {
        return {};
    }
    const auto st = m_staged.constFind(key(ix.row(), ix.column()));
    const bool isStaged = st != m_staged.constEnd();

    switch (role)
    {
    case Qt::DisplayRole:
    case Qt::EditRole:
    {
        const QVariant v = isStaged ? st->value : cell(ix.row(), ix.column());
        if (isStaged || rowLoaded(ix.row()))
        {
            return v.isValid() ? v : QStringLiteral("NULL");
        }
        return QStringLiteral("…"); // window in flight
    }
    case Qt::ToolTipRole:
    {
        const QVariant v = isStaged ? st->value : cell(ix.row(), ix.column());
        return v.isValid() ? v : QStringLiteral("NULL");
    }
    case Qt::UserRole: // raw value: invalid means SQL NULL
        return isStaged ? st->value : cell(ix.row(), ix.column());
    case Qt::UserRole + 1:
        return isStaged;
    case Qt::UserRole + 2:
    {
        const QVariant v = isStaged ? st->value : cell(ix.row(), ix.column());
        return !v.isValid() && rowLoaded(ix.row()); // render NULL in italics
    }
    default:
        return {};
    }
}

QVariant ResultModel::headerData(int section, Qt::Orientation o, int role) const
{
    if (o != Qt::Horizontal || section < 0 || section >= m_cols.size())
    {
        return {};
    }
    switch (role)
    {
    case Qt::DisplayRole:
        return m_cols.at(section).name;
    case Qt::UserRole:
        return m_cols.at(section).type;
    case Qt::ToolTipRole:
        return m_cols.at(section).name + " (" + m_cols.at(section).type + ")";
    default:
        return {};
    }
}

Qt::ItemFlags ResultModel::flags(const QModelIndex &ix) const
{
    Qt::ItemFlags f = Qt::ItemIsSelectable | Qt::ItemIsEnabled;
    if (m_editable && m_editableCols.contains(ix.column()))
    {
        f |= Qt::ItemIsEditable;
    }
    return f;
}

void ResultModel::setEditable(
    bool on, const QSet<int> &editableCols, const QVector<QPair<QString, int>> &keyCols
)
{
    m_editable = on;
    m_editableCols = editableCols;
    m_keyCols = keyCols;
}

StagedEdit ResultModel::buildEdit(int row, int col, const QVariant &value) const
{
    StagedEdit e;
    e.row = row;
    e.col = col;
    e.colName = col < m_cols.size() ? m_cols.at(col).name : QString();
    e.value = value;
    for (const auto &k : m_keyCols)
    {
        e.key.insert(k.first, cell(row, k.second));
    }
    return e;
}

bool ResultModel::setData(const QModelIndex &ix, const QVariant &value, int role)
{
    if (role != Qt::EditRole || !m_editable || !m_editableCols.contains(ix.column()))
    {
        return false;
    }
    // Identity has to come from loaded key cells; refuse rather than stage an
    // edit whose WHERE clause we cannot build.
    if (!rowLoaded(ix.row()))
    {
        emit const_cast<ResultModel *>(this)->error(
            QStringLiteral("row not loaded yet — scroll to it and retry")
        );
        return false;
    }
    // The inline editor shows NULL as empty text, so empty handed back on a
    // NULL cell means "still NULL", not an edit to ''.
    const auto same = [](const QVariant &a, const QVariant &b)
    {
        if (a.isValid() != b.isValid())
        {
            return a.toString().isEmpty() && b.toString().isEmpty();
        }
        return !a.isValid() || a.toString() == b.toString();
    };
    const QVariant buffer = cell(ix.row(), ix.column());
    if (same(value, buffer))
    {
        // Committing the original value back is an un-edit, not a dirty cell:
        // a plain double-click-and-Enter stays clean, and retyping the old
        // value clears a pending edit.
        if (m_staged.remove(key(ix.row(), ix.column())) > 0)
        {
            emit dataChanged(ix, ix);
            emit stagedChanged(int(m_staged.size()));
        }
        return true;
    }
    if (auto it = m_staged.constFind(key(ix.row(), ix.column()));
        it != m_staged.constEnd() && same(value, it->value))
    {
        return true; // re-committing the pending value changes nothing
    }
    stage(buildEdit(ix.row(), ix.column(), value));
    return true;
}

void ResultModel::stage(const StagedEdit &e)
{
    m_staged.insert(key(e.row, e.col), e);
    const QModelIndex ix = index(e.row, e.col);
    emit dataChanged(ix, ix);
    emit stagedChanged(int(m_staged.size()));
}

void ResultModel::stageNull(int row, int col)
{
    if (!rowLoaded(row))
    {
        return;
    }
    stage(buildEdit(row, col, QVariant()));
}

void ResultModel::discardStaged()
{
    if (m_staged.isEmpty())
    {
        return;
    }
    m_staged.clear();
    if (m_rows > 0 && !m_cols.isEmpty())
    {
        emit dataChanged(index(0, 0), index(m_rows - 1, int(m_cols.size()) - 1));
    }
    emit stagedChanged(0);
}
