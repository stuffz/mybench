#pragma once
// Shared plumbing for the read-only tables in the admin panels and the
// inspector: construction, cell helpers, and content-based column sizing.
// The result grid has its own (windowed) model and does its own fitting.
#include <QString>
#include <QStringList>
#include <QVector>

class QTableWidget;
class QTableWidgetItem;

QTableWidget *makeTable(const QStringList &headers);

// Numeric cells must sort numerically, not lexically. The text overload keeps
// a formatted rendering ("1,024", "1.2 MiB") while still sorting by value.
QTableWidgetItem *numItem(qint64 v);
QTableWidgetItem *numItem(qint64 v, const QString &text);

void putRow(QTableWidget *t, const QStringList &cells);
void putRow(QTableWidget *t, const QList<QTableWidgetItem *> &items);

// Fills a two-column key/value table, keys in column 0. A table that refreshes
// twice a second-ish cannot be rebuilt each time: that throws away the scroll
// position and the selection, which makes reading a row impossible. Same rows
// → the values are updated in place.
void putKV(QTableWidget *t, const QVector<QPair<QString, QString>> &rows);

// Suspends sorting while rows are inserted: with sorting live, setItem on the
// sort column can re-sort the row away mid-insert, leaving the remaining
// cells on whatever row took its index. Restores (and re-sorts) on scope exit.
class SortPause
{
public:
    explicit SortPause(QTableWidget *t);
    ~SortPause();
    SortPause(const SortPause &) = delete;
    SortPause &operator=(const SortPause &) = delete;

private:
    QTableWidget *m_t;
    bool m_was;
};

// Size columns to their contents, bounded so one long value (a statement, a
// user agent) cannot push everything else off screen. Same character clamp the
// result grid uses. Call after populating.
void fitColumns(QTableWidget *t, int minChars = 8, int maxChars = 60);

// Size a table to its rows, up to maxHeight: an empty list should be one
// header line, not a screen of blank grid. Call after populating.
void fitHeight(QTableWidget *t, int maxHeight);
