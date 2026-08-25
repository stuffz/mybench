#include "ui/tableutil.h"

#include <QFontMetrics>
#include <QHash>
#include <QHeaderView>
#include <QTableWidget>

#include <algorithm>

QTableWidget *makeTable(const QStringList &headers)
{
    auto *t = new QTableWidget;
    t->setColumnCount(int(headers.size()));
    t->setHorizontalHeaderLabels(headers);
    t->verticalHeader()->setVisible(false);
    t->setSelectionBehavior(QAbstractItemView::SelectRows);
    t->setSelectionMode(QAbstractItemView::SingleSelection);
    t->setEditTriggers(QAbstractItemView::NoEditTriggers);
    t->setShowGrid(false);
    t->setAlternatingRowColors(false);
    t->setWordWrap(false);
    t->setSortingEnabled(true);
    t->setTextElideMode(Qt::ElideRight);
    t->verticalHeader()->setDefaultSectionSize(24);
    // Left-aligned headers, like the web tables (Qt centres by default).
    t->horizontalHeader()->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    t->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    t->horizontalHeader()->setStretchLastSection(true);
    return t;
}

namespace
{

// Sorts by the raw value stashed in UserRole while displaying formatted text.
class FormattedNumItem : public QTableWidgetItem
{
public:
    using QTableWidgetItem::QTableWidgetItem;

    bool operator<(const QTableWidgetItem &other) const override
    {
        return data(Qt::UserRole).toLongLong() < other.data(Qt::UserRole).toLongLong();
    }
};

} // namespace

QTableWidgetItem *numItem(qint64 v)
{
    auto *it = new QTableWidgetItem;
    it->setData(Qt::DisplayRole, QVariant::fromValue(v));
    return it;
}

QTableWidgetItem *numItem(qint64 v, const QString &text)
{
    auto *it = new FormattedNumItem(text);
    it->setData(Qt::UserRole, QVariant::fromValue(v));
    it->setToolTip(text);
    return it;
}

void putRow(QTableWidget *t, const QStringList &cells)
{
    const SortPause pause(t);
    const int row = t->rowCount();
    t->insertRow(row);
    for (int i = 0; i < cells.size(); ++i)
    {
        auto *it = new QTableWidgetItem(cells.at(i));
        it->setToolTip(cells.at(i));
        t->setItem(row, i, it);
    }
}

void putRow(QTableWidget *t, const QList<QTableWidgetItem *> &items)
{
    const SortPause pause(t);
    const int row = t->rowCount();
    t->insertRow(row);
    for (int i = 0; i < items.size(); ++i)
    {
        t->setItem(row, i, items.at(i));
    }
}

void putKV(QTableWidget *t, const QVector<QPair<QString, QString>> &rows)
{
    QHash<QString, int> at;
    for (int r = 0; r < t->rowCount(); ++r)
    {
        if (QTableWidgetItem *item = t->item(r, 0))
        {
            at.insert(item->text(), r);
        }
    }

    bool sameRows = t->rowCount() == rows.size();
    if (sameRows)
    {
        for (const auto &kv : rows)
        {
            if (!at.contains(kv.first))
            {
                sameRows = false;
                break;
            }
        }
    }
    if (sameRows)
    {
        // Suspend sorting here too: with the table user-sorted by the Value
        // column, each setText would re-sort and stale the row map mid-loop.
        const SortPause pause(t);
        for (const auto &kv : rows)
        {
            t->item(at.value(kv.first), 1)->setText(kv.second);
        }
        return;
    }

    {
        const SortPause pause(t);
        t->setRowCount(0);
        for (const auto &kv : rows)
        {
            putRow(t, {kv.first, kv.second});
        }
    }
    fitColumns(t);
    t->scrollToTop();
}

SortPause::SortPause(QTableWidget *t) : m_t(t), m_was(t->isSortingEnabled())
{
    t->setSortingEnabled(false);
}

SortPause::~SortPause()
{
    m_t->setSortingEnabled(m_was);
}

void fitColumns(QTableWidget *t, int minChars, int maxChars)
{
    if (!t || t->columnCount() == 0)
    {
        return;
    }
    const QFontMetrics fm(t->font());
    const int ch = qMax(1, fm.horizontalAdvance(QLatin1Char('0')));
    // Let the view measure, then clamp: measuring is what makes a column fit,
    // clamping is what keeps a 4 KB statement from taking the whole viewport.
    t->resizeColumnsToContents();
    for (int c = 0; c < t->columnCount(); ++c)
    {
        t->setColumnWidth(c, qBound(minChars * ch, t->columnWidth(c), maxChars * ch));
    }
}

void fitHeight(QTableWidget *t, int maxHeight)
{
    int h = t->horizontalHeader()->height() + 4;
    for (int r = 0; r < t->rowCount(); ++r)
    {
        h += t->rowHeight(r);
    }
    t->setFixedHeight(std::clamp(h, 32, maxHeight));
}
